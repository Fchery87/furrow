#include "SpikeProcessor.h"

namespace
{
using namespace furrow;
using namespace furrow::sampler;

bool integer (const juce::var& value) { return value.isInt() || value.isInt64(); }
int getInt (const juce::var& value) { return value.isInt64() ? static_cast<int> (value.toString().getLargeIntValue()) : static_cast<int> (value); }
const juce::var& field (const juce::var& value, const char* name)
{
    static const juce::var empty;
    return value.isObject() && value.hasProperty (name) ? value[name] : empty;
}
int bounded (const juce::var& value, int low, int high)
{
    if (! integer (value)) return low - 1;
    const auto result = getInt (value);
    return result >= low && result <= high ? result : low - 1;
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
    const auto action = field (command, "action").toString();
    const int pattern = selectedPattern.load();
    const int lane = bounded (field (command, "lane"), 0, furrow::lanes - 1);
    if (action == "undo" || action == "redo")
    {
        const std::lock_guard lock (modelLock);
        return restoreHistory (action == "redo");
    }
    if (action == "panic" || action == "audition")
    {
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
        if (lane < 0) return juce::Result::fail ("Lane is invalid.");
        const auto value = field (command, action == "renameLane" ? "name" : "color").toString();
        if (value.isEmpty() || value.length() > 128) return juce::Result::fail ("Value is invalid.");
        const std::lock_guard lock (modelLock);
        remember();
        if (action == "renameLane") laneNames[static_cast<std::size_t> (lane)] = value;
        else laneColors[static_cast<std::size_t> (lane)] = value;
        return juce::Result::ok();
    }
    if (action == "renamePattern")
    {
        const auto value = field (command, "name").toString();
        if (value.isEmpty() || value.length() > 128) return juce::Result::fail ("Pattern name is invalid.");
        const std::lock_guard lock (modelLock);
        remember();
        patternNames[static_cast<std::size_t> (pattern)] = value;
        return juce::Result::ok();
    }
    if (action == "pattern")
    {
        const int next = bounded (field (command, "value"), 0, furrow::patterns - 1);
        if (next < 0) return juce::Result::fail ("Pattern is invalid.");
        selectedPattern = next;
        return juce::Result::ok();
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
        if (actual < 0) return juce::Result::fail ("Step value is invalid.");
        const Command commandValue { Op::SetStep, pattern, lane, step, fieldValue, actual };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "setLane")
    {
        const int fieldValue = bounded (field (command, "field"), ProbabilityKey, ModBCc);
        if (lane < 0 || fieldValue < 0) return juce::Result::fail ("Lane edit is invalid.");
        const int actual = bounded (field (command, "value"), laneMin[static_cast<std::size_t> (fieldValue)], laneMax[static_cast<std::size_t> (fieldValue)]);
        if (actual < 0) return juce::Result::fail ("Lane value is invalid.");
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
        const Command commandValue { Op::RemoveLane, lane, 0, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "loadSample" || action == "retrySample")
    {
        const auto path = field (command, "path").toString();
        if (lane < 0 || ! juce::File::isAbsolutePath (path)) return juce::Result::fail ("Sample path must be absolute.");
        const auto extension = juce::File (path).getFileExtension().toLowerCase();
        if (! juce::StringArray { ".wav", ".aif", ".aiff", ".flac" }.contains (extension)) return juce::Result::fail ("Use a WAV, AIFF or FLAC sample.");
        const std::lock_guard lock (modelLock);
        model[static_cast<std::size_t> (lane)].setup.sample.kind = SampleRef::Kind::File;
        model[static_cast<std::size_t> (lane)].setup.sample.path = path.toStdString();
        requestSample (lane, model[static_cast<std::size_t> (lane)], model[static_cast<std::size_t> (lane)].setup.sample);
        return juce::Result::ok();
    }
    return juce::Result::fail ("Unknown edit.");
}
