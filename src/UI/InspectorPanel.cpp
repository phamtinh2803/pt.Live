/*
    pt.Live - Live Singing DAW
    InspectorPanel implementation  (spec section 7)

    Layout (300px wide, all measured in layoutFor()):
        type tag (16) | header (24) | 10 | LEVEL card | INSERTS card | SENDS card
    Every card = stage body + raised 20px caption strip + rule + outline.
*/

#include "include/Audio/PtLog.h"
#include <algorithm>
#include "include/UI/InspectorPanel.h"
#include "include/UI/PluginScanner.h"
#include "include/UI/UIWidgets.h"
#include "include/Audio/PluginWindow.h"
#include "include/Audio/AudioEngine.h"
#include "include/Theme.h"

using namespace ptl;

namespace
{
    constexpr int kRowH = 26;          // insert / send / add-button height
    constexpr int kRowGap = 4;
    constexpr int kCaptionH = 20;
    constexpr int kCardPad = 8;
    constexpr int kInnerGap = 6;       // caption -> content
    constexpr int kCardGap = 8;
    constexpr int kKnobH = 84;
    constexpr int kHeaderH = 26;           // the track name, under the type tag
    constexpr int kKnobGrowMax = 30;       // the knobs may grow this much when the panel is taller than its content
    constexpr int kSendSliderW = 134;     // send gain slider (96 x 1.4); the rest of the row is the name label

    // Row fill shared by insert + send rows: white alpha 0.075 (hover 0.115)
    void paintSlotRow(juce::Graphics& g, juce::Rectangle<int> bounds, bool hover, bool enabled, bool railOn)
    {
        auto b = bounds.toFloat();

        if (enabled)
        {
            g.setColour(juce::Colours::white.withAlpha(hover ? 0.115f : 0.075f));
            g.fillRoundedRectangle(b, theme::radius);
        }
        else
        {
            g.setColour(theme::outline);
            g.drawRoundedRectangle(b.reduced(0.5f), theme::radius, 1.0f);
        }

        g.setColour(railOn ? theme::accent : theme::outlineStrong);
        g.fillRoundedRectangle(b.getX(), b.getY(), 2.0f, b.getHeight(), 1.0f);
    }


    // Rounded, softer search field: pill-ish background, magnifier icon, thin outline that
    // warms up to the accent colour on focus. Text is vertically centred.
    class SoftSearchBox : public juce::TextEditor
    {
    public:
        static constexpr float kCorner = 9.0f;
        static constexpr int   kIconW  = 30;    // space reserved on the left for the magnifier

        SoftSearchBox()
        {
            setMultiLine(false);
            setReturnKeyStartsNewLine(false);
            setJustification(juce::Justification::centredLeft);   // <- vertical centring
            setBorder(juce::BorderSize<int>(0));                  // no hidden 1/3px border skewing the text
            setIndents(kIconW, 0);
            setScrollbarsShown(false);
            setCaretVisible(true);

            // We draw background + outline ourselves (rounded), so silence the stock square ones.
            setColour(juce::TextEditor::backgroundColourId,       juce::Colours::transparentBlack);
            setColour(juce::TextEditor::outlineColourId,          juce::Colours::transparentBlack);
            setColour(juce::TextEditor::focusedOutlineColourId,   juce::Colours::transparentBlack);
            setColour(juce::TextEditor::textColourId,             theme::text);
            setColour(juce::TextEditor::highlightColourId,        theme::accent.withAlpha(0.35f));
            setColour(juce::TextEditor::highlightedTextColourId,  theme::text);
            setColour(juce::CaretComponent::caretColourId,        theme::accent);
        }

        void paint(juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(theme::stage.brighter(0.04f));
            g.fillRoundedRectangle(b, kCorner);
        }

        void paintOverChildren(juce::Graphics& g) override
        {
            juce::TextEditor::paintOverChildren(g);   // placeholder text

            auto b = getLocalBounds().toFloat().reduced(0.5f);
            const bool focused = hasKeyboardFocus(true);

            if (focused)
            {
                // soft glow + accent line
                g.setColour(theme::accent.withAlpha(0.18f));
                g.drawRoundedRectangle(b.expanded(1.0f), kCorner + 1.0f, 2.0f);
                g.setColour(theme::accent.withAlpha(0.85f));
            }
            else
                g.setColour(theme::outline.withAlpha(0.8f));

            g.drawRoundedRectangle(b, kCorner, 1.0f);

            // magnifier icon
            const float cx = 15.0f, cy = (float) getHeight() * 0.5f - 1.0f, r = 4.5f;
            g.setColour(focused ? theme::textDim : theme::textFaint);
            g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.4f);
            g.drawLine(cx + r * 0.72f, cy + r * 0.72f, cx + r + 3.2f, cy + r + 3.2f, 1.4f);
        }

        void focusGained(FocusChangeType t) override { juce::TextEditor::focusGained(t); repaint(); }
        void focusLost(FocusChangeType t) override   { juce::TextEditor::focusLost(t);   repaint(); }
    };

    // Searchable plugin picker shown in a call-out under "Add insert".
    // Type to filter (every word must match the plugin name or manufacturer),
    // Up/Down to move, Enter or click to choose, Esc to close.
    class PluginPicker : public juce::Component,
                         private juce::ListBoxModel,
                         private juce::KeyListener
    {
    public:
        using PickFn = std::function<void(const juce::PluginDescription&)>;

        PluginPicker(juce::Array<juce::PluginDescription> pluginsIn, PickFn onPickIn)
            : all(std::move(pluginsIn)), onPick(std::move(onPickIn))
        {
            // Stable, readable order: by name, then manufacturer.
            std::stable_sort(all.begin(), all.end(), [](const juce::PluginDescription& a,
                                                         const juce::PluginDescription& b)
            {
                const int c = a.name.compareNatural(b.name, false);
                return c != 0 ? c < 0 : a.manufacturerName.compareNatural(b.manufacturerName, false) < 0;
            });

            search.setTextToShowWhenEmpty("Search plugins...", theme::textFaint);
            search.setFont(theme::font(theme::szBody));
            search.setSelectAllWhenFocused(true);
            search.onTextChange = [this] { refilter(); };
            search.onEscapeKey = [this] { close(); };
            search.addKeyListener(this);
            addAndMakeVisible(search);

            list.setModel(this);
            list.setRowHeight(kItemH);
            list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
            list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
            list.setMouseMoveSelectsRows(true);
            list.getVerticalScrollBar().setAutoHide(true);
            addAndMakeVisible(list);

            emptyLabel.setText("No plugins match", juce::dontSendNotification);
            emptyLabel.setFont(theme::font(theme::szSmall));
            emptyLabel.setColour(juce::Label::textColourId, theme::textFaint);
            emptyLabel.setJustificationType(juce::Justification::centred);
            emptyLabel.setInterceptsMouseClicks(false, false);
            addChildComponent(emptyLabel);

            setSize(320, 372);
            refilter();

            // The call-out has to be on screen before the editor can take keyboard focus.
            juce::Component::SafePointer<PluginPicker> safe(this);
            juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->search.grabKeyboardFocus(); });
        }

        ~PluginPicker() override { search.removeKeyListener(this); }

        void paint(juce::Graphics& g) override { g.fillAll(theme::raised); }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            search.setBounds(r.removeFromTop(34));
            r.removeFromTop(8);
            list.setBounds(r);
            emptyLabel.setBounds(r);
        }

    private:
        static constexpr int kItemH = 28;

        void refilter()
        {
            const auto words = juce::StringArray::fromTokens(search.getText().trim(), " ", "\"");

            shown.clear();
            for (const auto& d : all)
            {
                bool ok = true;
                for (const auto& w : words)
                    if (! d.name.containsIgnoreCase(w) && ! d.manufacturerName.containsIgnoreCase(w))
                    {
                        ok = false;
                        break;
                    }

                if (ok)
                    shown.add(d);
            }

            list.updateContent();
            list.repaint();
            emptyLabel.setVisible(shown.isEmpty());

            if (! shown.isEmpty())
            {
                list.selectRow(0);
                list.scrollToEnsureRowIsOnscreen(0);
            }
        }

        void moveSelection(int delta)
        {
            if (shown.isEmpty())
                return;

            const int row = juce::jlimit(0, shown.size() - 1, juce::jmax(0, list.getSelectedRow()) + delta);
            list.selectRow(row);
            list.scrollToEnsureRowIsOnscreen(row);
        }

        void close()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        void pick(int row)
        {
            if (! juce::isPositiveAndBelow(row, shown.size()))
                return;

            // Close first, insert afterwards: the insert may open alerts and this picker
            // is deleted together with its call-out box.
            const auto desc = shown.getReference(row);
            auto cb = onPick;
            juce::Component::SafePointer<juce::CallOutBox> box(findParentComponentOfClass<juce::CallOutBox>());

            juce::MessageManager::callAsync([box, desc, cb]
            {
                if (box != nullptr)
                    box->dismiss();

                if (cb)
                    cb(desc);
            });
        }

        // KeyListener (keys typed in the search box) -------------------------------------
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            if (key == juce::KeyPress::downKey)   { moveSelection(+1);  return true; }
            if (key == juce::KeyPress::upKey)     { moveSelection(-1);  return true; }
            if (key == juce::KeyPress::pageDownKey) { moveSelection(+8); return true; }
            if (key == juce::KeyPress::pageUpKey)   { moveSelection(-8); return true; }

            if (key == juce::KeyPress::returnKey)
            {
                pick(list.getSelectedRow());
                return true;
            }

            return false;
        }

        // ListBoxModel ---------------------------------------------------------------
        int getNumRows() override { return shown.size(); }

        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow(row, shown.size()))
                return;

            const auto& d = shown.getReference(row);

            if (selected)
            {
                g.setColour(theme::selection);
                g.fillRoundedRectangle(2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 7.0f);
            }

            const auto fmt = d.pluginFormatName == "VST" ? juce::String("VST2") : d.pluginFormatName;
            const int fmtW = 38, makerW = juce::jmin(110, w / 3);

            g.setColour(theme::text);
            g.setFont(theme::font(theme::szBody));
            g.drawText(d.name, 10, 0, w - 10 - fmtW - makerW - 12, h, juce::Justification::centredLeft, true);

            g.setColour(theme::textDim);
            g.setFont(theme::font(theme::szSmall));
            g.drawText(d.manufacturerName, w - fmtW - makerW - 6, 0, makerW, h,
                       juce::Justification::centredRight, true);

            g.setColour(theme::textFaint);
            g.setFont(theme::font(theme::szMicro));
            g.drawText(fmt, w - fmtW - 4, 0, fmtW, h, juce::Justification::centredRight, true);
        }

        void listBoxItemClicked(int row, const juce::MouseEvent&) override { pick(row); }
        void returnKeyPressed(int row) override { pick(row); }

        juce::Array<juce::PluginDescription> all, shown;
        PickFn onPick;

        SoftSearchBox search;
        juce::ListBox list { "plugins", nullptr };
        juce::Label emptyLabel;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginPicker)
    };

    // Section bypass: stored on the track's state (so it is saved with the session) plus, on every
    // plugin the bypass switched off, a marker telling that it was ON before (restored on un-bypass).
    const juce::Identifier kInsertsBypassId { "ptlInsertsBypass" };
    const juce::Identifier kSendsBypassId   { "ptlSendsBypass" };
    const juce::Identifier kWasOnId         { "ptlWasOn" };

    // Power glyph in the card caption, left of the collapse button. Lit (amber) = section bypassed.
    class BypassButton : public juce::Button
    {
    public:
        BypassButton() : juce::Button(juce::String()) {}

        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const bool on = getToggleState();

            if (on)
            {
                g.setColour(theme::warning.withAlpha(0.22f));
                g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            }

            g.setColour(on ? theme::warning : (over ? theme::text : theme::textDim));

            const auto b = getLocalBounds().toFloat().reduced(3.5f);
            const auto c = b.getCentre();
            const float r = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;

            juce::Path arc;
            arc.addCentredArc(c.x, c.y + 0.5f, r, r, 0.0f,
                              juce::degreesToRadians(40.0f), juce::degreesToRadians(320.0f), true);
            g.strokePath(arc, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.drawLine(c.x, c.y - r - 0.5f, c.x, c.y, 1.3f);
        }
    };
}

//==============================================================================
// One insert (external plugin) row:  [x] Name   [delete]      (click the row / name = open the editor)
class InspectorPanel::InsertRow : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    std::function<void(InsertRow*, int)> onDrag;       // dragged row, y in the parent's coordinates
    std::function<void(InsertRow*)> onDragEnd;         // button released after a drag
    te::Plugin* getPlugin() const { return plugin.get(); }

    InsertRow(te::Plugin::Ptr p, std::function<void()> removed)
        : plugin(std::move(p)), onRemoved(std::move(removed)),
          deleteButton(icons::Id::Trash, ui::IconButton::Style::Delete)
    {
        missing = ptl::pluginhost::isMissing(*plugin);

        enableToggle.setToggleState(plugin->isEnabled(), juce::dontSendNotification);
        enableToggle.onClick = [this]
        {
            plugin->setEnabled(enableToggle.getToggleState());
            nameLabel.setColour(juce::Label::textColourId, plugin->isEnabled() ? theme::text : theme::textFaint);
            repaint();
        };
        addAndMakeVisible(enableToggle);

        nameLabel.setText(plugin->getName(), juce::dontSendNotification);
        nameLabel.setFont(theme::font(theme::szBody));
        nameLabel.setColour(juce::Label::textColourId, plugin->isEnabled() ? theme::text : theme::textFaint);
        nameLabel.setInterceptsMouseClicks(false, false);     // a click anywhere on the row opens the editor
        addAndMakeVisible(nameLabel);

        deleteButton.setFillColour(juce::Colours::transparentBlack);
        addAndMakeVisible(deleteButton);

        setTooltip(missing ? "Plugin not found in the scanned list - click for details"
                           : "Click to open the plugin editor - drag to reorder");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);

        deleteButton.setTooltip("Remove insert");
        deleteButton.onClick = [this]
        {
            plugin->deleteFromParent();
            if (onRemoved)
                onRemoved();
        };

        addMouseListener(this, true);
    }

    // One click on the row (the toggle and the delete button handle their own clicks) opens the
    // editor. A quick double-click must not open it twice (the second one would toggle it away).
    // Hold the mouse on a row and drag: the row follows the pointer and drops onto another insert's slot,
    // which swaps their positions in the chain.
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.eventComponent != this || e.mods.isPopupMenu() || ! onDrag)
            return;

        if (! dragging)
        {
            if (e.getDistanceFromDragStart() < 5)
                return;
            dragging = true;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            toFront(false);
            repaint();
        }

        if (auto* parent = getParentComponent())
            onDrag(this, e.getEventRelativeTo(parent).getPosition().y);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging)
        {
            dragging = false;
            repaint();
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            if (onDragEnd)
                onDragEnd(this);
            return;
        }

        if (e.eventComponent != this || e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown())
            return;

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastOpenMs < 700)
            return;

        lastOpenMs = now;
        openEditor();
    }

    // The INSERTS section is bypassed: this plugin stays off and its own toggle is greyed out.
    void setLocked(bool shouldBeLocked)
    {
        locked = shouldBeLocked;
        enableToggle.setEnabled(! locked);
        repaint();
    }

    // Follow the plugin when it is switched from outside this row (control bar REMIX buttons).
    void syncFromPlugin()
    {
        // The red border follows the scan: it disappears once the plugin is scanned / installed.
        const bool nowMissing = ptl::pluginhost::isMissing(*plugin);
        if (nowMissing != missing)
        {
            missing = nowMissing;
            setTooltip(missing ? "Plugin not found in the scanned list - click for details"
                               : "Click to open the plugin editor - drag to reorder");
            repaint();
        }

        const bool on = plugin->isEnabled();
        if (enableToggle.getToggleState() != on)
        {
            enableToggle.setToggleState(on, juce::dontSendNotification);
            nameLabel.setColour(juce::Label::textColourId, on ? theme::text : theme::textFaint);
            repaint();
        }
    }

    void mouseEnter(const juce::MouseEvent&) override { updateHover(); }
    void mouseExit(const juce::MouseEvent&) override  { updateHover(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(3, 0);
        r.removeFromLeft(4);                                  // clearance for the rail
        enableToggle.setBounds(r.removeFromLeft(22));
        deleteButton.setBounds(r.removeFromRight(18).withSizeKeepingCentre(18, 18));
        r.removeFromRight(2);
        nameLabel.setBounds(r.reduced(4, 0));
    }

    void paint(juce::Graphics& g) override
    {
        if (dragging)
        {
            // Picked up: a translucent neutral "ghost" frame (the whole row is drawn at reduced alpha by the panel
            // while dragging, see dragInsert). No coloured outline: it only floats a little to the right of its slot.
            g.setColour(theme::raisedHi);
            g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::radius);
        }
        else
            paintSlotRow(g, getLocalBounds(), hover, plugin->isEnabled(), plugin->isEnabled());

        // Missing plugin: the .pt references a VST that is not in the scanned list. Red border so
        // it is easy to spot (stronger than the heavy-plugin one; it is never loaded).
        if (missing)
        {
            g.setColour(theme::missingPluginBorder.withAlpha(0.14f));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), theme::radius);
            g.setColour(theme::missingPluginBorder);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), theme::radius, 1.5f);
        }

        // Heavy plugin warning: while PDC is ON, a red border marks a plugin whose reported
        // latency is high enough to delay the whole mix. Nothing is bypassed
        // automatically - the user decides whether to disable, replace or reconfigure it.
        if (! missing && AudioEngine::isPDCEnabled() && AudioEngine::isHeavyPlugin(*plugin))
        {
            g.setColour(theme::heavyPluginBorder);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), theme::radius, 1.5f);
        }
    }

private:
    void updateHover()
    {
        const bool over = isMouseOver(true);
        if (over != hover)
        {
            hover = over;
            repaint();
        }
    }

    // Opens the plugin's editor window. The VST3 is (re)initialised first, so a plugin that
    // is loaded lazily - or whose instance was dropped - still gets a proper editor. When it
    // really cannot be loaded, the user is told WHY instead of a generic message.
    void openEditor()
    {
        openAttempts = 0;

        // Not scanned / not installed: tell the user and STOP. Nothing is loaded, no retry loop,
        // so the app cannot freeze on a plugin it cannot instantiate.
        if (ptl::pluginhost::isMissing(*plugin))
        {
            missing = true;
            repaint();

            if (! warningShown)
            {
                warningShown = true;
                juce::Component::SafePointer<InsertRow> safe (this);

                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Plugin not found",
                    ptl::pluginhost::missingMessage(plugin->getName()), "OK", nullptr,
                    juce::ModalCallbackFunction::create([safe](int)
                    {
                        if (safe != nullptr)
                            safe->warningShown = false;
                    }));
            }
            return;
        }

        tryOpenEditor();
    }

    void tryOpenEditor()
    {
        const bool wasOn = plugin->isEnabled();
        const auto problem = ptl::pluginhost::ensureLoaded (*plugin);

        // ensureLoaded() switches a lazily-loaded, disabled plugin on: inside a bypassed section it
        // must stay off (the editor still opens).
        if (locked && ! wasOn && plugin->isEnabled())
            plugin->setEnabled (false);

        if (problem.isEmpty())
        {
            // ensureLoaded() may have re-enabled a bypassed plugin: keep the toggle truthful.
            enableToggle.setToggleState (plugin->isEnabled(), juce::dontSendNotification);
            nameLabel.setColour (juce::Label::textColourId, plugin->isEnabled() ? theme::text : theme::textFaint);
            repaint();

            plugin->showWindowExplicitly();
            return;
        }

        // Some plugins finish loading a moment later: retry a few times before giving up.
        if (++openAttempts < 6)
        {
            juce::Component::SafePointer<InsertRow> safe (this);
            juce::Timer::callAfterDelay (250, [safe] { if (safe != nullptr) safe->tryOpenEditor(); });
            return;
        }

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin",
            "\"" + plugin->getName() + "\" failed to load, so it has no editor window.\n\n" + problem);
    }

    te::Plugin::Ptr plugin;
    std::function<void()> onRemoved;
    bool hover = false;
    bool locked = false;
    bool missing = false;          // VST not in the scanned list -> red border, never loaded
    bool warningShown = false;     // one "Plugin not found" box at a time
    int openAttempts = 0;
    juce::uint32 lastOpenMs = 0;
    bool dragging = false;          // being dragged to another position in the chain

    juce::ToggleButton enableToggle;
    juce::Label nameLabel;
    ui::IconButton deleteButton;
};

//==============================================================================
// Bigger, softer popup menu used by "+ Add send": rounded card, tall rows, rounded hover
// pill, a coloured dot per target (accent = Master, grey = FX channel).
class SendMenuLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr int kItemH  = 36;
    static constexpr int kBorder = 6;
    static constexpr int kMinW   = 180;

    SendMenuLookAndFeel()
    {
        // Slightly translucent so the window is non-opaque and the rounded corners show.
        setColour(juce::PopupMenu::backgroundColourId, theme::raised.withAlpha(0.99f));
    }

    juce::Font getPopupMenuFont() override { return theme::font(theme::szLabel); }
    int getPopupMenuBorderSize() override  { return kBorder; }

    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int,
                                   int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 11;
            return;
        }

        juce::GlyphArrangement ga;
        ga.addLineOfText(theme::font(theme::szLabel), text, 0.0f, 0.0f);
        idealWidth  = juce::jmax(kMinW, 14 + 14 + 10 + (int) std::ceil(ga.getBoundingBox(0, -1, true).getWidth()) + 16);
        idealHeight = kItemH;
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        auto b = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height);
        g.setColour(theme::raised);
        g.fillRoundedRectangle(b, theme::radiusLg + 2.0f);
        g.setColour(theme::outlineStrong);
        g.drawRoundedRectangle(b.reduced(0.5f), theme::radiusLg + 2.0f, 1.0f);
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                           bool isActive, bool isHighlighted, bool, bool,
                           const juce::String& text, const juce::String&,
                           const juce::Drawable*, const juce::Colour*) override
    {
        if (isSeparator)
        {
            g.setColour(theme::outline);
            g.fillRect(area.reduced(12, 0).withSizeKeepingCentre(area.getWidth() - 24, 1));
            return;
        }

        auto r = area.reduced(2, 1);

        if (isHighlighted && isActive)
        {
            g.setColour(theme::selectionHover);
            g.fillRoundedRectangle(r.toFloat(), theme::radius + 1.0f);
        }

        const bool master = text.equalsIgnoreCase("master");
        const float dot = 8.0f;
        auto dotArea = juce::Rectangle<float>(dot, dot).withCentre({ (float) r.getX() + 16.0f, (float) r.getCentreY() });
        g.setColour((master ? theme::accent : theme::textDim).withMultipliedAlpha(isActive ? 1.0f : 0.4f));
        g.fillEllipse(dotArea);

        g.setColour(theme::text.withMultipliedAlpha(isActive ? 1.0f : 0.4f));
        g.setFont(theme::font(theme::szLabel, master));
        g.drawText(text, r.withTrimmedLeft(32).withTrimmedRight(12), juce::Justification::centredLeft, true);
    }
};

//==============================================================================
// One send row:  [x] FX 1 / Master  [=====-16.0 dB=====] [edit] [delete]
class InspectorPanel::SendRow : public juce::Component
{
public:
    SendRow(TrackManager& mgr, te::AuxSendPlugin* s, std::function<void()> removed)
        : trackManager(mgr), send(s), onRemoved(std::move(removed)),
          deleteButton(icons::Id::Trash, ui::IconButton::Style::Delete)
    {
        enableToggle.setToggleState(send->isEnabled(), juce::dontSendNotification);
        enableToggle.onClick = [this]
        {
            send->setEnabled(enableToggle.getToggleState());
            repaint();
        };
        addAndMakeVisible(enableToggle);

        // "MASTER" / FX channel name / "(missing FX)" — full width
        nameLabel.setText(mgr.getSendTargetName(send), juce::dontSendNotification);
        nameLabel.setFont(theme::font(theme::szBody));
        nameLabel.setColour(juce::Label::textColourId, theme::text);
        nameLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(nameLabel);

        gainSlider.setSliderStyle(juce::Slider::LinearBar);
        gainSlider.setRange(-60.0, 6.02, 0.1);
        gainSlider.setDoubleClickReturnValue(true, -16.0);
        gainSlider.setTextValueSuffix(" dB");
        gainSlider.setColour(juce::Slider::backgroundColourId, theme::stage);
        gainSlider.setColour(juce::Slider::trackColourId, theme::textFaint);   // grey bar
        gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text);
        gainSlider.setValue(send->getGainDb(), juce::dontSendNotification);
        gainSlider.onValueChange = [this] { send->setGainDb(static_cast<float>(gainSlider.getValue())); };
        addAndMakeVisible(gainSlider);

        deleteButton.setFillColour(juce::Colours::transparentBlack);
        addAndMakeVisible(deleteButton);

        deleteButton.setTooltip("Remove send");
        deleteButton.onClick = [this]
        {
            send->deleteFromParent();
            if (onRemoved)
                onRemoved();
        };

        addMouseListener(this, true);
    }

    // The SENDS section is bypassed: this send stays off and its own toggle is greyed out.
    void setLocked(bool locked)
    {
        enableToggle.setEnabled(! locked);
        repaint();
    }

    // Re-read the plugin: the send may have been switched / changed from outside this row
    // (control bar VANG button). Never touches a control the user is holding.
    void syncFromPlugin()
    {
        const bool on = send->isEnabled();
        if (enableToggle.getToggleState() != on)
        {
            enableToggle.setToggleState(on, juce::dontSendNotification);
            repaint();
        }

        if (! gainSlider.isMouseButtonDown())
        {
            const double g = send->getGainDb();
            if (std::abs(gainSlider.getValue() - g) > 0.04)
                gainSlider.setValue(g, juce::dontSendNotification);
        }
    }

    void mouseEnter(const juce::MouseEvent&) override { updateHover(); }
    void mouseExit(const juce::MouseEvent&) override  { updateHover(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(3, 0);
        r.removeFromLeft(4);                                  // clearance for the rail
        enableToggle.setBounds(r.removeFromLeft(22));
        deleteButton.setBounds(r.removeFromRight(18).withSizeKeepingCentre(18, 18));
        r.removeFromRight(2);
        gainSlider.setBounds(r.removeFromRight(kSendSliderW).reduced(1, 4));
        nameLabel.setBounds(r.reduced(2, 0));                 // full name: "FX 1", "FX 2", "MASTER"
    }

    void paint(juce::Graphics& g) override
    {
        paintSlotRow(g, getLocalBounds(), hover, true, send->isEnabled());
    }

private:
    void updateHover()
    {
        const bool over = isMouseOver(true);
        if (over != hover)
        {
            hover = over;
            repaint();
        }
    }

    TrackManager& trackManager;
    juce::ReferenceCountedObjectPtr<te::AuxSendPlugin> send;
    std::function<void()> onRemoved;
    bool hover = false;

    juce::ToggleButton enableToggle;
    juce::Label nameLabel;
    juce::Slider gainSlider;
    ui::IconButton deleteButton;
};

//==============================================================================
class InspectorPanel::Content : public juce::Component,
                                private juce::Timer
{
public:
    Content(TrackManager& m, PluginScanner* s)
        : trackManager(m), scanner(s)
    {
        for (auto* k : { &volumeKnob, &panKnob })
        {
            k->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);
            k->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 66, 18);
            k->setColour(juce::Slider::rotarySliderFillColourId, theme::accent);
            k->setColour(juce::Slider::rotarySliderOutlineColourId, theme::stage);
            k->setColour(juce::Slider::thumbColourId, theme::text);
            k->setColour(juce::Slider::textBoxTextColourId, theme::text);
            k->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addChildComponent(k);
        }

        volumeKnob.setRange(-60.0, 6.02, 0.1);
        volumeKnob.setSkewFactorFromMidPoint(-12.0);
        volumeKnob.setDoubleClickReturnValue(true, 0.0);
        volumeKnob.getProperties().set("unityPos", volumeKnob.valueToProportionOfLength(0.0));   // tick at 0 dB
        volumeKnob.textFromValueFunction = [](double v)
        {
            return v <= -59.9 ? juce::String("-inf dB") : juce::String(v, 1) + " dB";
        };
        volumeKnob.onValueChange = [this]
        {
            if (updating) return;
            if (auto* vp = trackManager.getVolumePlugin(track))
            {
                const double v = volumeKnob.getValue();
                vp->setVolumeDb(v <= -59.9 ? -100.0f : static_cast<float>(v));
            }
        };

        panKnob.setRange(-1.0, 1.0, 0.01);
        panKnob.setDoubleClickReturnValue(true, 0.0);
        panKnob.getProperties().set("bipolar", true);                                           // arc from 12 o'clock
        panKnob.textFromValueFunction = [](double v)
        {
            if (std::abs(v) < 0.005) return juce::String("C");
            return (v < 0 ? juce::String("L ") : juce::String("R ")) + juce::String(juce::roundToInt(std::abs(v) * 100));
        };
        panKnob.onValueChange = [this]
        {
            if (updating) return;
            if (auto* vp = trackManager.getVolumePlugin(track))
                vp->setPan(static_cast<float>(panKnob.getValue()));
        };

        for (auto* l : { &volCaption, &panCaption })
        {
            l->setFont(theme::font(theme::szMicro, true));
            l->setColour(juce::Label::textColourId, theme::textDim);
            l->setJustificationType(juce::Justification::centred);
            l->setInterceptsMouseClicks(false, false);
            addChildComponent(l);
        }
        volCaption.setText("VOLUME", juce::dontSendNotification);
        panCaption.setText("PAN", juce::dontSendNotification);

        startTimerHz(30);       // follow volume / pan changes made from elsewhere (control bar, automation)

        for (auto* b : { &addInsertBtn, &addSendBtn })
        {
            b->setColour(juce::TextButton::buttonColourId, theme::raisedHi);
            b->setColour(juce::TextButton::textColourOffId, theme::text);
            addChildComponent(b);
        }
        addInsertBtn.setButtonText("+   Add insert");
        addSendBtn.setButtonText("+   Add send");
        addInsertBtn.onClick = [this] { showInsertMenu(); };
        addSendBtn.onClick   = [this] { showSendMenu(); };

        // INSERTS / SENDS collapse and expand with ONE click on the card caption (mouseUp below).

        insertsBypassBtn.setTooltip("Bypass all inserts");
        sendsBypassBtn.setTooltip("Bypass all sends to FX channels");
        insertsBypassBtn.onClick = [this] { setInsertsBypassed(! insertsBypassed()); };
        sendsBypassBtn.onClick   = [this] { setSendsBypassed(! sendsBypassed()); };
        addChildComponent(insertsBypassBtn);
        addChildComponent(sendsBypassBtn);
    }

    std::function<void()> onLayoutNeeded;

    void setTrack(te::Track* t)
    {
        track = t;
        rebuild();
    }

    // The knobs used to read the plugin only in rebuild(), so a change made outside the inspector
    // (control bar BEAT / MICRO fader) never showed here. Poll it; never fight a knob being dragged.
    void timerCallback() override
    {
        if (track == nullptr || ! isShowing())
            return;

        // A scan finished (Plugin Browser): hand the new plugins to the ENGINE too, so inserts that
        // were marked missing (red border) can be recognised and opened without re-opening the file.
        if (scanner != nullptr && ! scanner->isScanning())
        {
            const auto scanned = scanner->getScannedPlugins();

            if (scanned.size() != lastScannedCount)
            {
                lastScannedCount = scanned.size();
                auto& known = trackManager.getEdit().engine.getPluginManager().knownPluginList;

                for (const auto& d : scanned)
                    known.addType(d);
            }
        }

        for (auto* row : sendRows)                 // tick / untick + gain follow the real send (e.g. VANG button)
            row->syncFromPlugin();
        for (auto* row : insertRows)               // same for inserts (e.g. REMIX buttons)
            row->syncFromPlugin();

        auto* vp = trackManager.getVolumePlugin(track);
        if (vp == nullptr)
            return;

        const juce::ScopedValueSetter<bool> guard(updating, true);

        if (! volumeKnob.isMouseButtonDown())
        {
            const double db = juce::jmax(-60.0, (double) vp->getVolumeDb());
            if (std::abs(volumeKnob.getValue() - db) > 0.04)
                volumeKnob.setValue(db, juce::dontSendNotification);
        }

        if (! panKnob.isMouseButtonDown() && panKnob.isVisible())
        {
            const double pan = vp->getPan();
            if (std::abs(panKnob.getValue() - pan) > 0.004)
                panKnob.setValue(pan, juce::dontSendNotification);
        }
    }

    void rebuild()
    {
        insertRows.clear();
        sendRows.clear();

        const bool has = (track != nullptr);
        kind = trackManager.getTrackType(track);
        const bool isMaster = has && kind == TrackManager::TrackType::Master;
        const bool isInput  = has && (kind == TrackManager::TrackType::MonoIn
                                   || kind == TrackManager::TrackType::StereoIn);
        // Input tracks AND FX channels have a SENDS card (the Master has none).
        const bool hasSends = isInput || (has && kind == TrackManager::TrackType::FXChannel);

        volumeKnob.setVisible(has);
        volCaption.setVisible(has);
        panKnob.setVisible(has && ! isMaster);
        panCaption.setVisible(has && ! isMaster);
        addInsertBtn.setVisible(has && ! insertsCollapsed);
        addSendBtn.setVisible(hasSends && ! sendsCollapsed);
        hasSendsSection = hasSends;
        insertsBypassBtn.setVisible(has);
        sendsBypassBtn.setVisible(hasSends);
        insertsBypassBtn.setToggleState(has && insertsBypassed(), juce::dontSendNotification);
        sendsBypassBtn.setToggleState(hasSends && sendsBypassed(), juce::dontSendNotification);

        if (has)
        {
            updating = true;
            if (auto* vp = trackManager.getVolumePlugin(track))
            {
                volumeKnob.setValue(vp->getVolumeDb(), juce::dontSendNotification);
                panKnob.setValue(vp->getPan(), juce::dontSendNotification);
            }
            updating = false;

            // A bypassed section stays bypassed for plugins / sends added after the bypass was pressed.
            if (insertsBypassed())
                applyInsertsBypass();
            if (hasSends && sendsBypassed())
                applySendsBypass();

            // Inserts: external (VST3) plugins on this track / master chain.
            for (auto* p : trackManager.getPluginList(track).getPlugins())
            {
                if (dynamic_cast<te::ExternalPlugin*>(p) == nullptr)
                    continue;

                auto* row = insertRows.add(new InsertRow(te::Plugin::Ptr(p), [this] { requestRebuild(); }));
                row->setLocked(insertsBypassed());
                row->onDrag    = [this](InsertRow* r, int y) { dragInsert(r, y); };
                row->onDragEnd = [this](InsertRow* r) { dropInsert(r); };
                addAndMakeVisible(row);
                row->setVisible(! insertsCollapsed);
            }

            // Sends (input tracks + FX channels)
            if (hasSends)
                if (auto* at = dynamic_cast<te::AudioTrack*>(track))
                    for (auto* s : trackManager.getSends(at))
                    {
                        auto* row = sendRows.add(new SendRow(trackManager, s, [this] { requestRebuild(); }));
                        row->setLocked(sendsBypassed());     // Master send locked too, like FX sends
                        addAndMakeVisible(row);
                        row->setVisible(! sendsCollapsed);
                    }
        }

        if (onLayoutNeeded)
            onLayoutNeeded();

        repaint();
    }

    // Height of the viewport the content sits in. When it is taller than the cards need, the spare height is
    // spent on bigger knobs and EMPTY slots (clickable, like an empty insert slot in a mixer) instead of a blank hole.
    void setAvailableHeight(int h) { availableH = h; }

    // Lays the children out for the given width and returns the total content height.
    int layoutFor(int width)
    {
        sections.clear();

        if (track == nullptr)
            return 200;

        const int nIns = insertRows.size(), nSnd = sendRows.size();

        // Height of everything for a given knob height / number of ghost slots (the same maths as the placement below).
        auto totalFor = [&](int knobH, int gIns, int gSnd)
        {
            int y = 12 + 4 + 16 + kHeaderH + kCardGap + 2;
            y += kCardPad * 2 + kCaptionH + kInnerGap + knobH + kCardGap;                              // LEVEL
            y += (insertsCollapsed ? kCaptionH + 2
                                   : kCardPad * 2 + kCaptionH + kInnerGap + (nIns + 1 + gIns) * kRowH + (nIns + gIns) * kRowGap) + kCardGap;
            if (hasSendsSection)
                y += (sendsCollapsed ? kCaptionH + 2
                                     : kCardPad * 2 + kCaptionH + kInnerGap + (nSnd + 1 + gSnd) * kRowH + (nSnd + gSnd) * kRowGap) + kCardGap;
            return y + 4;
        };

        // Spend the spare height: knobs first, then empty slots (inserts and sends in turn).
        const int gIns = 0, gSnd = 0;
        int knobH = kKnobH;
        {
            int spare = availableH - totalFor(knobH, 0, 0);

            if (spare > 0)
            {
                const int grow = juce::jmin(spare, kKnobGrowMax);
                knobH += grow;
                spare -= grow;
            }

        }

        // area = bounds.reduced(12); removeFromTop(4); tag(16); header(kHeaderH); gap(10)
        const int x = 12, w = width - 24;
        int y = 12 + 4;

        tagBounds = { x, y, w, 16 };
        y += 16;
        headerBounds = { x, y, w, kHeaderH };                 // track name (big)
        y += kHeaderH;
        y += kCardGap + 2;

        // LEVEL ---------------------------------------------------------------
        {
            const int h = kCardPad * 2 + kCaptionH + kInnerGap + knobH;
            juce::Rectangle<int> card(x, y, w, h);
            sections.push_back({ "LEVEL", card, false, nullptr });

            auto row = card.reduced(kCardPad);
            row.removeFromTop(kCaptionH + kInnerGap);

            if (kind == TrackManager::TrackType::Master)
            {
                auto col = row.withSizeKeepingCentre(juce::jmin(120, row.getWidth()), row.getHeight());
                volCaption.setBounds(col.removeFromTop(15));
                col.removeFromTop(2);
                volumeKnob.setBounds(col);
            }
            else
            {
                auto left  = row.removeFromLeft(row.getWidth() / 2);
                auto right = row;
                levelDividerX = right.getX();

                volCaption.setBounds(left.removeFromTop(15));
                left.removeFromTop(2);
                volumeKnob.setBounds(left);

                panCaption.setBounds(right.removeFromTop(15));
                right.removeFromTop(2);
                panKnob.setBounds(right);
            }
            y += h + kCardGap;
        }

        // INSERTS -------------------------------------------------------------
        {
            const int n = nIns;
            const int contentH = (n + 1 + gIns) * kRowH + (n + gIns) * kRowGap;
            const int h = insertsCollapsed ? kCaptionH + 2
                                           : kCardPad * 2 + kCaptionH + kInnerGap + contentH;
            juce::Rectangle<int> card(x, y, w, h);
            sections.push_back({ "INSERTS", card, true, &insertsBypassBtn });

            if (! insertsCollapsed)
            {
                auto inner = card.reduced(kCardPad);
                inner.removeFromTop(kCaptionH + kInnerGap);

                int ry = inner.getY();
                for (auto* row : insertRows)
                {
                    row->setBounds(inner.getX(), ry, inner.getWidth(), kRowH);
                    ry += kRowH + kRowGap;
                }
                addInsertBtn.setBounds(inner.getX(), ry, inner.getWidth(), kRowH);
                insertTopY = inner.getY();
            }
            y += h + kCardGap;
        }

        // SENDS (input tracks + FX channels) ------------------------------------
        if (hasSendsSection)
        {
            const int n = nSnd;
            const int contentH = (n + 1 + gSnd) * kRowH + (n + gSnd) * kRowGap;
            const int h = sendsCollapsed ? kCaptionH + 2
                                         : kCardPad * 2 + kCaptionH + kInnerGap + contentH;
            juce::Rectangle<int> card(x, y, w, h);
            sections.push_back({ "SENDS", card, true, &sendsBypassBtn });

            if (! sendsCollapsed)
            {
                auto inner = card.reduced(kCardPad);
                inner.removeFromTop(kCaptionH + kInnerGap);

                int ry = inner.getY();
                for (auto* row : sendRows)
                {
                    row->setBounds(inner.getX(), ry, inner.getWidth(), kRowH);
                    ry += kRowH + kRowGap;
                }
                addSendBtn.setBounds(inner.getX(), ry, inner.getWidth(), kRowH);
            }
            y += h + kCardGap;
        }

        // bypass buttons: 16x16 at the right end of each caption
        for (auto& s : sections)
        {
            const int by = s.bounds.getY() + (kCaptionH - 16) / 2;

            if (s.bypass != nullptr)
                s.bypass->setBounds(s.bounds.getRight() - 7 - 16, by, 16, 16);
        }

        return y + 4;
    }

    void paint(juce::Graphics& g) override
    {
        using namespace theme;

        if (track == nullptr)
            return;                                           // empty state: just the panel, no glyph / text

        // Drag in progress: the slot the held insert came from stays as a faint empty frame (no outline).
        if (dragFrom >= 0 && dragFrom < insertRows.size())
        {
            g.setColour(juce::Colours::white.withAlpha(0.04f));
            g.fillRoundedRectangle(insertSlotRect(dragFrom).toFloat(), radius);
        }

        // Type tag: "AUDIO INPUT · MONO", letter-spaced
        juce::String tag;
        switch (kind)
        {
            case TrackManager::TrackType::MonoIn:    tag = juce::String::fromUTF8("AUDIO INPUT \xc2\xb7 MONO"); break;
            case TrackManager::TrackType::StereoIn:  tag = juce::String::fromUTF8("AUDIO INPUT \xc2\xb7 STEREO"); break;
            case TrackManager::TrackType::FXChannel: tag = "FX CHANNEL";              break;
            case TrackManager::TrackType::Master:    tag = juce::String::fromUTF8("STEREO OUT \xc2\xb7 MASTER"); break;
        }

        g.setColour(textDim);
        g.setFont(spacedFont(szSmall, 0.3f, true));
        g.drawText(tag, tagBounds, juce::Justification::centredLeft, false);

        // Track name: fills the slot under the tag (it used to be an empty gap) and tells which track the cards below belong to.
        g.setColour(text);
        g.setFont(font(szTitle, true));
        g.drawText(kind == TrackManager::TrackType::Master ? juce::String("MASTER") : track->getName(),
                   headerBounds, juce::Justification::centredLeft, true);

        // Section cards
        for (auto& s : sections)
        {
            auto b = s.bounds.toFloat();

            g.setColour(stage);
            g.fillRoundedRectangle(b, radius);

            juce::Path strip;
            strip.addRoundedRectangle(b.getX(), b.getY(), b.getWidth(), (float) kCaptionH,
                                      radius, radius, true, true, false, false);
            g.setColour(raised);
            g.fillPath(strip);

            if (s.collapsible && s.title == hoverCaption)
            {
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.fillPath(strip);
            }

            g.setColour(outline);
            g.fillRect(b.getX() + 1.0f, b.getY() + (float) kCaptionH, b.getWidth() - 2.0f, 1.0f);

            // caption: uppercase, bold, heavily tracked, full-strength text colour
            g.setColour(text);
            g.setFont(spacedFont(szSmall, 1.1f, true));
            g.drawText(s.title,
                       s.bounds.withHeight(kCaptionH).withTrimmedLeft(9).withTrimmedRight(7),
                       juce::Justification::centredLeft, false);

            g.setColour(outline);
            g.drawRoundedRectangle(b.reduced(0.5f), radius, 1.0f);

            if (s.title == "LEVEL" && kind != TrackManager::TrackType::Master)
            {
                g.setColour(hairline);
                g.fillRect(levelDividerX, s.bounds.getY() + kCaptionH + 10, 1, s.bounds.getHeight() - kCaptionH - 20);
            }
        }
    }

private:
    struct Section { juce::String title; juce::Rectangle<int> bounds; bool collapsible; juce::Component* bypass = nullptr; };

    //=== Click on a caption = collapse / expand =====================================
    // Only INSERTS and SENDS are collapsible. The bypass button is a child component and takes
    // its own clicks, so only the rest of the caption strip reaches these handlers.
    const Section* collapsibleSectionAt(juce::Point<int> pos) const
    {
        for (auto& s : sections)
            if (s.collapsible && s.bounds.withHeight(kCaptionH).contains(pos))
                return &s;

        return nullptr;
    }

public:
    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto* s = collapsibleSectionAt(e.getPosition());
        const juce::String t = s != nullptr ? s->title : juce::String();

        setMouseCursor(s != nullptr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);

        if (t != hoverCaption)
        {
            hoverCaption = t;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverCaption.isNotEmpty())
        {
            hoverCaption = {};
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown())
            return;

        const auto* s = collapsibleSectionAt(e.getPosition());
        if (s == nullptr)
            return;

        if (s->title == "INSERTS")     insertsCollapsed = ! insertsCollapsed;
        else if (s->title == "SENDS")  sendsCollapsed   = ! sendsCollapsed;
        else                           return;

        rebuild();
    }

private:

    //=== Section bypass ==========================================================
    bool insertsBypassed() const { return track != nullptr && static_cast<bool>(track->state.getProperty(kInsertsBypassId, false)); }
    bool sendsBypassed()   const { return track != nullptr && static_cast<bool>(track->state.getProperty(kSendsBypassId, false)); }

    // Every VST insert of the track goes off (or back to what it was before the bypass).
    void applyInsertsBypass()
    {
        const bool on = insertsBypassed();

        for (auto* p : trackManager.getPluginList(track).getPlugins())
        {
            if (dynamic_cast<te::ExternalPlugin*>(p) == nullptr)
                continue;

            if (on)
            {
                if (p->isEnabled())                            // already off by the user: leave it off afterwards
                {
                    p->state.setProperty(kWasOnId, true, nullptr);
                    p->setEnabled(false);
                }
            }
            else if (static_cast<bool>(p->state.getProperty(kWasOnId, false)))
            {
                p->state.removeProperty(kWasOnId, nullptr);
                p->setEnabled(true);
            }
        }
    }

    // Every send goes off, INCLUDING the Master send. Unlike inserts, a send is not a route—it's
    // an AuxSendPlugin that can be toggled independently. Bypassing all sends = disable every AuxSend
    // plugin on the current track (FX sends + Master send both), letting the user mute outputs at once.
    void applySendsBypass()
    {
        auto* at = dynamic_cast<te::AudioTrack*>(track);
        if (at == nullptr)
            return;

        const bool on = sendsBypassed();

        for (auto* s : trackManager.getSends(at))
        {
            // Previously we skipped Master sends with 'continue'. That was wrong: Master send IS a send
            // and should be toggled by the "all sends bypass" button—just like in Cubase. We remove
            // that skip here so both FX sends AND the Master send are disabled together.
            if (on)
            {
                if (s->isEnabled())
                {
                    s->state.setProperty(kWasOnId, true, nullptr);
                    s->setEnabled(false);
                }
            }
            else if (static_cast<bool>(s->state.getProperty(kWasOnId, false)))
            {
                s->state.removeProperty(kWasOnId, nullptr);
                s->setEnabled(true);
            }
        }
    }

    void setInsertsBypassed(bool on)
    {
        if (track == nullptr)
            return;

        if (on) track->state.setProperty(kInsertsBypassId, true, nullptr);
        else    track->state.removeProperty(kInsertsBypassId, nullptr);

        applyInsertsBypass();
        rebuild();
    }

    void setSendsBypassed(bool on)
    {
        if (track == nullptr)
            return;

        if (on) track->state.setProperty(kSendsBypassId, true, nullptr);
        else    track->state.removeProperty(kSendsBypassId, nullptr);

        applySendsBypass();
        rebuild();
    }

    //=== Drag & drop: reorder inserts ============================================
    // The dragged row follows the pointer (vertically); the slot under it is outlined. Dropping on another
    // insert's slot moves the plugin there, so the chain order (= the order the sound passes through) changes.
    int slotAt(int y) const
    {
        const int n = insertRows.size();
        return n <= 0 ? -1 : juce::jlimit(0, n - 1, (y - insertTopY) / (kRowH + kRowGap));
    }

    juce::Rectangle<int> insertSlotRect(int slot) const
    {
        const int x = dragSlotW > 0 ? dragSlotX : (insertRows.isEmpty() ? 12 + kCardPad : insertRows[0]->getX());   // real column of the rows
        return { x, insertTopY + slot * (kRowH + kRowGap), dragSlotW > 0 ? dragSlotW : (insertRows.isEmpty() ? 0 : insertRows[0]->getWidth()), kRowH };
    }

    void dragInsert(InsertRow* row, int yInContent)
    {
        const int n = insertRows.size();
        if (n < 2)
            return;

        const int top = insertTopY, bottom = insertTopY + (n - 1) * (kRowH + kRowGap);

        if (dragFrom < 0)                                   // first move of this drag: dim everything, lift the held row
        {
            dragSlotW = row->getWidth();
            dragSlotX = row->getX();
            for (auto* r : insertRows)
                r->setAlpha(r == row ? 0.78f : 0.5f);       // all inserts fade a little, the held one is a see-through ghost
        }
        dragFrom = insertRows.indexOf(row);

        // Ghost frame: follows the pointer vertically and sits a few px to the RIGHT of the slot (Cubase-like).
        const auto home = insertSlotRect(dragFrom);
        row->setBounds(home.getX() + kGhostShift, juce::jlimit(top, bottom, yInContent - kRowH / 2),
                       juce::jmax(40, dragSlotW - kGhostShift), kRowH);

        const int slot = slotAt(yInContent);
        if (slot != dropIndex)
        {
            dropIndex = slot;
            repaint();
        }
    }

    void dropInsert(InsertRow* row)
    {
        const int from = insertRows.indexOf(row);
        const int to   = dropIndex;
        dropIndex = -1;
        dragFrom  = -1;
        endDragLook();

        if (from < 0 || to < 0 || from == to || track == nullptr)
        {
            updateLayoutAfterDrag();                        // everything snaps back to its own slot
            return;
        }

        te::Plugin* moved  = row->getPlugin();
        te::Plugin* target = insertRows[to]->getPlugin();

        auto parent = moved->state.getParent();
        const int fromIdx = parent.indexOf(moved->state);
        const int toIdx   = parent.indexOf(target->state);

        if (parent.isValid() && parent == target->state.getParent() && fromIdx >= 0 && toIdx >= 0)
        {
            auto& edit = trackManager.getEdit();
            parent.moveChild(fromIdx, toIdx, &edit.getUndoManager());
            edit.restartPlayback();                         // the processing order changed: rebuild the playback graph
        }

        requestRebuild();                                   // rows are re-created in the new order
    }

    // Back to the normal look: every insert fully opaque again, ghost shift forgotten.
    void endDragLook()
    {
        dragSlotW = 0;
        for (auto* r : insertRows)
            r->setAlpha(1.0f);
    }

    void updateLayoutAfterDrag()
    {
        dropIndex = -1;
        dragFrom  = -1;
        endDragLook();
        if (onLayoutNeeded)
            onLayoutNeeded();
        repaint();
    }

    // While dragging nothing is marked: no outline, no drop line (the faded rows + the ghost are enough).

    void requestRebuild()
    {
        juce::Component::SafePointer<Content> safe(this);
        juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->rebuild(); });
    }

    int insertionIndex() const
    {
        // Inserts go BEFORE the volume/pan fader.
        int idx = 0;
        for (auto* p : trackManager.getPluginList(track).getPlugins())
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*>(p) != nullptr)
                break;
            if (p->state.getProperty(juce::Identifier("ptlMonoFold"), false))   // hidden L->R fold stays last
                break;
            ++idx;
        }
        return idx;
    }

    void showInsertMenu()
    {
        if (track == nullptr || scanner == nullptr)
            return;

        if (scanner->getScannedPlugins().isEmpty())
            scanner->loadCachedPlugins();

        juce::Array<juce::PluginDescription> plugins;
        for (auto& d : scanner->getScannedPlugins())
            if (! d.isInstrument)
                plugins.add(d);

        if (plugins.isEmpty() && scanner->isSafeMode())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("Safe mode: plugin bị tắt"),
                juce::String::fromUTF8("Ở Safe mode không thêm được plugin. Đóng pt.Live rồi mở lại, không giữ Shift."));
            return;
        }

        if (plugins.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Add insert",
                "No plugins found yet. Press \"Plugins\" in the top bar to scan your plugin folders.");
            return;
        }

        juce::Component::SafePointer<Content> safe(this);
        auto picker = std::make_unique<PluginPicker>(plugins, [safe](const juce::PluginDescription& desc)
        {
            if (safe != nullptr)
                safe->insertPlugin(desc);
        });

        // Attach to the window so the search box gets normal keyboard focus.
        if (auto* top = getTopLevelComponent())
            juce::CallOutBox::launchAsynchronously(std::move(picker),
                                                   top->getLocalArea(&addInsertBtn, addInsertBtn.getLocalBounds()),
                                                   top);
        else
            juce::CallOutBox::launchAsynchronously(std::move(picker), addInsertBtn.getScreenBounds(), nullptr);
    }

    void insertPlugin(const juce::PluginDescription& desc)
    {
        if (track == nullptr)
            return;

        auto& edit = trackManager.getEdit();
        auto& list = trackManager.getPluginList(track);

        // The ENGINE re-creates an ExternalPlugin from its own KnownPluginList, which is
        // separate from the scanner used by the browser. Without this the plugin is inserted
        // but never instantiated ("failed to load, so it has no editor window").
        edit.engine.getPluginManager().knownPluginList.addType(desc);

        ptl::diag::write("---- INSERT '" + desc.name + "' (" + desc.pluginFormatName + ", "
                        + juce::String(desc.numInputChannels) + " in / " + juce::String(desc.numOutputChannels)
                        + " out per scanner) on " + trackManager.describeTrack(track));

        // The engine caps the number of plugins per track / master (EditLimits). If the list is
        // full, insertPlugin() below would silently drop the plugin (the editor would open and
        // then vanish). Report it instead.
        if (! list.canInsertPlugin())
        {
            const bool isMaster = trackManager.getTrackType(track) == TrackManager::TrackType::Master;
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Add insert",
                "This " + juce::String(isMaster ? "master bus" : "track") + " already has the maximum number of "
                + "plugins.\n\nRemove an insert first, then try again.");
            return;
        }

        if (auto plugin = edit.getPluginCache().createNewPlugin(te::ExternalPlugin::xmlTypeName, desc))
        {
            plugin->setEnabled(true);
            list.insertPlugin(plugin, insertionIndex(), nullptr);
            trackManager.logTrackState("after insertPlugin, before load", track);

            // Instantiate right away so a broken plugin is reported now, not on "edit".
            const auto problem = ptl::pluginhost::ensureLoaded(*plugin);

            // MONO IN track: force the plugin to a mono layout so the chain stays mono.
            ptl::diag::write("ensureLoaded -> " + (problem.isEmpty() ? juce::String("OK") : "PROBLEM: " + problem));
            trackManager.logTrackState("after load, before mono-fix", track);

            // First insert of this VST ever: keep its original state (for "Reset to factory"),
            // then apply the user's "Set as default" preset if there is one.
            if (problem.isEmpty())
                ptl::pluginhost::presets::onNewInsert(*plugin);

            if (problem.isEmpty())
                trackManager.applyTrackChannelLayout(dynamic_cast<te::AudioTrack*>(track));

            trackManager.logTrackState("after mono-fix", track);

            // The layout may change AFTER we return (playback graph rebuild, async plugin
            // init, plugin resetting its own buses). Re-dump the state later to catch that.
            {
                const auto trackId = track->itemID;
                auto* tm = &trackManager;
                auto* ed = &edit;

                for (int delayMs : { 300, 1500, 4000 })
                    juce::Timer::callAfterDelay(delayMs, [tm, ed, trackId, delayMs]
                    {
                        if (auto* t = te::findTrackForID(*ed, trackId))
                            tm->logTrackState("T+" + juce::String(delayMs) + "ms after insert", t);
                    });
            }

            if (problem.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Add insert",
                    "\"" + desc.name + "\" was added but could not be loaded.\n\n" + problem);
            else
            {
                // Open the editor right away (after the picker closed and the layout settled).
                // PtUIBehaviour keeps it above the main window and closes any other open editor.
                te::Plugin::Ptr toOpen (plugin.get());
                juce::MessageManager::callAsync([toOpen]
                {
                    if (toOpen != nullptr)
                        toOpen->showWindowExplicitly();
                });
            }
        }
        else
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Add insert", "Could not load this plugin.");

        rebuild();
    }

    void showSendMenu()
    {
        auto* at = dynamic_cast<te::AudioTrack*>(track);
        if (at == nullptr)
            return;

        // What this track can still send to:
        //  - "Master" (Send Master): Mono In / Stereo In / FX channels, at most one per track.
        //  - FX channels it does not send to yet. An FX channel never offers itself nor an FX
        //    channel that would close a feedback loop (FX1 -> FX2 -> FX1, or longer).
        const bool offerMaster = trackManager.canHaveMasterSend(at) && ! trackManager.hasMasterSend(at);

        juce::Array<int> candidateBuses;
        juce::StringArray candidateNames;
        for (auto* fx : trackManager.getSendCandidates(at))
        {
            candidateBuses.add(trackManager.getFxBus(fx));
            candidateNames.add(fx->getName());
        }

        if (! offerMaster && candidateBuses.isEmpty())
        {
            const char* msg = "This track already sends to Master and to every FX channel.";

            if (kind == TrackManager::TrackType::FXChannel)
                msg = "No FX channel available to send to (a send to itself, or one that would create a feedback loop, is not allowed).";
            else if (trackManager.getTracksOfType(TrackManager::TrackType::FXChannel).isEmpty())
                msg = "Create an FX channel first (+ in the FX CHANNELS group).";

            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Add send", msg);
            return;
        }

        constexpr int kMasterItemId = 100000;   // FX items use 1..n

        juce::PopupMenu menu;
        menu.setLookAndFeel(&sendMenuLnF);          // bigger, rounded menu (weak-referenced by the menu)
        if (offerMaster)
        {
            menu.addItem(kMasterItemId, "Master");
            if (! candidateBuses.isEmpty())
                menu.addSeparator();
        }
        for (int i = 0; i < candidateNames.size(); ++i)
            menu.addItem(i + 1, candidateNames[i]);

        juce::Component::SafePointer<Content> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addSendBtn)
                                                     .withMinimumWidth(addSendBtn.getWidth())
                                                     .withStandardItemHeight(SendMenuLookAndFeel::kItemH),
                           [safe, candidateBuses](int result)
        {
            if (safe == nullptr || result <= 0)
                return;

            auto* source = dynamic_cast<te::AudioTrack*>(safe->track);
            if (source == nullptr)
                return;

            auto& tm = safe->trackManager;
            int bus = -1;

            if (result == kMasterItemId)
            {
                if (! tm.canHaveMasterSend(source) || tm.hasMasterSend(source))
                    return;

                // The Master Return (head of the master chain) is created on the first Send Master.
                if (tm.ensureMasterSendReturn() == nullptr)
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                           "Add send", "Could not create the Master return.");
                    return;
                }

                bus = TrackManager::kMasterSendBus;
            }
            else if (result - 1 < candidateBuses.size())
            {
                bus = candidateBuses[result - 1];

                // Re-check now: the session may have changed while the menu was open.
                auto* fx = tm.getFxTrackForBus(bus);
                if (fx == nullptr)
                    return;

                if (tm.getTrackType(source) == TrackManager::TrackType::FXChannel
                    && (fx == source || tm.wouldCreateSendLoop(source, fx)))
                    return;
            }
            else
                return;

            auto& edit = tm.getEdit();

            // Sends are always post-fader.
            if (tm.addSend (source, bus) == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Add send", "Could not create the send.");
                return;
            }

            safe->rebuild();
        });
    }

    TrackManager& trackManager;
    PluginScanner* scanner { nullptr };
    int lastScannedCount = -1;       // detects a finished scan (see timerCallback)
    te::Track* track { nullptr };
    TrackManager::TrackType kind { TrackManager::TrackType::MonoIn };
    bool updating { false };
    bool insertsCollapsed { false }, sendsCollapsed { false };

    juce::Slider volumeKnob, panKnob;
    juce::Label volCaption, panCaption;
    juce::TextButton addInsertBtn, addSendBtn;
    bool hasSendsSection { false };
    juce::String hoverCaption;                // caption under the mouse (hover feedback)
    BypassButton insertsBypassBtn, sendsBypassBtn;
    juce::OwnedArray<InsertRow> insertRows;
    juce::OwnedArray<SendRow> sendRows;
    SendMenuLookAndFeel sendMenuLnF;

    std::vector<Section> sections;
    juce::Rectangle<int> tagBounds, headerBounds;
    int insertTopY { 0 };                                           // y of the first insert row (drag & drop reorder)
    int dragSlotX { 0 };                                            // x of the insert column while a drag is running
    int dragSlotW { 0 };                                            // width of an insert slot while a drag is running
    static constexpr int kGhostShift = 14;                          // the dragged ghost floats this many px to the right
    int dragFrom { -1 };                                            // slot the dragged insert came from
    int dropIndex { -1 };                                           // insert slot a dragged insert would land on (-1 = no drag)
    int availableH { 0 };                                           // height of the viewport (see setAvailableHeight)
    int levelDividerX { 0 };
};

//==============================================================================
InspectorPanel::InspectorPanel(TrackManager& mgr, PluginScanner* scanner)
    : content(std::make_unique<Content>(mgr, scanner))
{
    viewport.setViewedComponent(content.get(), false);
    viewport.setScrollBarsShown(true, false);
    content->onLayoutNeeded = [this] { updateContentSize(); };
    addAndMakeVisible(viewport);
    setOpaque(true);  // P0-3: reduce repaints by not redrawing parent when transparent
}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::setSelectedTrack(te::Track* trk)
{
    content->setTrack(trk);
}

void InspectorPanel::refresh()
{
    // Re-read from the same track (names of FX channels / plugin list may have changed).
    content->rebuild();
}

void InspectorPanel::updateContentSize()
{
    const int w = juce::jmax(200, viewport.getMaximumVisibleWidth());
    content->setAvailableHeight(viewport.getHeight());      // spare height becomes bigger knobs + empty slots, not a blank hole
    const int h = content->layoutFor(w);
    content->setSize(w, juce::jmax(h, viewport.getHeight()));
    content->repaint();
}

void InspectorPanel::resized()
{
    viewport.setBounds(getLocalBounds().withTrimmedRight(1));      // keep the 1px right separator visible
    updateContentSize();
}

void InspectorPanel::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);

    g.setColour(theme::outline);
    g.fillRect(0, 0, getWidth(), 1);                                // rule on top (only meaningful with a track)

    g.setColour(theme::hairline);
    g.fillRect(getWidth() - 1, 0, 1, getHeight());                  // separator against the track list
}
