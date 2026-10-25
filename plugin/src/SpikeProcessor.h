#pragma once

#include "furrow/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>
#include <vector>

// The development plugin: the sequencer engine with no editor yet. A new instance plays the
// 4 × 16 pattern in probes/pattern-4x16.txt in time with the host. It sends lane n's MIDI on
// channel n + 1 and, unless Tones is off, plays one sine blip per hit so it can be heard without
// any routing. Its saved state is the engine's state, so a host project restores the rack.
class SpikeProcessor final : public juce::AudioProcessor
{
public:
    SpikeProcessor();

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destination) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    struct Tone
    {
        double phase = 0.0;
        double step = 0.0;      // phase increment per sample, in radians
        float level = 0.0f;
    };

    furrow::BlockInfo readTransport (int numSamples);
    void renderTones (juce::AudioBuffer<float>& buffer, int start, int end);

    juce::AudioParameterBool* tones = nullptr;
    furrow::Engine engine;
    std::array<Tone, furrow::lanes> voices {};
    float decayPerSample = 0.0f;
    // Reused by getStateInformation, which runs off the audio thread.
    std::vector<std::int32_t> stateWords;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeProcessor)
};
