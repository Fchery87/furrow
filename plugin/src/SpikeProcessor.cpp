#include "SpikeProcessor.h"
#if FURROW_ENABLE_WEBVIEW
#include "PluginEditor.h"
#endif
#include "furrow/PluginState.h"
#include "furrow/sampler/BuiltinKit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using namespace furrow;
using namespace furrow::sampler;
std::vector<std::int32_t> defaultRack()
{
    Engine scratch;
    std::vector<Command> commands;
    const int notes[] {36, 38, 42, 46};
    const char* grid[] {"1000100010001000", "0010001000100010", "1010101010101010", "0000100000001001"};
    for (int s = 0; s < 4; ++s)
        commands.push_back ({Op::AddLane, s, s + 1, s + 1, notes[s], s});
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 16; ++k)
            if (grid[s][k] == '1') commands.push_back ({Op::SetStep, 0, s, k, On, 1});
    scratch.apply (commands, [] (const MidiEvent&) {});
    std::vector<std::int32_t> words;
    scratch.saveState (words);
    return words;
}
LaneSetup midiSetup (int slot, int id)
{
    LaneSetup result;
    result.slot = slot;
    result.id = id;
    result.settings.channel = slot % 16 + 1;
    return result;
}
bool sameSample (const SampleRef& a, const SampleRef& b)
{
    return a.kind == b.kind && (a.kind == SampleRef::Kind::None
        || (a.kind == SampleRef::Kind::Builtin && a.builtin == b.builtin)
        || (a.kind == SampleRef::Kind::File && a.path == b.path && a.hash == b.hash));
}
}

SpikeProcessor::SpikeProcessor() : AudioProcessor (buses()), modelEngine (std::make_unique<furrow::Engine>())
{
    const auto words = defaultRack();
    modelEngine->loadState (words);
    audioRack = new PreparedRack {std::make_unique<furrow::Engine>(), 0};
    audioEngine = audioRack->engine.get();
    audioEngine->loadState (words);
    for (int s = 0; s < furrow::lanes; ++s)
    {
        playheads[static_cast<std::size_t> (s)] = -1;
        laneNames[static_cast<std::size_t> (s)] = "Lane " + juce::String (s + 1);
        laneColors[static_cast<std::size_t> (s)] = "#c3b374";
    }
    for (int p = 0; p < furrow::patterns; ++p)
        patternNames[static_cast<std::size_t> (p)] = "Pattern " + juce::String (p + 1);
    const char* names[] {"Kick", "Snare", "Closed hat", "Open hat"};
    std::vector<LaneSetup> setups;
    for (int s = 0; s < 4; ++s)
    {
        auto setup = midiSetup (s, s + 1);
        setup.settings.target = Target::Sample;
        setup.settings.choke = s >= 2 ? 1 : 0;
        setup.sample.kind = SampleRef::Kind::Builtin;
        setup.sample.builtin = static_cast<furrow::sampler::Builtin> (s);
        laneNames[static_cast<std::size_t> (s)] = names[s];
        setups.push_back (setup);
    }
    applySetups (setups, laneIds());
    for (std::size_t i = 0; i < midiSlots; ++i)
    {
        midiBuffers[i].ensureSize (midiBytes);
        readyMidi.push (static_cast<int> (i));
    }
    loader = std::make_unique<SampleLoader> ([this] (SampleLoader::Result&& r) { onLoaded (std::move (r)); }, [this] {
        const std::lock_guard lock (modelLock);
        collect();
        sync();
    });
}
SpikeProcessor::~SpikeProcessor()
{
    loader.reset();
    delete preparedEngine.exchange (nullptr);
    PreparedRack* rack = nullptr;
    while (retiredEngines.pop (rack)) delete rack;
    delete audioRack;
    EditBatch* batch = nullptr;
    while (edits.pop (batch)) delete batch;
    while (retiredEdits.pop (batch)) delete batch;
}
juce::AudioProcessorEditor* SpikeProcessor::createEditor()
{
#if FURROW_ENABLE_WEBVIEW
    return new PluginEditor (*this);
#else
    return nullptr;
#endif
}
bool SpikeProcessor::hasEditor() const
{
#if FURROW_ENABLE_WEBVIEW
    return true;
#else
    return false;
#endif
}
juce::AudioProcessor::BusesProperties SpikeProcessor::buses()
{
    auto result = BusesProperties().withOutput ("Main", juce::AudioChannelSet::stereo(), true);
    for (int bus = 2; bus <= furrow::sampler::outputs; ++bus)
        result = result.withOutput ("Out " + juce::String (bus), juce::AudioChannelSet::stereo(), false);
    return result;
}
void SpikeProcessor::prepareToPlay (double rate, int)
{
    audioEngine->prepare (rate);
    sampler.prepare (rate);
    resetRequested = true;
}
void SpikeProcessor::reset() { resetRequested.store (true, std::memory_order_release); }
bool SpikeProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto usable = [] (const juce::AudioChannelSet& set) {
        return set == juce::AudioChannelSet::stereo() || set == juce::AudioChannelSet::mono();
    };
    if (! layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty() || ! usable (layout.outputBuses[0])) return false;
    for (int b = 1; b < layout.outputBuses.size(); ++b)
        if (! layout.outputBuses[b].isDisabled() && ! usable (layout.outputBuses[b])) return false;
    return true;
}
furrow::BlockInfo SpikeProcessor::readTransport (int samples)
{
    furrow::BlockInfo info;
    info.numSamples = samples;
    info.sampleRate = getSampleRate();
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            if (auto beat = position->getPpqPosition()) info.beat = *beat;
            if (auto bpm = position->getBpm()) info.bpm = *bpm;
            info.playing = position->getIsPlaying() || position->getIsRecording();
            const auto signature = position->getTimeSignature().orFallback (juce::AudioPlayHead::TimeSignature {4, 4});
            const double bar = signature.denominator > 0 ? signature.numerator * 4.0 / signature.denominator : 4.0;
            const double start = position->getPpqPositionOfLastBarStart().orFallback (std::floor (info.beat / bar) * bar);
            nextBar = start + bar;
        }
    hostBeat = info.beat;
    hostBpm = info.bpm;
    hostPlaying = info.playing;
    return info;
}
void SpikeProcessor::route (const furrow::MidiEvent& event, juce::MidiBuffer& midi)
{
    const auto& lane = sampler.lane (event.lane);
    if (lane.target == Target::Sample)
    {
        sampler.handle (event, audioEngine->laneField (event.lane, furrow::Note), audioEngine->laneField (event.lane, furrow::BendRange));
        return;
    }
    if (emittedBytes + 9 > midiBytes)
    {
        midiOverflow = true;
        resetRequested = true;
        return;
    }
    midi.addEvent (juce::MidiMessage (static_cast<int> (event.status) | (lane.channel - 1), event.data1, event.data2), event.offset);
    emittedBytes += 9;
}
void SpikeProcessor::beginBlock (juce::MidiBuffer& midi)
{
    if (retiredCount.load (std::memory_order_acquire) < 63)
        if (auto* ready = preparedEngine.exchange (nullptr, std::memory_order_acq_rel))
        {
            audioEngine->panic ([&] (const auto& e) { route (e, midi); });
            sampler.reset();
            retiredEngines.push (audioRack);
            retiredCount.fetch_add (1, std::memory_order_release);
            audioRack = ready;
            audioEngine = ready->engine.get();
            audioGeneration = ready->generation;
            audioEngine->prepare (getSampleRate());
        }
    if (routeChanged.exchange (false) || resetRequested.exchange (false))
    {
        audioEngine->panic ([&] (const auto& e) { route (e, midi); });
        sampler.reset();
        audioEngine->reset();
    }
    sampler.update();
    EditBatch* batch = nullptr;
    for (int n = 0; n < 128 && edits.pop (batch); ++n)
    {
        if (batch->generation == audioGeneration && ! blocked.load())
        {
            if (batch->panic) { audioEngine->panic ([&] (const auto& e) { route (e, midi); }); sampler.reset(); }
            if (batch->audition >= 0) audioEngine->audition (batch->audition, 100, [&] (const auto& e) { route (e, midi); });
            if (! batch->commands.empty()) audioEngine->apply (batch->commands, [&] (const auto& e) { route (e, midi); });
        }
        retiredEdits.push (batch);
    }
}
int SpikeProcessor::acquireMidi (juce::MidiBuffer& midi)
{
    if (lastMidiSlot >= 0)
    {
        midi.swapWith (midiBuffers[static_cast<std::size_t> (lastMidiSlot)]);
        midiBuffers[static_cast<std::size_t> (lastMidiSlot)].clear();
        return lastMidiSlot;
    }
    int slot;
    if (readyMidi.pop (slot)) return slot;
    midiOverflow = true;
    resetRequested = true;
    return -1;
}
void SpikeProcessor::deliverMidi (int slot, juce::MidiBuffer& midi)
{
    auto& output = midiBuffers[static_cast<std::size_t> (slot)];
    output.swapWith (midi);
    lastMidiSlot = slot;
}
void SpikeProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    midi.clear();
    const int slot = acquireMidi (midi);
    if (slot < 0) return;
    auto& output = midiBuffers[static_cast<std::size_t> (slot)];
    output.clear();
    emittedBytes = 0;
    beginBlock (output);
    audioEngine->panic ([&] (const auto& e) { route (e, output); });
    sampler.reset();
    audioEngine->reset();
    deliverMidi (slot, midi);
}
void SpikeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();
    if (buffer.getNumSamples() <= 0) return;
    const int slot = acquireMidi (midi);
    if (slot < 0) return;
    auto& output = midiBuffers[static_cast<std::size_t> (slot)];
    output.clear();
    emittedBytes = 0;
    beginBlock (output);
    std::array<furrow::sampler::Bus, furrow::sampler::outputs> outputs {};
    for (int b = 0; b < std::min (getBusCount (false), furrow::sampler::outputs); ++b)
    {
        auto bus = getBusBuffer (buffer, false, b);
        if (bus.getNumChannels() > 0) outputs[static_cast<std::size_t> (b)] = {bus.getWritePointer (0), bus.getNumChannels() > 1 ? bus.getWritePointer (1) : nullptr};
    }
    const auto transport = readTransport (buffer.getNumSamples());
    if (! blocked.load (std::memory_order_acquire))
    {
        int rendered = 0;
        audioEngine->process (transport, [&] (const furrow::MidiEvent& e) {
            if (sampler.lane (e.lane).target == Target::Sample && e.offset > rendered)
            {
                sampler.render (outputs, rendered, e.offset);
                rendered = e.offset;
            }
            route (e, output);
        });
        sampler.render (outputs, rendered, buffer.getNumSamples());
    }
    else
    {
        audioEngine->panic ([&] (const auto& e) { route (e, output); });
        sampler.reset();
    }
    for (int s = 0; s < furrow::lanes; ++s) playheads[static_cast<std::size_t> (s)] = audioEngine->playhead (s);
    deliverMidi (slot, midi);
}
void SpikeProcessor::collect()
{
    PreparedRack* rack = nullptr;
    while (retiredEngines.pop (rack)) { delete rack; retiredCount.fetch_sub (1, std::memory_order_release); }
    EditBatch* batch = nullptr;
    while (retiredEdits.pop (batch)) { delete batch; --outstandingEdits; }
    int slot;
    while (usedMidi.pop (slot))
    {
        midiBuffers[static_cast<std::size_t> (slot)].clear();
        midiBuffers[static_cast<std::size_t> (slot)].ensureSize (midiBytes);
        readyMidi.push (slot);
    }
    sampler.collect();
}
void SpikeProcessor::publishEngine (std::unique_ptr<furrow::Engine> replacement)
{
    auto* rack = new PreparedRack {std::move (replacement), ++generation};
    delete preparedEngine.exchange (rack, std::memory_order_acq_rel);
}
SpikeProcessor::LaneIds SpikeProcessor::laneIds() const
{
    LaneIds ids {};
    for (int s = 0; s < furrow::lanes; ++s)
        if (modelEngine->laneField (s, furrow::Active)) ids[static_cast<std::size_t> (s)] = modelEngine->laneField (s, furrow::Id);
    return ids;
}
void SpikeProcessor::applySetups (std::span<const LaneSetup> setups, const LaneIds& ids)
{
    for (int s = 0; s < furrow::lanes; ++s)
    {
        auto next = midiSetup (s, ids[static_cast<std::size_t> (s)]);
        for (const auto& saved : setups) if (saved.slot == s && saved.id == next.id && next.id != 0) next = saved;
        auto& lane = model[static_cast<std::size_t> (s)];
        if (next.settings != lane.setup.settings) lane.settingsDirty = true;
        const bool replace = ! sameSample (next.sample, lane.requested) || lane.generation == 0;
        lane.setup = next;
        if (replace) requestSample (s, lane, next.sample);
    }
    sync();
}
void SpikeProcessor::requestSample (int slot, LaneModel& lane, const SampleRef& ref)
{
    lane.requested = ref;
    ++lane.generation;
    lane.pending.reset();
    lane.error.clear();
    lane.sampleDirty = false;
    if (ref.kind == SampleRef::Kind::None) lane.sampleDirty = true;
    else if (ref.kind == SampleRef::Kind::Builtin)
    {
        lane.pending = furrow::sampler::makeBuiltin (ref.builtin);
        lane.sampleDirty = true;
    }
    else if (loader) loader->request (slot, lane.generation, juce::String::fromUTF8 (ref.path.c_str()));
}
void SpikeProcessor::sync()
{
    for (int s = 0; s < furrow::lanes; ++s)
    {
        auto& lane = model[static_cast<std::size_t> (s)];
        if (lane.settingsDirty && sampler.setLane (s, lane.setup.settings)) lane.settingsDirty = false;
        if (lane.sampleDirty && sampler.setSample (s, lane.pending)) lane.sampleDirty = false;
    }
}
void SpikeProcessor::onLoaded (SampleLoader::Result&& result)
{
    const std::lock_guard lock (modelLock);
    auto& lane = model[static_cast<std::size_t> (result.lane)];
    if (lane.generation != result.generation) return;
    lane.pending = std::move (result.sample);
    lane.error = result.error;
    lane.sampleDirty = true;
    if (lane.pending && lane.setup.sample.kind == SampleRef::Kind::File) lane.setup.sample.hash = result.hash;
    sync();
}
std::vector<SpikeProcessor::LaneSetup> SpikeProcessor::laneSetups()
{
    const std::lock_guard lock (modelLock);
    std::vector<LaneSetup> result;
    const auto ids = laneIds();
    for (int s = 0; s < furrow::lanes; ++s) if (ids[static_cast<std::size_t> (s)] != 0) result.push_back (model[static_cast<std::size_t> (s)].setup);
    return result;
}
std::unique_ptr<furrow::Engine> SpikeProcessor::patternEngineSnapshot()
{
    const std::lock_guard lock (modelLock);
    auto engine = std::make_unique<furrow::Engine>();
    std::vector<std::int32_t> words;
    modelEngine->saveState (words);
    engine->loadState (words);
    return engine;
}
void SpikeProcessor::saveLocked (juce::MemoryBlock& destination)
{
    if (! rejectedState.empty()) { destination.replaceAll (rejectedState.data(), rejectedState.size()); return; }
    std::vector<std::int32_t> words;
    modelEngine->saveState (words);
    std::vector<std::uint8_t> bytes;
    furrow::Metadata metadata;
    for (std::size_t i = 0; i < laneNames.size(); ++i)
    {
        metadata.laneNames[i] = laneNames[i].toStdString();
        metadata.laneColors[i] = juce::Colour::fromString ("ff" + laneColors[i].trimCharactersAtStart ("#")).getARGB();
    }
    for (std::size_t i = 0; i < patternNames.size(); ++i) metadata.patternNames[i] = patternNames[i].toStdString();
    metadata.windowWidth = windowWidth;
    metadata.windowHeight = windowHeight;
    furrow::encodePluginState (words, laneSetups(), metadata, bytes);
    destination.replaceAll (bytes.data(), bytes.size());
}
void SpikeProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    const std::lock_guard lock (modelLock);
    saveLocked (destination);
}
void SpikeProcessor::setStateInformation (const void* data, int size)
{
    if (! data || size < 4) return;
    const auto bytes = std::span (static_cast<const std::uint8_t*> (data), static_cast<std::size_t> (size));
    if (std::memcmp (bytes.data(), "FRWS", 4) != 0) return;
    const std::lock_guard lock (modelLock);
    furrow::PluginState state;
    auto replacement = std::make_unique<furrow::Engine>();
    if (! furrow::decodePluginState (bytes, state) || replacement->loadState (state.words) != furrow::LoadError::None)
    {
        rejectedState.assign (bytes.begin(), bytes.end());
        lastError = "This Furrow state cannot be read. Load a compatible state to recover. The original data is retained.";
        blocked = true;
        resetRequested = true;
        return;
    }
    auto audio = std::make_unique<furrow::Engine>();
    audio->loadState (state.words);
    modelEngine = std::move (replacement);
    selectedPattern = modelEngine->pattern();
    if (state.version >= furrow::pluginMetadataVersion)
    {
        for (std::size_t i = 0; i < laneNames.size(); ++i)
        {
            laneNames[i] = juce::String::fromUTF8 (state.metadata.laneNames[i].c_str());
            laneColors[i] = "#" + juce::String::toHexString (static_cast<int> (state.metadata.laneColors[i] & 0xffffffu)).paddedLeft ('0', 6);
        }
        for (std::size_t i = 0; i < patternNames.size(); ++i) patternNames[i] = juce::String::fromUTF8 (state.metadata.patternNames[i].c_str());
        windowWidth = state.metadata.windowWidth;
        windowHeight = state.metadata.windowHeight;
    }
    applySetups (state.setups, laneIds());
    publishEngine (std::move (audio));
    rejectedState.clear();
    blocked = false;
    lastError.clear();
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}
void SpikeProcessor::remember()
{
    juce::MemoryBlock bytes;
    saveLocked (bytes);
    if (undoStates.size() >= 64) undoStates.erase (undoStates.begin());
    undoStates.push_back (std::move (bytes));
    redoStates.clear();
}
juce::Result SpikeProcessor::restoreHistory (bool redo)
{
    auto& source = redo ? redoStates : undoStates;
    auto& destination = redo ? undoStates : redoStates;
    if (source.empty()) return juce::Result::fail (redo ? "Nothing to redo." : "Nothing to undo.");
    juce::MemoryBlock current;
    saveLocked (current);
    auto previous = std::move (source.back());
    source.pop_back();
    destination.push_back (std::move (current));
    setStateInformation (previous.getData(), static_cast<int> (previous.getSize()));
    return juce::Result::ok();
}
juce::Result SpikeProcessor::applyLocked (std::span<const furrow::Command> commands, bool history)
{
    if (blocked) return juce::Result::fail ("Load a readable state before editing.");
    collect();
    if (outstandingEdits >= 128) return juce::Result::fail ("The edit queue is full. Start processing and try again.");
    auto batch = std::make_unique<EditBatch>();
    batch->commands.assign (commands.begin(), commands.end());
    batch->generation = generation;
    juce::MemoryBlock previous;
    if (history) saveLocked (previous);
    const auto result = modelEngine->apply (commands, [] (const furrow::MidiEvent&) {});
    if (result != furrow::Result::Ok) return juce::Result::fail (result == furrow::Result::Capacity ? "The pattern capacity is full." : "The edit contains an invalid value.");
    if (history)
    {
        if (undoStates.size() >= 64) undoStates.erase (undoStates.begin());
        undoStates.push_back (std::move (previous));
        redoStates.clear();
    }
    const bool queued = edits.push (batch.get());
    jassert (queued);
    if (! queued) return juce::Result::fail ("The edit queue is full.");
    batch.release();
    ++outstandingEdits;
    selectedPattern = modelEngine->pattern();
    if (std::any_of (commands.begin(), commands.end(), [] (const auto& command) {
        return command.op == furrow::Op::AddLane || command.op == furrow::Op::RemoveLane;
    })) applySetups (laneSetups(), laneIds());
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    return juce::Result::ok();
}
juce::Result SpikeProcessor::applyCommands (std::span<const furrow::Command> commands)
{
    const std::lock_guard lock (modelLock);
    return applyLocked (commands);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SpikeProcessor(); }
