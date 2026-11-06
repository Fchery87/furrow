#pragma once

#include "furrow/PatternStore.h"
#include "furrow/Probability.h"
#include "furrow/Rules.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace furrow
{

// The host's transport at the start of a block.
struct BlockInfo
{
    bool playing = false;
    double beat = 0.0;          // project position in quarter notes
    double bpm = 120.0;
    double sampleRate = 44100.0;
    int numSamples = 0;
};

// One MIDI message from a lane. The engine does not pick MIDI channels: the status holds the
// message kind only (0x80, 0x90, 0xB0 or 0xE0) and the caller routes the lane.
struct MidiEvent
{
    int offset = 0;             // sample offset inside the block
    int lane = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

// Edits. The numbers match SeqCore's wire format 1, so the golden traces use them directly.
enum class Op : int
{
    SetStep = 2,        // a pattern, b lane, c step, d field, v value
    AddLane = 3,        // a lane, b id, c probability key, d note, v row
    RemoveLane = 4,     // a lane
    SetLane = 5,        // a lane, b field (ProbabilityKey to ModBCc), c value
    SetRack = 6,        // a field, b value
    SetLength = 7,      // a pattern, b lane, c length from 1 to 512
    QueuePattern = 10,  // a pattern, beat: the project beat where it starts
    ClearPattern = 11,  // a pattern
    // Plugin-only operation. Keep it outside SeqCore's wire opcode range so golden traces that
    // intentionally reject opcode 12 continue to exercise the same validation boundary.
    ClearPatternLane = 1000, // a pattern, b lane
};

struct Command
{
    Op op = Op::SetStep;
    int a = 0, b = 0, c = 0, d = 0, v = 0;
    double beat = 0.0;
};

// Transaction results, numbered as SeqCore's acknowledgements.
enum class Result : int
{
    Ok = 1,
    Invalid = 3,    // a command is malformed or out of range; nothing changed
    Capacity = 4,   // the edit would exceed the page pool or value limit; nothing changed
    LoadError = 6,  // the last state load failed; edits are refused until a good load
};

enum class LoadError : int
{
    None = 0, Format = 1, Truncated = 2, Range = 3, Capacity = 4, TooLarge = 5
};

// The sequencer engine: SeqCore's V1 rules in C++.
//
// It follows the host transport block by block, turns steps into timed MIDI events, applies
// edits as all-or-nothing transactions, and saves and loads SeqCore's state format 1.
//
// Threading: one thread at a time. apply, audition, panic and process are real-time safe (no
// allocation, locks or I/O) and run on the audio thread. saveState and loadState allocate and
// belong on another thread, with the audio thread kept out while they run.
//
// Events go to an emit callback, in the order SeqCore sends them. The events of one block are
// in time order, except that a block's settling events (see settle) come first at offset 0.
class Engine
{
public:
    static constexpr std::int32_t stateMagic = 4617586;
    static constexpr std::int32_t stateFormat = 1;
    // SeqCore reads at most this many state words.
    static constexpr std::size_t stateWordsMax = 2508288;

    // SeqCore waits two blocks before playing after a load or a return to Live mode, so the
    // REAPER item gate can close. The plugin has no item gate and passes 0. The golden tests
    // pass 2 to replay SeqCore.
    explicit Engine (int liveHoldBlocks = 0) : liveHoldBlocks_ (liveHoldBlocks)
    {
        resetState();
        held_.fill (-1);
        phT_.fill (-1.0);
        lastBend_.fill (0);
        lastModA_.fill (modRest);
        lastModB_.fill (modRest);
        relV_.fill (64);
        rel_.fill (-1.0);
        aud_.fill (-1);
        savedHeld_.fill (-1);
        posn_.fill (-1);
    }

    void prepare (double sampleRate) { srate_ = sampleRate; }

    // For a host reset: the next block releases every held note and controller and starts
    // over, as after a state load.
    void reset() { stateReplaced_ = true; }

    // Applies the commands as one transaction: all of them, or none.
    template <typename Emit>
    Result apply (std::span<const Command> commands, Emit&& emit)
    {
        settle (emit);
        if (loadError_ != LoadError::None)
            return Result::LoadError;
        if (commands.size() > static_cast<std::size_t> (transactionMax))
            return Result::Capacity;
        const Result result = preflight (commands);
        if (result == Result::Ok)
            applyChecked (commands, emit);
        return result;
    }

    // Plays a lane's note now with its own settings and default step values, for 100 ms. Only
    // while stopped; otherwise ignored.
    template <typename Emit>
    void audition (int lane, int velocity, Emit&& emit)
    {
        settle (emit);
        if (! wasPlaying_ && inRange (lane, 0, lanes - 1) && laneField (lane, Active) && inRange (velocity, 1, 127))
        {
            sendHit (lane, 0, laneField (lane, Note), velocity, -1, emit);
            aud_[at (lane)] = static_cast<int> (std::floor (0.1 * srate_));
        }
    }

    // Releases every note and controller, then sends all-notes-off on every lane.
    template <typename Emit>
    void panic (Emit&& emit)
    {
        settle (emit);
        releaseAll (0, emit);
        for (int s = 0; s < lanes; ++s)
            emit (MidiEvent { 0, s, 0xB0, 123, 0 });
    }

    // Runs one block. Empty blocks are ignored and change nothing.
    template <typename Emit>
    void process (const BlockInfo& block, Emit&& emit)
    {
        settle (emit);
        if (block.numSamples <= 0)
            return;

        const bool transportValid = std::isfinite (block.beat) && std::isfinite (block.bpm) && block.bpm > 0
                                 && std::isfinite (block.sampleRate) && block.sampleRate > 0;
        srate_ = block.sampleRate > 0 && std::isfinite (block.sampleRate) ? block.sampleRate : srate_;
        tempo_ = block.bpm;
        beatPosition_ = std::isfinite (block.beat) ? block.beat : 0.0;
        samples_ = block.numSamples;

        playing_ = mode_ == 0 && liveHold_ <= 0 && loadError_ == LoadError::None && block.playing && transportValid;
        if (liveHold_ > 0)
            --liveHold_;

        if (! playing_)
            stoppedBlock (emit);
        else
            playingBlock (emit);
    }

    // State format 1 as words: SeqCore's @serialize layout. A state that failed to load is
    // given back unchanged, so a newer format is never replaced by an empty rack.
    void saveState (std::vector<std::int32_t>& words) const
    {
        words.clear();
        if (loadError_ != LoadError::None && ! rawWords_.empty())
        {
            words = rawWords_;
            return;
        }
        words.push_back (stateMagic);
        words.push_back (stateFormat);
        for (const int v : { rackId_, seed_, pattern_, mode_, swing_, truncate_ })
            words.push_back (v);
        for (const auto w : encodeDouble (origin_))
            words.push_back (w);
        words.push_back (laneCount_);
        for (int s = 0; s < lanes; ++s)
        {
            if (! laneField (s, Active))
                continue;
            words.push_back (s);
            for (int f = Id; f < laneFields; ++f)
                words.push_back (laneField (s, f));
            words.push_back (held_[at (s)]);
        }
        words.push_back (store_.lengthCount());
        store_.forEachLength ([&] (int key, int length) {
            words.push_back (key);
            words.push_back (length);
        });
        words.push_back (store_.nonDefaultCount());
        store_.forEachValue ([&] (int key, int cell, int value) {
            words.push_back (key);
            words.push_back (cell);
            words.push_back (value);
        });
    }

    // Replaces the state. The words are checked completely first. A bad state leaves the
    // musical state alone but silences playback and refuses edits until a good load, as SeqCore
    // does, and saveState gives it back. Held notes are released at the start of the next
    // block. An empty list resets to an empty rack.
    LoadError loadState (std::span<const std::int32_t> words)
    {
        rawWords_.clear();
        if (words.empty())
        {
            resetState();
            loadError_ = LoadError::None;
        }
        else if (words.size() > stateWordsMax)
        {
            loadError_ = LoadError::TooLarge;
        }
        else
        {
            loadError_ = parseState (words, false);
            if (loadError_ == LoadError::None)
                parseState (words, true);
            else
                rawWords_.assign (words.begin(), words.end());
        }
        stateReplaced_ = true;
        return loadError_;
    }

    int pattern() const { return pattern_; }
    int pendingPattern() const { return pendingPattern_; }
    int playhead (int lane) const { return posn_[at (lane)]; }
    bool playing() const { return playing_; }
    LoadError loadError() const { return loadError_; }
    int laneField (int lane, int field) const { return lane_[at (lane)][at (field)]; }
    int rackField (int field) const
    {
        switch (field)
        {
            case RackId: return rackId_;
            case Seed: return seed_;
            case CurrentPattern: return pattern_;
            case Mode: return mode_;
            case GlobalSwing: return swing_;
            case Truncate: return truncate_;
            default: return 0;
        }
    }
    const PatternStore& store() const { return store_; }

private:
    static constexpr int modRest = 64;
    static constexpr double rampFitBeats = 0.000001;
    static constexpr double epsilonSteps = 0.000001;

    static std::size_t at (int i) { return static_cast<std::size_t> (i); }

    static MidiEvent event (int offset, int lane, int status, int data1, int data2)
    {
        return { offset, lane, static_cast<std::uint8_t> (status), static_cast<std::uint8_t> (data1),
                 static_cast<std::uint8_t> (data2) };
    }

    void resetState()
    {
        store_.reset();
        for (auto& l : lane_)
            l.fill (0);
        laneCount_ = 0;
        rackId_ = 0;
        liveHold_ = liveHoldBlocks_;
        seed_ = 0;
        pattern_ = 0;
        mode_ = 0;
        swing_ = 0;
        truncate_ = 0;
        origin_ = 0.0;
    }

    // Work SeqCore does at the start of a block, before commands: the first block sends
    // all-notes-off on every lane, and a replaced state releases everything held.
    template <typename Emit>
    void settle (Emit& emit)
    {
        if (startupRelease_)
        {
            for (int s = 0; s < lanes; ++s)
            {
                if (savedHeld_[at (s)] >= 0)
                    emit (event (0, s, 0x80, savedHeld_[at (s)], 0));
                emit (event (0, s, 0xB0, 123, 0));
                savedHeld_[at (s)] = -1;
            }
            startupRelease_ = false;
        }
        if (stateReplaced_)
        {
            releaseAll (0, emit);
            wasPlaying_ = false;
            stateReplaced_ = false;
        }
    }

    // Timing ---------------------------------------------------------------------------------

    // The sample offset of a beat inside the block. Inverts
    // beats = (tempo * x + slope * x^2 / 2) / (60 * srate) for x, then rounds and clamps.
    int offsetOf (double beat) const
    {
        const double beats = beat - beatPosition_;
        const double root = tempo_ * tempo_ + 2 * slope_ * 60 * srate_ * beats;
        double x = root > 0 ? 120 * srate_ * beats / (tempo_ + std::sqrt (root)) : 60 * srate_ * beats / tempo_;
        x = std::floor (x + 0.5);
        return static_cast<int> (std::max (0.0, std::min (static_cast<double> (samples_ - 1), x)));
    }

    template <typename Emit>
    void stoppedBlock (Emit& emit)
    {
        if (wasPlaying_)
            releaseAll (0, emit);
        wasPlaying_ = false;
        prevTempo_ = -1;
        posn_.fill (-1);
        // A stopped switch is immediate and puts the pattern's first step at the cursor.
        if (pendingPattern_ >= 0)
        {
            pattern_ = pendingPattern_;
            pendingPattern_ = -1;
            origin_ = beatPosition_;
            nextK_ = 0;
        }
        for (int s = 0; s < lanes; ++s)
        {
            int& left = aud_[at (s)];
            if (left < 0)
                continue;
            if (left < samples_)
                sendOff (s, left, emit);
            else
                left -= samples_;
        }
    }

    template <typename Emit>
    void playingBlock (Emit& emit)
    {
        // A tempo change inside the last block moves this block's start by less than that
        // block's beats, so only a larger jump is a seek or loop wrap.
        const bool jumped = ! wasPlaying_ || std::abs (beatPosition_ - blockEndBeat_) > blockBeats_;
        if (jumped)
        {
            releaseAll (0, emit);
            nextK_ = firstStep();
        }
        // REAPER 7.82 reports tempo once per block and ramps linearly in time. Continue the last
        // block's tempo change only if that block advanced exactly as a ramp would.
        slope_ = 0;
        if (! jumped && prevTempo_ >= 0)
        {
            const double rampBeats = (prevTempo_ + tempo_) * prevBlock_ / (120 * srate_);
            if (std::abs (beatPosition_ - prevBeat_ - rampBeats) < rampFitBeats)
                slope_ = (tempo_ - prevTempo_) / prevBlock_;
        }
        prevTempo_ = tempo_;
        prevBeat_ = beatPosition_;
        prevBlock_ = samples_;
        wasPlaying_ = true;
        const double n = samples_;
        blockBeats_ = (tempo_ * n + slope_ * n * n / 2) / (60 * srate_);
        blockEndBeat_ = beatPosition_ + blockBeats_;
        const bool soloOn = anySolo();

        for (;;)
        {
            double hit = origin_ + static_cast<double> (nextK_) * stepBeats;
            // A queued switch lands on its boundary, ahead of any step at or after it.
            if (pendingPattern_ >= 0 && pendingBeat_ < blockEndBeat_ && pendingBeat_ <= hit + 0.000001)
            {
                flushPending (pendingBeat_, emit);
                flushReleases (pendingBeat_, true, emit);
                releaseAll (offsetOf (pendingBeat_), emit);
                pattern_ = pendingPattern_;
                pendingPattern_ = -1;
                origin_ = pendingBeat_;
                nextK_ = std::max<std::int64_t> (0, firstStep());
                posn_.fill (-1);
                hit = origin_ + static_cast<double> (nextK_) * stepBeats;
            }
            if (hit >= blockEndBeat_)
                break;
            flushPending (hit, emit);
            flushReleases (hit, true, emit);
            if (nextK_ >= 0)
                for (int s = 0; s < lanes; ++s)
                    if (audible (s, soloOn))
                        scheduleStep (s, hit);
            ++nextK_;
        }
        flushPending (blockEndBeat_, emit);
        flushReleases (blockEndBeat_, false, emit);
    }

    // The first step at or after the block start, counted from the pattern origin.
    std::int64_t firstStep() const
    {
        return static_cast<std::int64_t> (std::ceil ((beatPosition_ - origin_) / stepBeats - epsilonSteps));
    }

    // Sets up lane s's hits for the step at `hit`, if it plays.
    void scheduleStep (int s, double hit)
    {
        const int pos = static_cast<int> (nextK_ % store_.length (pattern_, s));
        posn_[at (s)] = pos;
        if (! store_.get (pattern_, s, pos, On) || ! stepPlays (s, hit, pos))
            return;
        const int note = laneField (s, Note) + store_.get (pattern_, s, pos, Pitch);
        if (note < 0 || note > 127)
            return;
        // Swing delays every second step by a third of a step at full global and lane swing.
        // Shift then delays the step by its fraction of a step, after swing.
        double hitT = nextK_ % 2 == 1 ? hit + swing_ / 10000.0 * laneField (s, Swing) / 10000.0 * stepBeats / 3 : hit;
        hitT += store_.get (pattern_, s, pos, Shift) / 10000.0 * stepBeats;
        double hitEnd = hitT + store_.get (pattern_, s, pos, Gate) / 10000.0 * stepBeats;
        if (truncate_)
            hitEnd = std::min (hitEnd, hit + stepBeats);
        // A truncated step that shift pushed to its boundary has no room and is dropped.
        if (hitEnd <= hitT)
            return;
        // The gate divides into equal ratchet intervals, each released by its end.
        phN_[at (s)] = store_.get (pattern_, s, pos, Ratchet);
        phD_[at (s)] = (hitEnd - hitT) / phN_[at (s)];
        phT_[at (s)] = hitT;
        phNote_[at (s)] = note;
        phPos_[at (s)] = pos;
    }

    // The project step number is the floor of the hit's absolute beat over the step length.
    bool stepPlays (int s, double hit, int pos) const
    {
        return probability::plays (store_.get (pattern_, s, pos, Probability), seed_, pattern_,
                                   laneField (s, ProbabilityKey), std::floor (hit / stepBeats + epsilonSteps));
    }

    // Lanes and notes ------------------------------------------------------------------------

    bool anySolo() const
    {
        for (int s = 0; s < lanes; ++s)
            if (laneField (s, Active) && laneField (s, Solo))
                return true;
        return false;
    }

    bool audible (int s, bool soloOn) const
    {
        return laneField (s, Active) && ! laneField (s, Mute) && (! soloOn || laneField (s, Solo));
    }

    template <typename Emit>
    void sendOff (int s, int offset, Emit& emit)
    {
        if (held_[at (s)] >= 0)
        {
            emit (event (offset, s, 0x80, held_[at (s)], relV_[at (s)]));
            held_[at (s)] = -1;
        }
        relV_[at (s)] = 64;
        rel_[at (s)] = -1;
        aud_[at (s)] = -1;
        phT_[at (s)] = -1;
    }

    // Puts the lane's pitch bend and Mod A and B back to rest. Only controllers a step moved
    // are sent.
    template <typename Emit>
    void resetControllers (int s, int offset, Emit& emit)
    {
        if (lastBend_[at (s)] != 0)
            emit (event (offset, s, 0xE0, 0, 64));
        if (lastModA_[at (s)] != modRest)
            emit (event (offset, s, 0xB0, laneField (s, ModACc), modRest));
        if (lastModB_[at (s)] != modRest)
            emit (event (offset, s, 0xB0, laneField (s, ModBCc), modRest));
        lastBend_[at (s)] = 0;
        lastModA_[at (s)] = modRest;
        lastModB_[at (s)] = modRest;
    }

    template <typename Emit>
    void releaseAll (int offset, Emit& emit)
    {
        for (int s = 0; s < lanes; ++s)
        {
            sendOff (s, offset, emit);
            resetControllers (s, offset, emit);
        }
    }

    template <typename Emit>
    void releaseInaudible (Emit& emit)
    {
        const bool soloOn = anySolo();
        for (int s = 0; s < lanes; ++s)
        {
            if (! audible (s, soloOn))
            {
                sendOff (s, 0, emit);
                resetControllers (s, 0, emit);
            }
        }
    }

    static int volumeCc (int v) { return static_cast<int> (std::floor (v * 127 / 10000.0 + 0.5)); }

    static int panCc (int v)
    {
        return static_cast<int> (v < 0 ? std::floor (64 + v * 64 / 10000.0 + 0.5) : std::floor (64 + v * 63 / 10000.0 + 0.5));
    }

    // Releases the lane's held note first, then writes its controllers, then the note-on. A
    // step position of -1 is an audition, which uses the lane's settings and default values.
    template <typename Emit>
    void sendHit (int s, int offset, int note, int velocity, int pos, Emit& emit)
    {
        sendOff (s, offset, emit);
        emit (event (offset, s, 0xB0, 7, volumeCc (laneField (s, Volume))));
        int pan = panCc (laneField (s, LanePan));
        if (pos >= 0)
            pan += store_.get (pattern_, s, pos, Pan);
        emit (event (offset, s, 0xB0, 10, std::max (0, std::min (127, pan))));
        if (pos >= 0)
        {
            // Fine pitch is pitch bend over the lane's bend range, sent only when it changes.
            const int cents = store_.get (pattern_, s, pos, Fine);
            const int range = laneField (s, BendRange);
            if (cents != lastBend_[at (s)] && range > 0)
            {
                const double raw = std::floor (8192 + cents * 8192.0 / (range * 100.0) + 0.5);
                const int bend = static_cast<int> (std::max (0.0, std::min (16383.0, raw)));
                emit (event (offset, s, 0xE0, bend % 128, bend / 128));
                lastBend_[at (s)] = cents;
            }
            const int modA = store_.get (pattern_, s, pos, ModA);
            if (modA != lastModA_[at (s)])
            {
                emit (event (offset, s, 0xB0, laneField (s, ModACc), modA));
                lastModA_[at (s)] = modA;
            }
            const int modB = store_.get (pattern_, s, pos, ModB);
            if (modB != lastModB_[at (s)])
            {
                emit (event (offset, s, 0xB0, laneField (s, ModBCc), modB));
                lastModB_[at (s)] = modB;
            }
            relV_[at (s)] = store_.get (pattern_, s, pos, Release);
        }
        emit (event (offset, s, 0x90, note, velocity));
        held_[at (s)] = note;
    }

    // Sends due note-offs in beat order. Inclusive puts a release at the limit before a hit
    // there. A release leaves the lane's waiting hit alone, so a ratchet released by its
    // interval still plays the next one.
    template <typename Emit>
    void flushReleases (double limit, bool inclusive, Emit& emit)
    {
        for (;;)
        {
            int best = -1;
            double bestBeat = 0;
            for (int s = 0; s < lanes; ++s)
            {
                const double r = rel_[at (s)];
                const bool due = inclusive ? r <= limit : r < limit;
                if (r >= 0 && due && (best < 0 || r < bestBeat))
                {
                    best = s;
                    bestBeat = r;
                }
            }
            if (best < 0)
                return;
            const double waiting = phT_[at (best)];
            sendOff (best, offsetOf (bestBeat), emit);
            phT_[at (best)] = waiting;
        }
    }

    // Sends swung, shifted and ratcheted hits that fall before limit, earliest first and lowest
    // lane first on a tie.
    template <typename Emit>
    void flushPending (double limit, Emit& emit)
    {
        for (;;)
        {
            int best = -1;
            double bestBeat = 0;
            for (int s = 0; s < lanes; ++s)
            {
                const double t = phT_[at (s)];
                if (t >= 0 && t < limit && (best < 0 || t < bestBeat))
                {
                    best = s;
                    bestBeat = t;
                }
            }
            if (best < 0)
                return;
            const auto b = at (best);
            flushReleases (bestBeat, true, emit);
            sendHit (best, offsetOf (bestBeat), phNote_[b], store_.get (pattern_, best, phPos_[b], Velocity), phPos_[b], emit);
            rel_[b] = bestBeat + phD_[b];
            // The hit's own send cleared the lane's pending state, so the next ratchet is set again.
            if (phN_[b] > 1)
            {
                phT_[b] = bestBeat + phD_[b];
                --phN_[b];
            }
        }
    }

    // Transactions ---------------------------------------------------------------------------

    // Checks every command against the state it would see, without changing anything. Lane
    // activity is simulated, so a lane added earlier in the transaction can be set up later.
    Result preflight (std::span<const Command> commands)
    {
        std::array<bool, lanes> sim {};
        for (int s = 0; s < lanes; ++s)
            sim[at (s)] = laneField (s, Active) != 0;
        int need = 0;
        int adds = 0;
        store_.beginCount();
        for (const auto& cmd : commands)
        {
            const int a = cmd.a, b = cmd.b, c = cmd.c, d = cmd.d, v = cmd.v;
            switch (cmd.op)
            {
                case Op::SetStep:
                    if (! (inRange (a, 0, patterns - 1) && inRange (b, 0, lanes - 1) && inRange (c, 0, maxSteps - 1)
                           && inRange (d, 0, stepFields - 1))
                        || ! sim[at (b)] || ! inRange (v, fieldMin[at (d)], fieldMax[at (d)]))
                        return Result::Invalid;
                    if (v != fieldDefault[at (d)])
                    {
                        ++adds;
                        if (store_.countPage (a, b, c))
                            ++need;
                    }
                    break;
                case Op::AddLane:
                    if (! inRange (a, 0, lanes - 1) || sim[at (a)] || ! inRange (b, 1, idMax) || ! inRange (c, 0, idMax)
                        || ! inRange (d, 0, 127) || ! inRange (v, 0, lanes - 1))
                        return Result::Invalid;
                    sim[at (a)] = true;
                    break;
                case Op::RemoveLane:
                    if (! inRange (a, 0, lanes - 1) || ! sim[at (a)])
                        return Result::Invalid;
                    sim[at (a)] = false;
                    break;
                case Op::SetLane:
                    if (! inRange (a, 0, lanes - 1) || ! sim[at (a)] || ! inRange (b, ProbabilityKey, ModBCc)
                        || ! inRange (c, laneMin[at (b)], laneMax[at (b)]))
                        return Result::Invalid;
                    break;
                case Op::SetRack:
                    if (! rackValueValid (a, b))
                        return Result::Invalid;
                    break;
                case Op::SetLength:
                    if (! inRange (a, 0, patterns - 1) || ! inRange (b, 0, lanes - 1) || ! sim[at (b)] || ! inRange (c, 1, maxSteps))
                        return Result::Invalid;
                    break;
                case Op::QueuePattern:
                    // SeqCore's wire carries the beat in 1/1024 beat ticks below 2^48.
                    if (! inRange (a, 0, patterns - 1) || ! std::isfinite (cmd.beat) || cmd.beat < 0
                        || cmd.beat >= 16777216.0 * 16777216.0 / 1024)
                        return Result::Invalid;
                    break;
                case Op::ClearPattern:
                    if (! inRange (a, 0, patterns - 1))
                        return Result::Invalid;
                    break;
                case Op::ClearPatternLane:
                    if (! inRange (a, 0, patterns - 1) || ! inRange (b, 0, lanes - 1) || ! sim[at (b)])
                        return Result::Invalid;
                    break;
                default:
                    return Result::Invalid;
            }
        }
        if (need > store_.freePages() || store_.nonDefaultCount() + adds > nonDefaultMax)
            return Result::Capacity;
        return Result::Ok;
    }

    static bool rackValueValid (int field, int value)
    {
        switch (field)
        {
            case RackId: return inRange (value, 1, idMax);
            case Seed: return inRange (value, 0, idMax);
            case CurrentPattern: return inRange (value, 0, patterns - 1);
            case Mode: return inRange (value, 0, 1);
            case GlobalSwing: return inRange (value, 0, 10000);
            case Truncate: return inRange (value, 0, 1);
            default: return false;
        }
    }

    template <typename Emit>
    void applyChecked (std::span<const Command> commands, Emit& emit)
    {
        for (const auto& cmd : commands)
        {
            const int a = cmd.a, b = cmd.b, c = cmd.c;
            switch (cmd.op)
            {
                case Op::SetStep:
                    store_.set (a, b, c, cmd.d, cmd.v);
                    break;
                case Op::AddLane:
                {
                    auto& l = lane_[at (a)];
                    l = laneDefault;
                    l[Active] = 1;
                    l[Id] = b;
                    l[ProbabilityKey] = c;
                    l[Note] = cmd.d;
                    l[Row] = cmd.v;
                    ++laneCount_;
                    break;
                }
                case Op::RemoveLane:
                    sendOff (a, 0, emit);
                    resetControllers (a, 0, emit);
                    store_.clearLane (a);
                    lane_[at (a)].fill (0);
                    --laneCount_;
                    break;
                case Op::SetLane:
                    if (b == Note && laneField (a, Note) != c)
                        sendOff (a, 0, emit);
                    // Changing what a moved controller means returns it to rest under the old
                    // setting first.
                    if ((b == BendRange || b == ModACc || b == ModBCc) && laneField (a, b) != c)
                        resetControllers (a, 0, emit);
                    lane_[at (a)][at (b)] = c;
                    break;
                case Op::SetRack:
                    setRack (a, b, emit);
                    break;
                case Op::SetLength:
                    store_.setLength (a, b, c);
                    break;
                case Op::QueuePattern:
                    if (a == pattern_)
                    {
                        pendingPattern_ = -1;
                    }
                    else
                    {
                        pendingPattern_ = a;
                        pendingBeat_ = cmd.beat;
                    }
                    break;
                case Op::ClearPattern:
                    store_.clearPattern (a);
                    break;
                case Op::ClearPatternLane:
                    store_.clearPatternLane (a, b);
                    break;
            }
        }
        releaseInaudible (emit);
    }

    template <typename Emit>
    void setRack (int field, int value, Emit& emit)
    {
        switch (field)
        {
            case RackId: rackId_ = value; break;
            case Seed: seed_ = value; break;
            case CurrentPattern:
                if (pattern_ != value)
                    releaseAll (0, emit);
                pattern_ = value;
                break;
            case Mode:
                if (mode_ != value)
                    releaseAll (0, emit);
                if (value == 0 && mode_ != 0)
                    liveHold_ = liveHoldBlocks_;
                mode_ = value;
                break;
            case GlobalSwing: swing_ = value; break;
            case Truncate: truncate_ = value; break;
            default: break;
        }
    }

    // State ----------------------------------------------------------------------------------

    // A double as four 16-bit words, least significant first: SeqCore's encode gives the IEEE
    // 754 bits, except that it writes zero for negative zero.
    static std::array<std::int32_t, 4> encodeDouble (double x)
    {
        const auto bits = std::fpclassify (x) == FP_ZERO ? std::uint64_t { 0 } : std::bit_cast<std::uint64_t> (x);
        return { static_cast<std::int32_t> (bits & 0xFFFF), static_cast<std::int32_t> ((bits >> 16) & 0xFFFF),
                 static_cast<std::int32_t> ((bits >> 32) & 0xFFFF), static_cast<std::int32_t> (bits >> 48) };
    }

    // Pass one (apply false) checks the words without touching state. Pass two replaces state.
    // Stricter than SeqCore in one place: the origin words must be 16-bit and give a finite beat.
    LoadError parseState (std::span<const std::int32_t> w, bool apply)
    {
        const std::size_t count = w.size();
        auto word = [&] (std::size_t i) { return w[i]; };

        if (count < 13 || word (0) != stateMagic || word (1) != stateFormat)
            return LoadError::Format;
        if (! inRange (word (2), 0, idMax) || ! inRange (word (3), 0, idMax) || ! inRange (word (4), 0, patterns - 1)
            || ! inRange (word (5), 0, 1) || ! inRange (word (6), 0, 10000) || ! inRange (word (7), 0, 1))
            return LoadError::Range;
        std::uint64_t bits = 0;
        for (int i = 3; i >= 0; --i)
        {
            if (! inRange (word (8 + static_cast<std::size_t> (i)), 0, 65535))
                return LoadError::Range;
            bits = (bits << 16) | static_cast<std::uint64_t> (word (8 + static_cast<std::size_t> (i)));
        }
        const double origin = std::bit_cast<double> (bits);
        if (! std::isfinite (origin))
            return LoadError::Range;

        if (apply)
        {
            resetState();
            rackId_ = word (2);
            seed_ = word (3);
            pattern_ = word (4);
            mode_ = word (5);
            swing_ = word (6);
            truncate_ = word (7);
            origin_ = origin;
        }

        std::size_t pos = 12;
        std::array<bool, lanes> sim {};
        int n = word (pos++);
        if (! inRange (n, 0, lanes))
            return LoadError::Range;
        for (; n > 0; --n, pos += 16)
        {
            if (pos + 16 > count)
                return LoadError::Truncated;
            const int s = word (pos);
            if (! inRange (s, 0, lanes - 1) || (! apply && sim[at (s)]))
                return LoadError::Range;
            for (int f = Id; f < laneFields; ++f)
                if (! inRange (word (pos + at (f)), laneMin[at (f)], laneMax[at (f)]))
                    return LoadError::Range;
            if (! inRange (word (pos + 15), -1, 127))
                return LoadError::Range;
            if (! apply)
            {
                sim[at (s)] = true;
                continue;
            }
            for (int f = Id; f < laneFields; ++f)
                lane_[at (s)][at (f)] = word (pos + at (f));
            lane_[at (s)][Active] = 1;
            savedHeld_[at (s)] = word (pos + 15);
            ++laneCount_;
        }

        if (pos >= count)
            return LoadError::Truncated;
        n = word (pos++);
        if (! inRange (n, 0, patterns * lanes))
            return LoadError::Range;
        for (; n > 0; --n, pos += 2)
        {
            if (pos + 2 > count)
                return LoadError::Truncated;
            const int key = word (pos);
            if (! inRange (key, 0, patterns * lanes - 1) || ! inRange (word (pos + 1), 1, maxSteps))
                return LoadError::Range;
            if (apply)
                store_.setLength (key / lanes, key % lanes, word (pos + 1));
        }

        if (pos >= count)
            return LoadError::Truncated;
        n = word (pos++);
        if (! inRange (n, 0, nonDefaultMax))
            return LoadError::Capacity;
        store_.beginCount();
        int need = 0;
        for (; n > 0; --n, pos += 3)
        {
            if (pos + 3 > count)
                return LoadError::Truncated;
            const int key = word (pos);
            const int cell = word (pos + 1);
            const int v = word (pos + 2);
            if (! inRange (key, 0, patterns * lanes - 1) || ! inRange (cell, 0, maxSteps * stepFields - 1))
                return LoadError::Range;
            const int p = key / lanes, s = key % lanes, step = cell / stepFields, f = cell % stepFields;
            if (! inRange (v, fieldMin[at (f)], fieldMax[at (f)]) || (! apply && ! sim[at (s)]))
                return LoadError::Range;
            if (apply)
                store_.set (p, s, step, f, v);
            else if (store_.countPage (p, s, step, true) && ++need > poolPages)
                return LoadError::Capacity;
        }
        if (pos != count)
            return LoadError::Truncated;
        return LoadError::None;
    }

    // Musical state, saved.
    PatternStore store_;
    std::array<std::array<int, laneFields>, lanes> lane_ {};
    int laneCount_ = 0;
    int rackId_ = 0, seed_ = 0, pattern_ = 0, mode_ = 0, swing_ = 0, truncate_ = 0;
    double origin_ = 0.0;

    // Playback state.
    int liveHoldBlocks_ = 0;
    int liveHold_ = 0;
    int pendingPattern_ = -1;
    double pendingBeat_ = 0.0;
    std::int64_t nextK_ = 0;
    bool playing_ = false;
    bool wasPlaying_ = false;
    bool startupRelease_ = true;
    bool stateReplaced_ = false;
    LoadError loadError_ = LoadError::None;
    std::vector<std::int32_t> rawWords_;

    // This block's transport, and the last block's for jump and ramp detection.
    double srate_ = 44100.0;
    double tempo_ = 120.0;
    double beatPosition_ = 0.0;
    int samples_ = 0;
    double slope_ = 0.0;
    double blockBeats_ = 0.0;
    double blockEndBeat_ = 0.0;
    double prevTempo_ = -1.0;
    double prevBeat_ = 0.0;
    int prevBlock_ = 0;

    // Per lane: the held note, its release beat and velocity, the audition countdown in
    // samples, the waiting hit (beat, note, step, ratchet interval and count), the playhead and
    // the controllers a step moved. -1 means none.
    std::array<int, lanes> held_ {};
    std::array<double, lanes> rel_ {};
    std::array<int, lanes> relV_ {};
    std::array<int, lanes> aud_ {};
    std::array<int, lanes> savedHeld_ {};
    std::array<int, lanes> posn_ {};
    std::array<double, lanes> phT_ {};
    std::array<int, lanes> phNote_ {};
    std::array<int, lanes> phPos_ {};
    std::array<double, lanes> phD_ {};
    std::array<int, lanes> phN_ {};
    std::array<int, lanes> lastBend_ {};
    std::array<int, lanes> lastModA_ {};
    std::array<int, lanes> lastModB_ {};
};

} // namespace furrow
