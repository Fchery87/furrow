#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>

class SpikeProcessor;

class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer,
                           private juce::FileDragAndDropTarget
{
public:
    explicit PluginEditor (SpikeProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    void sendEdit (const juce::var&);
    void chooseSample();
    void chooseFile (const juce::String& action, const juce::String& wildcard);
    class LocalBrowser final : public juce::WebBrowserComponent
    {
    public:
        using juce::WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& url) override;
    };

    SpikeProcessor& processor;
    LocalBrowser browser;
    std::unique_ptr<juce::FileChooser> chooser;
    int selectedLane = 0;
    bool choosingFile = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
