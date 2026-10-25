#pragma once

#include <algorithm>
#include <cmath>

namespace furrow
{

// What the host reports about one audio block.
struct BlockInfo
{
    bool playing = false;
    double beat = 0.0;          // project position in quarter notes at the block's first sample
    double bpm = 120.0;         // tempo at the block's first sample
    double sampleRate = 44100.0;
    int numSamples = 0;
};

// Follows the host transport block by block, as SeqCore's @block does. It spots seeks and loop
// wraps, and turns a beat inside the block into a sample offset.
//
// Tempo is held constant across a block. SeqCore also continues a tempo ramp when the previous
// block fits one; that arrives with the engine port, together with its golden tests.
//
// Real-time safe: no allocation, no locks.
class BlockClock
{
public:
    // Starts a playing block. Returns false, and leaves the clock unchanged, when the block holds
    // no time: zero samples, or a tempo or sample rate that is not positive.
    bool begin (const BlockInfo& block) noexcept
    {
        if (block.numSamples <= 0 || ! (block.bpm > 0.0) || ! (block.sampleRate > 0.0))
            return false;

        const double samplesPerBeat = 60.0 * block.sampleRate / block.bpm;
        const double beats = block.numSamples / samplesPerBeat;

        // A tempo change inside the last block moves this block's start by less than that
        // block's beats, so only a larger difference is a seek or loop wrap.
        jumped_ = ! running_ || std::abs (block.beat - endBeat_) > previousBeats_;

        startBeat_ = block.beat;
        endBeat_ = block.beat + beats;
        previousBeats_ = beats;
        samplesPerBeat_ = samplesPerBeat;
        numSamples_ = block.numSamples;
        running_ = true;
        return true;
    }

    // Forgets the previous block, so the next playing block counts as a jump.
    void stop() noexcept { running_ = false; }

    bool running() const noexcept { return running_; }
    bool jumped() const noexcept { return jumped_; }
    double startBeat() const noexcept { return startBeat_; }
    double endBeat() const noexcept { return endBeat_; }

    // The sample offset of a beat in the current block. Rounds as SeqCore does, floor(x + 0.5),
    // and clamps into the block.
    int offsetOf (double beat) const noexcept
    {
        const double x = std::floor ((beat - startBeat_) * samplesPerBeat_ + 0.5);
        return static_cast<int> (std::clamp (x, 0.0, static_cast<double> (numSamples_ - 1)));
    }

private:
    bool running_ = false;
    bool jumped_ = true;
    double startBeat_ = 0.0;
    double endBeat_ = 0.0;
    double previousBeats_ = 0.0;
    double samplesPerBeat_ = 1.0;
    int numSamples_ = 1;
};

} // namespace furrow
