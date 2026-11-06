#include "SpikeProcessor.h"
#include "furrow/KitState.h"

#include <algorithm>

namespace
{
using namespace furrow;
using namespace furrow::sampler;

bool integer (const juce::var& value) { return value.isInt() || value.isInt64(); }
const juce::var& field (const juce::var& value, const char* name)
{
    static const juce::var empty;
    return value.isObject() && value.hasProperty (name) ? value[name] : empty;
}
int bounded (const juce::var& value, int low, int high)
{
    if (! integer (value)) return low - 1;
    const auto result = static_cast<juce::int64> (value);
    return result >= low && result <= high ? static_cast<int> (result) : low - 1;
}
}

juce::var SpikeProcessor::editorState()
{
    const std::lock_guard lock (modelLock);
    auto* root = new juce::DynamicObject();
    root->setProperty ("pattern", selectedPattern.load());
    root->setProperty ("playing", hostPlaying.load());
    root->setProperty ("bpm", hostBpm.load());
    root->setProperty ("beat", hostBeat.load());
    root->setProperty ("error", lastError);
    root->setProperty ("canUndo", ! undoStates.empty());
    root->setProperty ("canRedo", ! redoStates.empty());
    root->setProperty ("width", windowWidth);
    root->setProperty ("height", windowHeight);
    root->setProperty ("midiPreview", midiPreviewState);
    juce::Array<juce::var> patterns;
    for (const auto& name : patternNames) patterns.add (name);
    root->setProperty ("patterns", patterns);
    juce::Array<juce::var> lanes;
    for (int s = 0; s < furrow::lanes; ++s)
    {
        const auto index = static_cast<std::size_t> (s);
        if (! modelEngine->laneField (s, Active)) continue;
        auto* lane = new juce::DynamicObject();
        lane->setProperty ("slot", s);
        lane->setProperty ("name", laneNames[index]);
        lane->setProperty ("color", laneColors[index]);
        lane->setProperty ("length", modelEngine->store().length (selectedPattern.load(), s));
        lane->setProperty ("note", modelEngine->laneField (s, Note));
        lane->setProperty ("mute", modelEngine->laneField (s, Mute) != 0);
        lane->setProperty ("solo", modelEngine->laneField (s, Solo) != 0);
        lane->setProperty ("volume", modelEngine->laneField (s, Volume));
        lane->setProperty ("pan", modelEngine->laneField (s, LanePan));
        lane->setProperty ("swing", modelEngine->laneField (s, Swing));
        lane->setProperty ("target", model[index].setup.settings.target == Target::Sample ? "sample" : "midi");
        lane->setProperty ("channel", model[index].setup.settings.channel);
        lane->setProperty ("output", model[index].setup.settings.output);
        lane->setProperty ("start", model[index].setup.settings.start);
        lane->setProperty ("end", model[index].setup.settings.end);
        lane->setProperty ("tune", model[index].setup.settings.tune);
        lane->setProperty ("attack", model[index].setup.settings.attack);
        lane->setProperty ("decay", model[index].setup.settings.decay);
        lane->setProperty ("release", model[index].setup.settings.release);
        lane->setProperty ("choke", model[index].setup.settings.choke);
        lane->setProperty ("samplePath", juce::String::fromUTF8 (model[index].setup.sample.path.c_str()));
        lane->setProperty ("sampleError", model[index].error);
        juce::Array<juce::var> steps;
        const auto length = modelEngine->store().length (selectedPattern.load(), s);
        for (int step = 0; step < length; ++step)
        {
            juce::Array<juce::var> values;
            for (int f = 0; f < stepFields; ++f) values.add (modelEngine->store().get (selectedPattern.load(), s, step, f));
            steps.add (values);
        }
        lane->setProperty ("steps", steps);
        lanes.add (lane);
    }
    root->setProperty ("lanes", lanes);
    return juce::var (root);
}

juce::Result SpikeProcessor::edit (const juce::var& command)
{
    const std::lock_guard lock (modelLock);
    const auto result = editValidated (command);
    lastError = result.failed() ? result.getErrorMessage() : juce::String();
    if (result.wasOk()) updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    return result;
}

juce::Result SpikeProcessor::editValidated (const juce::var& command)
{
    const auto action = field (command, "action").toString();
    const int pattern = selectedPattern.load();
    const int lane = bounded (field (command, "lane"), 0, furrow::lanes - 1);
    if (blocked && action != "panic") return juce::Result::fail ("Load a readable state before editing.");
    if (action == "kitSave" || action == "kitLoad" || action == "midiExport" || action == "midiPreview" || action == "midiImport")
        return editFile (command);
    if (action == "undo" || action == "redo")
    {
        const std::lock_guard lock (modelLock);
        return restoreHistory (action == "redo");
    }
    if (action == "panic" || action == "audition")
    {
        if (action == "audition" && (lane < 0 || ! modelEngine->laneField (lane, Active)))
            return juce::Result::fail ("Lane is invalid.");
        const std::lock_guard lock (modelLock);
        if (outstandingEdits >= 128) return juce::Result::fail ("The edit queue is full.");
        auto batch = std::make_unique<EditBatch>();
        batch->generation = generation;
        batch->panic = action == "panic";
        batch->audition = action == "audition" ? lane : -1;
        if (! edits.push (batch.get())) return juce::Result::fail ("The edit queue is full.");
        batch.release();
        ++outstandingEdits;
        return juce::Result::ok();
    }
    if (action == "renameLane" || action == "colorLane")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        const auto value = field (command, action == "renameLane" ? "name" : "color").toString();
        if (! field (command, action == "renameLane" ? "name" : "color").isString()
            || value.isEmpty() || value.length() > 128 || ! furrow::sampler::detail::validUtf8 (value.toStdString()))
            return juce::Result::fail ("Value is invalid.");
        if (action == "colorLane" && (value.length() != 7 || ! value.startsWithChar ('#')
            || ! value.substring (1).containsOnly ("0123456789abcdefABCDEF")))
            return juce::Result::fail ("Use a six-digit hexadecimal color.");
        const std::lock_guard lock (modelLock);
        remember();
        if (action == "renameLane") laneNames[static_cast<std::size_t> (lane)] = value;
        else laneColors[static_cast<std::size_t> (lane)] = value;
        return juce::Result::ok();
    }
    if (action == "renamePattern")
    {
        const auto value = field (command, "name").toString();
        if (! field (command, "name").isString() || value.isEmpty() || value.length() > 128
            || ! furrow::sampler::detail::validUtf8 (value.toStdString())) return juce::Result::fail ("Pattern name is invalid.");
        const std::lock_guard lock (modelLock);
        remember();
        patternNames[static_cast<std::size_t> (pattern)] = value;
        return juce::Result::ok();
    }
    if (action == "pattern")
    {
        const int next = bounded (field (command, "value"), 0, furrow::patterns - 1);
        if (next < 0) return juce::Result::fail ("Pattern is invalid.");
        const Command selection { Op::SetRack, CurrentPattern, next, 0, 0, 0 };
        const auto result = applyCommands (std::span (&selection, 1));
        if (result.wasOk()) selectedPattern = next;
        return result;
    }
    if (action == "setLength")
    {
        const int length = bounded (field (command, "value"), 1, maxSteps);
        if (lane < 0 || length < 0) return juce::Result::fail ("Length is invalid.");
        const Command commandValue { Op::SetLength, pattern, lane, length, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "setStep")
    {
        const int step = bounded (field (command, "step"), 0, maxSteps - 1);
        const int fieldValue = bounded (field (command, "field"), 0, stepFields - 1);
        if (lane < 0 || step < 0 || fieldValue < 0) return juce::Result::fail ("Step edit is invalid.");
        const auto value = field (command, "value");
        const int actual = bounded (value, fieldMin[static_cast<std::size_t> (fieldValue)], fieldMax[static_cast<std::size_t> (fieldValue)]);
        if (actual < fieldMin[static_cast<std::size_t> (fieldValue)]) return juce::Result::fail ("Step value is invalid.");
        const Command commandValue { Op::SetStep, pattern, lane, step, fieldValue, actual };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "setLane")
    {
        const int fieldValue = bounded (field (command, "field"), ProbabilityKey, ModBCc);
        if (lane < 0 || fieldValue < ProbabilityKey) return juce::Result::fail ("Lane edit is invalid.");
        const int actual = bounded (field (command, "value"), laneMin[static_cast<std::size_t> (fieldValue)], laneMax[static_cast<std::size_t> (fieldValue)]);
        if (actual < laneMin[static_cast<std::size_t> (fieldValue)]) return juce::Result::fail ("Lane value is invalid.");
        const Command commandValue { Op::SetLane, lane, fieldValue, actual, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "setRack")
    {
        const int fieldValue = bounded (field (command, "field"), RackId, Truncate);
        const int actual = fieldValue == Seed ? bounded (field (command, "value"), 0, idMax) : bounded (field (command, "value"), 0, idMax);
        if (fieldValue < 0 || actual < 0) return juce::Result::fail ("Rack value is invalid.");
        const Command commandValue { Op::SetRack, fieldValue, actual, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "addLane")
    {
        if (command.hasProperty ("lane") && lane < 0) return juce::Result::fail ("Lane is invalid.");
        int slot = lane;
        if (slot < 0) for (slot = 0; slot < furrow::lanes && modelEngine->laneField (slot, Active); ++slot) {}
        if (slot >= furrow::lanes) return juce::Result::fail ("There is no free lane.");
        const int id = modelEngine->laneField (slot, Active) ? 0 : slot + 1;
        const int note = bounded (field (command, "note"), 0, 127);
        if (note < 0) return juce::Result::fail ("Note is invalid.");
        const Command commandValue { Op::AddLane, slot, id, 0, note, slot };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "removeLane")
    {
        if (lane < 0) return juce::Result::fail ("Lane is invalid.");
        const Command commandValue { Op::RemoveLane, lane, 0, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "clearPattern")
    {
        const Command commandValue { Op::ClearPattern, pattern, 0, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "clearLane")
    {
        const Command commandValue { Op::ClearPatternLane, pattern, lane, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "loadSample" || action == "retrySample")
    {
        const auto path = field (command, "path").toString();
        if (lane < 0 || ! juce::File::isAbsolutePath (path) || path.getNumBytesAsUTF8() > pathMax)
            return juce::Result::fail ("Sample path must be absolute and at most 4096 bytes.");
        const auto extension = juce::File (path).getFileExtension().toLowerCase();
        if (! juce::StringArray { ".wav", ".aif", ".aiff", ".flac" }.contains (extension)) return juce::Result::fail ("Use a WAV, AIFF or FLAC sample.");
        const std::lock_guard lock (modelLock);
        if (! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is inactive.");
        remember();
        model[static_cast<std::size_t> (lane)].setup.settings.target = Target::Sample;
        model[static_cast<std::size_t> (lane)].settingsDirty = true;
        routeChanged = true;
        model[static_cast<std::size_t> (lane)].setup.sample.hash.fill (0);
        model[static_cast<std::size_t> (lane)].setup.sample.kind = SampleRef::Kind::File;
        model[static_cast<std::size_t> (lane)].setup.sample.path = path.toStdString();
        requestSample (lane, model[static_cast<std::size_t> (lane)], model[static_cast<std::size_t> (lane)].setup.sample);
        sync();
        return juce::Result::ok();
    }
    if (action == "resize")
    {
        const int width = bounded (field (command, "width"), 720, 4096);
        const int height = bounded (field (command, "height"), 420, 2160);
        if (width < 720 || height < 420) return juce::Result::fail ("Window size is invalid.");
        if (width != windowWidth || height != windowHeight) remember();
        windowWidth = width;
        windowHeight = height;
        return juce::Result::ok();
    }
    if (action == "setSampler")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        auto setups = laneSetups();
        auto found = std::find_if (setups.begin(), setups.end(), [lane] (const auto& s) { return s.slot == lane; });
        if (found == setups.end()) return juce::Result::fail ("Lane setup is missing.");
        auto& settings = found->settings;
        const auto name = field (command, "field").toString();
        const auto value = field (command, "value");
        if (name == "target")
        {
            if (value != juce::var ("midi") && value != juce::var ("sample")) return juce::Result::fail ("Target is invalid.");
            settings.target = value.toString() == "sample" ? Target::Sample : Target::Midi;
        }
        else if (name == "channel") settings.channel = bounded (value, 1, 16);
        else if (name == "output") settings.output = bounded (value, 1, outputs);
        else if (name == "tune") settings.tune = bounded (value, -tuneMax, tuneMax);
        else if (name == "choke") settings.choke = bounded (value, 0, chokeGroups);
        else
        {
            if (! value.isDouble() && ! integer (value)) return juce::Result::fail ("Setting must be numeric.");
            const auto number = static_cast<double> (value);
            if (name == "start") settings.start = number;
            else if (name == "end") settings.end = number;
            else if (name == "attack") settings.attack = number;
            else if (name == "decay") settings.decay = number;
            else if (name == "release") settings.release = number;
            else return juce::Result::fail ("Unknown sampler setting.");
        }
        if (! valid (settings)) return juce::Result::fail ("Sampler setting is out of range.");
        remember();
        routeChanged = true;
        applySetups (setups, laneIds());
        return juce::Result::ok();
    }
    return juce::Result::fail ("Unknown edit.");
}

juce::Result SpikeProcessor::editFile (const juce::var& command)
{
    const auto action = field (command, "action").toString();
    if (action == "midiImport")
    {
        if (! midiPreview.accepted) return juce::Result::fail ("Preview a compatible MIDI file first.");
        if (laneSetups() != midiPreviewSetups) return juce::Result::fail ("The lanes changed. Preview the MIDI file again.");
        std::vector<Command> commands { { Op::ClearPattern, selectedPattern.load(), 0, 0, 0, 0 } };
        for (const int slot : midiPreviewSlots)
            commands.push_back ({ Op::SetLength, selectedPattern.load(), slot, midiPreview.lengthSteps, 0, 0 });
        for (auto imported : midiPreview.commands)
        {
            if (imported.op == Op::SetStep)
            {
                imported.a = selectedPattern.load();
                imported.b = midiPreviewSlots[static_cast<std::size_t> (imported.b)];
            }
            else if (imported.op == Op::SetLane)
                imported.a = midiPreviewSlots[static_cast<std::size_t> (imported.a)];
            commands.push_back (imported);
        }
        const auto result = applyCommands (commands);
        if (result.wasOk())
        {
            midiPreview = {};
            midiPreviewState = juce::var();
        }
        return result;
    }
    if (action == "midiPreview")
    {
        midiPreview = {};
        midiPreviewState = juce::var();
        midiPreviewSlots.clear();
        midiPreviewSetups.clear();
    }
    const auto path = field (command, "path").toString();
    if (! juce::File::isAbsolutePath (path)) return juce::Result::fail ("File path must be absolute.");
    const juce::File file (path);
    if (action == "kitSave" || action == "kitLoad")
    {
        juce::MemoryBlock current;
        saveLocked (current);
        furrow::PluginState state;
        if (! furrow::decodePluginState (std::span (static_cast<const std::uint8_t*> (current.getData()), current.getSize()), state))
            return juce::Result::fail ("The current state cannot be saved.");
        if (action == "kitSave")
        {
            furrow::KitState kit;
            kit.setups = state.setups;
            kit.metadata = state.metadata;
            for (const auto& setup : kit.setups)
                for (int f = 0; f < furrow::laneFields; ++f)
                    kit.laneFields[static_cast<std::size_t> (setup.slot)][static_cast<std::size_t> (f)] = modelEngine->laneField (setup.slot, f);
            std::vector<std::uint8_t> bytes;
            furrow::encodeKitState (kit, bytes);
            return file.replaceWithData (bytes.data(), bytes.size()) ? juce::Result::ok() : juce::Result::fail ("Could not write kit.");
        }
        juce::MemoryBlock bytes;
        if (file.getSize() > 16 * 1024 * 1024 || ! file.loadFileAsData (bytes)) return juce::Result::fail ("Could not read kit.");
        furrow::KitState kit;
        if (! furrow::decodeKitState (std::span (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize()), kit))
            return juce::Result::fail ("Kit is malformed or unsupported.");
        auto replacement = patternEngineSnapshot();
        std::vector<Command> commands;
        for (auto& setup : kit.setups)
        {
            const auto slot = static_cast<std::size_t> (setup.slot);
            if (! replacement->laneField (setup.slot, Active))
                commands.push_back ({ Op::AddLane, setup.slot, setup.id, 0, 60, setup.slot });
            else setup.id = replacement->laneField (setup.slot, Id);
            for (int f = ProbabilityKey; f <= ModBCc; ++f)
            {
                auto value = kit.laneFields[slot][static_cast<std::size_t> (f)];
                if (value == 0 && laneMin[static_cast<std::size_t> (f)] > 0) value = laneDefault[static_cast<std::size_t> (f)];
                commands.push_back ({ Op::SetLane, setup.slot, f, value, 0, 0 });
            }
        }
        if (replacement->apply (commands, [] (const MidiEvent&) {}) != furrow::Result::Ok)
            return juce::Result::fail ("Kit lanes are incompatible with this rack.");
        replacement->saveState (state.words);
        for (const auto& setup : kit.setups)
        {
            auto existing = std::find_if (state.setups.begin(), state.setups.end(), [&] (const auto& s) { return s.slot == setup.slot; });
            if (existing == state.setups.end()) state.setups.push_back (setup);
            else *existing = setup;
            const auto slot = static_cast<std::size_t> (setup.slot);
            state.metadata.laneNames[slot] = kit.metadata.laneNames[slot];
            state.metadata.laneColors[slot] = kit.metadata.laneColors[slot];
        }
        std::vector<std::uint8_t> restored;
        furrow::encodePluginState (state.words, state.setups, state.metadata, restored);
        remember();
        setStateInformation (restored.data(), static_cast<int> (restored.size()));
        return juce::Result::ok();
    }
    const auto setups = laneSetups();
    std::vector<int> channels;
    std::vector<int> slots;
    for (const auto& setup : setups)
    {
        channels.push_back (setup.settings.channel);
        slots.push_back (setup.slot);
    }
    if (channels.empty()) return juce::Result::fail ("Add a lane before using MIDI files.");
    if (action == "midiExport")
    {
        channels.assign (static_cast<std::size_t> (slots.back() + 1), 1);
        furrow::plugin::PatternMidiExportOptions options;
        options.lengthSteps = 1;
        for (const auto& setup : setups)
        {
            channels[static_cast<std::size_t> (setup.slot)] = setup.settings.channel;
            options.lengthSteps = std::max (options.lengthSteps, modelEngine->store().length (selectedPattern.load(), setup.slot));
        }
        options.bpm = hostBpm.load();
        juce::MemoryBlock bytes;
        const auto result = furrow::plugin::exportPatternMidi (*modelEngine, channels, options, bytes);
        if (result.failed()) return result;
        if (slots.size() != channels.size())
        {
            juce::MemoryInputStream input (bytes, false);
            juce::MidiFile expanded;
            if (! expanded.readFrom (input)) return juce::Result::fail ("Could not read rendered MIDI.");
            juce::MidiFile compact;
            compact.setTicksPerQuarterNote (options.ticksPerQuarter);
            for (const int slot : slots)
            {
                if (slot >= expanded.getNumTracks()) return juce::Result::fail ("Rendered MIDI has missing lanes.");
                compact.addTrack (*expanded.getTrack (slot));
            }
            bytes.reset();
            juce::MemoryOutputStream output (bytes, false);
            if (! compact.writeTo (output)) return juce::Result::fail ("Could not render active MIDI lanes.");
        }
        return file.replaceWithData (bytes.getData(), bytes.getSize()) ? juce::Result::ok() : juce::Result::fail ("Could not write MIDI file.");
    }
    juce::MemoryBlock bytes;
    if (file.getSize() > 16 * 1024 * 1024 || ! file.loadFileAsData (bytes)) return juce::Result::fail ("Could not read MIDI file.");
    midiPreview = furrow::plugin::previewPatternMidi (bytes.getData(), bytes.getSize(), channels);
    midiPreviewSlots = slots;
    midiPreviewSetups = setups;
    auto* preview = new juce::DynamicObject();
    preview->setProperty ("accepted", midiPreview.accepted);
    preview->setProperty ("lanes", midiPreview.lanes);
    preview->setProperty ("lengthSteps", midiPreview.lengthSteps);
    juce::Array<juce::var> diagnostics;
    for (const auto& diagnostic : midiPreview.diagnostics)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty ("message", juce::String (diagnostic.message));
        item->setProperty ("severity", diagnostic.severity == furrow::plugin::PatternMidiDiagnostic::Severity::error ? "error" : "warning");
        item->setProperty ("track", diagnostic.track);
        item->setProperty ("event", diagnostic.event);
        diagnostics.add (juce::var (item));
    }
    preview->setProperty ("diagnostics", diagnostics);
    midiPreviewState = juce::var (preview);
    return midiPreview.accepted ? juce::Result::ok() : juce::Result::fail ("MIDI import was refused. See preview diagnostics.");
}
