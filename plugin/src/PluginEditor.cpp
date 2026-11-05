#include "PluginEditor.h"
#include "SpikeProcessor.h"
#include <BinaryData.h>
#include <cstddef>
#include <vector>

namespace
{
const auto bg = juce::Colour (0xff241f1b);
}

PluginEditor::PluginEditor (SpikeProcessor& p)
    : AudioProcessorEditor (p), processor (p),
      browser (juce::WebBrowserComponent::Options {}
          .withNativeIntegrationEnabled (true)
          .withResourceProvider ([] (const auto& path) -> std::optional<juce::WebBrowserComponent::Resource>
          {
              const auto key = path.isEmpty() || path == "/" ? "index_html" : path.trimCharactersAtStart ("/").replace ("/", "_");
              int size = 0;
              if (const auto* data = BinaryData::getNamedResource (key.toRawUTF8(), size))
                  return juce::WebBrowserComponent::Resource { std::vector<std::byte> (reinterpret_cast<const std::byte*> (data), reinterpret_cast<const std::byte*> (data) + size), "text/html" };
              return {};
          })
          .withEventListener ("edit", [this] (const juce::var& v) { sendEdit (v); }))
{
    addAndMakeVisible (browser);
    setOpaque (true);
    setResizable (true, true);
    setSize (1200, 760);
    startTimerHz (20);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
}

PluginEditor::~PluginEditor() { stopTimer(); }
void PluginEditor::paint (juce::Graphics& g) { g.fillAll (bg); }
void PluginEditor::resized()
{
    browser.setBounds (getLocalBounds());
    juce::DynamicObject::Ptr o (new juce::DynamicObject);
    o->setProperty ("action", "resize");
    o->setProperty ("width", getWidth());
    o->setProperty ("height", getHeight());
    sendEdit (juce::var (o));
}
bool PluginEditor::isInterestedInFileDrag (const juce::StringArray& files) { return files.size() > 0; }
void PluginEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() > 0)
    {
        juce::DynamicObject::Ptr o (new juce::DynamicObject);
        const auto isMidi = files[0].endsWithIgnoreCase (".mid") || files[0].endsWithIgnoreCase (".midi");
        o->setProperty ("action", isMidi ? "dragMidi" : "loadSample");
        o->setProperty ("path", files[0]);
        sendEdit (juce::var (o));
    }
}
void PluginEditor::chooseSample()
{
    chooseFile ("loadSample", "*.wav;*.aif;*.aiff;*.flac");
}
void PluginEditor::chooseFile (const juce::String& action, const juce::String& wildcard)
{
    pendingFileAction = action;
    choosingSave = action == "kitSave" || action == "midiExport";
    chooser = std::make_unique<juce::FileChooser> ("Choose file", juce::File {}, wildcard);
    auto chooserFlags = juce::FileBrowserComponent::canSelectFiles
               | (choosingSave ? juce::FileBrowserComponent::saveMode
                               : juce::FileBrowserComponent::openMode);
    juce::Component::SafePointer<PluginEditor> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis] (const juce::FileChooser& c)
                         {
                             if (safeThis == nullptr)
                                 return;
                             if (safeThis->choosingSave || c.getResult().existsAsFile())
                             {
                                 juce::DynamicObject::Ptr o (new juce::DynamicObject);
                                 o->setProperty ("action", safeThis->pendingFileAction);
                                 o->setProperty ("path", c.getResult().getFullPathName());
                                 safeThis->sendEdit (juce::var (o));
                             }
                         });
}
void PluginEditor::sendEdit (const juce::var& v)
{
    if (const auto* object = v.getDynamicObject())
    {
        const auto action = object->getProperty ("action").toString();
        if (action == "chooseSample") { chooseSample(); return; }
        if (action == "kitSave") { chooseFile (action, "*.furrow"); return; }
        if (action == "kitLoad") { chooseFile (action, "*.furrow"); return; }
        if (action == "midiExport") { chooseFile (action, "*.mid"); return; }
        if (action == "midiPreview") { processor.edit (v); return; }
    }
    processor.edit (v);
}
void PluginEditor::timerCallback()
{
    browser.emitEventIfBrowserIsVisible ("state", processor.editorState());
}
