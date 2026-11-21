#include "PatternAudio.h"
#include "SampleLoader.h"
#include "furrow/sampler/BuiltinKit.h"
#include "furrow/sampler/Sampler.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace furrow::plugin
{

juce::Result exportPatternWav (const Engine& source, std::span<const sampler::LaneSetup> setups,
                               const PatternAudioExportOptions& options, const juce::File& destination)
{
    if (options.lengthSteps < 1 || options.lengthSteps > maxSteps
        || ! std::isfinite (options.bpm) || options.bpm <= 0.0
        || ! std::isfinite (options.sampleRate) || options.sampleRate <= 0.0
        || options.onlyLane < -1 || options.onlyLane >= lanes)
        return juce::Result::fail ("Invalid audio export options.");

    std::vector<std::int32_t> saved;
    source.saveState (saved);
    Engine scratch;
    if (scratch.loadState (saved) != LoadError::None)
        return juce::Result::fail ("Could not clone engine state.");
    scratch.prepare (options.sampleRate);
    const Command origin { Op::QueuePattern, source.pattern(), 0, 0, 0, 0, 0.0 };
    if (scratch.apply (std::span<const Command> (&origin, 1), [] (const MidiEvent&) {}) != Result::Ok)
        return juce::Result::fail ("Could not queue pattern at origin.");

    sampler::Sampler scratchSampler;
    scratchSampler.prepare (options.sampleRate);
    juce::AudioFormatManager formats;
    SampleLoader::registerAudioFormats (formats);

    bool anySampleLane = false;
    for (const auto& setup : setups)
    {
        if (setup.slot < 0 || setup.slot >= lanes)
            continue;
        if (options.onlyLane >= 0 && setup.slot != options.onlyLane)
            continue;
        if (setup.settings.target != sampler::Target::Sample)
            continue;
        auto settings = setup.settings;
        settings.output = 1;
        scratchSampler.setLane (setup.slot, settings);

        std::unique_ptr<sampler::SampleBuffer> buf;
        if (setup.sample.kind == sampler::SampleRef::Kind::Builtin)
            buf = sampler::makeBuiltin (setup.sample.builtin);
        else if (setup.sample.kind == sampler::SampleRef::Kind::File && ! setup.sample.path.empty())
            buf = SampleLoader::load (formats, juce::String::fromUTF8 (setup.sample.path.c_str())).sample;
        if (buf != nullptr)
        {
            scratchSampler.setSample (setup.slot, buf);
            anySampleLane = true;
        }
    }
    if (! anySampleLane)
        return juce::Result::fail ("No sample lanes are available to export as audio.");
    scratchSampler.update();

    const double totalBeats = options.lengthSteps * stepBeats;
    const auto loopSamples = std::max<std::int64_t> (1, std::llround (totalBeats * 60.0 / options.bpm * options.sampleRate));
    const auto tailSamples = options.includeTail
        ? std::max<std::int64_t> (0, std::llround (std::clamp (options.maxTailSeconds, 0.0, 4.0) * options.sampleRate))
        : 0;
    const auto maxSamples = loopSamples + tailSamples;
    if (maxSamples > 192000LL * 600LL)
        return juce::Result::fail ("Rendered audio exceeds maximum length.");

    std::vector<float> left (static_cast<std::size_t> (maxSamples), 0.0f);
    std::vector<float> right (static_cast<std::size_t> (maxSamples), 0.0f);

    constexpr int block = 512;
    std::int64_t cursor = 0;
    while (cursor < loopSamples)
    {
        const int n = static_cast<int> (std::min<std::int64_t> (block, loopSamples - cursor));
        const double blockBeat = static_cast<double> (cursor) / options.sampleRate * options.bpm / 60.0;
        const sampler::Bus bus { left.data() + cursor, right.data() + cursor };
        const std::span<const sampler::Bus> buses (&bus, 1);
        int rendered = 0;
        scratch.process ({ true, blockBeat, options.bpm, options.sampleRate, n }, [&] (const MidiEvent& event) {
            const double beat = blockBeat + static_cast<double> (event.offset) / options.sampleRate * options.bpm / 60.0;
            if (beat >= totalBeats - 1e-9 && event.status == 0x90 && event.data2 > 0)
                return;
            if (options.onlyLane >= 0 && event.lane != options.onlyLane)
                return;
            if (event.offset > rendered)
            {
                scratchSampler.render (buses, rendered, event.offset);
                rendered = event.offset;
            }
            if (scratchSampler.lane (event.lane).target == sampler::Target::Sample)
                scratchSampler.handle (event, scratch.laneField (event.lane, Note), scratch.laneField (event.lane, BendRange));
        });
        scratchSampler.render (buses, rendered, n);
        cursor += n;
    }

    std::int64_t outputFrames = loopSamples;
    if (tailSamples > 0)
    {
        scratch.panic ([&] (const MidiEvent& event) {
            if (options.onlyLane >= 0 && event.lane != options.onlyLane)
                return;
            if (scratchSampler.lane (event.lane).target == sampler::Target::Sample)
                scratchSampler.handle (event, scratch.laneField (event.lane, Note), scratch.laneField (event.lane, BendRange));
        });
        while (cursor < maxSamples)
        {
            const int n = static_cast<int> (std::min<std::int64_t> (block, maxSamples - cursor));
            const sampler::Bus bus { left.data() + cursor, right.data() + cursor };
            scratchSampler.render (std::span<const sampler::Bus> (&bus, 1), 0, n);
            cursor += n;
        }
        std::int64_t lastAudible = loopSamples;
        for (std::int64_t f = maxSamples - 1; f >= loopSamples; --f)
        {
            const auto idx = static_cast<std::size_t> (f);
            if (std::abs (left[idx]) > 1e-5f || std::abs (right[idx]) > 1e-5f)
            {
                lastAudible = f + 1;
                break;
            }
        }
        outputFrames = lastAudible;
    }

    for (std::int64_t f = 0; f < outputFrames; ++f)
    {
        const auto idx = static_cast<std::size_t> (f);
        left[idx] = std::clamp (left[idx], -1.0f, 1.0f);
        right[idx] = std::clamp (right[idx], -1.0f, 1.0f);
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (destination);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return juce::Result::fail ("Could not open WAV file for writing.");
    const auto writerOpts = juce::AudioFormatWriterOptions()
                                .withSampleRate (options.sampleRate)
                                .withNumChannels (2)
                                .withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, writerOpts);
    if (writer == nullptr)
        return juce::Result::fail ("Could not create WAV writer.");
    const float* channels[2] = { left.data(), right.data() };
    if (! writer->writeFromFloatArrays (channels, 2, static_cast<int> (outputFrames)))
        return juce::Result::fail ("Could not write WAV samples.");
    return juce::Result::ok();
}

} // namespace furrow::plugin
