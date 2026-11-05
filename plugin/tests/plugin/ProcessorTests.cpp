#include "SpikeProcessor.h"
#include "furrow/PluginState.h"
#include "furrow/sampler/Curves.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_cryptography/juce_cryptography.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// The plugin as a host drives it: transport in, audio and MIDI out, state saved and restored.
// The default rack plays probes/pattern-4x16.txt, so at 120 BPM and 44.1 kHz its steps fall on
// multiples of 5,512.5 samples.

using namespace furrow;
using namespace furrow::sampler;
using Catch::Approx;

namespace
{
constexpr double rate = 44100.0;
constexpr int block = 512;

struct Transport final : juce::AudioPlayHead
{
    bool playing = true;
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setBpm (120.0);
        p.setPpqPosition (ppq);
        return p;
    }
};

struct Run
{
    std::vector<std::vector<float>> channels;   // every output channel, main bus first
    std::vector<std::pair<int, juce::MidiMessage>> midi;
    float main (int frame) const { return channels[0][static_cast<std::size_t> (frame)]; }
};

void prepare (juce::AudioProcessor& p)
{
    p.setRateAndBufferSizeDetails (rate, block);
    p.prepareToPlay (rate, block);
}

// Plays from beat 0 for the given number of samples.
Run play (juce::AudioProcessor& p, int samples)
{
    Transport transport;
    p.setPlayHead (&transport);
    Run run;
    const int channels = p.getTotalNumOutputChannels();
    run.channels.assign (static_cast<std::size_t> (channels), std::vector<float> (static_cast<std::size_t> (samples), 0.0f));
    juce::AudioBuffer<float> buffer (channels, block);
    juce::MidiBuffer midi;
    for (int at = 0; at < samples; at += block)
    {
        const int n = std::min (block, samples - at);
        buffer.setSize (channels, n, false, false, true);
        midi.clear();
        transport.ppq = at / rate * 2.0;
        p.processBlock (buffer, midi);
        for (int c = 0; c < channels; ++c)
            std::copy_n (buffer.getReadPointer (c), n, run.channels[static_cast<std::size_t> (c)].begin() + at);
        for (const auto m : midi)
            run.midi.emplace_back (at + m.samplePosition, m.getMessage());
    }
    p.setPlayHead (nullptr);
    return run;
}

std::vector<std::uint8_t> save (juce::AudioProcessor& p)
{
    juce::MemoryBlock memory;
    p.getStateInformation (memory);
    const auto* data = static_cast<const std::uint8_t*> (memory.getData());
    return { data, data + memory.getSize() };
}

void load (juce::AudioProcessor& p, const std::vector<std::uint8_t>& bytes)
{
    p.setStateInformation (bytes.data(), static_cast<int> (bytes.size()));
}

PluginState saved (juce::AudioProcessor& p)
{
    PluginState state;
    REQUIRE (decodePluginState (save (p), state));
    return state;
}

std::vector<std::uint8_t> encode (const PluginState& state)
{
    std::vector<std::uint8_t> bytes;
    encodePluginState (state.words, state.setups, bytes);
    return bytes;
}

// Waits until the lane's saved setup shows a loaded file's hash.
bool waitForHash (juce::AudioProcessor& p, int slot)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& s : saved (p).setups)
            if (s.slot == slot && s.sample.hash != std::array<std::uint8_t, 32> {})
                return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return false;
}

bool silent (const Run& run, int from, int to)
{
    for (int f = from; f < to; ++f)
        if (run.main (f) != 0.0f)
            return false;
    return true;
}

struct TempFile
{
    juce::File file = juce::File::createTempFile (".wav");
    ~TempFile() { file.deleteFile(); }
};

// Writes a 32-bit float mono WAV, so its frames come back exactly.
void writeWav (const juce::File& file, const std::vector<float>& frames)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    REQUIRE (static_cast<juce::FileOutputStream*> (stream.get())->openedOk());
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* channels[] = { frames.data() };
    REQUIRE (writer->writeFromFloatArrays (channels, 1, static_cast<int> (frames.size())));
}
} // namespace

TEST_CASE ("A new instance plays the built-in kit, starting each hit on its sample")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    prepare (p);
    const auto run = play (p, 2 * static_cast<int> (rate));

    // Step 0: kick and closed hat at sample 0. Both are cut by the default half-step gate at
    // 5,512 samples and a 1 ms release, so the main output is exactly silent until step 2.
    CHECK (run.main (0) != 0.0f);
    CHECK (silent (run, 5600, 11025));
    CHECK (run.main (11025) != 0.0f);
    CHECK (silent (run, 11025 + 5600, 22050));
    CHECK (run.main (22050) != 0.0f);

    // Sample lanes play inside the plugin, so no notes go out as MIDI.
    for (const auto& [at, m] : run.midi)
        CHECK_FALSE (m.isNoteOn());
}

TEST_CASE ("A MIDI lane sends its notes on its channel instead of playing a sample")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto state = saved (p);
    REQUIRE (state.setups.size() == 4);
    state.setups[0].settings.target = Target::Midi;
    state.setups[0].settings.channel = 10;
    load (p, encode (state));
    prepare (p);
    const auto run = play (p, block);

    int kicks = 0;
    for (const auto& [at, m] : run.midi)
        if (m.isNoteOn())
        {
            CHECK (at == 0);
            CHECK (m.getChannel() == 10);
            CHECK (m.getNoteNumber() == 36);
            ++kicks;
        }
    CHECK (kicks == 1);
}

TEST_CASE ("Saving and reloading restores the rack byte for byte")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor a;
    auto state = saved (a);
    state.setups[1].settings.tune = 300;
    state.setups[1].settings.output = 4;
    state.setups[2].settings.decay = 80.0;
    load (a, encode (state));
    const auto bytes = save (a);

    SpikeProcessor b;
    load (b, bytes);
    CHECK (save (b) == bytes);
}

TEST_CASE ("A sample file loads in the background and plays from its first frame")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TempFile wav;
    std::vector<float> frames (4410);
    for (std::size_t i = 0; i < frames.size(); ++i)
        frames[i] = 0.5f * static_cast<float> (i % 100) / 100.0f;
    writeWav (wav.file, frames);

    SpikeProcessor p;
    auto state = saved (p);
    auto& kick = state.setups[0];
    kick.sample.kind = SampleRef::Kind::File;
    kick.sample.path = wav.file.getFullPathName().toStdString();
    kick.settings.attack = 0.0;
    // Only the kick lane, so nothing else sounds at sample 0.
    state.setups[2].settings.target = Target::Midi;
    load (p, encode (state));
    REQUIRE (waitForHash (p, 0));

    // The saved hash is the file's SHA-256.
    const auto digest = juce::SHA256 (wav.file).getRawData();
    for (const auto& s : saved (p).setups)
        if (s.slot == 0)
            CHECK (std::equal (s.sample.hash.begin(), s.sample.hash.end(), static_cast<const std::uint8_t*> (digest.getData())));

    prepare (p);
    const auto run = play (p, 4096);
    // The engine sends the lane's controllers before each hit: CC 7 = 64 for volume 5,000, and
    // CC 10 = 64 for centre pan. The step's velocity is the default 100.
    const double gain = velocityGain (100) * volumeGain (64) * panGains (64).left;
    // The default gate is half a step, 2,756.25 samples, so the note-off lands on frame 2,756.
    for (int k = 0; k < 2756; ++k)
    {
        INFO ("frame " << k);
        REQUIRE (run.main (k) == Approx (frames[static_cast<std::size_t> (k)] * gain).margin (1e-6));
    }
    // Then the 1 ms release, 44 frames, and silence.
    CHECK (std::abs (run.main (2756)) < std::abs (frames[2756] * gain));
    CHECK (silent (run, 2756 + 44, 4096));
}

TEST_CASE ("A missing sample file keeps its lane and its reference")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto state = saved (p);
    state.setups[0].sample.kind = SampleRef::Kind::File;
    state.setups[0].sample.path = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("furrow-no-such-file.wav").getFullPathName().toStdString();
    state.setups[0].sample.hash.fill (7);
    state.setups[2].settings.target = Target::Midi;
    std::vector<std::uint8_t> bytes;
    SECTION ("legacy state migrates without losing the missing reference")
    {
        bytes = encode (state);
    }
    SECTION ("current state round-trips without losing the missing reference")
    {
        encodePluginState (state.words, state.setups, state.metadata, bytes);
    }
    load (p, bytes);
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    const auto restored = saved (p);
    CHECK (restored.version == pluginMetadataVersion);
    CHECK (restored.words == state.words);
    CHECK (restored.setups == state.setups);
    CHECK (restored.metadata == state.metadata);
    std::vector<std::uint8_t> currentBytes;
    encodePluginState (state.words, state.setups, state.metadata, currentBytes);
    CHECK (save (p) == currentBytes);

    prepare (p);
    const auto run = play (p, block);
    CHECK (silent (run, 0, block));
}

TEST_CASE ("A lane plays on its own output bus when the host turns that bus on")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto layout = p.getBusesLayout();
    REQUIRE (layout.outputBuses.size() == outputs);
    layout.outputBuses.getReference (1) = juce::AudioChannelSet::stereo();
    REQUIRE (p.setBusesLayout (layout));
    auto state = saved (p);
    state.setups[0].settings.output = 2;
    load (p, encode (state));
    prepare (p);
    const auto run = play (p, 11025 + block);

    REQUIRE (run.channels.size() == 4);
    // The kick plays on Out 2 only; the closed hat at the same time stays on the main bus.
    CHECK (run.channels[2][0] != 0.0f);
    CHECK (run.channels[3][0] != 0.0f);
    CHECK (run.main (0) != 0.0f);
    // At step 2 only the snare and the hat play, so Out 2 is silent there.
    CHECK (run.channels[2][11025] == 0.0f);
    CHECK (run.main (11025) != 0.0f);
}

TEST_CASE ("A version 1 state from the development builds loads with every lane as a MIDI lane")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    const auto words = saved (p).words;
    std::vector<std::uint8_t> engineBytes;
    state::toBytes (words, engineBytes);
    std::vector<std::uint8_t> v1 { 'F', 'R', 'W', 'S', 1, 1 };
    v1.insert (v1.end(), engineBytes.begin(), engineBytes.end());
    load (p, v1);
    prepare (p);
    const auto run = play (p, block);

    std::vector<std::pair<int, int>> notes;
    for (const auto& [at, m] : run.midi)
        if (m.isNoteOn())
            notes.emplace_back (m.getChannel(), m.getNoteNumber());
    CHECK (notes == std::vector<std::pair<int, int>> { { 1, 36 }, { 3, 42 } });
    CHECK (silent (run, 0, block));
}

TEST_CASE ("Bypass releases a held MIDI note on its original channel")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto state = saved (p);
    state.setups[0].settings.target = Target::Midi;
    state.setups[0].settings.channel = 10;
    load (p, encode (state));
    prepare (p);

    Transport transport;
    p.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (p.getTotalNumOutputChannels(), block);
    juce::MidiBuffer midi;
    p.processBlock (audio, midi);
    REQUIRE (std::any_of (midi.begin(), midi.end(), [] (const auto metadata) {
        return metadata.getMessage().isNoteOn() && metadata.getMessage().getChannel() == 10;
    }));

    midi.clear();
    p.processBlockBypassed (audio, midi);
    CHECK (std::any_of (midi.begin(), midi.end(), [] (const auto metadata) {
        const auto message = metadata.getMessage();
        return message.isNoteOff() && message.getNoteNumber() == 36 && message.getChannel() == 10;
    }));
    p.setPlayHead (nullptr);
}

TEST_CASE ("State replacement releases a held MIDI note through its old channel")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto state = saved (p);
    state.setups[0].settings.target = Target::Midi;
    state.setups[0].settings.channel = 10;
    load (p, encode (state));
    prepare (p);

    Transport transport;
    p.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (p.getTotalNumOutputChannels(), block);
    juce::MidiBuffer midi;
    p.processBlock (audio, midi);
    REQUIRE (std::any_of (midi.begin(), midi.end(), [] (const auto metadata) {
        return metadata.getMessage().isNoteOn() && metadata.getMessage().getChannel() == 10;
    }));

    state.setups[0].settings.channel = 11;
    load (p, encode (state));
    midi.clear();
    transport.ppq = block / rate * 2.0;
    p.processBlock (audio, midi);

    int oldChannelReleases = 0;
    int newChannelReleases = 0;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (! message.isNoteOff() || message.getNoteNumber() != 36)
            continue;
        oldChannelReleases += message.getChannel() == 10;
        newChannelReleases += message.getChannel() == 11;
    }
    CHECK (oldChannelReleases == 1);
    CHECK (newChannelReleases == 0);
    p.setPlayHead (nullptr);
}

TEST_CASE ("An unreadable Furrow state is preserved and silences playback")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto rejected = save (p);
    rejected[4] = 99;
    load (p, rejected);
    CHECK (save (p) == rejected);

    prepare (p);
    const auto run = play (p, block);
    CHECK (silent (run, 0, block));
}

TEST_CASE ("A good state recovers from an unreadable Furrow container")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    const auto good = save (p);
    for (const std::size_t length : { 4u, 5u, 8u })
    {
        const std::vector<std::uint8_t> truncated (good.begin(), good.begin() + static_cast<std::ptrdiff_t> (length));
        load (p, truncated);
        CHECK (save (p) == truncated);
        prepare (p);
        CHECK (silent (play (p, block), 0, block));
        load (p, good);
        CHECK (save (p) == good);
        prepare (p);
        CHECK_FALSE (silent (play (p, block), 0, block));
    }
}

TEST_CASE ("Unrelated state bytes do not erase a valid rack or its load error")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    const auto good = save (p);
    const std::vector<std::uint8_t> unrelated { 'N', 'O', 'P', 'E', 1, 2, 3 };
    load (p, unrelated);
    CHECK (save (p) == good);
    auto bad = good;
    bad[4] = 99;
    load (p, bad);
    load (p, unrelated);
    CHECK (save (p) == bad);
}

TEST_CASE ("Changing a playing MIDI lane into a sample lane releases the MIDI route")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor p;
    auto state = saved (p);
    state.setups[0].settings.target = Target::Midi;
    state.setups[0].settings.channel = 10;
    load (p, encode (state));
    prepare (p);
    Transport transport;
    p.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (p.getTotalNumOutputChannels(), block);
    juce::MidiBuffer midi;
    p.processBlock (audio, midi);
    state.setups[0].settings.target = Target::Sample;
    load (p, encode (state));
    midi.clear();
    p.processBlockBypassed (audio, midi);
    CHECK (std::any_of (midi.begin(), midi.end(), [] (const auto metadata) {
        const auto message = metadata.getMessage();
        return message.isNoteOff() && message.getNoteNumber() == 36 && message.getChannel() == 10;
    }));
    p.setPlayHead (nullptr);
}
