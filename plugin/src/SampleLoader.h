#pragma once

#include "furrow/sampler/SampleBuffer.h"
#include "furrow/Rules.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

// Decodes sample files on its own thread, so neither the audio thread nor the host's message
// thread waits for a disk. Each finished file is passed to the `done` callback on that thread.
// The thread also calls `tick` about every 50 ms, which the plugin uses for upkeep that must not
// run on the audio thread.
class SampleLoader final : private juce::Thread
{
public:
    struct Result
    {
        int lane = 0;
        std::uint64_t generation = 0;
        std::unique_ptr<furrow::sampler::SampleBuffer> sample;  // empty when the file could not be read
        std::array<std::uint8_t, 32> hash {};                   // SHA-256 of the file's bytes
        juce::String error;                                     // why it could not be read
    };

    SampleLoader (std::function<void (Result&&)> done, std::function<void()> tick);
    ~SampleLoader() override;

    // Queues the latest file for each lane, coalescing obsolete pending requests.
    // generation lets the caller ignore an in-flight result it no longer wants.
    void request (int lane, std::uint64_t generation, const juce::String& path);

    // Reads one file now, on the calling thread. WAV, AIFF and FLAC only.
    static Result load (juce::AudioFormatManager& formats, const juce::String& path);

private:
    struct Job
    {
        int lane = 0;
        std::uint64_t generation = 0;
        juce::String path;
    };

    void run() override;

    std::function<void (Result&&)> done_;
    std::function<void()> tick_;
    juce::AudioFormatManager formats_;
    std::mutex lock_;
    std::deque<Job> jobs_;
};
