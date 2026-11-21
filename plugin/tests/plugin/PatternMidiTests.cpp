#include "../../src/PatternMidi.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace furrow;
using namespace furrow::plugin;

TEST_CASE ("MIDI preview rejects empty input without commands")
{
    const std::array<int, 1> channels { 1 };
    const auto preview = previewPatternMidi (nullptr, 0, channels);
    CHECK (! preview.accepted);
    CHECK (preview.commands.empty());
    REQUIRE (! preview.diagnostics.empty());
    CHECK (preview.diagnostics.front().severity == PatternMidiDiagnostic::Severity::error);
}

TEST_CASE ("MIDI export validates channels")
{
    Engine engine;
    juce::MemoryBlock output;
    const std::array<int, 1> channels { 17 };
    CHECK (exportPatternMidi (engine, channels, {}, output).failed());
    CHECK (output.isEmpty());
}

TEST_CASE ("MIDI export accepts a bounded capture window")
{
    Engine engine;
    juce::MemoryBlock output;
    PatternMidiExportOptions options;
    options.lengthSteps = 1;
    options.sampleRate = 48000.0;
    options.bpm = 97.0;
    const std::array<int, 1> channels { 2 };
    REQUIRE (exportPatternMidi (engine, channels, options, output).wasOk());
    CHECK (! output.isEmpty());
    juce::MemoryInputStream input (output.getData(), output.getSize(), false);
    juce::MidiFile file;
    REQUIRE (file.readFrom (input));
    CHECK (file.getNumTracks() == 1);
}

TEST_CASE ("MIDI preview refuses a malformed file atomically")
{
    const std::array<std::uint8_t, 4> bytes { 0, 1, 2, 3 };
    const std::array<int, 1> channels { 1 };
    const auto preview = previewPatternMidi (bytes.data(), bytes.size(), channels);
    CHECK (! preview.accepted);
    CHECK (preview.commands.empty());
}

TEST_CASE ("MIDI export supports single-lane output and bounded length, and preview captures shift, gate, and pitch")
{
    Engine engine;
    const std::array<Command, 4> init {
        Command { Op::AddLane, 0, 1, 1, 36, 0 },
        Command { Op::AddLane, 1, 2, 2, 38, 1 },
        Command { Op::SetStep, 0, 0, 0, On, 1 },
        Command { Op::SetStep, 0, 1, 2, On, 1 },
    };
    REQUIRE (engine.apply (init, [] (const MidiEvent&) {}) == Result::Ok);

    juce::MemoryBlock single;
    PatternMidiExportOptions opts;
    opts.lengthSteps = 4;
    opts.onlyLane = 1;
    const std::array<int, 2> channels { 1, 2 };
    REQUIRE (exportPatternMidi (engine, channels, opts, single).wasOk());

    juce::MemoryInputStream in (single.getData(), single.getSize(), false);
    juce::MidiFile mf;
    REQUIRE (mf.readFrom (in));
    CHECK (mf.getNumTracks() == 1);

    // Preview accepts a 1-track MIDI even when the caller passes 2 channels, and captures shift/gate/pitch.
    juce::MidiFile custom;
    custom.setTicksPerQuarterNote (960);
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::noteOn (3, 60, juce::uint8 (110)), 240.0 + 120.0); // step 1, shift 0.5 (5000)
    seq.addEvent (juce::MidiMessage::noteOff (3, 60, juce::uint8 (50)), 240.0 + 120.0 + 360.0); // gate 1.5 steps (15000)
    seq.addEvent (juce::MidiMessage::noteOn (3, 67, juce::uint8 (90)), 960.0); // step 4, pitch +7
    seq.addEvent (juce::MidiMessage::noteOff (3, 67, juce::uint8 (64)), 1080.0); // gate 0.5 steps
    custom.addTrack (seq);
    juce::MemoryBlock customBytes;
    juce::MemoryOutputStream out (customBytes, false);
    REQUIRE (custom.writeTo (out));

    const auto preview = previewPatternMidi (customBytes.getData(), customBytes.getSize(), channels);
    REQUIRE (preview.accepted);
    CHECK (preview.lanes == 1);
    CHECK (preview.lengthSteps == 5);
    REQUIRE (preview.baseNotes.size() == 1);
    CHECK (preview.baseNotes[0] == 60);
}
