#include "SpikeProcessor.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
juce::var command (const char* action, std::initializer_list<std::pair<const char*, juce::var>> fields = {})
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("action", action);
    for (const auto& [key, value] : fields) object->setProperty (key, value);
    return juce::var (object);
}

juce::MemoryBlock save (SpikeProcessor& processor)
{
    juce::MemoryBlock bytes;
    processor.getStateInformation (bytes);
    return bytes;
}

struct TemporaryFile
{
    juce::File file = juce::File::createTempFile (".furrow-test");
    ~TemporaryFile() { file.deleteFile(); }
};
}

TEST_CASE ("The processor exposes a structured editor snapshot")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    const auto snapshot = processor.editorState();
    REQUIRE (snapshot.isObject());
    CHECK (snapshot.hasProperty ("lanes"));
    CHECK (snapshot.hasProperty ("patterns"));
    CHECK (snapshot["lanes"].isArray());
    CHECK (snapshot["patterns"].isArray());
}

TEST_CASE ("Clearing a lane silences only the selected pattern and is undoable")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    REQUIRE (processor.edit (command ("setStep", {{"lane", 0}, {"step", 0}, {"field", furrow::On}, {"value", 1}})).wasOk());
    REQUIRE (processor.edit (command ("pattern", {{"value", 1}})).wasOk());
    REQUIRE (processor.edit (command ("setStep", {{"lane", 0}, {"step", 511}, {"field", furrow::On}, {"value", 1}})).wasOk());
    const auto before = save (processor);
    REQUIRE (processor.edit (command ("clearLane", {{"lane", 0}})).wasOk());
    const auto engine = processor.patternEngineSnapshot();
    CHECK (engine->laneField (0, furrow::Active) == 1);
    CHECK (engine->store().get (0, 0, 0, furrow::On) == 1);
    CHECK (engine->store().get (1, 0, 511, furrow::On) == 0);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == before);
}

TEST_CASE ("The editor rejects malformed edits and accepts a bounded step edit")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    CHECK_FALSE (processor.edit (juce::var (new juce::DynamicObject)).wasOk());

    auto* command = new juce::DynamicObject();
    command->setProperty ("action", "setStep");
    command->setProperty ("lane", 0);
    command->setProperty ("step", 0);
    command->setProperty ("field", furrow::On);
    command->setProperty ("value", 1);
    CHECK (processor.edit (juce::var (command)).wasOk());
}

TEST_CASE ("Editor changes survive host save and reopen before an audio block")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor source;
    REQUIRE (source.edit (command ("pattern", {{"value", 3}})).wasOk());
    REQUIRE (source.edit (command ("renamePattern", {{"name", "Fill"}})).wasOk());
    REQUIRE (source.edit (command ("renameLane", {{"lane", 0}, {"name", "Deep kick"}})).wasOk());
    REQUIRE (source.edit (command ("colorLane", {{"lane", 0}, {"color", "#123abc"}})).wasOk());
    REQUIRE (source.edit (command ("setLength", {{"lane", 0}, {"value", 23}})).wasOk());
    REQUIRE (source.edit (command ("setStep", {{"lane", 0}, {"step", 22}, {"field", furrow::Pitch}, {"value", -12}})).wasOk());
    REQUIRE (source.edit (command ("setSampler", {{"lane", 0}, {"field", "tune"}, {"value", -300}})).wasOk());
    REQUIRE (source.edit (command ("resize", {{"width", 1280}, {"height", 800}})).wasOk());
    const auto bytes = save (source);
    SpikeProcessor restored;
    restored.setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));
    const auto state = restored.editorState();
    CHECK (static_cast<int> (state["pattern"]) == 3);
    CHECK (state["patterns"][3].toString() == "Fill");
    CHECK (state["lanes"][0]["name"].toString() == "Deep kick");
    CHECK (state["lanes"][0]["color"].toString() == "#123abc");
    CHECK (static_cast<int> (state["lanes"][0]["length"]) == 23);
    CHECK (static_cast<int> (state["lanes"][0]["steps"][22][furrow::Pitch]) == -12);
    CHECK (static_cast<int> (state["lanes"][0]["tune"]) == -300);
    CHECK (static_cast<int> (state["width"]) == 1280);
    CHECK (static_cast<int> (state["height"]) == 800);
    CHECK (save (restored) == bytes);
    restored.setRateAndBufferSizeDetails (44100.0, 64);
    restored.prepareToPlay (44100.0, 64);
    juce::AudioBuffer<float> audio (restored.getTotalNumOutputChannels(), 64);
    juce::MidiBuffer midi;
    restored.processBlock (audio, midi);
    CHECK (static_cast<int> (restored.editorState()["pattern"]) == 3);
}

TEST_CASE ("Rejected editor operations preserve persisted state and redo history")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    REQUIRE (processor.edit (command ("renameLane", {{"lane", 0}, {"name", "Changed"}})).wasOk());
    REQUIRE (processor.edit (command ("undo")).wasOk());
    const auto before = save (processor);
    TemporaryFile malformed;
    REQUIRE (malformed.file.replaceWithText ("not a kit or MIDI file"));
    const auto invalid = {
        command ("setStep", {{"lane", 0}, {"step", 0}, {"field", furrow::On}, {"value", juce::int64 (4294967297LL)}}),
        command ("setSampler", {{"lane", 0}, {"field", "start"}, {"value", 1.0}}),
        command ("resize", {{"width", 719}, {"height", 800}}),
        command ("colorLane", {{"lane", 0}, {"color", "not-a-color"}}),
        command ("kitLoad", {{"path", malformed.file.getFullPathName()}}),
        command ("midiPreview", {{"path", malformed.file.getFullPathName()}}),
        command ("midiImport")
    };
    for (const auto& edit : invalid)
    {
        INFO (juce::JSON::toString (edit).toStdString());
        CHECK_FALSE (processor.edit (edit).wasOk());
        CHECK (save (processor) == before);
        CHECK (static_cast<bool> (processor.editorState()["canRedo"]));
        CHECK (processor.editorState()["error"].toString().isNotEmpty());
    }
    REQUIRE (processor.edit (command ("redo")).wasOk());
    CHECK (processor.editorState()["lanes"][0]["name"].toString() == "Changed");
}

TEST_CASE ("Kit loading restores sounds without replacing patterns and is undoable")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    TemporaryFile kit;
    REQUIRE (processor.edit (command ("kitSave", {{"path", kit.file.getFullPathName()}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "tune"}, {"value", 200}})).wasOk());
    REQUIRE (processor.edit (command ("renameLane", {{"lane", 0}, {"name", "Other"}})).wasOk());
    REQUIRE (processor.edit (command ("setStep", {{"lane", 0}, {"step", 1}, {"field", furrow::On}, {"value", 1}})).wasOk());
    const auto before = save (processor);
    REQUIRE (processor.edit (command ("kitLoad", {{"path", kit.file.getFullPathName()}})).wasOk());
    const auto state = processor.editorState();
    CHECK (state["lanes"][0]["name"].toString() == "Kick");
    CHECK (static_cast<int> (state["lanes"][0]["tune"]) == 0);
    CHECK (static_cast<int> (state["lanes"][0]["steps"][1][furrow::On]) == 1);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == before);
}

TEST_CASE ("MIDI preview is nonmutating and acceptance is a single undoable edit")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TemporaryFile input;
    juce::MidiFile file;
    file.setTicksPerQuarterNote (960);
    for (int channel = 1; channel <= 4; ++channel)
    {
        juce::MidiMessageSequence track;
        track.addEvent (juce::MidiMessage::noteOn (channel, 36 + channel, juce::uint8 (90)), 480.0);
        track.addEvent (juce::MidiMessage::noteOff (channel, 36 + channel), 600.0);
        file.addTrack (track);
    }
    juce::MemoryBlock bytes;
    juce::MemoryOutputStream stream (bytes, false);
    REQUIRE (file.writeTo (stream));
    REQUIRE (input.file.replaceWithData (bytes.getData(), bytes.getSize()));
    SpikeProcessor processor;
    REQUIRE (processor.edit (command ("pattern", {{"value", 2}})).wasOk());
    const auto before = save (processor);
    const auto canUndo = static_cast<bool> (processor.editorState()["canUndo"]);
    REQUIRE (processor.edit (command ("midiPreview", {{"path", input.file.getFullPathName()}})).wasOk());
    CHECK (save (processor) == before);
    CHECK (static_cast<bool> (processor.editorState()["canUndo"]) == canUndo);
    CHECK (static_cast<bool> (processor.editorState()["midiPreview"]["accepted"]));
    REQUIRE (processor.edit (command ("midiImport")).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][2][furrow::On]) == 1);
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][2][furrow::Velocity]) == 90);
    CHECK (processor.patternEngineSnapshot()->store().get (0, 0, 0, furrow::On) == 1);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == before);
    REQUIRE (processor.edit (command ("midiPreview", {{"path", input.file.getFullPathName()}})).wasOk());
    REQUIRE (input.file.replaceWithText ("broken"));
    CHECK_FALSE (processor.edit (command ("midiPreview", {{"path", input.file.getFullPathName()}})).wasOk());
    CHECK_FALSE (processor.edit (command ("midiImport")).wasOk());
    CHECK (save (processor) == before);
}

TEST_CASE ("A full audio edit queue refuses changes without consuming undo history")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    REQUIRE (processor.edit (command ("renameLane", {{"lane", 0}, {"name", "Changed"}})).wasOk());
    REQUIRE (processor.edit (command ("undo")).wasOk());
    for (int i = 0; i < 128; ++i) REQUIRE (processor.edit (command ("panic")).wasOk());
    const auto before = save (processor);
    CHECK_FALSE (processor.edit (command ("setLength", {{"lane", 0}, {"value", 32}})).wasOk());
    CHECK (save (processor) == before);
    CHECK (static_cast<bool> (processor.editorState()["canRedo"]));
    REQUIRE (processor.edit (command ("redo")).wasOk());
    CHECK (processor.editorState()["lanes"][0]["name"].toString() == "Changed");
}

TEST_CASE ("Sampler settings are undoable and saved routing reaches a reopened processor")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    SpikeProcessor processor;
    const auto original = save (processor);
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "channel"}, {"value", 10}})).wasOk());
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == original);
    REQUIRE (processor.edit (command ("redo")).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "target"}, {"value", "midi"}})).wasOk());
    const auto bytes = save (processor);
    SpikeProcessor restored;
    restored.setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));
    restored.setRateAndBufferSizeDetails (44100.0, 64);
    restored.prepareToPlay (44100.0, 64);
    REQUIRE (restored.edit (command ("audition", {{"lane", 0}})).wasOk());
    juce::AudioBuffer<float> audio (restored.getTotalNumOutputChannels(), 64);
    juce::MidiBuffer midi;
    restored.processBlock (audio, midi);
    int notes = 0;
    for (const auto event : midi)
    {
        const auto message = event.getMessage();
        if (message.isNoteOn())
        {
            CHECK (message.getChannel() == 10);
            CHECK (message.getNoteNumber() == 36);
            ++notes;
        }
    }
    CHECK (notes == 1);
}
