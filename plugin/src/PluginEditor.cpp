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
    setResizeLimits (720, 420, 4096, 2160);
    const auto state = processor.editorState();
    setSize (int (state["width"]), int (state["height"]));
    startTimerHz (10);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
}

PluginEditor::~PluginEditor() { stopTimer(); }
void PluginEditor::paint (juce::Graphics& g) { g.fillAll (bg); }
void PluginEditor::resized()
{
    browser.setBounds (getLocalBounds());
    const auto state = processor.editorState();
    if (int (state["width"]) != getWidth() || int (state["height"]) != getHeight())
    {
        juce::DynamicObject::Ptr size (new juce::DynamicObject);
        size->setProperty ("action", "resize");
        size->setProperty ("width", getWidth());
        size->setProperty ("height", getHeight());
        processor.edit (juce::var (size));
    }
}
bool PluginEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && juce::StringArray { ".wav", ".aif", ".aiff", ".flac", ".mid", ".midi" }
        .contains (juce::File (files[0]).getFileExtension().toLowerCase());
}
void PluginEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (isInterestedInFileDrag (files))
    {
        juce::DynamicObject::Ptr o (new juce::DynamicObject);
        const auto isMidi = files[0].endsWithIgnoreCase (".mid") || files[0].endsWithIgnoreCase (".midi");
        o->setProperty ("action", isMidi ? "midiPreview" : "loadSample");
        o->setProperty ("path", files[0]);
        o->setProperty ("lane", selectedLane);
        sendEdit (juce::var (o));
    }
}
void PluginEditor::chooseSample()
{
    chooseFile ("loadSample", "*.wav;*.aif;*.aiff;*.flac");
}
void PluginEditor::chooseFile (const juce::String& action, const juce::String& wildcard)
{
    if (choosingFile) return;
    choosingFile = true;
    const bool choosingSave = action == "kitSave" || action == "midiExport";
    chooser = std::make_unique<juce::FileChooser> ("Choose file", juce::File {}, wildcard);
    auto chooserFlags = juce::FileBrowserComponent::canSelectFiles
               | (choosingSave ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                               : juce::FileBrowserComponent::openMode);
    juce::Component::SafePointer<PluginEditor> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis, action, lane = selectedLane, choosingSave] (const juce::FileChooser& c)
                         {
                             if (safeThis == nullptr)
                                 return;
                             safeThis->choosingFile = false;
                             const auto file = c.getResult();
                             if (file != juce::File {} && (choosingSave || file.existsAsFile()))
                             {
                                 juce::DynamicObject::Ptr o (new juce::DynamicObject);
                                 o->setProperty ("action", action);
                                 o->setProperty ("path", file.getFullPathName());
                                 o->setProperty ("lane", lane);
                                 safeThis->processor.edit (juce::var (o));
                             }
                         });
}
void PluginEditor::sendEdit (const juce::var& v)
{
    if (const auto* object = v.getDynamicObject())
    {
        const auto action = object->getProperty ("action").toString();
        if (object->hasProperty ("lane")) selectedLane = int (object->getProperty ("lane"));
        if (action == "selectLane") return;
        if (action == "chooseSample") { chooseSample(); return; }
        if (! object->hasProperty ("path"))
        {
            if (action == "kitSave" || action == "kitLoad") { chooseFile (action, "*.furrow"); return; }
            if (action == "midiExport" || action == "midiPreview") { chooseFile (action, "*.mid;*.midi"); return; }
        }
    }
    processor.edit (v);
}
bool PluginEditor::LocalBrowser::pageAboutToLoad (const juce::String& url)
{
    return url.startsWith (juce::WebBrowserComponent::getResourceProviderRoot());
}
void PluginEditor::timerCallback()
{
    const auto state = processor.editorState();
    if (int (state["width"]) != getWidth() || int (state["height"]) != getHeight())
        setSize (int (state["width"]), int (state["height"]));
    browser.emitEventIfBrowserIsVisible ("state", state);
}
