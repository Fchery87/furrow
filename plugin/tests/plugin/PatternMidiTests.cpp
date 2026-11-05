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
