#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>

class FurrowProcessor;

class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer,
                           private juce::FileDragAndDropTarget,
                           public juce::DragAndDropContainer
{
public:
    explicit PluginEditor (FurrowProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    void parentHierarchyChanged() override;
    void loadUiIfReady();
    void sendEdit (const juce::var&);
    void chooseSample();
    void chooseFile (const juce::String& action, const juce::String& wildcard);
    void chooseFolder (const juce::String& action, const juce::String& title);
    void startMidiDrag (int lane);
    void startAudioDrag (int lane, bool includeTail);
    class LocalBrowser final : public juce::WebBrowserComponent
    {
    public:
        using juce::WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& url) override;
    };

    FurrowProcessor& processor;
    LocalBrowser browser;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<juce::File> tempDragFiles;
    int selectedLane = 0;
    bool choosingFile = false;
    bool uiLoaded = false;
    bool uiReady = false;
    std::uint64_t lastSentRevision = 0;
    std::uint64_t lastWindowRevision = 0;
    bool lastPlaying = false;
    double lastBpm = 0.0;
    int lastPattern = -1;
    int lastPendingPattern = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
