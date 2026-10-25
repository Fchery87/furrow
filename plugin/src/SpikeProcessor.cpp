#include "SpikeProcessor.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
// A tone falls by 60 dB over this time.
constexpr double toneDecaySeconds = 0.15;
constexpr float toneLevel = 0.25f;

// Room for every event the spike can send in one block. MidiBuffer keeps its capacity between
// blocks, so after the first block reserving this never allocates on the audio thread.
constexpr int midiReserveBytes = 4096;
} // namespace

SpikeProcessor::SpikeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (tones = new juce::AudioParameterBool ({ "tones", 1 }, "Tones", true));
}

void SpikeProcessor::prepareToPlay (double sampleRate, int)
{
    decayPerSample = static_cast<float> (std::pow (0.001, 1.0 / (toneDecaySeconds * sampleRate)));
    reset();
}

void SpikeProcessor::reset()
{
    pattern.reset();
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

    // Without a position, tempo and play state, the spike treats the transport as stopped.
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
    pattern.process (readTransport (numSamples), [&] (const furrow::MidiEvent& e) {
        midi.addEvent (juce::MidiMessage (e.status, e.note, e.velocity), e.offset);
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

// Saved state: a four-byte tag, a format version, then the Tones switch as 0 or 1.
namespace
{
constexpr char stateTag[4] = { 'F', 'S', 'P', 'K' };
constexpr std::uint8_t stateVersion = 1;
constexpr std::size_t stateSize = sizeof (stateTag) + 2;
} // namespace

void SpikeProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    destination.reset();
    juce::MemoryOutputStream out (destination, false);
    out.write (stateTag, sizeof (stateTag));
    out.writeByte (static_cast<char> (stateVersion));
    out.writeByte (tones->get() ? 1 : 0);
}

void SpikeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Anything that is not exactly this format is rejected and the current state is kept. JUCE
    // gives no way to report the failure to the host.
    if (data == nullptr || sizeInBytes != static_cast<int> (stateSize))
        return;
    const auto* bytes = static_cast<const std::uint8_t*> (data);
    if (std::memcmp (bytes, stateTag, sizeof (stateTag)) != 0 || bytes[4] != stateVersion || bytes[5] > 1)
        return;

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
