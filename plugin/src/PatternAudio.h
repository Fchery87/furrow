#pragma once

#include "furrow/Engine.h"
#include "furrow/sampler/LaneSetup.h"

#include <juce_core/juce_core.h>

#include <span>

namespace furrow::plugin
{

struct PatternAudioExportOptions
{
    double bpm = 120.0;
    double sampleRate = 44100.0;
    int lengthSteps = 16;
    int onlyLane = -1;
    bool includeTail = false;
    double maxTailSeconds = 2.0;
};

juce::Result exportPatternWav (const Engine& source, std::span<const sampler::LaneSetup> setups,
                               const PatternAudioExportOptions& options, const juce::File& destination);

} // namespace furrow::plugin
