#pragma once

#include "furrow/SpikePattern.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// Milestone 1 spike: a plugin with no editor that plays the fixed 4 × 16 pattern in time with
// the host. It sends MIDI out on channels 1 to 4 and, unless Tones is off, plays one sine blip
// per hit so it can be heard without any routing.
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
    furrow::SpikePattern pattern;
    std::array<Tone, furrow::SpikePattern::lanes> voices {};
    float decayPerSample = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeProcessor)
};
