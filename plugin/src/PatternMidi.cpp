#include "PatternMidi.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace furrow::plugin
{
namespace
{
void error (PatternMidiImportPreview& p, std::string text, int track = -1, int event = -1)
{
    p.diagnostics.push_back ({ PatternMidiDiagnostic::Severity::error, std::move (text), track, event });
}
}

juce::Result exportPatternMidi (const Engine& source, std::span<const int> channels,
                                const PatternMidiExportOptions& options, juce::MemoryBlock& output)
{
    if (channels.empty() || channels.size() > static_cast<size_t> (lanes) || options.lengthSteps < 1
        || options.lengthSteps > maxSteps || options.ticksPerQuarter < 1 || !std::isfinite (options.bpm)
        || options.bpm <= 0.0 || !std::isfinite (options.sampleRate) || options.sampleRate <= 0.0)
        return juce::Result::fail ("Invalid MIDI export options");
    for (const auto channel : channels)
        if (channel < 1 || channel > 16)
            return juce::Result::fail ("MIDI channels must be in the range 1 to 16");

    std::vector<std::int32_t> saved;
    source.saveState (saved);
    Engine scratch;
    if (scratch.loadState (saved) != LoadError::None)
        return juce::Result::fail ("Could not clone engine state");
    const Command origin { Op::QueuePattern, source.pattern(), 0, 0, 0, 0, 0.0 };
    if (scratch.apply (std::span<const Command> (&origin, 1), [] (const MidiEvent&) {}) != Result::Ok)
        return juce::Result::fail ("Could not queue pattern at origin");
    juce::MidiFile file;
    file.setTicksPerQuarterNote (options.ticksPerQuarter);
    std::vector<std::vector<std::pair<double, MidiEvent>>> captured (channels.size());
    const auto totalBeats = options.lengthSteps * stepBeats;
    constexpr int block = 512;
    const double blockBeats = block * 60.0 / options.bpm / options.sampleRate;
    for (double blockBeat = 0.0; blockBeat < totalBeats; blockBeat += blockBeats)
    {
        scratch.process ({ true, blockBeat, options.bpm, options.sampleRate, block },
                         [&] (const MidiEvent& event) {
                             if (event.lane >= 0 && event.lane < static_cast<int> (captured.size()))
                                 captured[static_cast<size_t> (event.lane)].emplace_back (blockBeat, event);
                         });
    }
    for (size_t lane = 0; lane < channels.size(); ++lane)
    {
        juce::MidiMessageSequence sequence;
        const auto channel = channels[lane];
        for (const auto& [blockBeat, event] : captured[lane])
        {
            const double beat = blockBeat + static_cast<double> (event.offset) / options.sampleRate * options.bpm / 60.0;
            const auto tick = static_cast<double> (beat * options.ticksPerQuarter);
            auto message = juce::MidiMessage (static_cast<int> (event.status | (channel - 1)), event.data1, event.data2);
            sequence.addEvent (message, tick);
        }
        sequence.updateMatchedPairs();
        file.addTrack (sequence);
    }
    juce::MemoryOutputStream stream (output, false);
    if (!file.writeTo (stream)) return juce::Result::fail ("Could not write MIDI file");
    return juce::Result::ok();
}

PatternMidiImportPreview previewPatternMidi (const void* data, size_t size, std::span<const int> channels)
{
    PatternMidiImportPreview preview;
    if (data == nullptr || size == 0) { error (preview, "MIDI data is empty"); return preview; }
    for (const auto channel : channels)
        if (channel < 1 || channel > 16) { error (preview, "MIDI channel is outside 1 to 16"); return preview; }
    juce::MemoryInputStream input (data, size, false);
    juce::MidiFile file;
    if (!file.readFrom (input)) { error (preview, "MIDI file is malformed"); return preview; }
    if (file.getNumTracks() != static_cast<int> (channels.size()))
    { error (preview, "MIDI must contain exactly one track per lane"); return preview; }
    const auto division = file.getTimeFormat();
    if (division <= 0) { error (preview, "MIDI must use PPQ timing"); return preview; }
    std::vector<Command> commands;
    int maxStep = 0;
    for (int track = 0; track < file.getNumTracks(); ++track)
    {
        const auto* seq = file.getTrack (track);
        std::map<int, double> held;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& event = *seq->getEventPointer (i);
            const auto message = event.message;
            if (!message.isNoteOnOrOff())
            { if (!message.isMetaEvent() && !message.isEndOfTrackMetaEvent()) error (preview, "Unsupported MIDI event", track, i); continue; }
            if (message.getChannel() != channels[static_cast<size_t> (track)])
            { error (preview, "MIDI event uses the wrong channel", track, i); continue; }
            const auto tick = event.message.getTimeStamp();
            const int step = static_cast<int> (std::llround (tick / division / stepBeats));
            if (step < 0 || step >= maxSteps) { error (preview, "MIDI note exceeds the 512-step limit", track, i); continue; }
            if (message.isNoteOn())
            {
                if (held.contains (message.getNoteNumber())) { error (preview, "Overlapping notes are not representable", track, i); continue; }
                held[message.getNoteNumber()] = tick;
                commands.push_back ({ Op::SetStep, 0, track, step, On, 1 });
                commands.push_back ({ Op::SetLane, track, Note, message.getNoteNumber(), 0 });
                commands.push_back ({ Op::SetStep, 0, track, step, Velocity, message.getVelocity() });
                maxStep = std::max (maxStep, step + 1);
            }
            else if (!held.erase (message.getNoteNumber())) error (preview, "Note-off has no matching note-on", track, i);
        }
        if (!held.empty()) error (preview, "A note remains held at end of track", track);
    }
    preview.commands = std::move (commands);
    preview.lanes = static_cast<int> (channels.size());
    preview.lengthSteps = std::max (1, maxStep);
    preview.accepted = preview.diagnostics.empty();
    if (!preview.accepted) preview.commands.clear();
    return preview;
}
} // namespace furrow::plugin
