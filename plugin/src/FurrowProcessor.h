#pragma once

#include "SampleLoader.h"
#include "PatternMidi.h"
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

class FurrowProcessor final : public juce::AudioProcessor
{
public:
    FurrowProcessor();
    ~FurrowProcessor() override;
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
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
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
    juce::var transportState();
    std::uint64_t stateRevision() const { return revision.load (std::memory_order_acquire); }
    std::uint64_t windowStateRevision() const { return windowRevision.load (std::memory_order_acquire); }
    juce::Result edit (const juce::var& command);
    juce::Result applyCommands (std::span<const furrow::Command> commands);
    juce::Result renderMidiToFile (int lane, const juce::File& file);
    juce::Result renderAudioToFile (int lane, bool includeTail, const juce::File& file);
    void setBrowserSettingsFile (const juce::File& file);
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
        std::array<float, SampleLoader::peakBins> peaks {};
        bool hasPeaks = false;
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
    void resetMetadata();
    void bumpRevision() { revision.fetch_add (1, std::memory_order_release); }
    void syncAudioPatternLocked();
    void syncParametersFromModelLocked();
    void applySetups (std::span<const LaneSetup> setups, const LaneIds& ids);
    void requestSample (int slot, LaneModel& lane, const SampleRef& ref);
    void sync();
    void onLoaded (SampleLoader::Result&& result);
    void collect();
    void beginBlock (juce::MidiBuffer& midi);
    void route (const furrow::MidiEvent& event, juce::MidiBuffer& midi);
    void deliverMidi (juce::MidiBuffer& midi);
    void publishEngine (std::unique_ptr<furrow::Engine> replacement);
    void saveLocked (juce::MemoryBlock& destination);
    juce::Result applyLocked (std::span<const furrow::Command> commands, bool remember = true, bool notify = true);
    void remember();
    juce::Result restoreHistory (bool redo);
    juce::Result editFile (const juce::var& command);
    juce::Result editValidated (const juce::var& command);
    void loadBrowserSettingsLocked();
    void saveBrowserSettingsLocked() const;
    juce::Result listBrowserFolderLocked (const juce::String& path);
    furrow::plugin::PatternMidiImportPreview midiPreview;
    juce::var midiPreviewState;
    std::vector<int> midiPreviewSlots;
    std::vector<LaneSetup> midiPreviewSetups;

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
    std::atomic<int> audioPattern { 0 };
    std::atomic<int> audioPendingPattern { -1 };
    std::atomic<std::uint64_t> revision { 1 };
    std::atomic<std::uint64_t> windowRevision { 1 };
    std::atomic<double> hostBeat { 0.0 }, hostBpm { 120.0 }, nextBar { 4.0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<bool> internalPlaying { false };
    std::atomic<bool> stopVoicesRequested { false };
    std::atomic<double> internalBpm { 120.0 };
    double internalBeat = 0.0;
    bool wasAudioPlaying = false;
    std::atomic<int> patternMidiChannel { 0 };
    std::array<std::atomic<int>, furrow::lanes> playheads {};
    std::array<std::atomic<float>, furrow::lanes> laneMeters {};
    std::vector<juce::MemoryBlock> undoStates, redoStates;
    juce::String lastError;
    std::array<juce::String, furrow::lanes> laneNames;
    std::array<juce::String, furrow::lanes> laneColors;
    std::array<juce::String, furrow::patterns> patternNames;
    int windowWidth = 1100, windowHeight = 700;
    static constexpr std::size_t midiSlots = 256;
    static constexpr int midiBytes = 32768;
    std::array<juce::MidiBuffer, midiSlots> midiBuffers;
    furrow::sampler::SpscQueue<int, midiSlots> readyMidi, usedMidi;
    std::atomic<bool> midiOverflow { false };
    int emittedBytes = 0;
    int scratchMidiSlot = -1;
    juce::AudioParameterFloat* globalSwingParam = nullptr;
    juce::AudioParameterBool* truncateParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    int lastAudioSwing = 0;
    int lastAudioTruncate = 0;
    juce::File browserSettingsFile;
    std::vector<juce::String> browserFolders;
    juce::var browserListing;
    std::uint64_t previewGeneration = 0;
    std::unique_ptr<furrow::sampler::SampleBuffer> pendingPreview;
    bool previewDirty = false;
    std::unique_ptr<SampleLoader> loader;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FurrowProcessor)
};
