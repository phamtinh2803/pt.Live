/*
    pt.Live - Live Singing DAW
    Plugin editor windows.

    Tracktion Engine does NOT open plugin windows by itself: Plugin::showWindowExplicitly()
    asks engine.getUIBehaviour().createPluginWindow(...), and the default UIBehaviour
    returns nothing. So the host app must provide its own UIBehaviour -> this file.

    Behaviour:
      - a plugin editor opens as soon as the plugin is inserted (see InspectorPanel::insertPlugin)
      - right-click the preset bar on top of the editor: Save preset / Set as default / Reset to factory
        (the original state is stored the first time a plugin is inserted, see ptl::pluginhost::presets)
      - the editor window always stays above the main window (owned window, not system-wide topmost)
      - at most TWO plugin editors are open at a time (kMaxOpenEditors): opening a third one closes the oldest

    The only non-header part is setOwnerWindow() in src/Audio/PluginWindowPlatform.cpp.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TracktionHeaders.h"
#include "include/Theme.h"

//==============================================================================
namespace ptl::pluginhost
{
    /** Makes `child` an owned window of `owner`: it is always drawn above the owner (and hides /
        minimises with it) without being topmost for the whole desktop. Falls back to
        setAlwaysOnTop() on platforms without owned windows. Implemented in PluginWindowPlatform.cpp. */
    void setOwnerWindow (juce::Component& child, juce::Component* owner);

    inline juce::Component::SafePointer<juce::Component>& mainWindowRef()
    {
        static juce::Component::SafePointer<juce::Component> w;
        return w;
    }

    /** The application's main window: plugin editors are kept above it. */
    inline void setMainWindow (juce::Component* w)   { mainWindowRef() = w; }
    inline juce::Component* getMainWindow()          { return mainWindowRef().getComponent(); }

    /** TRUE when `plugin` is an external plugin that has no live instance AND is not in the engine's
        list of scanned plugins (e.g. a .pt saved on another machine, or a VST3 the scanner has not
        seen yet). Such a plugin can never be instantiated, and trying to do it on the message thread
        (initialiseFully) can freeze the app - so callers must NOT try to load it.
        Cheap enough to call from a UI timer. Message thread only. */
    inline bool isMissing (te::Plugin& plugin)
    {
        auto* ext = dynamic_cast<te::ExternalPlugin*> (&plugin);

        if (ext == nullptr || ext->getAudioPluginInstance() != nullptr)
            return false;

        const auto& wanted = ext->desc;

        for (const auto& d : plugin.engine.getPluginManager().knownPluginList.getTypes())
        {
            if (d.isDuplicateOf (wanted))
                return false;

            if (d.pluginFormatName != wanted.pluginFormatName)
                continue;

            // Same plugin installed at a different path (another PC): match by id, then by name + maker.
            if (wanted.uniqueId != 0 && d.uniqueId == wanted.uniqueId)
                return false;

            if (d.name == wanted.name && d.manufacturerName == wanted.manufacturerName)
                return false;
        }

        return true;
    }

    /** Message shown to the user when an insert is missing from the scanned list. */
    inline juce::String missingMessage (const juce::String& pluginName)
    {
        return "\"" + pluginName + "\" is not in the list of scanned plugins, so it cannot be opened.\n\n"
               "It was probably saved in this session on another computer, or it has not been scanned yet.\n\n"
               "Open the plugin browser and scan your plugin folders (or install the plugin), "
               "then try again. Until then this insert is skipped.";
    }

    /** Makes sure an ExternalPlugin has a live juce::AudioPluginInstance.
        Returns an empty string when the plugin is ready (or is not an external plugin),
        otherwise a human readable reason why it could not be loaded. */
    inline juce::String ensureLoaded (te::Plugin& plugin)
    {
        auto* ext = dynamic_cast<te::ExternalPlugin*> (&plugin);

        if (ext == nullptr || ext->getAudioPluginInstance() != nullptr)
            return {};

        // Not scanned / not installed: never touch the plugin DLL, just report it (no freeze).
        if (isMissing (plugin))
            return missingMessage (plugin.getName());

        // Complete the (lazy) instantiation of the VST3 now, on the message thread.
        ext->initialiseFully();

        // A bypassed plugin may not have been instantiated: enable it and try once more.
        if (ext->getAudioPluginInstance() == nullptr && ! ext->isEnabled())
        {
            ext->setEnabled (true);
            ext->initialiseFully();
        }

        if (ext->getAudioPluginInstance() != nullptr)
            return {};

        const auto reason = ext->getLoadError();

        return reason.isNotEmpty() ? reason
                                   : juce::String ("The plugin could not be instantiated "
                                                   "(it is missing, blocked, or crashed while loading).");
    }

    /** Creates the editor component for a loaded plugin instance: the plugin's own editor
        when it has one, otherwise a generic parameter editor. Never returns nullptr for a
        valid instance. */
    inline std::unique_ptr<juce::AudioProcessorEditor> createEditorFor (juce::AudioPluginInstance& instance)
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed;

        if (instance.hasEditor())
            ed.reset (instance.createEditorIfNeeded());

        if (ed == nullptr)                                       // no editor, or it failed to build
            ed = std::make_unique<juce::GenericAudioProcessorEditor> (instance);

        return ed;
    }
}


//==============================================================================
/*  Per-plugin presets kept by pt.Live itself (independent of the plugin's own preset system).

    %APPDATA%\ptLive\PluginPresets\<Plugin_hash>\
        factory.ptstate      state of the plugin the very FIRST time it was inserted (never overwritten)
        default.ptstate      user's "Set as default": applied automatically to every new insert
        presets\<name>.ptstate
*/
namespace ptl::pluginhost::presets
{
    inline juce::File rootDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive").getChildFile ("PluginPresets");
    }

    inline te::ExternalPlugin* asExternal (te::Plugin& p)  { return dynamic_cast<te::ExternalPlugin*> (&p); }

    inline juce::File pluginDir (te::Plugin& p)
    {
        auto* ext = asExternal (p);
        const auto id   = ext != nullptr ? ext->desc.createIdentifierString() : p.getName();
        const auto name = juce::File::createLegalFileName (p.getName()).trim();
        return rootDir().getChildFile ((name.isEmpty() ? juce::String ("plugin") : name)
                                       + "_" + juce::String::toHexString (id.hashCode64()));
    }

    inline juce::File factoryFile (te::Plugin& p)  { return pluginDir (p).getChildFile ("factory.ptstate"); }
    inline juce::File defaultFile (te::Plugin& p)  { return pluginDir (p).getChildFile ("default.ptstate"); }
    inline juce::File presetsDir  (te::Plugin& p)  { return pluginDir (p).getChildFile ("presets"); }

    inline bool writeState (const juce::File& f, const juce::MemoryBlock& mb)
    {
        return f.getParentDirectory().createDirectory().wasOk() && f.replaceWithData (mb.getData(), mb.getSize());
    }

    inline bool readState (const juce::File& f, juce::MemoryBlock& mb)
    {
        return f.existsAsFile() && f.loadFileAsData (mb) && mb.getSize() > 0;
    }

    /** Current state of the live plugin instance. */
    inline bool captureState (te::Plugin& p, juce::MemoryBlock& mb)
    {
        auto* ext = asExternal (p);
        auto* inst = ext != nullptr ? ext->getAudioPluginInstance() : nullptr;

        if (inst == nullptr)
            return false;

        mb.reset();
        inst->getStateInformation (mb);
        return mb.getSize() > 0;
    }

    /** Pushes a saved state into the live plugin (and into the engine's ValueTree so it is saved in the .pt). */
    inline bool applyState (te::Plugin& p, const juce::MemoryBlock& mb)
    {
        auto* ext = asExternal (p);
        auto* inst = ext != nullptr ? ext->getAudioPluginInstance() : nullptr;

        if (inst == nullptr || mb.getSize() == 0)
            return false;

        inst->setStateInformation (mb.getData(), (int) mb.getSize());
        ext->flushPluginStateToValueTree();
        return true;
    }

    /** First insert of this plugin ever: remember its original state. Does nothing if already stored. */
    inline void captureFactoryIfMissing (te::Plugin& p)
    {
        if (factoryFile (p).existsAsFile())
            return;

        juce::MemoryBlock mb;
        if (captureState (p, mb))
            writeState (factoryFile (p), mb);
    }

    /** Call right after a NEW insert is loaded: stores the factory snapshot, then applies the user's default (if any). */
    inline void onNewInsert (te::Plugin& p)
    {
        captureFactoryIfMissing (p);

        juce::MemoryBlock mb;
        if (readState (defaultFile (p), mb))
            applyState (p, mb);
    }

    /** Factory state: the stored snapshot, or (inserts made before this feature existed) a state
        taken from a brand-new, untouched instance of the same plugin. */
    inline bool getFactoryState (te::Plugin& p, juce::MemoryBlock& mb)
    {
        if (readState (factoryFile (p), mb))
            return true;

        auto* ext = asExternal (p);
        if (ext == nullptr)
            return false;

        juce::String error;
        auto fresh = ext->engine.getPluginManager().pluginFormatManager
                        .createPluginInstance (ext->desc, 44100.0, 512, error);

        if (fresh == nullptr)
            return false;

        fresh->getStateInformation (mb);
        fresh.reset();

        if (mb.getSize() == 0)
            return false;

        writeState (factoryFile (p), mb);
        return true;
    }

    inline juce::StringArray listPresets (te::Plugin& p)
    {
        juce::StringArray names;
        for (const auto& f : presetsDir (p).findChildFiles (juce::File::findFiles, false, "*.ptstate"))
            names.add (f.getFileNameWithoutExtension());

        names.sortNatural();
        return names;
    }

    inline juce::File presetFile (te::Plugin& p, const juce::String& name)
    {
        return presetsDir (p).getChildFile (juce::File::createLegalFileName (name) + ".ptstate");
    }
}

//==============================================================================
/*  Slim bar above the plugin editor.  Right-click (or click) it for:
        Save preset...   Set as default   Reset to factory   + the list of saved presets.
    (A plugin's own editor is a native child window that swallows mouse clicks, so the menu lives on this bar.) */
class PluginPresetBar : public juce::Component
{
public:
    static constexpr int kHeight = 28;

    explicit PluginPresetBar (te::Plugin& p) : plugin (&p)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ptl::theme::raised);
        g.setColour (ptl::theme::hairline);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        g.setFont (ptl::theme::font (15.0f, true));
        g.setColour (ptl::theme::text);
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe  ")) + "Presets", getLocalBounds().withTrimmedLeft (10),
                    juce::Justification::centredLeft);

        g.setFont (ptl::theme::font (13.0f));
        g.setColour (ptl::theme::accentHover);
        g.drawText (status, getLocalBounds().withTrimmedRight (10), juce::Justification::centredRight);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown())
            return;

        showMenu();
    }

private:
    enum { idSave = 1, idDefault, idFactory, idPresetBase = 100, idDeleteBase = 1000 };

    void showMenu()
    {
        const auto names = ptl::pluginhost::presets::listPresets (*plugin);

        juce::PopupMenu m;
        m.addItem (idSave,    "Save preset...");
        m.addItem (idDefault, "Set as default");
        m.addItem (idFactory, "Reset to factory");

        if (! names.isEmpty())
        {
            m.addSeparator();
            for (int i = 0; i < names.size(); ++i)
                m.addItem (idPresetBase + i, names[i]);

            juce::PopupMenu del;
            for (int i = 0; i < names.size(); ++i)
                del.addItem (idDeleteBase + i, names[i]);

            m.addSeparator();
            m.addSubMenu ("Delete preset", del);
        }

        juce::Component::SafePointer<PluginPresetBar> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [safe, names] (int r) { if (safe != nullptr && r != 0) safe->handle (r, names); });
    }

    void handle (int r, const juce::StringArray& names)
    {
        namespace pp = ptl::pluginhost::presets;
        juce::MemoryBlock mb;

        if (r == idSave)
        {
            askName();
        }
        else if (r == idDefault)
        {
            if (pp::captureState (*plugin, mb) && pp::writeState (pp::defaultFile (*plugin), mb))
                setStatus ("default saved");
            else
                setStatus ("could not save");
        }
        else if (r == idFactory)
        {
            if (pp::getFactoryState (*plugin, mb) && pp::applyState (*plugin, mb))
            {
                setStatus ("factory");
                refreshEditor();
            }
            else
                setStatus ("no factory state");
        }
        else if (r >= idDeleteBase && r - idDeleteBase < names.size())
        {
            pp::presetFile (*plugin, names[r - idDeleteBase]).deleteFile();
        }
        else if (r >= idPresetBase && r - idPresetBase < names.size())
        {
            if (pp::readState (pp::presetFile (*plugin, names[r - idPresetBase]), mb) && pp::applyState (*plugin, mb))
            {
                setStatus (names[r - idPresetBase]);
                refreshEditor();
            }
        }
    }

    void askName()
    {
        auto* box = new juce::AlertWindow ("Save preset", "Preset name:", juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("name", {}, {});
        box->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<PluginPresetBar> safe (this);
        juce::Component::SafePointer<juce::AlertWindow> safeBox (box);

        box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, safeBox] (int r)
        {
            if (r != 1 || safe == nullptr || safeBox == nullptr)
                return;

            const auto name = safeBox->getTextEditorContents ("name").trim();
            if (name.isNotEmpty())
                safe->savePreset (name);
        }), true);
    }

    void savePreset (const juce::String& name)
    {
        namespace pp = ptl::pluginhost::presets;
        juce::MemoryBlock mb;

        if (pp::captureState (*plugin, mb) && pp::writeState (pp::presetFile (*plugin, name), mb))
            setStatus (name);
        else
            setStatus ("could not save");
    }

    // After the state changed from outside, rebuild the editor so its knobs show the new values.
    void refreshEditor();

    void setStatus (const juce::String& s)  { status = s; repaint(); }

    te::Plugin::Ptr plugin;
    juce::String status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPresetBar)
};

//==============================================================================
/*  Content of a plugin window: preset bar on top, the plugin's editor below. */
class PluginEditorHolder : public juce::Component
{
public:
    PluginEditorHolder (te::Plugin& p, juce::AudioProcessorEditor& ed, std::function<void()> refresh)
        : bar (p), editor (ed), onRefresh (std::move (refresh))
    {
        addAndMakeVisible (bar);
        addAndMakeVisible (editor);
        setSize (juce::jmax (editor.getWidth(), 240), editor.getHeight() + PluginPresetBar::kHeight);
    }

    ~PluginEditorHolder() override { removeChildComponent (&editor); }

    void resized() override
    {
        inResized = true;
        auto r = getLocalBounds();
        bar.setBounds (r.removeFromTop (PluginPresetBar::kHeight));
        editor.setBounds (r);
        inResized = false;
    }

    // The plugin changed its own editor size (some do): follow it.
    void childBoundsChanged (juce::Component* c) override
    {
        if (c == &editor && ! inResized)
        {
            const int w = juce::jmax (editor.getWidth(), 240);
            const int h = editor.getHeight() + PluginPresetBar::kHeight;

            if (w != getWidth() || h != getHeight())
                setSize (w, h);
        }
    }

    void refresh() { if (onRefresh) onRefresh(); }

private:
    PluginPresetBar bar;
    juce::AudioProcessorEditor& editor;
    std::function<void()> onRefresh;
    bool inResized = false;
};

inline void PluginPresetBar::refreshEditor()
{
    if (auto* h = findParentComponentOfClass<PluginEditorHolder>())
        h->refresh();
}

//==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (te::Plugin& p)
        : juce::DocumentWindow (makeTitle (p), ptl::theme::bgPrimary,
                                juce::DocumentWindow::closeButton, true),
          plugin (p)
    {
        setUsingNativeTitleBar (true);
        recreateEditor();

        centreWithSize (getWidth(), getHeight());
        placeBesideOpenEditor();
        keepOnScreen();
        openWindows().add (this);
        setVisible (true);

        // Above the main app, always (needs the native peer, so after setVisible).
        ptl::pluginhost::setOwnerWindow (*this, ptl::pluginhost::getMainWindow());
        toFront (true);
    }

    ~PluginWindow() override
    {
        openWindows().removeFirstMatchingValue (this);
        clearContentComponent();
        holder.reset();      // holder -> editor -> plugin instance / this window
        editor.reset();
    }

    // Title shown next to the app icon in the (native) title bar:  [icon]  Autokey - pt.Live
    static juce::String makeTitle (te::Plugin& p)
    {
        const auto name = p.getName().trim();
        return name.isEmpty() ? juce::String ("pt.Live") : name + " - pt.Live";
    }

    // Returns nullptr when there is nothing to show (plugin failed to load).
    static std::unique_ptr<juce::Component> create (te::Plugin& plugin)
    {
        if (ptl::pluginhost::ensureLoaded (plugin).isNotEmpty())
            return {};

        // At most kMaxOpenEditors editors at once: the oldest ones close themselves to make room.
        closeOldestToFit (plugin);

        return std::make_unique<PluginWindow> (plugin);
    }

    // Plugin editor windows that are currently open (message thread only).
    static juce::Array<PluginWindow*>& openWindows()
    {
        static juce::Array<PluginWindow*> list;
        return list;
    }

    static constexpr int kMaxOpenEditors = 2;

    // Makes room for ONE new editor: closes the oldest open editors (list order = opening order)
    // until at most kMaxOpenEditors - 1 others remain. `keep` is never closed.
    static void closeOldestToFit (const te::Plugin& keep)
    {
        const auto snapshot = openWindows();
        int others = 0;

        for (auto* w : snapshot)
            if (&w->plugin != &keep)
                ++others;

        for (auto* w : snapshot)
        {
            if (others <= kMaxOpenEditors - 1)
                break;

            if (openWindows().contains (w) && &w->plugin != &keep)   // may already be gone
            {
                w->closeSelf();
                --others;
            }
        }
    }

    void closeSelf()
    {
        if (plugin.windowState != nullptr)
            plugin.windowState->closeWindowExplicitly();     // deletes this window
    }

    void recreateEditorAsync()
    {
        juce::Component::SafePointer<PluginWindow> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->recreateEditor(); });
    }

    void closeButtonPressed() override
    {
        closeSelf();
    }

private:
    void recreateEditor()
    {
        clearContentComponent();
        holder.reset();
        editor.reset();

        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&plugin))
        {
            if (auto* instance = ext->getAudioPluginInstance())
                editor = ptl::pluginhost::createEditorFor (*instance);
        }

        if (editor != nullptr)
        {
            // preset bar on top + the plugin's editor below; the window takes their combined size
            holder = std::make_unique<PluginEditorHolder> (plugin, *editor, [this] { recreateEditorAsync(); });
            setContentNonOwned (holder.get(), true);
            setResizable (editor->isResizable(), false);
        }
    }

    // When another editor is already open, sit next to it (right side, else left) instead of covering it.
    void placeBesideOpenEditor()
    {
        PluginWindow* other = openWindows().isEmpty() ? nullptr : openWindows().getLast();
        if (other == nullptr)
            return;

        auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (other->getBounds());
        const auto area = display != nullptr ? display->userArea : juce::Rectangle<int> (0, 0, 1920, 1080);
        const auto ob = other->getBounds();

        if (ob.getRight() + getWidth() <= area.getRight())
            setTopLeftPosition (ob.getRight(), ob.getY());
        else if (ob.getX() - getWidth() >= area.getX())
            setTopLeftPosition (ob.getX() - getWidth(), ob.getY());
        // else: not enough room for side by side, keepOnScreen() keeps it reachable
    }

    // Big editors must never end up with their title bar / close button off the work area.
    void keepOnScreen()
    {
        auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds());

        if (display == nullptr)
            return;

        const auto area = display->userArea;
        const auto b = getBounds();

        setTopLeftPosition (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight()  - b.getWidth()),  b.getX()),
                            juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - b.getHeight()), b.getY()));
    }

    te::Plugin& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<PluginEditorHolder> holder;          // declared after editor => destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

//==============================================================================
class PtUIBehaviour : public te::UIBehaviour
{
public:
    PtUIBehaviour() = default;

    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto* ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return PluginWindow::create (ws->plugin);

        return {};
    }

    void recreatePluginWindowContentAsync (te::Plugin& p) override
    {
        if (p.windowState != nullptr)
            if (auto* w = dynamic_cast<PluginWindow*> (p.windowState->pluginWindow.get()))
                w->recreateEditorAsync();
    }
};
