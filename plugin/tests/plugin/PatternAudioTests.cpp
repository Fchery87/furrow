#include "FurrowProcessor.h"
#include "PatternAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace
{
struct TempWav
{
    juce::File file = juce::File::createTempFile (".wav");
    ~TempWav() { file.deleteFile(); }
};
}

TEST_CASE ("Offline WAV export renders exact loop-cut audio and optional tail")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    const auto engine = processor.patternEngineSnapshot();
    const auto setups = processor.laneSetups();

    TempWav loopWav;
    furrow::plugin::PatternAudioExportOptions opts;
    opts.bpm = 120.0;
    opts.sampleRate = 44100.0;
    opts.lengthSteps = 16;
    opts.includeTail = false;
    REQUIRE (furrow::plugin::exportPatternWav (*engine, setups, opts, loopWav.file).wasOk());

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (loopWav.file), true));
    REQUIRE (reader != nullptr);
    CHECK (reader->numChannels == 2);
    CHECK (reader->bitsPerSample == 24);
    // 16 steps = 4 beats = 2.0 seconds at 120 BPM = 88200 frames.
    CHECK (reader->lengthInSamples == 88200);

    juce::AudioBuffer<float> audio (2, static_cast<int> (reader->lengthInSamples));
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    float peak = 0.0f;
    for (int f = 0; f < audio.getNumSamples(); ++f)
        peak = std::max (peak, std::abs (audio.getSample (0, f)));
    CHECK (peak > 0.05f);

    // Single-lane export isolates lane 0 cleanly.
    TempWav laneWav;
    opts.onlyLane = 0;
    REQUIRE (furrow::plugin::exportPatternWav (*engine, setups, opts, laneWav.file).wasOk());
    std::unique_ptr<juce::AudioFormatReader> laneReader (wav.createReaderFor (new juce::FileInputStream (laneWav.file), true));
    REQUIRE (laneReader != nullptr);
    CHECK (laneReader->lengthInSamples == 88200);

    // Include tail extends past loop-cut when the final step rings past the loop boundary.
    TempWav tailWav;
    opts.onlyLane = -1;
    opts.includeTail = true;
    opts.maxTailSeconds = 2.0;
    REQUIRE (furrow::plugin::exportPatternWav (*engine, setups, opts, tailWav.file).wasOk());
    std::unique_ptr<juce::AudioFormatReader> tailReader (wav.createReaderFor (new juce::FileInputStream (tailWav.file), true));
    REQUIRE (tailReader != nullptr);
    CHECK (tailReader->lengthInSamples > 88200);
    CHECK (tailReader->lengthInSamples <= 88200 + 88200);
}

TEST_CASE ("Offline WAV export rejects invalid options")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    const auto engine = processor.patternEngineSnapshot();
    const auto setups = processor.laneSetups();
    TempWav wav;
    furrow::plugin::PatternAudioExportOptions bad;
    bad.bpm = 0.0;
    CHECK_FALSE (furrow::plugin::exportPatternWav (*engine, setups, bad, wav.file).wasOk());
}
