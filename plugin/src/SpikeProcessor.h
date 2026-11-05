#pragma once

#include "SampleLoader.h"
#include "furrow/Engine.h"
#include "furrow/sampler/LaneSetup.h"
#include "furrow/sampler/Sampler.h"
#include "furrow/sampler/SpscQueue.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

class SpikeProcessor final : public juce::AudioProcessor
{
public:
    SpikeProcessor();
    ~SpikeProcessor() override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.01; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destination) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::var editorState();
    juce::Result edit (const juce::var& command);
    juce::Result applyCommands (std::span<const furrow::Command> commands);
    std::vector<furrow::sampler::LaneSetup> laneSetups();
    std::unique_ptr<furrow::Engine> patternEngineSnapshot();

private:
    using LaneSetup = furrow::sampler::LaneSetup;
    using SampleRef = furrow::sampler::SampleRef;
    using Target = furrow::sampler::Target;
    using LaneIds = std::array<std::int32_t, furrow::lanes>;
    struct LaneModel
    {
        LaneSetup setup;
        SampleRef requested;
        std::uint64_t generation = 0;
        std::unique_ptr<furrow::sampler::SampleBuffer> pending;
        bool settingsDirty = true;
        bool sampleDirty = false;
        juce::String error;
    };
    struct EditBatch
    {
        std::vector<furrow::Command> commands;
        int audition = -1;
        bool panic = false;
        std::uint64_t generation = 0;
    };
    struct PreparedRack
    {
        std::unique_ptr<furrow::Engine> engine;
        std::uint64_t generation = 0;
    };
    static BusesProperties buses();
    furrow::BlockInfo readTransport (int numSamples);
    LaneIds laneIds() const;
    void applySetups (std::span<const LaneSetup> setups, const LaneIds& ids);
    void requestSample (int slot, LaneModel& lane, const SampleRef& ref);
    void sync();
    void onLoaded (SampleLoader::Result&& result);
    void collect();
    void beginBlock (juce::MidiBuffer& midi);
    void route (const furrow::MidiEvent& event, juce::MidiBuffer& midi);
    int acquireMidi (juce::MidiBuffer& midi);
    void deliverMidi (int slot, juce::MidiBuffer& midi);
    void publishEngine (std::unique_ptr<furrow::Engine> replacement);
    void saveLocked (juce::MemoryBlock& destination);
    juce::Result applyLocked (std::span<const furrow::Command> commands, bool remember = true);
    void remember();
    juce::Result restoreHistory (bool redo);
    juce::Result editFile (const juce::var& command);

    std::unique_ptr<furrow::Engine> modelEngine;
    furrow::Engine* audioEngine = nullptr;
    std::atomic<PreparedRack*> preparedEngine { nullptr };
    furrow::sampler::SpscQueue<PreparedRack*, 64> retiredEngines;
    PreparedRack* audioRack = nullptr;
    std::uint64_t generation = 0;
    std::uint64_t audioGeneration = 0;
    std::atomic<int> retiredCount { 0 };
    furrow::sampler::SpscQueue<EditBatch*, 128> edits;
    furrow::sampler::SpscQueue<EditBatch*, 128> retiredEdits;
    int outstandingEdits = 0;
    furrow::sampler::Sampler sampler;
    std::recursive_mutex modelLock;
    std::array<LaneModel, furrow::lanes> model;
    std::vector<std::uint8_t> rejectedState;
    std::atomic<bool> blocked { false };
    std::atomic<bool> routeChanged { false };
    std::atomic<bool> resetRequested { false };
    std::atomic<int> selectedPattern { 0 };
    std::atomic<double> hostBeat { 0.0 }, hostBpm { 120.0 }, nextBar { 4.0 };
    std::atomic<bool> hostPlaying { false };
    std::array<std::atomic<int>, furrow::lanes> playheads {};
    std::vector<juce::MemoryBlock> undoStates, redoStates;
    juce::String lastError;
    std::array<juce::String, furrow::lanes> laneNames;
    std::array<juce::String, furrow::lanes> laneColors;
    std::array<juce::String, furrow::patterns> patternNames;
    int windowWidth = 1100, windowHeight = 700;
    static constexpr std::size_t midiSlots = 128;
    static constexpr int midiBytes = 65536;
    std::array<juce::MidiBuffer, midiSlots> midiBuffers;
    furrow::sampler::SpscQueue<int, midiSlots> readyMidi, usedMidi;
    std::atomic<bool> midiOverflow { false };
    int emittedBytes = 0;
    int lastMidiSlot = -1;
    std::unique_ptr<SampleLoader> loader;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeProcessor)
};
