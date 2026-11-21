#pragma once

#include "furrow/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace furrow::plugin
{

struct PatternMidiExportOptions
{
    double bpm = 120.0;
    double sampleRate = 44100.0;
    int lengthSteps = 16;
    int ticksPerQuarter = 960;
    int onlyLane = -1;
};

struct PatternMidiDiagnostic
{
    enum class Severity { warning, error };
    Severity severity = Severity::error;
    std::string message;
    int track = -1;
    int event = -1;
};

struct PatternMidiImportPreview
{
    std::vector<Command> commands;
    std::vector<int> baseNotes;
    std::vector<int> trackChannels;
    int lanes = 0;
    int lengthSteps = 0;
    std::vector<PatternMidiDiagnostic> diagnostics;
    bool accepted = false;
};

// Renders the current engine pattern from beat zero into a type-1 MIDI file. The engine is
// restored after rendering; rendering never changes the caller's engine state.
juce::Result exportPatternMidi (const Engine& source, std::span<const int> channels,
                               const PatternMidiExportOptions& options, juce::MemoryBlock& output);

// Parses a MIDI file without changing an engine. Only note events on one track per lane are
// representable. Refusals are returned as diagnostics and never produce a partially valid preview.
PatternMidiImportPreview previewPatternMidi (const void* data, size_t size,
                                             std::span<const int> channels);

} // namespace furrow::plugin
