#include "FurrowProcessor.h"
#include "PatternAudio.h"
#include "furrow/KitState.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstring>

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
juce::String hexHash (const std::array<std::uint8_t, 32>& hash)
{
    bool any = false;
    for (const auto b : hash) if (b != 0) { any = true; break; }
    if (! any) return {};
    return juce::String::toHexString (hash.data(), static_cast<int> (hash.size()), 0);
}
}

juce::var FurrowProcessor::transportState()
{
    auto* root = new juce::DynamicObject();
    const bool playing = hostPlaying.load (std::memory_order_acquire);
    root->setProperty ("playing", playing);
    root->setProperty ("bpm", hostBpm.load (std::memory_order_acquire));
    root->setProperty ("beat", hostBeat.load (std::memory_order_acquire));
    root->setProperty ("pattern", playing ? audioPattern.load (std::memory_order_acquire) : selectedPattern.load (std::memory_order_acquire));
    root->setProperty ("pendingPattern", playing ? audioPendingPattern.load (std::memory_order_acquire) : -1);
    juce::Array<juce::var> heads;
    juce::Array<juce::var> meters;
    for (int s = 0; s < furrow::lanes; ++s)
    {
        const auto idx = static_cast<std::size_t> (s);
        const int step = playheads[idx].load (std::memory_order_relaxed);
        if (step >= 0)
        {
            auto* item = new juce::DynamicObject();
            item->setProperty ("slot", s);
            item->setProperty ("step", step);
            heads.add (juce::var (item));
        }
        const float peak = laneMeters[idx].exchange (0.0f, std::memory_order_relaxed);
        if (peak > 0.0005f)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("slot", s);
            m->setProperty ("peak", std::min (1.0f, peak));
            meters.add (juce::var (m));
        }
    }
    root->setProperty ("playheads", heads);
    root->setProperty ("meters", meters);
    return juce::var (root);
}

juce::var FurrowProcessor::editorState()
{
    const std::lock_guard lock (modelLock);
    syncAudioPatternLocked();
    auto* root = new juce::DynamicObject();
    root->setProperty ("revision", static_cast<juce::int64> (stateRevision()));
    root->setProperty ("standalone", wrapperType == wrapperType_Standalone);
    root->setProperty ("patternMidiChannel", patternMidiChannel.load (std::memory_order_acquire));
    root->setProperty ("pattern", selectedPattern.load());
    root->setProperty ("pendingPattern", hostPlaying.load (std::memory_order_acquire) ? audioPendingPattern.load (std::memory_order_acquire) : modelEngine->pendingPattern());
    root->setProperty ("playing", hostPlaying.load());
    root->setProperty ("bpm", wrapperType == wrapperType_Standalone ? internalBpm.load (std::memory_order_acquire) : hostBpm.load());
    root->setProperty ("beat", hostBeat.load());
    root->setProperty ("globalSwing", modelEngine->rackField (GlobalSwing));
    root->setProperty ("truncate", modelEngine->rackField (Truncate) != 0);
    root->setProperty ("seed", modelEngine->rackField (Seed));
    root->setProperty ("error", lastError);
    root->setProperty ("canUndo", ! undoStates.empty());
    root->setProperty ("canRedo", ! redoStates.empty());
    root->setProperty ("width", windowWidth);
    root->setProperty ("height", windowHeight);
    root->setProperty ("midiPreview", midiPreviewState);
    juce::Array<juce::var> folderList;
    for (const auto& f : browserFolders)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty ("path", f);
        item->setProperty ("name", juce::File (f).getFileName());
        folderList.add (juce::var (item));
    }
    root->setProperty ("browserFolders", folderList);
    root->setProperty ("browserListing", browserListing);
    juce::Array<juce::var> patternList;
    for (const auto& name : patternNames) patternList.add (name);
    root->setProperty ("patterns", patternList);

    juce::Array<juce::var> nonEmpty;
    for (int p = 0; p < furrow::patterns; ++p)
    {
        bool any = false;
        for (int s = 0; s < furrow::lanes && ! any; ++s)
        {
            if (! modelEngine->laneField (s, Active)) continue;
            const int len = modelEngine->store().length (p, s);
            for (int k = 0; k < len; ++k)
                if (modelEngine->store().get (p, s, k, On) != 0) { any = true; break; }
        }
        if (any) nonEmpty.add (p);
    }
    root->setProperty ("nonEmptyPatterns", nonEmpty);

    std::vector<int> orderedSlots;
    for (int s = 0; s < furrow::lanes; ++s)
        if (modelEngine->laneField (s, Active)) orderedSlots.push_back (s);
    std::stable_sort (orderedSlots.begin(), orderedSlots.end(), [this] (int a, int b) {
        const int ra = modelEngine->laneField (a, Row), rb = modelEngine->laneField (b, Row);
        return ra != rb ? ra < rb : a < b;
    });

    juce::Array<juce::var> laneList;
    for (const int s : orderedSlots)
    {
        const auto index = static_cast<std::size_t> (s);
        auto* lane = new juce::DynamicObject();
        lane->setProperty ("slot", s);
        lane->setProperty ("row", modelEngine->laneField (s, Row));
        lane->setProperty ("name", laneNames[index]);
        lane->setProperty ("color", laneColors[index]);
        lane->setProperty ("length", modelEngine->store().length (selectedPattern.load(), s));
        lane->setProperty ("note", modelEngine->laneField (s, Note));
        lane->setProperty ("mute", modelEngine->laneField (s, Mute) != 0);
        lane->setProperty ("solo", modelEngine->laneField (s, Solo) != 0);
        lane->setProperty ("volume", modelEngine->laneField (s, Volume));
        lane->setProperty ("pan", modelEngine->laneField (s, LanePan));
        lane->setProperty ("bendRange", modelEngine->laneField (s, BendRange));
        lane->setProperty ("swing", modelEngine->laneField (s, Swing));
        lane->setProperty ("modACc", modelEngine->laneField (s, ModACc));
        lane->setProperty ("modBCc", modelEngine->laneField (s, ModBCc));
        const auto& st = model[index].setup.settings;
        lane->setProperty ("target", st.target == Target::Sample ? "sample" : "midi");
        lane->setProperty ("playMode", st.playMode == PlayMode::OneShot ? "oneshot" : "gate");
        lane->setProperty ("reverse", st.reverse);
        lane->setProperty ("filterMode", st.filterMode == FilterMode::LowPass ? "lp" : (st.filterMode == FilterMode::HighPass ? "hp" : "off"));
        lane->setProperty ("filterCutoff", st.filterCutoffHz);
        lane->setProperty ("filterResonance", st.filterResonance);
        lane->setProperty ("channel", st.channel);
        lane->setProperty ("output", st.output);
        lane->setProperty ("start", st.start);
        lane->setProperty ("end", st.end);
        lane->setProperty ("tune", st.tune);
        lane->setProperty ("attack", st.attack);
        lane->setProperty ("decay", st.decay);
        lane->setProperty ("release", st.release);
        lane->setProperty ("choke", st.choke);
        const auto kind = model[index].setup.sample.kind;
        lane->setProperty ("sampleKind", kind == SampleRef::Kind::Builtin ? "builtin" : (kind == SampleRef::Kind::File ? "file" : "none"));
        lane->setProperty ("sampleBuiltin", static_cast<int> (model[index].setup.sample.builtin));
        lane->setProperty ("samplePath", juce::String::fromUTF8 (model[index].setup.sample.path.c_str()));
        lane->setProperty ("sampleHash", hexHash (model[index].setup.sample.hash));
        lane->setProperty ("sampleMissing", kind == SampleRef::Kind::File && model[index].error.isNotEmpty());
        lane->setProperty ("sampleError", model[index].error);
        if (model[index].hasPeaks)
        {
            juce::Array<juce::var> peaks;
            for (const float p : model[index].peaks) peaks.add (p);
            lane->setProperty ("peaks", peaks);
        }
        juce::Array<juce::var> steps;
        const auto length = modelEngine->store().length (selectedPattern.load(), s);
        for (int step = 0; step < length; ++step)
        {
            juce::Array<juce::var> values;
            for (int f = 0; f < stepFields; ++f) values.add (modelEngine->store().get (selectedPattern.load(), s, step, f));
            steps.add (values);
        }
        lane->setProperty ("steps", steps);
        laneList.add (lane);
    }
    root->setProperty ("lanes", laneList);
    return juce::var (root);
}

juce::Result FurrowProcessor::edit (const juce::var& command)
{
    const std::lock_guard lock (modelLock);
    syncAudioPatternLocked();
    const auto action = field (command, "action").toString();
    const auto result = editValidated (command);
    lastError = result.failed() ? result.getErrorMessage() : juce::String();
    const bool uiOnly = action == "browserPreview" || action == "browserStopPreview" || action == "browserList" || action == "resize";
    if (! (action == "browserPreview" || action == "browserStopPreview"))
        bumpRevision();
    if (result.wasOk() && ! uiOnly) updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    return result;
}

juce::Result FurrowProcessor::editValidated (const juce::var& command)
{
    const auto action = field (command, "action").toString();
    const int pattern = selectedPattern.load();
    const int lane = bounded (field (command, "lane"), 0, furrow::lanes - 1);
    if (blocked && action != "panic") return juce::Result::fail ("Load a readable state before editing.");
    if (action == "kitSave" || action == "kitLoad" || action == "midiExport" || action == "midiPreview" || action == "midiImport" || action == "relinkFolder")
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
        if (hostPlaying.load (std::memory_order_acquire))
        {
            const Command queue { Op::QueuePattern, next, 0, 0, 0, 0, nextBar.load (std::memory_order_acquire) };
            const auto result = applyCommands (std::span (&queue, 1));
            if (result.wasOk()) audioPendingPattern.store (next, std::memory_order_release);
            return result;
        }
        const Command selection { Op::SetRack, CurrentPattern, next, 0, 0, 0 };
        const auto result = applyCommands (std::span (&selection, 1));
        if (result.wasOk())
        {
            selectedPattern = next;
            audioPattern.store (next, std::memory_order_release);
            audioPendingPattern.store (-1, std::memory_order_release);
        }
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
        const int fieldValue = bounded (field (command, "field"), Seed, Truncate);
        if (fieldValue < Seed || fieldValue == Mode) return juce::Result::fail ("Rack field is invalid.");
        int high = idMax;
        if (fieldValue == CurrentPattern) high = furrow::patterns - 1;
        else if (fieldValue == GlobalSwing) high = 10000;
        else if (fieldValue == Truncate) high = 1;
        const int actual = bounded (field (command, "value"), 0, high);
        if (actual < 0) return juce::Result::fail ("Rack value is invalid.");
        const Command commandValue { Op::SetRack, fieldValue, actual, 0, 0, 0 };
        const auto result = applyCommands (std::span (&commandValue, 1));
        if (result.wasOk()) syncParametersFromModelLocked();
        return result;
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
    if (action == "setStepsBatch")
    {
        const auto list = field (command, "edits");
        if (! list.isArray() || list.size() == 0 || list.size() > transactionMax)
            return juce::Result::fail ("Step batch is invalid.");
        std::vector<Command> commands;
        commands.reserve (static_cast<std::size_t> (list.size()));
        for (int i = 0; i < list.size(); ++i)
        {
            const auto& item = list[i];
            const int l = bounded (field (item, "lane"), 0, furrow::lanes - 1);
            const int step = bounded (field (item, "step"), 0, maxSteps - 1);
            const int f = bounded (field (item, "field"), 0, stepFields - 1);
            if (l < 0 || step < 0 || f < 0) return juce::Result::fail ("Step edit is invalid.");
            const int actual = bounded (field (item, "value"), fieldMin[static_cast<std::size_t> (f)], fieldMax[static_cast<std::size_t> (f)]);
            if (actual < fieldMin[static_cast<std::size_t> (f)]) return juce::Result::fail ("Step value is invalid.");
            commands.push_back ({ Op::SetStep, pattern, l, step, f, actual });
        }
        return applyCommands (commands);
    }
    if (action == "fillLane")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        const int every = bounded (field (command, "every"), 2, 8);
        if (every != 2 && every != 3 && every != 4 && every != 8) return juce::Result::fail ("Fill interval must be 2, 3, 4 or 8.");
        const int length = modelEngine->store().length (pattern, lane);
        std::vector<Command> commands;
        commands.reserve (static_cast<std::size_t> (length));
        for (int step = 0; step < length; ++step)
            commands.push_back ({ Op::SetStep, pattern, lane, step, On, (step % every == 0) ? 1 : 0 });
        return applyCommands (commands);
    }
    if (action == "invertLane")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        const int length = modelEngine->store().length (pattern, lane);
        std::vector<Command> commands;
        commands.reserve (static_cast<std::size_t> (length));
        for (int step = 0; step < length; ++step)
            commands.push_back ({ Op::SetStep, pattern, lane, step, On, modelEngine->store().get (pattern, lane, step, On) ? 0 : 1 });
        return applyCommands (commands);
    }
    if (action == "rotateLane")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        const int dir = bounded (field (command, "direction"), -1, 1);
        if (dir != -1 && dir != 1) return juce::Result::fail ("Direction must be -1 or 1.");
        const int length = modelEngine->store().length (pattern, lane);
        if (length <= 1) return juce::Result::ok();
        std::vector<std::array<int, stepFields>> snapshot (static_cast<std::size_t> (length));
        for (int step = 0; step < length; ++step)
            for (int f = 0; f < stepFields; ++f)
                snapshot[static_cast<std::size_t> (step)][static_cast<std::size_t> (f)] = modelEngine->store().get (pattern, lane, step, f);
        std::vector<Command> commands;
        for (int step = 0; step < length; ++step)
        {
            const int src = (step - dir + length) % length;
            for (int f = 0; f < stepFields; ++f)
            {
                const int nextVal = snapshot[static_cast<std::size_t> (src)][static_cast<std::size_t> (f)];
                if (nextVal != snapshot[static_cast<std::size_t> (step)][static_cast<std::size_t> (f)])
                    commands.push_back ({ Op::SetStep, pattern, lane, step, f, nextVal });
            }
        }
        if (commands.empty()) return juce::Result::ok();
        if (commands.size() <= static_cast<std::size_t> (transactionMax))
            return applyCommands (commands);
        auto replacement = patternEngineSnapshot();
        for (std::size_t i = 0; i < commands.size(); i += static_cast<std::size_t> (transactionMax))
        {
            const auto count = std::min<std::size_t> (static_cast<std::size_t> (transactionMax), commands.size() - i);
            if (replacement->apply (std::span (commands.data() + i, count), [] (const MidiEvent&) {}) != furrow::Result::Ok)
                return juce::Result::fail ("The pattern capacity is full.");
        }
        remember();
        auto audio = std::make_unique<furrow::Engine>();
        std::vector<std::int32_t> words;
        replacement->saveState (words);
        audio->loadState (words);
        modelEngine = std::move (replacement);
        publishEngine (std::move (audio));
        return juce::Result::ok();
    }
    if (action == "clonePattern")
    {
        int target = command.hasProperty ("target") ? bounded (field (command, "target"), 0, furrow::patterns - 1) : -1;
        if (command.hasProperty ("target") && target < 0) return juce::Result::fail ("Target pattern is invalid.");
        if (target < 0)
        {
            for (int p = 0; p < furrow::patterns; ++p)
            {
                if (p == pattern) continue;
                bool empty = true;
                for (int s = 0; s < furrow::lanes && empty; ++s)
                {
                    if (! modelEngine->laneField (s, Active)) continue;
                    const int len = modelEngine->store().length (p, s);
                    if (len != defaultLength) { empty = false; break; }
                    for (int k = 0; k < len; ++k)
                        if (modelEngine->store().get (p, s, k, On) != 0) { empty = false; break; }
                }
                if (empty) { target = p; break; }
            }
            if (target < 0) return juce::Result::fail ("No empty pattern is available to clone into.");
        }
        if (target == pattern) return juce::Result::fail ("Cannot clone a pattern onto itself.");
        auto replacement = patternEngineSnapshot();
        std::vector<Command> commands { { Op::ClearPattern, target, 0, 0, 0, 0 } };
        for (int s = 0; s < furrow::lanes; ++s)
        {
            if (! replacement->laneField (s, Active)) continue;
            const int len = replacement->store().length (pattern, s);
            commands.push_back ({ Op::SetLength, target, s, len, 0, 0 });
            for (int k = 0; k < len; ++k)
                for (int f = 0; f < stepFields; ++f)
                {
                    const int val = replacement->store().get (pattern, s, k, f);
                    if (val != fieldDefault[static_cast<std::size_t> (f)])
                        commands.push_back ({ Op::SetStep, target, s, k, f, val });
                }
        }
        for (std::size_t i = 0; i < commands.size(); i += static_cast<std::size_t> (transactionMax))
        {
            const auto count = std::min<std::size_t> (static_cast<std::size_t> (transactionMax), commands.size() - i);
            const auto r = replacement->apply (std::span (commands.data() + i, count), [] (const MidiEvent&) {});
            if (r != furrow::Result::Ok)
                return juce::Result::fail (r == furrow::Result::Capacity ? "The pattern capacity is full." : "Could not clone pattern.");
        }
        if (! hostPlaying.load (std::memory_order_acquire))
        {
            const Command sel { Op::SetRack, CurrentPattern, target, 0, 0, 0 };
            replacement->apply (std::span (&sel, 1), [] (const MidiEvent&) {});
        }
        remember();
        patternNames[static_cast<std::size_t> (target)] = (patternNames[static_cast<std::size_t> (pattern)] + " copy").substring (0, 128);
        auto audio = std::make_unique<furrow::Engine>();
        std::vector<std::int32_t> words;
        replacement->saveState (words);
        audio->loadState (words);
        modelEngine = std::move (replacement);
        if (! hostPlaying.load (std::memory_order_acquire))
        {
            selectedPattern = target;
            audioPattern.store (target, std::memory_order_release);
        }
        publishEngine (std::move (audio));
        return juce::Result::ok();
    }
    if (action == "standaloneTransport")
    {
        if (command.hasProperty ("playing"))
            internalPlaying.store (static_cast<bool> (field (command, "playing")), std::memory_order_release);
        if (command.hasProperty ("bpm"))
        {
            const auto v = field (command, "bpm");
            if (! v.isDouble() && ! integer (v)) return juce::Result::fail ("BPM must be numeric.");
            const double bpmVal = static_cast<double> (v);
            if (! std::isfinite (bpmVal) || bpmVal < 20.0 || bpmVal > 300.0) return juce::Result::fail ("BPM must be between 20 and 300.");
            internalBpm.store (bpmVal, std::memory_order_release);
        }
        return juce::Result::ok();
    }
    if (action == "setPatternMidiChannel")
    {
        const int ch = bounded (field (command, "value"), 0, 16);
        if (ch < 0) return juce::Result::fail ("Pattern MIDI channel must be 0 to 16.");
        remember();
        patternMidiChannel.store (ch, std::memory_order_release);
        return juce::Result::ok();
    }
    if (action == "reorderLanes")
    {
        const auto order = field (command, "order");
        if (! order.isArray() || order.size() == 0 || order.size() > furrow::lanes)
            return juce::Result::fail ("Lane order is invalid.");
        std::array<bool, furrow::lanes> seen {};
        std::vector<Command> commands;
        commands.reserve (static_cast<std::size_t> (order.size()));
        for (int i = 0; i < order.size(); ++i)
        {
            const int s = bounded (order[i], 0, furrow::lanes - 1);
            if (s < 0 || seen[static_cast<std::size_t> (s)] || ! modelEngine->laneField (s, Active))
                return juce::Result::fail ("Lane order contains an invalid or duplicate lane.");
            seen[static_cast<std::size_t> (s)] = true;
            commands.push_back ({ Op::SetLane, s, Row, i, 0, 0 });
        }
        return applyCommands (commands);
    }
    if (action == "duplicateLane")
    {
        if (lane < 0 || ! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is invalid.");
        int dst = -1;
        for (int s = 0; s < furrow::lanes; ++s)
            if (! modelEngine->laneField (s, Active)) { dst = s; break; }
        if (dst < 0) return juce::Result::fail ("There is no free lane.");
        const int maxRow = [&] {
            int r = 0;
            for (int s = 0; s < furrow::lanes; ++s)
                if (modelEngine->laneField (s, Active)) r = std::max (r, modelEngine->laneField (s, Row) + 1);
            return std::min (furrow::lanes - 1, r);
        }();
        std::vector<Command> commands;
        commands.push_back ({ Op::AddLane, dst, dst + 1, dst + 1, modelEngine->laneField (lane, Note), maxRow });
        for (int f = Note; f <= ModBCc; ++f)
        {
            if (f == Row) continue;
            commands.push_back ({ Op::SetLane, dst, f, modelEngine->laneField (lane, f), 0, 0 });
        }
        const int len = modelEngine->store().length (pattern, lane);
        commands.push_back ({ Op::SetLength, pattern, dst, len, 0, 0 });
        for (int step = 0; step < len; ++step)
            for (int f = 0; f < stepFields; ++f)
            {
                const int val = modelEngine->store().get (pattern, lane, step, f);
                if (val != fieldDefault[static_cast<std::size_t> (f)])
                    commands.push_back ({ Op::SetStep, pattern, dst, step, f, val });
            }
        const auto res = applyLocked (commands, true);
        if (res.failed()) return res;
        const auto srcIdx = static_cast<std::size_t> (lane), dstIdx = static_cast<std::size_t> (dst);
        laneNames[dstIdx] = (laneNames[srcIdx] + " copy").substring (0, 128);
        laneColors[dstIdx] = laneColors[srcIdx];
        auto copied = model[srcIdx].setup;
        copied.slot = dst;
        copied.id = modelEngine->laneField (dst, Id);
        model[dstIdx].setup = copied;
        model[dstIdx].settingsDirty = true;
        requestSample (dst, model[dstIdx], copied.sample);
        sync();
        return juce::Result::ok();
    }
    if (action == "rerollSeed")
    {
        const int current = modelEngine->rackField (Seed);
        int next = juce::Random::getSystemRandom().nextInt (idMax + 1);
        if (next == current) next = (next + 1) % (idMax + 1);
        const Command commandValue { Op::SetRack, Seed, next, 0, 0, 0 };
        return applyCommands (std::span (&commandValue, 1));
    }
    if (action == "addSampleFiles")
    {
        const auto paths = field (command, "paths");
        if (! paths.isArray() || paths.size() == 0) return juce::Result::fail ("No audio files provided.");
        std::vector<juce::String> validPaths;
        for (int i = 0; i < paths.size(); ++i)
        {
            const auto p = paths[i].toString();
            const auto ext = juce::File (p).getFileExtension().toLowerCase();
            if (juce::File::isAbsolutePath (p) && p.getNumBytesAsUTF8() <= pathMax
                && SampleLoader::isSupportedAudioExtension (ext))
                validPaths.push_back (p);
        }
        if (validPaths.empty()) return juce::Result::fail ("Drop WAV, AIFF, FLAC, OGG or MP3 audio files.");
        std::vector<int> freeSlots;
        for (int s = 0; s < furrow::lanes && freeSlots.size() < validPaths.size(); ++s)
            if (! modelEngine->laneField (s, Active)) freeSlots.push_back (s);
        if (freeSlots.empty()) return juce::Result::fail ("There is no free lane.");
        std::vector<Command> commands;
        for (std::size_t i = 0; i < freeSlots.size(); ++i)
            commands.push_back ({ Op::AddLane, freeSlots[i], freeSlots[i] + 1, freeSlots[i] + 1, std::min (127, 36 + freeSlots[i]), freeSlots[i] });
        const auto result = applyLocked (commands, true);
        if (result.failed()) return result;
        const char* palette[] { "#E2B24A", "#8DB3D6", "#C987AE", "#A3BE6E", "#D2814F", "#A89BE0" };
        for (std::size_t i = 0; i < freeSlots.size(); ++i)
        {
            const int s = freeSlots[i];
            auto& laneModel = model[static_cast<std::size_t> (s)];
            laneNames[static_cast<std::size_t> (s)] = juce::File (validPaths[i]).getFileNameWithoutExtension().substring (0, 128);
            laneColors[static_cast<std::size_t> (s)] = palette[s % 6];
            laneModel.setup.settings.target = Target::Sample;
            laneModel.setup.settings.playMode = PlayMode::OneShot;
            laneModel.settingsDirty = true;
            laneModel.setup.sample.kind = SampleRef::Kind::File;
            laneModel.setup.sample.path = validPaths[i].toStdString();
            laneModel.setup.sample.hash.fill (0);
            requestSample (s, laneModel, laneModel.setup.sample);
        }
        sync();
        return juce::Result::ok();
    }
    if (action == "loadSample" || action == "retrySample" || action == "browserHotSwap")
    {
        const auto path = field (command, "path").toString();
        if (lane < 0 || ! juce::File::isAbsolutePath (path) || path.getNumBytesAsUTF8() > pathMax)
            return juce::Result::fail ("Sample path must be absolute and at most 4096 bytes.");
        const auto extension = juce::File (path).getFileExtension().toLowerCase();
        if (! SampleLoader::isSupportedAudioExtension (extension))
            return juce::Result::fail ("Use a WAV, AIFF, FLAC, OGG or MP3 sample.");
        const std::lock_guard lock (modelLock);
        if (! modelEngine->laneField (lane, Active)) return juce::Result::fail ("Lane is inactive.");
        remember();
        auto& lm = model[static_cast<std::size_t> (lane)];
        const bool switchedTarget = lm.setup.settings.target != Target::Sample;
        lm.setup.settings.target = Target::Sample;
        if (action == "loadSample")
            lm.setup.settings.playMode = PlayMode::OneShot;
        lm.settingsDirty = true;
        if (switchedTarget)
            routeChanged = true;
        lm.setup.sample.hash.fill (0);
        lm.setup.sample.kind = SampleRef::Kind::File;
        lm.setup.sample.path = path.toStdString();
        requestSample (lane, lm, lm.setup.sample);
        sync();
        return juce::Result::ok();
    }
    if (action == "browserAddFolder")
    {
        const auto path = field (command, "path").toString();
        const juce::File dir (path);
        if (! juce::File::isAbsolutePath (path) || ! dir.isDirectory())
            return juce::Result::fail ("Select an existing folder to add to the browser.");
        const auto normalized = dir.getFullPathName();
        if (std::find (browserFolders.begin(), browserFolders.end(), normalized) == browserFolders.end())
        {
            browserFolders.push_back (normalized);
            saveBrowserSettingsLocked();
        }
        return listBrowserFolderLocked (normalized);
    }
    if (action == "browserRemoveFolder")
    {
        const auto path = field (command, "path").toString();
        const auto before = browserFolders.size();
        browserFolders.erase (std::remove (browserFolders.begin(), browserFolders.end(), path), browserFolders.end());
        if (browserFolders.size() != before)
            saveBrowserSettingsLocked();
        if (browserListing.isObject() && browserListing["path"].toString().startsWith (path))
            browserListing = juce::var();
        return juce::Result::ok();
    }
    if (action == "browserList")
    {
        const auto path = field (command, "path").toString();
        return listBrowserFolderLocked (path);
    }
    if (action == "browserPreview")
    {
        const auto path = field (command, "path").toString();
        const juce::File file (path);
        if (! juce::File::isAbsolutePath (path) || ! file.existsAsFile()
            || ! SampleLoader::isSupportedAudioExtension (file.getFileExtension()))
            return juce::Result::fail ("Choose an existing audio file to preview.");
        ++previewGeneration;
        if (loader)
            loader->request (SampleLoader::previewLane, previewGeneration, file.getFullPathName());
        return juce::Result::ok();
    }
    if (action == "browserStopPreview")
    {
        ++previewGeneration;
        pendingPreview.reset();
        previewDirty = true;
        sync();
        return juce::Result::ok();
    }
    if (action == "resize")
    {
        const int width = bounded (field (command, "width"), 720, 4096);
        const int height = bounded (field (command, "height"), 420, 2160);
        if (width < 720 || height < 420) return juce::Result::fail ("Window size is invalid.");
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
        const auto prevTarget = settings.target;
        const auto prevChannel = settings.channel;
        const auto name = field (command, "field").toString();
        const auto value = field (command, "value");
        if (name == "target")
        {
            if (value != juce::var ("midi") && value != juce::var ("sample")) return juce::Result::fail ("Target is invalid.");
            settings.target = value.toString() == "sample" ? Target::Sample : Target::Midi;
        }
        else if (name == "playMode")
        {
            if (value != juce::var ("oneshot") && value != juce::var ("gate")) return juce::Result::fail ("Play mode is invalid.");
            settings.playMode = value.toString() == "oneshot" ? PlayMode::OneShot : PlayMode::Gate;
        }
        else if (name == "reverse")
        {
            settings.reverse = static_cast<bool> (value);
        }
        else if (name == "filterMode")
        {
            const auto m = value.toString();
            if (m == "off") settings.filterMode = FilterMode::Off;
            else if (m == "lp") settings.filterMode = FilterMode::LowPass;
            else if (m == "hp") settings.filterMode = FilterMode::HighPass;
            else return juce::Result::fail ("Filter mode is invalid.");
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
            else if (name == "filterCutoff") settings.filterCutoffHz = number;
            else if (name == "filterResonance") settings.filterResonance = number;
            else return juce::Result::fail ("Unknown sampler setting.");
        }
        if (! valid (settings)) return juce::Result::fail ("Sampler setting is out of range.");
        remember();
        if (settings.target != prevTarget || settings.channel != prevChannel)
            routeChanged = true;
        applySetups (setups, laneIds());
        return juce::Result::ok();
    }
    return juce::Result::fail ("Unknown edit.");
}

juce::Result FurrowProcessor::renderMidiToFile (int onlyLane, const juce::File& file)
{
    const std::lock_guard lock (modelLock);
    const auto setups = laneSetups();
    if (setups.empty()) return juce::Result::fail ("Add a lane before exporting MIDI.");
    std::vector<int> slots;
    for (const auto& setup : setups) slots.push_back (setup.slot);
    std::vector<int> channels (static_cast<std::size_t> (slots.back() + 1), 1);
    furrow::plugin::PatternMidiExportOptions options;
    options.lengthSteps = 1;
    options.bpm = hostBpm.load();
    options.onlyLane = onlyLane;
    for (const auto& setup : setups)
    {
        channels[static_cast<std::size_t> (setup.slot)] = setup.settings.channel;
        if (onlyLane < 0 || setup.slot == onlyLane)
            options.lengthSteps = std::max (options.lengthSteps, modelEngine->store().length (selectedPattern.load(), setup.slot));
    }
    juce::MemoryBlock bytes;
    const auto result = furrow::plugin::exportPatternMidi (*modelEngine, channels, options, bytes);
    if (result.failed()) return result;
    if (onlyLane < 0 && slots.size() != channels.size())
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

juce::Result FurrowProcessor::renderAudioToFile (int onlyLane, bool includeTail, const juce::File& file)
{
    const std::lock_guard lock (modelLock);
    const auto setups = laneSetups();
    if (setups.empty()) return juce::Result::fail ("Add a lane before exporting audio.");
    furrow::plugin::PatternAudioExportOptions options;
    options.lengthSteps = 1;
    options.bpm = wrapperType == wrapperType_Standalone ? internalBpm.load (std::memory_order_acquire) : hostBpm.load (std::memory_order_acquire);
    if (! std::isfinite (options.bpm) || options.bpm <= 0.0) options.bpm = 120.0;
    options.sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
    options.onlyLane = onlyLane;
    options.includeTail = includeTail;
    for (const auto& setup : setups)
        if (onlyLane < 0 || setup.slot == onlyLane)
            options.lengthSteps = std::max (options.lengthSteps, modelEngine->store().length (selectedPattern.load(), setup.slot));
    return furrow::plugin::exportPatternWav (*modelEngine, setups, options, file);
}

void FurrowProcessor::setBrowserSettingsFile (const juce::File& file)
{
    const std::lock_guard lock (modelLock);
    browserSettingsFile = file;
    browserFolders.clear();
    browserListing = juce::var();
    loadBrowserSettingsLocked();
}

void FurrowProcessor::loadBrowserSettingsLocked()
{
    if (! browserSettingsFile.existsAsFile()) return;
    const auto parsed = juce::JSON::parse (browserSettingsFile);
    if (! parsed.isObject()) return;
    const auto arr = parsed["browserFolders"];
    if (! arr.isArray()) return;
    browserFolders.clear();
    for (int i = 0; i < arr.size(); ++i)
    {
        const auto p = arr[i].toString();
        if (juce::File::isAbsolutePath (p) && juce::File (p).isDirectory())
            browserFolders.push_back (p);
    }
}

void FurrowProcessor::saveBrowserSettingsLocked() const
{
    if (browserSettingsFile == juce::File {}) return;
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (const auto& f : browserFolders) arr.add (f);
    root->setProperty ("browserFolders", arr);
    const auto parent = browserSettingsFile.getParentDirectory();
    if (parent != juce::File {}) parent.createDirectory();
    browserSettingsFile.replaceWithText (juce::JSON::toString (juce::var (root), true));
}

juce::Result FurrowProcessor::listBrowserFolderLocked (const juce::String& path)
{
    const juce::File dir (path);
    if (! juce::File::isAbsolutePath (path) || ! dir.isDirectory())
        return juce::Result::fail ("Browser folder does not exist.");

    auto dirs = dir.findChildFiles (juce::File::findDirectories, false);
    auto files = dir.findChildFiles (juce::File::findFiles, false);
    auto byName = [] (const juce::File& a, const juce::File& b) {
        return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
    };
    std::sort (dirs.begin(), dirs.end(), byName);
    std::sort (files.begin(), files.end(), byName);

    juce::Array<juce::var> entries;
    constexpr int maxEntries = 1000;
    for (const auto& d : dirs)
    {
        if (entries.size() >= maxEntries) break;
        if (d.getFileName().startsWithChar ('.')) continue;
        auto* item = new juce::DynamicObject();
        item->setProperty ("name", d.getFileName());
        item->setProperty ("path", d.getFullPathName());
        item->setProperty ("isDir", true);
        entries.add (juce::var (item));
    }
    for (const auto& f : files)
    {
        if (entries.size() >= maxEntries) break;
        if (! SampleLoader::isSupportedAudioExtension (f.getFileExtension())) continue;
        auto* item = new juce::DynamicObject();
        item->setProperty ("name", f.getFileName());
        item->setProperty ("path", f.getFullPathName());
        item->setProperty ("isDir", false);
        entries.add (juce::var (item));
    }

    auto* listing = new juce::DynamicObject();
    listing->setProperty ("path", dir.getFullPathName());
    const auto parent = dir.getParentDirectory();
    listing->setProperty ("parent", parent != dir ? parent.getFullPathName() : juce::String());
    listing->setProperty ("entries", entries);
    browserListing = juce::var (listing);
    return juce::Result::ok();
}

juce::Result FurrowProcessor::editFile (const juce::var& command)
{
    const auto action = field (command, "action").toString();
    if (action == "midiImport")
    {
        if (! midiPreview.accepted) return juce::Result::fail ("Preview a compatible MIDI file first.");
        if (laneSetups() != midiPreviewSetups) return juce::Result::fail ("The lanes changed. Preview the MIDI file again.");

        const auto patternEmpty = [&] (int pat) {
            for (int s = 0; s < furrow::lanes; ++s)
            {
                if (! modelEngine->laneField (s, Active)) continue;
                const int len = modelEngine->store().length (pat, s);
                for (int k = 0; k < len; ++k)
                    if (modelEngine->store().get (pat, s, k, On) != 0) return false;
            }
            return true;
        };
        int targetPattern = selectedPattern.load();
        if (! patternEmpty (targetPattern))
        {
            targetPattern = -1;
            for (int p = 0; p < furrow::patterns; ++p)
                if (patternEmpty (p)) { targetPattern = p; break; }
            if (targetPattern < 0) return juce::Result::fail ("No empty pattern is available for MIDI import.");
        }

        std::vector<int> targetSlots = midiPreviewSlots;
        std::vector<Command> commands;
        for (int s = 0; s < furrow::lanes && static_cast<int> (targetSlots.size()) < midiPreview.lanes; ++s)
        {
            if (! modelEngine->laneField (s, Active))
            {
                const int idx = static_cast<int> (targetSlots.size());
                const int baseNote = idx < static_cast<int> (midiPreview.baseNotes.size()) ? midiPreview.baseNotes[static_cast<std::size_t> (idx)] : 60;
                commands.push_back ({ Op::AddLane, s, s + 1, s + 1, baseNote, s });
                targetSlots.push_back (s);
            }
        }
        if (static_cast<int> (targetSlots.size()) < midiPreview.lanes)
            return juce::Result::fail ("Not enough free lanes for this MIDI file.");

        commands.push_back ({ Op::ClearPattern, targetPattern, 0, 0, 0, 0 });
        for (int i = 0; i < midiPreview.lanes; ++i)
        {
            const int slot = targetSlots[static_cast<std::size_t> (i)];
            commands.push_back ({ Op::SetLength, targetPattern, slot, midiPreview.lengthSteps, 0, 0 });
            if (i < static_cast<int> (midiPreview.baseNotes.size()))
                commands.push_back ({ Op::SetLane, slot, Note, midiPreview.baseNotes[static_cast<std::size_t> (i)], 0, 0 });
        }
        for (auto imported : midiPreview.commands)
        {
            if (imported.op == Op::SetStep)
            {
                imported.a = targetPattern;
                imported.b = targetSlots[static_cast<std::size_t> (imported.b)];
            }
            else if (imported.op == Op::SetLane)
                imported.a = targetSlots[static_cast<std::size_t> (imported.a)];
            commands.push_back (imported);
        }
        if (targetPattern != selectedPattern.load() && ! hostPlaying.load (std::memory_order_acquire))
            commands.push_back ({ Op::SetRack, CurrentPattern, targetPattern, 0, 0, 0 });

        const auto result = applyCommands (commands);
        if (result.wasOk())
        {
            midiPreview = {};
            midiPreviewState = juce::var();
            if (! hostPlaying.load (std::memory_order_acquire))
            {
                selectedPattern = targetPattern;
                audioPattern.store (targetPattern, std::memory_order_release);
            }
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
    if (action == "relinkFolder")
    {
        if (! file.isDirectory()) return juce::Result::fail ("Select an existing folder to search for missing samples.");
        std::vector<int> missingSlots;
        for (int s = 0; s < furrow::lanes; ++s)
        {
            const auto idx = static_cast<std::size_t> (s);
            if (! modelEngine->laneField (s, Active)) continue;
            if (model[idx].setup.sample.kind == SampleRef::Kind::File && ! juce::File (juce::String::fromUTF8 (model[idx].setup.sample.path.c_str())).existsAsFile())
                missingSlots.push_back (s);
        }
        if (missingSlots.empty()) return juce::Result::ok();
        int relinked = 0;
        const auto candidates = file.findChildFiles (juce::File::findFiles, true, SampleLoader::audioWildcard());
        for (const auto& candidate : candidates)
        {
            const auto digest = juce::SHA256 (candidate).getRawData();
            if (digest.getSize() != 32) continue;
            std::array<std::uint8_t, 32> hash {};
            std::memcpy (hash.data(), digest.getData(), 32);
            for (const int s : missingSlots)
            {
                auto& laneModel = model[static_cast<std::size_t> (s)];
                const bool hashZero = std::all_of (laneModel.setup.sample.hash.begin(), laneModel.setup.sample.hash.end(), [] (auto b) { return b == 0; });
                const bool hashMatch = ! hashZero && laneModel.setup.sample.hash == hash;
                const bool nameMatch = hashZero && candidate.getFileName().equalsIgnoreCase (juce::File (juce::String::fromUTF8 (laneModel.setup.sample.path.c_str())).getFileName());
                if (hashMatch || nameMatch)
                {
                    if (relinked == 0) remember();
                    laneModel.setup.sample.path = candidate.getFullPathName().toStdString();
                    laneModel.setup.sample.hash = hash;
                    requestSample (s, laneModel, laneModel.setup.sample);
                    ++relinked;
                }
            }
        }
        sync();
        return relinked > 0 ? juce::Result::ok() : juce::Result::fail ("No matching samples were found in that folder.");
    }
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
        state.metadata.windowWidth = windowWidth;
        state.metadata.windowHeight = windowHeight;
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
        const int exportLane = command.hasProperty ("lane") ? bounded (field (command, "lane"), -1, furrow::lanes - 1) : -1;
        return renderMidiToFile (exportLane, file);
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
