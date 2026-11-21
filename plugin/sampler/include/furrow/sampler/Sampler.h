#pragma once

#include "furrow/Engine.h"
#include "furrow/sampler/Curves.h"
#include "furrow/sampler/LaneSettings.h"
#include "furrow/sampler/SampleBuffer.h"
#include "furrow/sampler/SpscQueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace furrow::sampler
{

// One output bus for render(). A mono bus has no right channel. A bus that is off has neither.
struct Bus
{
    float* left = nullptr;
    float* right = nullptr;
};

// Plays each sample lane from the engine's messages for that lane, as RS5K plays a REAPER edition
// lane: note-on starts a voice, note-off releases it, CC 7 and CC 10 set the lane's volume and
// pan, and pitch bend moves the lane's pitch. The caller sends only sample lanes' messages.
//
// Two sides use it. Any number of non-audio threads change lanes and samples through setLane(),
// setSample() and collect(), which a mutex serialises among themselves. The audio thread calls
// the rest, which never waits, locks or allocates. Changes reach the audio thread through a
// lock-free queue at the next update(). A replaced sample goes back the same way once no voice
// plays it, and collect() frees it.
class Sampler
{
public:
    static constexpr int voices = 256;
    static constexpr int laneVoices = 8;        // RS5K's default Max voices
    static constexpr std::size_t queueSize = 1024;
    static constexpr double chokeMs = 5.0;      // fade for a choked, stolen or replaced voice

    Sampler() = default;
    Sampler (const Sampler&) = delete;
    Sampler& operator= (const Sampler&) = delete;

    ~Sampler()
    {
        Change change;
        while (changes_.pop (change))
            delete change.sample;
        SampleBuffer* sample = nullptr;
        while (returned_.pop (sample))
            delete sample;
        for (int i = 0; i < retiring_; ++i)
            delete retired_[static_cast<std::size_t> (i)];
        for (auto& lane : lanes_)
            delete lane.sample;
        delete previewSample_;
    }

    // ---- Non-audio threads ----

    // Queues new settings for a lane. Fails when the queue is full or the settings are invalid.
    bool setLane (int lane, const LaneSettings& settings)
    {
        if (lane < 0 || lane >= furrow::lanes || ! valid (settings))
            return false;
        const std::lock_guard<std::mutex> lock (producer_);
        collectLocked();
        return changes_.push ({ lane, settings, false, nullptr });
    }

    // Queues a new sample for a lane, or none to clear it, and takes it from the caller. Fails,
    // leaving the sample with the caller, when the queue is full.
    bool setSample (int lane, std::unique_ptr<SampleBuffer>& sample)
    {
        if (lane < 0 || lane >= furrow::lanes)
            return false;
        const std::lock_guard<std::mutex> lock (producer_);
        collectLocked();
        // Each queued sample comes back once, so this bound keeps the return path from filling.
        if (outstanding_ >= static_cast<int> (queueSize))
            return false;
        if (! changes_.push ({ lane, {}, true, sample.get() }))
            return false;
        ++outstanding_;
        sample.release();
        return true;
    }

    // Queues a sample for immediate main-bus preview (pass empty unique_ptr to stop preview).
    bool setPreviewSample (std::unique_ptr<SampleBuffer>& sample)
    {
        const std::lock_guard<std::mutex> lock (producer_);
        collectLocked();
        if (outstanding_ >= static_cast<int> (queueSize))
            return false;
        if (! changes_.push ({ previewLane, {}, true, sample.get() }))
            return false;
        ++outstanding_;
        sample.release();
        return true;
    }

    // Frees samples the audio thread has finished with. Returns how many it freed.
    int collect()
    {
        const std::lock_guard<std::mutex> lock (producer_);
        return collectLocked();
    }

    // ---- Audio thread ----

    void prepare (double sampleRate)
    {
        rate_ = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 44100.0;
        chokeSamples_ = std::max (1, samples (chokeMs));
        reset();
    }

    // Silences every voice at once and returns the lanes' controllers to their defaults.
    void reset()
    {
        for (auto& v : voices_)
            v.stage = Stage::Off;
        previewVoice_.stage = Stage::Off;
        lanePeaks_.fill (0.0f);
        for (auto& lane : lanes_)
            lane.volume = lane.left = lane.right = lane.bendRatio = 1.0;
    }

    // Smoothly chokes all active lane voices and any preview voice (e.g. on transport stop).
    void stopAll()
    {
        for (auto& v : voices_)
            if (v.stage != Stage::Off)
                choke (v);
        if (previewVoice_.stage != Stage::Off)
            choke (previewVoice_);
    }

    // Applies queued changes and hands back samples no voice plays any more. Call at the start
    // of each block.
    void update()
    {
        Change change;
        while (changes_.pop (change))
        {
            if (change.lane == previewLane)
            {
                SampleBuffer* old = previewSample_;
                previewSample_ = change.sample;
                previewVoice_.stage = Stage::Off;
                if (old != nullptr)
                    retired_[static_cast<std::size_t> (retiring_++)] = old;
                else
                    returned_.push (nullptr);
                if (previewSample_ != nullptr)
                {
                    previewVoice_ = {};
                    previewVoice_.stage = Stage::Hold;
                    previewVoice_.sample = previewSample_;
                    previewVoice_.position = 0.0;
                    previewVoice_.startLimit = 0.0;
                    previewVoice_.end = static_cast<double> (previewSample_->frames());
                    previewVoice_.step = previewSample_->sampleRate() / rate_;
                    previewVoice_.velocity = 1.0;
                    previewVoice_.level = 1.0;
                }
                continue;
            }
            auto& lane = lanes_[static_cast<std::size_t> (change.lane)];
            if (! change.replace)
            {
                lane.settings = change.settings;
                continue;
            }
            SampleBuffer* old = lane.sample;
            lane.sample = change.sample;
            if (old == nullptr)
            {
                returned_.push (nullptr);
                continue;
            }
            for (auto& v : voices_)
                if (v.stage != Stage::Off && v.sample == old)
                    choke (v);
            retired_[static_cast<std::size_t> (retiring_++)] = old;
        }
        for (int i = 0; i < retiring_;)
        {
            SampleBuffer* sample = retired_[static_cast<std::size_t> (i)];
            const bool playing = (previewVoice_.stage != Stage::Off && previewVoice_.sample == sample)
                || std::any_of (voices_.begin(), voices_.end(), [sample] (const Voice& v) { return v.stage != Stage::Off && v.sample == sample; });
            if (playing)
            {
                ++i;
                continue;
            }
            returned_.push (sample);
            retired_[static_cast<std::size_t> (i)] = retired_[static_cast<std::size_t> (--retiring_)];
        }
    }

    // Acts on one message for a sample lane. rootNote is the note that plays the sample at its
    // own pitch, and bendRange is the lane's bend range in semitones.
    void handle (const MidiEvent& e, int rootNote, int bendRange)
    {
        if (e.lane < 0 || e.lane >= furrow::lanes)
            return;
        auto& lane = lanes_[static_cast<std::size_t> (e.lane)];
        switch (e.status)
        {
            case 0x90:
                if (e.data2 > 0)
                    noteOn (e.lane, e.data1, e.data2, rootNote);
                else
                    noteOff (e.lane, e.data1);
                break;
            case 0x80:
                noteOff (e.lane, e.data1);
                break;
            case 0xB0:
                if (e.data1 == 7)
                    lane.volume = volumeGain (e.data2);
                else if (e.data1 == 10)
                {
                    const auto gains = panGains (e.data2);
                    lane.left = gains.left;
                    lane.right = gains.right;
                }
                else if (e.data1 == 120)
                    forLane (e.lane, [] (Voice& v) { v.stage = Stage::Off; });
                else if (e.data1 == 121)
                    lane.volume = lane.left = lane.right = lane.bendRatio = 1.0;
                else if (e.data1 == 123)
                    forLane (e.lane, [] (Voice& v) { release (v); });
                break;
            case 0xE0:
                lane.bendRatio = std::exp2 (bendSemitones (e.data1 | (e.data2 << 7), bendRange) / 12.0);
                break;
            default:
                break;
        }
    }

    // Adds frames from..to-1 of every playing voice into the buses. buses[0] is the main output;
    // a lane whose bus is off plays there instead.
    void render (std::span<const Bus> buses, int from, int to)
    {
        if (to <= from)
            return;
        for (auto& v : voices_)
        {
            if (v.stage == Stage::Off)
                continue;
            const auto& lane = lanes_[static_cast<std::size_t> (v.lane)];
            Bus bus;
            const auto wanted = static_cast<std::size_t> (lane.settings.output - 1);
            if (wanted < buses.size() && buses[wanted].left != nullptr)
                bus = buses[wanted];
            else if (! buses.empty())
                bus = buses[0];
            renderVoice (v, lane, bus, from, to, lanePeaks_[static_cast<std::size_t> (v.lane)]);
        }
        if (previewVoice_.stage != Stage::Off && ! buses.empty() && buses[0].left != nullptr)
        {
            const Lane neutral;
            float ignoredPeak = 0.0f;
            renderVoice (previewVoice_, neutral, buses[0], from, to, ignoredPeak);
        }
    }

    const std::array<float, furrow::lanes>& lanePeaks() const { return lanePeaks_; }
    void clearPeaks() { lanePeaks_.fill (0.0f); }

    const LaneSettings& lane (int index) const { return lanes_[static_cast<std::size_t> (index)].settings; }

    int activeVoices() const
    {
        return static_cast<int> (std::count_if (voices_.begin(), voices_.end(), [] (const Voice& v) { return v.stage != Stage::Off; }));
    }

private:
    struct Change
    {
        int lane = 0;
        LaneSettings settings {};
        bool replace = false;
        SampleBuffer* sample = nullptr;
    };

    struct Lane
    {
        LaneSettings settings;
        SampleBuffer* sample = nullptr;
        // The lane's controllers, as a MIDI channel holds them.
        double volume = 1.0;
        double left = 1.0;
        double right = 1.0;
        double bendRatio = 1.0;
    };

    enum class Stage : std::uint8_t
    {
        Off, Attack, Hold, Decay, Release, Choke
    };

    struct Voice
    {
        Stage stage = Stage::Off;
        int lane = 0;
        int note = 0;
        int choke = 0;
        const SampleBuffer* sample = nullptr;
        double position = 0.0;  // in sample frames
        double startLimit = 0.0;
        double end = 0.0;
        double step = 1.0;      // frames per output frame before pitch bend
        bool reverse = false;
        double ic1L = 0.0, ic2L = 0.0;
        double ic1R = 0.0, ic2R = 0.0;
        double velocity = 1.0;
        double level = 0.0;
        double fall = 0.0;      // per-frame multiplier in Decay and Release, step in Choke
        int counter = 0;        // frames done in Attack, frames left in Decay, Release and Choke
        int attackFrames = 0;
        int decayFrames = 0;
        int releaseFrames = 0;
        std::uint64_t order = 0;
    };

    int samples (double ms) const { return static_cast<int> (std::lround (ms * rate_ / 1000.0)); }

    int collectLocked()
    {
        int freed = 0;
        SampleBuffer* sample = nullptr;
        while (returned_.pop (sample))
        {
            --outstanding_;
            if (sample != nullptr)
            {
                delete sample;
                ++freed;
            }
        }
        return freed;
    }

    template <typename Fn>
    void forLane (int lane, Fn fn)
    {
        for (auto& v : voices_)
            if (v.stage != Stage::Off && v.lane == lane)
                fn (v);
    }

    static double fallPerFrame (int frames) { return std::pow (0.001, 1.0 / frames); }

    void noteOn (int laneIndex, int note, int velocity, int rootNote)
    {
        const auto& lane = lanes_[static_cast<std::size_t> (laneIndex)];
        if (lane.sample == nullptr)
            return;
        const auto& s = lane.settings;

        if (s.choke > 0)
            for (auto& v : voices_)
                if (v.stage != Stage::Off && v.stage != Stage::Choke && v.lane != laneIndex && v.choke == s.choke)
                    choke (v);

        // Past the lane's voice limit, the oldest voice gives way.
        Voice* oldest = nullptr;
        int count = 0;
        for (auto& v : voices_)
            if (v.stage != Stage::Off && v.stage != Stage::Choke && v.lane == laneIndex)
            {
                ++count;
                if (oldest == nullptr || v.order < oldest->order)
                    oldest = &v;
            }
        if (count >= laneVoices)
            choke (*oldest);

        // A free voice, or else the quietest one.
        Voice* voice = nullptr;
        for (auto& v : voices_)
            if (v.stage == Stage::Off)
            {
                voice = &v;
                break;
            }
        if (voice == nullptr)
            voice = &*std::min_element (voices_.begin(), voices_.end(), [] (const Voice& a, const Voice& b) { return a.level < b.level; });

        const auto frames = static_cast<double> (lane.sample->frames());
        Voice& v = *voice;
        v.lane = laneIndex;
        v.note = note;
        v.choke = s.choke;
        v.sample = lane.sample;
        v.reverse = s.reverse;
        v.startLimit = s.start * frames;
        v.end = s.end * frames;
        v.position = s.reverse ? std::max (v.startLimit, v.end - 1.0) : v.startLimit;
        v.ic1L = v.ic2L = v.ic1R = v.ic2R = 0.0;
        v.step = lane.sample->sampleRate() / rate_ * std::exp2 ((note - rootNote + s.tune / 100.0) / 12.0);
        v.velocity = velocityGain (velocity);
        v.attackFrames = samples (s.attack);
        v.decayFrames = samples (s.decay);
        v.releaseFrames = samples (s.release);
        v.order = ++order_;
        v.counter = 0;
        if (v.attackFrames > 0)
        {
            v.stage = Stage::Attack;
            v.level = 0.0;
        }
        else
            sustain (v);
    }

    void noteOff (int lane, int note)
    {
        if (lanes_[static_cast<std::size_t> (lane)].settings.playMode == PlayMode::OneShot)
            return;
        forLane (lane, [note] (Voice& v) {
            if (v.note == note)
                release (v);
        });
    }

    // After the attack: Decay when the lane has a decay time, Hold otherwise.
    static void sustain (Voice& v)
    {
        v.level = 1.0;
        if (v.decayFrames > 0)
        {
            v.stage = Stage::Decay;
            v.counter = v.decayFrames;
            v.fall = fallPerFrame (v.decayFrames);
        }
        else
            v.stage = Stage::Hold;
    }

    static void release (Voice& v)
    {
        if (v.stage == Stage::Release || v.stage == Stage::Choke || v.stage == Stage::Off)
            return;
        if (v.releaseFrames <= 0)
        {
            v.stage = Stage::Off;
            return;
        }
        v.stage = Stage::Release;
        v.counter = v.releaseFrames;
        v.fall = fallPerFrame (v.releaseFrames);
    }

    void choke (Voice& v)
    {
        if (v.stage == Stage::Off || v.stage == Stage::Choke)
            return;
        v.stage = Stage::Choke;
        v.counter = chokeSamples_;
        v.fall = v.level / chokeSamples_;
    }

    // The envelope level for the next frame. Sets the stage to Off after a voice's last frame.
    static double envelope (Voice& v)
    {
        switch (v.stage)
        {
            case Stage::Attack:
                if (++v.counter >= v.attackFrames)
                {
                    sustain (v);
                    return 1.0;
                }
                v.level = static_cast<double> (v.counter) / v.attackFrames;
                return v.level;
            case Stage::Hold:
                return v.level;
            case Stage::Decay:
            {
                const double level = v.level;
                v.level *= v.fall;
                if (--v.counter <= 0)
                    v.stage = Stage::Off;
                return level;
            }
            case Stage::Release:
                v.level *= v.fall;
                if (--v.counter <= 0)
                    v.stage = Stage::Off;
                return v.level;
            case Stage::Choke:
                v.level = std::max (0.0, v.level - v.fall);
                if (--v.counter <= 0)
                    v.stage = Stage::Off;
                return v.level;
            case Stage::Off:
                break;
        }
        return 0.0;
    }

    // Four-point cubic Hermite interpolation at a fractional frame.
    static float read (const float* d, std::int64_t i, double t)
    {
        const double xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
        const double c1 = 0.5 * (x1 - xm1);
        const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        return static_cast<float> (((c3 * t + c2) * t + c1) * t + x0);
    }

    void renderVoice (Voice& v, const Lane& lane, Bus bus, int from, int to, float& peakOut) const
    {
        const double step = v.step * lane.bendRatio;
        const double gainLeft = v.velocity * lane.volume * lane.left;
        const double gainRight = v.velocity * lane.volume * lane.right;
        const float* first = v.sample->channel (0);
        const float* second = v.sample->channel (v.sample->channels() > 1 ? 1 : 0);
        const auto fMode = lane.settings.filterMode;
        double a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 2.0;
        if (fMode != FilterMode::Off)
        {
            const double fc = std::clamp (lane.settings.filterCutoffHz, filterMinHz, std::min (filterMaxHz, 0.45 * rate_));
            const double g = std::tan (3.14159265358979323846 * fc / rate_);
            k = 2.0 - 1.9 * std::clamp (lane.settings.filterResonance, 0.0, filterResMax);
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        float peak = peakOut;
        for (int f = from; f < to; ++f)
        {
            if (v.reverse ? (v.position < v.startLimit) : (v.position >= v.end))
            {
                v.stage = Stage::Off;
                peakOut = peak;
                return;
            }
            const double level = envelope (v);
            const auto i = static_cast<std::int64_t> (std::floor (v.position));
            const double t = v.position - static_cast<double> (i);
            double left = read (first, i, t) * gainLeft * level;
            double right = read (second, i, t) * gainRight * level;
            if (fMode != FilterMode::Off)
            {
                const double v3L = left - v.ic2L;
                const double v1L = a1 * v.ic1L + a2 * v3L;
                const double v2L = v.ic2L + a2 * v.ic1L + a3 * v3L;
                v.ic1L = 2.0 * v1L - v.ic1L;
                v.ic2L = 2.0 * v2L - v.ic2L;
                left = (fMode == FilterMode::LowPass) ? v2L : (left - k * v1L - v2L);

                const double v3R = right - v.ic2R;
                const double v1R = a1 * v.ic1R + a2 * v3R;
                const double v2R = v.ic2R + a2 * v.ic1R + a3 * v3R;
                v.ic1R = 2.0 * v1R - v.ic1R;
                v.ic2R = 2.0 * v2R - v.ic2R;
                right = (fMode == FilterMode::LowPass) ? v2R : (right - k * v1R - v2R);
            }
            peak = std::max (peak, static_cast<float> (std::max (std::abs (left), std::abs (right))));
            if (bus.right != nullptr)
            {
                bus.left[f] += static_cast<float> (left);
                bus.right[f] += static_cast<float> (right);
            }
            else if (bus.left != nullptr)
                bus.left[f] += static_cast<float> (0.5 * (left + right));
            v.position += v.reverse ? -step : step;
            if (v.stage == Stage::Off)
            {
                peakOut = peak;
                return;
            }
        }
        peakOut = peak;
    }

    static constexpr int previewLane = furrow::lanes;

    // Audio thread.
    std::array<Lane, furrow::lanes> lanes_ {};
    std::array<Voice, voices> voices_ {};
    SampleBuffer* previewSample_ = nullptr;
    Voice previewVoice_ {};
    std::array<float, furrow::lanes> lanePeaks_ {};
    std::array<SampleBuffer*, queueSize> retired_ {};
    int retiring_ = 0;
    double rate_ = 44100.0;
    int chokeSamples_ = 1;
    std::uint64_t order_ = 0;

    // Shared between the two sides.
    SpscQueue<Change, queueSize> changes_;
    SpscQueue<SampleBuffer*, queueSize> returned_;

    // Non-audio side.
    std::mutex producer_;
    int outstanding_ = 0;
};

} // namespace furrow::sampler
