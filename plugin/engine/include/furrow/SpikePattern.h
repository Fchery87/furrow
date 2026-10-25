#pragma once

#include "furrow/BlockClock.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace furrow
{

struct MidiEvent
{
    int offset = 0;             // sample offset inside the block
    std::uint8_t status = 0;    // 0x90 note-on or 0x80 note-off, with the channel in the low bits
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

// Milestone 1 spike: plays the fixed pattern in probes/pattern-4x16.txt from project beat 0, so
// its output can be compared with the REAPER edition's expected events. Lane n plays on MIDI
// channel n + 1. It follows SeqCore's rules for the parts it covers: one held note per lane,
// releases before hits at the same position, and every held note released on stop or seek.
//
// Real-time safe: no allocation, no locks. Events go to the caller's emit callback in time order.
class SpikePattern
{
public:
    static constexpr int lanes = 4;
    static constexpr int steps = 16;
    static constexpr double stepBeats = 0.25;
    static constexpr double gateSteps = 0.5;
    static constexpr std::uint8_t velocity = 100;
    static constexpr std::array<std::uint8_t, lanes> notes { 36, 38, 42, 46 };
    static constexpr std::array<std::array<bool, steps>, lanes> grid { {
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 },
        { 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0 },
        { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 },
        { 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1 },
    } };

    template <typename Emit>
    void process (const BlockInfo& block, Emit&& emit)
    {
        if (! block.playing)
        {
            releaseAll (emit);
            clock_.stop();
            return;
        }

        if (! clock_.begin (block))
            return;

        if (clock_.jumped())
        {
            releaseAll (emit);
            nextStep_ = firstStepAtOrAfter (block.beat);
        }

        for (;;)
        {
            const double hit = static_cast<double> (nextStep_) * stepBeats;
            if (hit >= clock_.endBeat())
                break;

            flushReleases (hit, true, emit);

            // Steps before the origin at beat 0 play nothing.
            if (nextStep_ >= 0)
            {
                const auto position = static_cast<std::size_t> (nextStep_ % steps);
                const int offset = clock_.offsetOf (hit);
                for (std::size_t lane = 0; lane < lanes; ++lane)
                {
                    if (! grid[lane][position])
                        continue;
                    if (held_[lane])
                        noteOff (lane, offset, emit);
                    emit (MidiEvent { offset, static_cast<std::uint8_t> (0x90 | lane), notes[lane], velocity });
                    held_[lane] = true;
                    releaseBeat_[lane] = hit + gateSteps * stepBeats;
                }
            }
            ++nextStep_;
        }

        flushReleases (clock_.endBeat(), false, emit);
    }

    // Called when the host resets the plugin. Nothing can be sent from here, so held notes stay
    // marked and the next block releases them, whether it is playing or stopped.
    void reset() noexcept { clock_.stop(); }

private:
    static std::int64_t firstStepAtOrAfter (double beat) noexcept
    {
        // The small tolerance keeps a step that falls exactly on the block start.
        return static_cast<std::int64_t> (std::ceil (beat / stepBeats - 0.000001));
    }

    template <typename Emit>
    void noteOff (std::size_t lane, int offset, Emit& emit)
    {
        emit (MidiEvent { offset, static_cast<std::uint8_t> (0x80 | lane), notes[lane], 0 });
        held_[lane] = false;
    }

    // Sends the releases due before `limit`, or at it when `inclusive` is set, earliest first, as
    // SeqCore's flush_releases does.
    template <typename Emit>
    void flushReleases (double limit, bool inclusive, Emit& emit)
    {
        for (;;)
        {
            std::size_t best = lanes;
            for (std::size_t lane = 0; lane < lanes; ++lane)
            {
                const double due = releaseBeat_[lane];
                const bool isDue = inclusive ? due <= limit : due < limit;
                if (held_[lane] && isDue && (best == lanes || due < releaseBeat_[best]))
                    best = lane;
            }
            if (best == lanes)
                return;
            noteOff (best, clock_.offsetOf (releaseBeat_[best]), emit);
        }
    }

    template <typename Emit>
    void releaseAll (Emit& emit)
    {
        for (std::size_t lane = 0; lane < lanes; ++lane)
            if (held_[lane])
                noteOff (lane, 0, emit);
    }

    BlockClock clock_;
    std::int64_t nextStep_ = 0;
    std::array<bool, lanes> held_ {};
    std::array<double, lanes> releaseBeat_ {};
};

} // namespace furrow
