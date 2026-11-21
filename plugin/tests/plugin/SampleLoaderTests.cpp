#include "SampleLoader.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

TEST_CASE ("Sample loading keeps only the latest queued request for a lane")
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::vector<std::uint64_t> completed;
    {
        SampleLoader loader ([&] (SampleLoader::Result&& result) {
            std::unique_lock lock (mutex);
            completed.push_back (result.generation);
            if (result.generation == 1)
            {
                entered = true;
                changed.notify_all();
                changed.wait (lock, [&] { return proceed; });
            }
            changed.notify_all();
        }, [] {});
        loader.request (0, 1, "/furrow-test-no-file-1.wav");
        bool ready;
        {
            std::unique_lock lock (mutex);
            ready = changed.wait_for (lock, std::chrono::seconds (5), [&] { return entered; });
        }
        // Never leave the worker waiting if the assertion fails.
        if (ready)
        {
            loader.request (0, 2, "/furrow-test-no-file-2.wav");
            loader.request (0, 3, "/furrow-test-no-file-3.wav");
        }
        {
            const std::lock_guard lock (mutex);
            proceed = true;
        }
        changed.notify_all();
        REQUIRE (ready);
        {
            std::unique_lock lock (mutex);
            REQUIRE (changed.wait_for (lock, std::chrono::seconds (5), [&] { return completed.size() >= 2; }));
            CHECK (completed == std::vector<std::uint64_t> { 1, 3 });
        }
    }
}

TEST_CASE ("Sample loader rejects an unsupported file with a useful error")
{
    juce::AudioFormatManager formats;
    SampleLoader::registerAudioFormats (formats);
    const auto result = SampleLoader::load (formats, "/furrow-test-does-not-exist.wav");
    CHECK (result.sample == nullptr);
    CHECK (result.error.contains ("not found"));

    CHECK (SampleLoader::isSupportedAudioExtension (".wav"));
    CHECK (SampleLoader::isSupportedAudioExtension (".aif"));
    CHECK (SampleLoader::isSupportedAudioExtension (".aiff"));
    CHECK (SampleLoader::isSupportedAudioExtension (".flac"));
    CHECK (SampleLoader::isSupportedAudioExtension (".ogg"));
    CHECK (SampleLoader::isSupportedAudioExtension (".MP3"));
    CHECK_FALSE (SampleLoader::isSupportedAudioExtension (".txt"));
    CHECK (formats.getNumKnownFormats() >= 5);
}
