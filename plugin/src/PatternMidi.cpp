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
        || options.lengthSteps > maxSteps || options.ticksPerQuarter < 1 || ! std::isfinite (options.bpm)
        || options.bpm <= 0.0 || ! std::isfinite (options.sampleRate) || options.sampleRate <= 0.0)
        return juce::Result::fail ("Invalid MIDI export options");
    if (options.onlyLane >= static_cast<int> (channels.size()) || options.onlyLane < -1)
        return juce::Result::fail ("Selected MIDI export lane is out of range");
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
    const double totalBeats = options.lengthSteps * stepBeats;
    const auto totalSamples = std::max<std::int64_t> (1, std::llround (totalBeats * 60.0 / options.bpm * options.sampleRate));
    constexpr int block = 512;
    std::int64_t sampleCursor = 0;
    while (sampleCursor < totalSamples)
    {
        const int n = static_cast<int> (std::min<std::int64_t> (block, totalSamples - sampleCursor));
        const double blockBeat = static_cast<double> (sampleCursor) / options.sampleRate * options.bpm / 60.0;
        scratch.process ({ true, blockBeat, options.bpm, options.sampleRate, n },
                         [&] (const MidiEvent& event) {
                             const double beat = blockBeat + static_cast<double> (event.offset) / options.sampleRate * options.bpm / 60.0;
                             if (beat >= totalBeats - 1e-9 && event.status == 0x90 && event.data2 > 0)
                                 return;
                             if (event.lane >= 0 && event.lane < static_cast<int> (captured.size()))
                                 captured[static_cast<size_t> (event.lane)].emplace_back (std::min (beat, totalBeats), event);
                         });
        sampleCursor += n;
    }
    scratch.panic ([&] (const MidiEvent& event) {
        if (event.status == 0x80 && event.lane >= 0 && event.lane < static_cast<int> (captured.size()))
            captured[static_cast<size_t> (event.lane)].emplace_back (totalBeats, event);
    });

    const size_t firstLane = options.onlyLane >= 0 ? static_cast<size_t> (options.onlyLane) : 0;
    const size_t endLane = options.onlyLane >= 0 ? firstLane + 1 : channels.size();
    bool wroteTempo = false;
    for (size_t lane = firstLane; lane < endLane; ++lane)
    {
        juce::MidiMessageSequence sequence;
        if (! wroteTempo)
        {
            sequence.addEvent (juce::MidiMessage::tempoMetaEvent (static_cast<int> (std::lround (60000000.0 / options.bpm))), 0.0);
            wroteTempo = true;
        }
        const auto channel = channels[lane];
        for (const auto& [beat, event] : captured[lane])
        {
            const double tick = beat * options.ticksPerQuarter;
            auto message = juce::MidiMessage (static_cast<int> (event.status | (channel - 1)), event.data1, event.data2);
            sequence.addEvent (message, tick);
        }
        sequence.updateMatchedPairs();
        file.addTrack (sequence);
    }
    juce::MemoryOutputStream stream (output, false);
    if (! file.writeTo (stream)) return juce::Result::fail ("Could not write MIDI file");
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
    if (! file.readFrom (input)) { error (preview, "MIDI file is malformed"); return preview; }
    const auto division = file.getTimeFormat();
    if (division <= 0) { error (preview, "MIDI must use PPQ timing"); return preview; }

    struct TrackEvents
    {
        int sourceTrack = 0;
        int channel = 1;
        std::vector<std::pair<int, juce::MidiMessage>> events;
    };
    std::vector<TrackEvents> tracks;
    for (int t = 0; t < file.getNumTracks(); ++t)
    {
        const auto* seq = file.getTrack (t);
        std::map<int, TrackEvents> byChannel;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isMetaEvent() || msg.isEndOfTrackMetaEvent())
                continue;
            if (! msg.isNoteOnOrOff())
            {
                error (preview, "Unsupported MIDI event", t, i);
                continue;
            }
            auto& bucket = byChannel[msg.getChannel()];
            bucket.sourceTrack = t;
            bucket.channel = msg.getChannel();
            bucket.events.emplace_back (i, msg);
        }
        for (auto& [ch, bucket] : byChannel)
            tracks.push_back (std::move (bucket));
    }

    if (tracks.empty()) { error (preview, "MIDI file contains no note events"); return preview; }
    if (static_cast<int> (tracks.size()) > lanes) { error (preview, "MIDI exceeds the 64-lane limit"); return preview; }

    std::vector<Command> commands;
    int maxStep = 0;
    for (size_t laneIdx = 0; laneIdx < tracks.size(); ++laneIdx)
    {
        const auto& tr = tracks[laneIdx];
        int baseNote = -1;
        for (const auto& [idx, msg] : tr.events)
            if (msg.isNoteOn()) { baseNote = msg.getNoteNumber(); break; }
        if (baseNote < 0) baseNote = 60;
        preview.baseNotes.push_back (baseNote);
        preview.trackChannels.push_back (tr.channel);

        struct Held { int note; int step; double onBeat; int vel; int eventIdx; };
        std::optional<Held> held;
        std::array<bool, maxSteps> occupied {};
        for (const auto& [eventIdx, msg] : tr.events)
        {
            const double beat = msg.getTimeStamp() / static_cast<double> (division);
            if (msg.isNoteOn())
            {
                if (held.has_value()) { error (preview, "Overlapping notes are not representable", tr.sourceTrack, eventIdx); continue; }
                const int step = static_cast<int> (std::floor (beat / stepBeats + 1e-6));
                if (step < 0 || step >= maxSteps) { error (preview, "MIDI note exceeds the 512-step limit", tr.sourceTrack, eventIdx); continue; }
                if (occupied[static_cast<size_t> (step)]) { error (preview, "Multiple notes in one step are not representable", tr.sourceTrack, eventIdx); continue; }
                const int shift = static_cast<int> (std::llround ((beat / stepBeats - step) * 10000.0));
                if (shift < 0 || shift > fieldMax[Shift]) { error (preview, "Note timing shift exceeds 0.99 steps", tr.sourceTrack, eventIdx); continue; }
                const int pitch = msg.getNoteNumber() - baseNote;
                if (pitch < fieldMin[Pitch] || pitch > fieldMax[Pitch]) { error (preview, "Note pitch exceeds +/-24 semitones from lane base note", tr.sourceTrack, eventIdx); continue; }
                occupied[static_cast<size_t> (step)] = true;
                held = Held { msg.getNoteNumber(), step, beat, std::clamp (static_cast<int> (msg.getVelocity()), 1, 127), eventIdx };
                commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), step, On, 1 });
                commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), step, Velocity, held->vel });
                if (pitch != 0) commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), step, Pitch, pitch });
                if (shift != 0) commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), step, Shift, shift });
                maxStep = std::max (maxStep, step + 1);
            }
            else
            {
                if (! held.has_value() || held->note != msg.getNoteNumber())
                {
                    error (preview, "Note-off has no matching note-on", tr.sourceTrack, eventIdx);
                    continue;
                }
                const int gate = static_cast<int> (std::llround ((beat - held->onBeat) / stepBeats * 10000.0));
                if (gate < fieldMin[Gate] || gate > fieldMax[Gate])
                    error (preview, "Note gate is outside the 0.05 to 16 step range", tr.sourceTrack, eventIdx);
                else if (gate != fieldDefault[Gate])
                    commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), held->step, Gate, gate });
                const int relVel = static_cast<int> (msg.getVelocity());
                if (relVel != fieldDefault[Release])
                    commands.push_back ({ Op::SetStep, 0, static_cast<int> (laneIdx), held->step, Release, relVel });
                held.reset();
            }
        }
        if (held.has_value()) error (preview, "A note remains held at end of track", tr.sourceTrack, held->eventIdx);
    }

    preview.commands = std::move (commands);
    preview.lanes = static_cast<int> (tracks.size());
    preview.lengthSteps = std::max (1, maxStep);
    preview.accepted = preview.diagnostics.empty();
    if (! preview.accepted) preview.commands.clear();
    return preview;
}
} // namespace furrow::plugin
