#include "SpikeProcessor.h"

#include <catch2/catch_test_macros.hpp>

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
