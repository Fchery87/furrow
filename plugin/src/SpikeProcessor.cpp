#include "SpikeProcessor.h"

#include "furrow/StateBytes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace
{
// A tone falls by 60 dB over this time.
constexpr double toneDecaySeconds = 0.15;
constexpr float toneLevel = 0.25f;

// MidiBuffer keeps its capacity between blocks, so reserving this once covers ordinary blocks
// without allocating on the audio thread. JUCE stores a three-byte message in 9 bytes, so a step
// on all 64 lanes with every controller (7 messages each) needs about 4 KB. Very large blocks
// with several steps or ratchets per lane can need more, and JUCE then grows the buffer, which
// allocates. Milestone 4's event queue removes that case.
constexpr int midiReserveBytes = 16384;

// The rack a new instance starts with: the pattern in probes/pattern-4x16.txt.
std::vector<std::int32_t> defaultRack()
{
    const int notes[] = { 36, 38, 42, 46 };
    const char* grid[] = { "1000100010001000", "0010001000100010", "1010101010101010", "0000100000001001" };
    std::vector<furrow::Command> commands;
    for (int lane = 0; lane < 4; ++lane)
        commands.push_back ({ furrow::Op::AddLane, lane, lane + 1, lane + 1, notes[lane], lane });
    for (int lane = 0; lane < 4; ++lane)
        for (int step = 0; step < 16; ++step)
            if (grid[lane][step] == '1')
                commands.push_back ({ furrow::Op::SetStep, 0, lane, step, furrow::On, 1 });

    // Built in a scratch engine and loaded as a state, so the real engine still sends its
    // startup all-notes-off in the first block.
    furrow::Engine scratch;
    [[maybe_unused]] const auto result = scratch.apply (commands, [] (const furrow::MidiEvent&) {});
    jassert (result == furrow::Result::Ok);
    std::vector<std::int32_t> words;
    scratch.saveState (words);
    return words;
}

// Saved state: a four-byte tag, a format version, the Tones switch as 0 or 1, then the engine's
// state bytes (see furrow/StateBytes.h).
constexpr char stateTag[4] = { 'F', 'R', 'W', 'S' };
constexpr std::uint8_t stateVersion = 1;
constexpr std::size_t stateHeader = sizeof (stateTag) + 2;
} // namespace

SpikeProcessor::SpikeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (tones = new juce::AudioParameterBool ({ "tones", 1 }, "Tones", true));
    engine.loadState (defaultRack());
}

void SpikeProcessor::prepareToPlay (double sampleRate, int)
{
    decayPerSample = static_cast<float> (std::pow (0.001, 1.0 / (toneDecaySeconds * sampleRate)));
    engine.prepare (sampleRate);
    reset();
}

void SpikeProcessor::reset()
{
    engine.reset();
    voices = {};
}

bool SpikeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
}

furrow::BlockInfo SpikeProcessor::readTransport (int numSamples)
{
    furrow::BlockInfo block;
    block.sampleRate = getSampleRate();
    block.numSamples = numSamples;

    // Without a position, tempo and play state, the transport counts as stopped at beat 0.
    auto* head = getPlayHead();
    if (head == nullptr)
        return block;
    const auto position = head->getPosition();
    if (! position)
        return block;
    const auto beat = position->getPpqPosition();
    const auto bpm = position->getBpm();
    if (! beat || ! bpm)
        return block;

    block.playing = position->getIsPlaying() || position->getIsRecording();
    block.beat = *beat;
    block.bpm = *bpm;
    return block;
}

void SpikeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();
    midi.ensureSize (midiReserveBytes);

    const int numSamples = buffer.getNumSamples();
    // Lane n on MIDI channel n + 1. Lanes from 16 up share channels until lanes choose their
    // own output, which arrives with the sampler and the interface.
    engine.process (readTransport (numSamples), [&] (const furrow::MidiEvent& e) {
        const auto status = static_cast<std::uint8_t> (e.status | (e.lane % 16));
        midi.addEvent (juce::MidiMessage (status, e.data1, e.data2), e.offset);
    });

    if (! tones->get())
        return;

    // Render the tones in segments, starting each one at its note-on's exact sample.
    int rendered = 0;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (! message.isNoteOn())
            continue;
        renderTones (buffer, rendered, metadata.samplePosition);
        rendered = metadata.samplePosition;

        auto& voice = voices[static_cast<std::size_t> (message.getChannel() - 1)];
        voice.phase = 0.0;
        voice.step = juce::MathConstants<double>::twoPi
                   * juce::MidiMessage::getMidiNoteInHertz (message.getNoteNumber()) / getSampleRate();
        voice.level = toneLevel * message.getFloatVelocity();
    }
    renderTones (buffer, rendered, numSamples);
}

void SpikeProcessor::renderTones (juce::AudioBuffer<float>& buffer, int start, int end)
{
    const int channels = buffer.getNumChannels();
    for (auto& voice : voices)
    {
        if (voice.level < 1.0e-5f)
            continue;
        for (int i = start; i < end; ++i)
        {
            const float sample = voice.level * static_cast<float> (std::sin (voice.phase));
            for (int c = 0; c < channels; ++c)
                buffer.addSample (c, i, sample);
            voice.phase = std::fmod (voice.phase + voice.step, juce::MathConstants<double>::twoPi);
            voice.level *= decayPerSample;
        }
    }
}

// The engine belongs to the audio thread. Until the interface brings a message-thread copy of
// the rack (milestone 4), saving and loading hold JUCE's callback lock, which every wrapper
// holds around processBlock, so the audio thread waits for them. Both take well under a
// millisecond for a typical rack, and hosts save and load mostly while stopped.
void SpikeProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    {
        const juce::ScopedLock lock (getCallbackLock());
        engine.saveState (stateWords);
    }
    std::vector<std::uint8_t> bytes;
    furrow::state::toBytes (stateWords, bytes);
    destination.reset();
    juce::MemoryOutputStream out (destination, false);
    out.write (stateTag, sizeof (stateTag));
    out.writeByte (static_cast<char> (stateVersion));
    out.writeByte (tones->get() ? 1 : 0);
    out.write (bytes.data(), bytes.size());
}

void SpikeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Data without this plugin's tag and version is not a Furrow state and is ignored, keeping
    // the current rack. JUCE gives no way to report it to the host. A Furrow state the engine
    // cannot read is kept by the engine and given back on the next save, and playback stays
    // silent until a good state loads (see furrow::Engine::loadState).
    if (data == nullptr || sizeInBytes < static_cast<int> (stateHeader))
        return;
    const auto* bytes = static_cast<const std::uint8_t*> (data);
    if (std::memcmp (bytes, stateTag, sizeof (stateTag)) != 0 || bytes[4] != stateVersion || bytes[5] > 1)
        return;
    std::vector<std::int32_t> words;
    if (! furrow::state::fromBytes (std::span (bytes + stateHeader, static_cast<std::size_t> (sizeInBytes) - stateHeader), words))
        return;

    {
        const juce::ScopedLock lock (getCallbackLock());
        engine.loadState (words);
    }
    *tones = bytes[5] == 1;

    // A load can change parameter values, and a CLAP host must be told to read them again. The
    // CLAP wrapper turns a program change into that request (CLAP_PARAM_RESCAN_VALUES). VST3 needs
    // nothing extra: after a state load, JUCE's setComponentState re-reads every parameter.
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpikeProcessor();
}
