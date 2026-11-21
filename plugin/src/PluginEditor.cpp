#include "PluginEditor.h"
#include "FurrowProcessor.h"
#include <BinaryData.h>
#include <cstddef>
#include <vector>

namespace
{
const auto bg = juce::Colour (0xff111215);

const char* mimeTypeForPath (const juce::String& path)
{
    if (path.endsWithIgnoreCase (".css")) return "text/css; charset=utf-8";
    if (path.endsWithIgnoreCase (".js")) return "application/javascript; charset=utf-8";
    if (path.endsWithIgnoreCase (".svg")) return "image/svg+xml";
    if (path.endsWithIgnoreCase (".woff2")) return "font/woff2";
    if (path.endsWithIgnoreCase (".json")) return "application/json";
    return "text/html; charset=utf-8";
}

juce::WebBrowserComponent::Options makeBrowserOptions (std::function<void (const juce::var&)> onEdit)
{
    auto opts = juce::WebBrowserComponent::Options {}
        .withNativeIntegrationEnabled (true)
        .withResourceProvider ([] (const auto& path) -> std::optional<juce::WebBrowserComponent::Resource>
        {
            const auto cleanPath = path.isEmpty() || path == "/" ? juce::String ("index.html") : path.trimCharactersAtStart ("/");
            const auto key = cleanPath.replaceCharacter ('/', '_').replaceCharacter ('.', '_').replaceCharacter ('-', '_');
            int size = 0;
            if (const auto* data = BinaryData::getNamedResource (key.toRawUTF8(), size))
                return juce::WebBrowserComponent::Resource {
                    std::vector<std::byte> (reinterpret_cast<const std::byte*> (data), reinterpret_cast<const std::byte*> (data) + size),
                    mimeTypeForPath (cleanPath)
                };
            return {};
        })
        .withEventListener ("edit", std::move (onEdit));
#if JUCE_WINDOWS
    opts = opts
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
            .withUserDataFolder (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Furrow/WebView2"))
            .withBackgroundColour (bg));
#endif
    return opts;
}
}

PluginEditor::PluginEditor (FurrowProcessor& p)
    : AudioProcessorEditor (p), processor (p),
      browser (makeBrowserOptions ([this] (const juce::var& v) { sendEdit (v); }))
{
    addAndMakeVisible (browser);
    setOpaque (true);
    setResizable (true, true);
    setResizeLimits (720, 420, 4096, 2160);
    lastWindowRevision = processor.windowStateRevision();
    const auto state = processor.editorState();
    setSize (int (state["width"]), int (state["height"]));
    startTimerHz (30);
    loadUiIfReady();
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    if (auto* docWin = findParentComponentOfClass<juce::DocumentWindow>())
        if (juce::Desktop::getInstance().getKioskModeComponent() == docWin)
            juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
    for (const auto& f : tempDragFiles) f.deleteFile();
}
void PluginEditor::paint (juce::Graphics& g) { g.fillAll (bg); }
void PluginEditor::parentHierarchyChanged()
{
    configureStandaloneWindow();
    loadUiIfReady();
}
void PluginEditor::configureStandaloneWindow()
{
    if (standaloneConfigured || processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;
    if (auto* docWin = findParentComponentOfClass<juce::DocumentWindow>())
    {
        standaloneConfigured = true;
        docWin->setTitleBarButtonsRequired (juce::DocumentWindow::allButtons, false);
        docWin->setBackgroundColour (juce::Colour (0xff17191e));
        docWin->setColour (juce::DocumentWindow::textColourId, juce::Colour (0xffeceef2));
        docWin->setColour (juce::ResizableWindow::backgroundColourId, bg);
        docWin->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f2229));
        docWin->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffb4bac6));
        docWin->setColour (juce::TextButton::textColourOnId, juce::Colour (0xffeceef2));
    }
}
void PluginEditor::toggleFullscreen()
{
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;
    if (auto* docWin = findParentComponentOfClass<juce::DocumentWindow>())
    {
        auto& desktop = juce::Desktop::getInstance();
        const bool isKiosk = desktop.getKioskModeComponent() == docWin;
        desktop.setKioskModeComponent (isKiosk ? nullptr : docWin, false);
    }
}
bool PluginEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::F11Key && processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        toggleFullscreen();
        return true;
    }
    return false;
}
void PluginEditor::loadUiIfReady()
{
    if (uiLoaded || getPeer() == nullptr)
        return;
    uiLoaded = true;
    juce::Component::SafePointer<PluginEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr && safeThis->getPeer() != nullptr)
            safeThis->browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    });
}
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
    if (files.isEmpty()) return false;
    if (files.size() == 1)
    {
        const auto ext = juce::File (files[0]).getFileExtension().toLowerCase();
        return SampleLoader::isSupportedAudioExtension (ext) || ext == ".mid" || ext == ".midi";
    }
    for (const auto& f : files)
        if (! SampleLoader::isSupportedAudioExtension (juce::File (f).getFileExtension()))
            return false;
    return true;
}
void PluginEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! isInterestedInFileDrag (files)) return;
    if (files.size() == 1 && (files[0].endsWithIgnoreCase (".mid") || files[0].endsWithIgnoreCase (".midi")))
    {
        juce::DynamicObject::Ptr o (new juce::DynamicObject);
        o->setProperty ("action", "midiPreview");
        o->setProperty ("path", files[0]);
        sendEdit (juce::var (o));
        return;
    }
    juce::Array<juce::var> list;
    for (const auto& f : files) list.add (f);
    juce::DynamicObject::Ptr o (new juce::DynamicObject);
    o->setProperty ("action", "addSampleFiles");
    o->setProperty ("paths", list);
    sendEdit (juce::var (o));
}
void PluginEditor::chooseFolder (const juce::String& action, const juce::String& title)
{
    if (choosingFile) return;
    choosingFile = true;
    chooser = std::make_unique<juce::FileChooser> (title, juce::File {});
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    juce::Component::SafePointer<PluginEditor> safeThis (this);
    chooser->launchAsync (chooserFlags, [safeThis, action] (const juce::FileChooser& c)
    {
        if (safeThis == nullptr) return;
        safeThis->choosingFile = false;
        const auto dir = c.getResult();
        if (dir.isDirectory())
        {
            juce::DynamicObject::Ptr o (new juce::DynamicObject);
            o->setProperty ("action", action);
            o->setProperty ("path", dir.getFullPathName());
            safeThis->processor.edit (juce::var (o));
        }
    });
}
void PluginEditor::startMidiDrag (int lane)
{
    const auto temp = juce::File::createTempFile (".mid");
    if (processor.renderMidiToFile (lane, temp).wasOk())
    {
        tempDragFiles.add (temp);
        performExternalDragDropOfFiles ({ temp.getFullPathName() }, false, this);
    }
    else
    {
        temp.deleteFile();
    }
}
void PluginEditor::startAudioDrag (int lane, bool includeTail)
{
    const auto temp = juce::File::createTempFile (".wav");
    if (processor.renderAudioToFile (lane, includeTail, temp).wasOk())
    {
        tempDragFiles.add (temp);
        performExternalDragDropOfFiles ({ temp.getFullPathName() }, false, this);
    }
    else
    {
        temp.deleteFile();
    }
}
void PluginEditor::chooseSample()
{
    chooseFile ("loadSample", SampleLoader::audioWildcard());
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
        if (action == "ready")
        {
            uiReady = true;
            lastSentRevision = 0;
            browser.emitEventIfBrowserIsVisible ("state", processor.editorState());
            lastSentRevision = processor.stateRevision();
            browser.emitEventIfBrowserIsVisible ("transport", processor.transportState());
            return;
        }
        if (object->hasProperty ("lane")) selectedLane = int (object->getProperty ("lane"));
        if (action == "selectLane") return;
        if (action == "toggleFullscreen") { toggleFullscreen(); return; }
        if (action == "chooseSample") { chooseSample(); return; }
        if (action == "midiDragStart")
        {
            const int dragLane = object->hasProperty ("lane") ? int (object->getProperty ("lane")) : -1;
            startMidiDrag (dragLane);
            return;
        }
        if (action == "audioDragStart")
        {
            const int dragLane = object->hasProperty ("lane") ? int (object->getProperty ("lane")) : -1;
            const bool includeTail = object->hasProperty ("includeTail") && bool (object->getProperty ("includeTail"));
            startAudioDrag (dragLane, includeTail);
            return;
        }
        if (! object->hasProperty ("path"))
        {
            if (action == "kitSave") { chooseFile (action, "*.furrowkit"); return; }
            if (action == "kitLoad") { chooseFile (action, "*.furrowkit;*.furrow"); return; }
            if (action == "midiExport" || action == "midiPreview") { chooseFile (action, "*.mid;*.midi"); return; }
            if (action == "relinkFolder") { chooseFolder (action, "Choose folder to relink missing samples"); return; }
            if (action == "browserAddFolder") { chooseFolder (action, "Choose sample library folder"); return; }
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
    if (! uiLoaded)
        loadUiIfReady();
    const auto winRev = processor.windowStateRevision();
    if (winRev != lastWindowRevision)
    {
        lastWindowRevision = winRev;
        const auto state = processor.editorState();
        if (int (state["width"]) != getWidth() || int (state["height"]) != getHeight())
            setSize (int (state["width"]), int (state["height"]));
    }
    const auto rev = processor.stateRevision();
    if (rev != lastSentRevision)
    {
        lastSentRevision = rev;
        browser.emitEventIfBrowserIsVisible ("state", processor.editorState());
    }
    const auto transport = processor.transportState();
    const bool playing = bool (transport["playing"]);
    const double bpm = double (transport["bpm"]);
    const int pat = int (transport["pattern"]);
    const int pending = int (transport["pendingPattern"]);
    const bool hasMeters = transport["meters"].isArray() && transport["meters"].size() > 0;
    if (playing || hasMeters || playing != lastPlaying || bpm != lastBpm || pat != lastPattern || pending != lastPendingPattern)
    {
        lastPlaying = playing;
        lastBpm = bpm;
        lastPattern = pat;
        lastPendingPattern = pending;
        browser.emitEventIfBrowserIsVisible ("transport", transport);
    }
}
