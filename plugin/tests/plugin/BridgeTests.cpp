#include "FurrowProcessor.h"

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

juce::MemoryBlock save (FurrowProcessor& processor)
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
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
    FurrowProcessor source;
    REQUIRE (source.edit (command ("pattern", {{"value", 3}})).wasOk());
    REQUIRE (source.edit (command ("renamePattern", {{"name", "Fill"}})).wasOk());
    REQUIRE (source.edit (command ("renameLane", {{"lane", 0}, {"name", "Deep kick"}})).wasOk());
    REQUIRE (source.edit (command ("colorLane", {{"lane", 0}, {"color", "#123abc"}})).wasOk());
    REQUIRE (source.edit (command ("setLength", {{"lane", 0}, {"value", 23}})).wasOk());
    REQUIRE (source.edit (command ("setStep", {{"lane", 0}, {"step", 22}, {"field", furrow::Pitch}, {"value", -12}})).wasOk());
    REQUIRE (source.edit (command ("setSampler", {{"lane", 0}, {"field", "tune"}, {"value", -300}})).wasOk());
    REQUIRE (source.edit (command ("resize", {{"width", 1280}, {"height", 800}})).wasOk());
    const auto bytes = save (source);
    FurrowProcessor restored;
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
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
    FurrowProcessor processor;
    const auto original = save (processor);
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "channel"}, {"value", 10}})).wasOk());
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == original);
    REQUIRE (processor.edit (command ("redo")).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "target"}, {"value", "midi"}})).wasOk());
    const auto bytes = save (processor);
    FurrowProcessor restored;
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

TEST_CASE ("Pattern switching while playing queues for the next bar and syncs when it lands")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    struct PlayingTransport final : juce::AudioPlayHead
    {
        double ppq = 0.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setIsPlaying (true);
            p.setBpm (120.0);
            p.setPpqPosition (ppq);
            p.setTimeSignature (TimeSignature { 4, 4 });
            p.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            return p;
        }
    };

    FurrowProcessor processor;
    processor.setRateAndBufferSizeDetails (44100.0, 512);
    processor.prepareToPlay (44100.0, 512);
    PlayingTransport transport;
    processor.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (processor.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;

    transport.ppq = 0.5;
    processor.processBlock (audio, midi);
    REQUIRE (processor.edit (command ("pattern", {{"value", 2}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["pattern"]) == 0);
    CHECK (static_cast<int> (processor.editorState()["pendingPattern"]) == 2);

    // Advance past bar 1 (beat 4.0) so the queued switch lands on the audio thread.
    for (int i = 0; i < 160; ++i)
    {
        transport.ppq = 0.5 + i * (512.0 / 44100.0 * 2.0);
        processor.processBlock (audio, midi);
    }
    CHECK (static_cast<int> (processor.editorState()["pattern"]) == 2);
    CHECK (static_cast<int> (processor.editorState()["pendingPattern"]) == -1);
    processor.setPlayHead (nullptr);
}

TEST_CASE ("Non-routing sampler edits do not kill active voices or reset playback")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    struct PlayingTransport final : juce::AudioPlayHead
    {
        double ppq = 0.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setIsPlaying (true);
            p.setBpm (120.0);
            p.setPpqPosition (ppq);
            return p;
        }
    };

    FurrowProcessor processor;
    processor.setRateAndBufferSizeDetails (44100.0, 512);
    processor.prepareToPlay (44100.0, 512);
    PlayingTransport transport;
    processor.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (processor.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;

    transport.ppq = 0.0;
    processor.processBlock (audio, midi);
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "decay"}, {"value", 250.0}})).wasOk());

    transport.ppq = 512.0 / 44100.0 * 2.0;
    processor.processBlock (audio, midi);
    float peak = 0.0f;
    for (int f = 0; f < 512; ++f) peak = std::max (peak, std::abs (audio.getSample (0, f)));
    CHECK (peak > 0.001f);
    processor.setPlayHead (nullptr);
}

TEST_CASE ("Window resize persists without polluting musical undo history")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    CHECK_FALSE (static_cast<bool> (processor.editorState()["canUndo"]));
    REQUIRE (processor.edit (command ("resize", {{"width", 1280}, {"height", 800}})).wasOk());
    CHECK_FALSE (static_cast<bool> (processor.editorState()["canUndo"]));
    CHECK (static_cast<int> (processor.editorState()["width"]) == 1280);
    CHECK (static_cast<int> (processor.editorState()["height"]) == 800);
}

TEST_CASE ("setRack validates per-field bounds and rejects Mode and RackId")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    CHECK_FALSE (processor.edit (command ("setRack", {{"field", furrow::RackId}, {"value", 1}})).wasOk());
    CHECK_FALSE (processor.edit (command ("setRack", {{"field", furrow::Mode}, {"value", 1}})).wasOk());
    CHECK_FALSE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 10001}})).wasOk());
    CHECK_FALSE (processor.edit (command ("setRack", {{"field", furrow::Truncate}, {"value", 2}})).wasOk());
    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 3500}})).wasOk());
    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::Truncate}, {"value", 1}})).wasOk());
    const auto state = processor.editorState();
    CHECK (static_cast<int> (state["globalSwing"]) == 3500);
    CHECK (static_cast<bool> (state["truncate"]));
    CHECK (state["lanes"][0]["peaks"].isArray());
    CHECK (state["lanes"][0]["peaks"].size() == SampleLoader::peakBins);
}

TEST_CASE ("Wave 2 pattern tools, batch step edits, clonePattern, and rerollSeed are atomic and undoable")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    const auto initial = save (processor);

    REQUIRE (processor.edit (command ("fillLane", {{"lane", 0}, {"every", 3}})).wasOk());
    {
        const auto st = processor.editorState()["lanes"][0]["steps"];
        CHECK (static_cast<int> (st[0][furrow::On]) == 1);
        CHECK (static_cast<int> (st[1][furrow::On]) == 0);
        CHECK (static_cast<int> (st[3][furrow::On]) == 1);
    }
    REQUIRE (processor.edit (command ("rotateLane", {{"lane", 0}, {"direction", 1}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][1][furrow::On]) == 1);
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][0][furrow::On]) == 1); // step 15 wrapped to 0

    REQUIRE (processor.edit (command ("invertLane", {{"lane", 0}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][1][furrow::On]) == 0);
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][2][furrow::On]) == 1);

    juce::Array<juce::var> batch;
    for (int s = 0; s < 4; ++s)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("lane", 0);
        e->setProperty ("step", s);
        e->setProperty ("field", furrow::Velocity);
        e->setProperty ("value", 60 + s * 10);
        batch.add (juce::var (e));
    }
    REQUIRE (processor.edit (command ("setStepsBatch", {{"edits", batch}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][3][furrow::Velocity]) == 90);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][3][furrow::Velocity]) == 100);

    REQUIRE (processor.edit (command ("clonePattern")).wasOk());
    CHECK (static_cast<int> (processor.editorState()["pattern"]) == 1);
    CHECK (processor.editorState()["patterns"][1].toString() == "Pattern 1 copy");
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["steps"][2][furrow::On]) == 1);

    const int beforeSeed = static_cast<int> (processor.editorState()["seed"]);
    REQUIRE (processor.edit (command ("rerollSeed")).wasOk());
    CHECK (static_cast<int> (processor.editorState()["seed"]) != beforeSeed);
}

TEST_CASE ("Wave 3 multi-file sample drop, MIDI import into new pattern, and SHA-256 folder relink")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("furrow-wave3-test");
    tempDir.deleteRecursively();
    REQUIRE (tempDir.createDirectory());
    const auto writeTestWav = [] (const juce::File& target, float value) {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        const auto opts = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (32)
                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor (stream, opts);
        REQUIRE (writer != nullptr);
        std::vector<float> samples (256, value);
        const float* ptrs[] = { samples.data() };
        REQUIRE (writer->writeFromFloatArrays (ptrs, 1, 256));
    };

    const auto wavA = tempDir.getChildFile ("TomHigh.wav");
    const auto wavB = tempDir.getChildFile ("RimShot.wav");
    writeTestWav (wavA, 0.25f);
    writeTestWav (wavB, 0.50f);

    FurrowProcessor processor;
    juce::Array<juce::var> dropPaths;
    dropPaths.add (wavA.getFullPathName());
    dropPaths.add (wavB.getFullPathName());
    REQUIRE (processor.edit (command ("addSampleFiles", {{"paths", dropPaths}})).wasOk());
    CHECK (processor.editorState()["lanes"].size() == 6);
    CHECK (processor.editorState()["lanes"][4]["name"].toString() == "TomHigh");
    CHECK (processor.editorState()["lanes"][5]["name"].toString() == "RimShot");

    // Wait for SHA-256 hash to populate, move wavA to a subfolder, reload state, then relink by SHA-256.
    for (int i = 0; i < 100 && processor.editorState()["lanes"][4]["sampleHash"].toString().isEmpty(); ++i)
        juce::Thread::sleep (20);
    REQUIRE (processor.editorState()["lanes"][4]["sampleHash"].toString().isNotEmpty());
    const auto savedWithHash = save (processor);
    const auto movedDir = tempDir.getChildFile ("moved");
    REQUIRE (movedDir.createDirectory());
    const auto movedWav = movedDir.getChildFile ("RenamedTom.wav");
    REQUIRE (wavA.moveFileTo (movedWav));

    FurrowProcessor reopened;
    reopened.setStateInformation (savedWithHash.getData(), static_cast<int> (savedWithHash.getSize()));
    REQUIRE (reopened.edit (command ("relinkFolder", {{"path", movedDir.getFullPathName()}})).wasOk());
    CHECK (reopened.editorState()["lanes"][4]["samplePath"].toString() == movedWav.getFullPathName());

    // MIDI import while on non-empty Pattern 0 imports into new empty Pattern 1 without overwriting Pattern 0.
    const auto midFile = tempDir.getChildFile ("import.mid");
    {
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        juce::MidiMessageSequence seq;
        seq.addEvent (juce::MidiMessage::noteOn (1, 48, juce::uint8 (100)), 0.0);
        seq.addEvent (juce::MidiMessage::noteOff (1, 48), 240.0);
        mf.addTrack (seq);
        juce::MemoryBlock bytes;
        juce::MemoryOutputStream out (bytes, false);
        REQUIRE (mf.writeTo (out));
        REQUIRE (midFile.replaceWithData (bytes.getData(), bytes.getSize()));
    }
    REQUIRE (processor.edit (command ("midiPreview", {{"path", midFile.getFullPathName()}})).wasOk());
    REQUIRE (processor.edit (command ("midiImport")).wasOk());
    CHECK (static_cast<int> (processor.editorState()["pattern"]) == 1);
    CHECK (processor.patternEngineSnapshot()->store().get (0, 0, 0, furrow::On) == 1);
    CHECK (processor.patternEngineSnapshot()->store().get (1, 0, 0, furrow::On) == 1);

    tempDir.deleteRecursively();
}

TEST_CASE ("Host-automatable Global Swing, Truncate Swing, and Bypass parameters sync with processor")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    REQUIRE (processor.getParameters().size() == 3);
    REQUIRE (processor.getBypassParameter() != nullptr);

    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 5000}})).wasOk());
    CHECK (std::abs (processor.getParameters()[0]->getValue() - 0.5f) < 1e-4f);

    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::Truncate}, {"value", 1}})).wasOk());
    CHECK (processor.getParameters()[1]->getValue() > 0.5f);

    processor.setRateAndBufferSizeDetails (44100.0, 256);
    processor.prepareToPlay (44100.0, 256);
    processor.getBypassParameter()->setValue (1.0f);
    juce::AudioBuffer<float> audio (processor.getTotalNumOutputChannels(), 256);
    juce::MidiBuffer midi;
    processor.processBlock (audio, midi);
    float peak = 0.0f;
    for (int f = 0; f < 256; ++f) peak = std::max (peak, std::abs (audio.getSample (0, f)));
    CHECK (peak <= 1e-6f);
}

TEST_CASE ("V1.1 reorderLanes, duplicateLane, standaloneTransport, and filter/reverse/oneshot edits")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "playMode"}, {"value", "gate"}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "reverse"}, {"value", true}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterMode"}, {"value", "lp"}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterCutoff"}, {"value", 1200.0}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterResonance"}, {"value", 0.5}})).wasOk());
    const auto s1 = processor.editorState();
    CHECK (s1["lanes"][0]["playMode"].toString() == "gate");
    CHECK (static_cast<bool> (s1["lanes"][0]["reverse"]));
    CHECK (s1["lanes"][0]["filterMode"].toString() == "lp");

    REQUIRE (processor.edit (command ("duplicateLane", {{"lane", 0}})).wasOk());
    CHECK (processor.editorState()["lanes"].size() == 5);
    CHECK (processor.editorState()["lanes"][4]["name"].toString() == "Kick copy");
    CHECK (static_cast<bool> (processor.editorState()["lanes"][4]["reverse"]));

    juce::Array<juce::var> order;
    order.add (3); order.add (2); order.add (1); order.add (0); order.add (4);
    REQUIRE (processor.edit (command ("reorderLanes", {{"order", order}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["lanes"][0]["slot"]) == 3);
    CHECK (static_cast<int> (processor.editorState()["lanes"][3]["slot"]) == 0);

    REQUIRE (processor.edit (command ("setPatternMidiChannel", {{"value", 16}})).wasOk());
    CHECK (static_cast<int> (processor.editorState()["patternMidiChannel"]) == 16);

    REQUIRE (processor.edit (command ("standaloneTransport", {{"playing", true}, {"bpm", 132.0}})).wasOk());
    CHECK_FALSE (processor.edit (command ("standaloneTransport", {{"bpm", 500.0}})).wasOk());
}

TEST_CASE ("V1.2 sample browser folders, listing, hot-swap, preview, and WAV audio export")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("furrow-v12-browser-test");
    tempDir.deleteRecursively();
    REQUIRE (tempDir.createDirectory());
    const auto subDir = tempDir.getChildFile ("Kicks");
    REQUIRE (subDir.createDirectory());
    const auto writeWav = [] (const juce::File& target, float value) {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        const auto opts = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (32)
                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor (stream, opts);
        REQUIRE (writer != nullptr);
        std::vector<float> samples (512, value);
        const float* ptrs[] = { samples.data() };
        REQUIRE (writer->writeFromFloatArrays (ptrs, 1, 512));
    };
    const auto kickWav = subDir.getChildFile ("PunchKick.wav");
    writeWav (kickWav, 0.4f);

    FurrowProcessor processor;
    processor.setBrowserSettingsFile (tempDir.getChildFile ("settings.json"));
    REQUIRE (processor.edit (command ("browserAddFolder", {{"path", tempDir.getFullPathName()}})).wasOk());
    CHECK (processor.editorState()["browserFolders"].size() == 1);
    CHECK (processor.editorState()["browserListing"]["entries"].size() == 1);
    CHECK (static_cast<bool> (processor.editorState()["browserListing"]["entries"][0]["isDir"]));

    REQUIRE (processor.edit (command ("browserList", {{"path", subDir.getFullPathName()}})).wasOk());
    REQUIRE (processor.editorState()["browserListing"]["entries"].size() == 1);
    CHECK (processor.editorState()["browserListing"]["entries"][0]["name"].toString() == "PunchKick.wav");

    // Hot-swap preserves existing steps and filter settings and is undoable.
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterMode"}, {"value", "lp"}})).wasOk());
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "playMode"}, {"value", "gate"}})).wasOk());
    const auto beforeSwap = save (processor);
    REQUIRE (processor.edit (command ("browserHotSwap", {{"lane", 0}, {"path", kickWav.getFullPathName()}})).wasOk());
    const auto afterSwap = processor.editorState();
    CHECK (afterSwap["lanes"][0]["samplePath"].toString() == kickWav.getFullPathName());
    CHECK (afterSwap["lanes"][0]["filterMode"].toString() == "lp");
    CHECK (afterSwap["lanes"][0]["playMode"].toString() == "gate");
    CHECK (static_cast<int> (afterSwap["lanes"][0]["steps"][0][furrow::On]) == 1);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == beforeSwap);

    // Preview loads asynchronously and plays on the main bus.
    processor.setRateAndBufferSizeDetails (44100.0, 256);
    processor.prepareToPlay (44100.0, 256);
    REQUIRE (processor.edit (command ("browserPreview", {{"path", kickWav.getFullPathName()}})).wasOk());
    float previewPeak = 0.0f;
    juce::AudioBuffer<float> audio (processor.getTotalNumOutputChannels(), 256);
    juce::MidiBuffer midi;
    for (int attempt = 0; attempt < 50 && previewPeak <= 0.01f; ++attempt)
    {
        juce::Thread::sleep (10);
        processor.processBlock (audio, midi);
        for (int f = 0; f < 256; ++f) previewPeak = std::max (previewPeak, std::abs (audio.getSample (0, f)));
    }
    CHECK (previewPeak > 0.1f);
    REQUIRE (processor.edit (command ("browserStopPreview")).wasOk());

    // Processor WAV render helper writes a valid 24-bit WAV.
    const auto outWav = tempDir.getChildFile ("bounce.wav");
    REQUIRE (processor.renderAudioToFile (-1, false, outWav).wasOk());
    CHECK (outWav.existsAsFile());
    CHECK (outWav.getSize() > 1000);

    // Browser settings persist across restarts and remove cleanly.
    FurrowProcessor reopened;
    reopened.setBrowserSettingsFile (tempDir.getChildFile ("settings.json"));
    CHECK (reopened.editorState()["browserFolders"].size() == 1);
    REQUIRE (reopened.edit (command ("browserRemoveFolder", {{"path", tempDir.getFullPathName()}})).wasOk());
    CHECK (reopened.editorState()["browserFolders"].size() == 0);

    tempDir.deleteRecursively();
}

TEST_CASE ("Live-drag gesture edits update state immediately with one atomic undo snapshot")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    FurrowProcessor processor;
    const auto initial = save (processor);
    const auto revBefore = processor.stateRevision();

    // Start + Move apply live without bumping UI revision; End bumps revision once and keeps one undo point.
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterCutoff"}, {"value", 4000.0}, {"gesture", "start"}})).wasOk());
    CHECK (processor.stateRevision() == revBefore);
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterCutoff"}, {"value", 1800.0}, {"gesture", "move"}})).wasOk());
    CHECK (processor.stateRevision() == revBefore);
    CHECK (std::abs (static_cast<double> (processor.editorState()["lanes"][0]["filterCutoff"]) - 1800.0) < 1e-3);
    REQUIRE (processor.edit (command ("setSampler", {{"lane", 0}, {"field", "filterCutoff"}, {"value", 950.0}, {"gesture", "end"}})).wasOk());
    CHECK (processor.stateRevision() > revBefore);
    CHECK (std::abs (static_cast<double> (processor.editorState()["lanes"][0]["filterCutoff"]) - 950.0) < 1e-3);

    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == initial);

    // Same atomic gesture behavior for engine commands (GlobalSwing).
    const auto swingRevBefore = processor.stateRevision();
    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 2000}, {"gesture", "start"}})).wasOk());
    CHECK (processor.stateRevision() == swingRevBefore);
    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 4500}, {"gesture", "move"}})).wasOk());
    CHECK (processor.stateRevision() == swingRevBefore);
    REQUIRE (processor.edit (command ("setRack", {{"field", furrow::GlobalSwing}, {"value", 6000}, {"gesture", "end"}})).wasOk());
    CHECK (processor.stateRevision() > swingRevBefore);
    CHECK (static_cast<int> (processor.editorState()["globalSwing"]) == 6000);
    REQUIRE (processor.edit (command ("undo")).wasOk());
    CHECK (save (processor) == initial);
}
