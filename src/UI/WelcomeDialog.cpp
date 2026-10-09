/*
    pt.Live - Live Singing DAW
    Start screen implementation (see include/UI/WelcomeDialog.h).
*/

#include "include/UI/WelcomeDialog.h"

namespace
{
    constexpr int headerH = 64, pad = 28, btnH = 42, btnGap = 10, rowH = 48, rowGap = 4;
    constexpr int maxRecent = 3;

    // The window: no OS title bar, no buttons - the dialog draws its own header.
    class WelcomeWindow : public juce::DialogWindow
    {
    public:
        explicit WelcomeWindow (WelcomeDialog* content)
            : juce::DialogWindow ("pt.Live", ptl::theme::window, false, true)
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (0);
            setTitleBarButtonsRequired (0, false);
            setResizable (false, false);
            setContentOwned (content, true);
            centreWithSize (getWidth(), getHeight());
            setAlwaysOnTop (true);
        }

        void closeButtonPressed() override
        {
            if (auto* c = dynamic_cast<WelcomeDialog*> (getContentComponent()))
                c->requestCancel();
        }
    };

    // Height of the dialog: fixed part + the "recent" block when there is something to show.
    int heightFor (int numRecent)
    {
        const int fixedPart = headerH + 22 + 28 + 4 + 18 + 10 + 18 + 18 + btnH + 28;
        const int recentPart = numRecent > 0 ? 22 + 16 + 8 + numRecent * rowH + (numRecent - 1) * rowGap : 0;
        return fixedPart + recentPart;
    }
}

//==============================================================================
// One recent file: name (bold), folder (dim) and last-saved time. Click = open it.
class WelcomeDialog::RecentRow : public juce::Component
{
public:
    RecentRow (const juce::File& f, std::function<void()> onClickFn)
        : file (f), onClick (std::move (onClickFn))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace ptl::theme;

        auto b = getLocalBounds().toFloat();
        g.setColour (hover ? raisedHi : raised);
        g.fillRoundedRectangle (b, radius);

        auto area = getLocalBounds().reduced (14, 7);
        auto stamp = area.removeFromRight (112);
        area.removeFromRight (10);

        g.setColour (textFaint);
        g.setFont (font (szSmall));
        g.drawText (file.getLastModificationTime().formatted ("%d/%m/%Y  %H:%M"), stamp,
                    juce::Justification::centredRight, false);

        g.setColour (text);
        g.setFont (font (szBody, true));
        g.drawText (file.getFileNameWithoutExtension(), area.removeFromTop (area.getHeight() / 2),
                    juce::Justification::centredLeft, true);

        g.setColour (textDim);
        g.setFont (font (szSmall));
        g.drawText (file.getParentDirectory().getFullPathName(), area, juce::Justification::centredLeft, true);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::File file;
    std::function<void()> onClick;
    bool hover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecentRow)
};

//==============================================================================
void WelcomeDialog::show (const juce::Array<juce::File>& recentFiles, Chosen onChosen)
{
    auto* content = new WelcomeDialog (recentFiles, std::move (onChosen));
    auto* window  = new WelcomeWindow (content);

    window->setVisible (true);
    window->toFront (true);
    window->enterModalState (true, nullptr, true);         // deleted by itself when dismissed

    content->grabKeyboardFocus();
}

WelcomeDialog::WelcomeDialog (const juce::Array<juce::File>& recent, Chosen done)
    : onChosen (std::move (done))
{
    for (int i = 0; i < recent.size() && i < maxRecent; ++i)
        recentFiles.add (recent[i]);

    setOpaque (true);
    setWantsKeyboardFocus (true);

    addAndMakeVisible (brand);


    openBtn.setButtonText ("Open...");
    openBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::accent);
    openBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::textOnAccent);
    openBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    openBtn.onClick = [this] { choose (Choice::open); };
    addAndMakeVisible (openBtn);

    emptyBtn.setButtonText ("Empty");
    emptyBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::raised);
    emptyBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::text);
    emptyBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    emptyBtn.onClick = [this] { choose (Choice::empty); };
    addAndMakeVisible (emptyBtn);

    cancelBtn.setButtonText ("Cancel");
    cancelBtn.setColour (juce::TextButton::buttonColourId, ptl::theme::raised);
    cancelBtn.setColour (juce::TextButton::textColourOffId, ptl::theme::textDim);
    cancelBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    cancelBtn.onClick = [this] { requestCancel(); };
    addAndMakeVisible (cancelBtn);

    for (const auto& f : recentFiles)
    {
        auto* row = rows.add (new RecentRow (f, [this, f] { choose (Choice::recent, f); }));
        addAndMakeVisible (row);
    }

    // setSize LAST: it triggers resized(), which lays out the RECENT label + rows. Called before the rows
    // were created (as it used to be), resized() saw an empty list -> rows stayed at 0x0 and were invisible.
    setSize (dialogWidth, heightFor (recentFiles.size()));
}

WelcomeDialog::~WelcomeDialog() = default;

void WelcomeDialog::requestCancel()
{
    choose (Choice::cancel);
}

void WelcomeDialog::choose (Choice c, const juce::File& file)
{
    if (finished)
        return;
    finished = true;

    auto cb = std::move (onChosen);

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);                           // window + this content are deleted a bit later

    // Do the real work from a clean message-loop turn (the dialog may be gone by then).
    juce::MessageManager::callAsync ([cb, c, file] { if (cb) cb (c, file); });
}

//==============================================================================
bool WelcomeDialog::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        requestCancel();
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        choose (Choice::open);
        return true;
    }

    return false;
}

void WelcomeDialog::mouseDown (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.startDraggingComponent (getTopLevelComponent(), e);
}

void WelcomeDialog::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.dragComponent (getTopLevelComponent(), e, nullptr);
}

//==============================================================================
void WelcomeDialog::resized()
{
    auto b = getLocalBounds();

    headerR = b.removeFromTop (headerH);
    brand.setBounds (0, 0, 320, headerH);

    auto body = b.reduced (pad, 0);
    body.removeFromTop (22);
    headlineR = body.removeFromTop (28);   body.removeFromTop (4);
    subR      = body.removeFromTop (18);   body.removeFromTop (10);
    explainR  = body.removeFromTop (18);   body.removeFromTop (18);

    auto buttons = body.removeFromTop (btnH);
    const int w = (buttons.getWidth() - 2 * btnGap) / 3;
    openBtn.setBounds   (buttons.removeFromLeft (w));   buttons.removeFromLeft (btnGap);
    emptyBtn.setBounds  (buttons.removeFromLeft (w));   buttons.removeFromLeft (btnGap);
    cancelBtn.setBounds (buttons);

    if (! rows.isEmpty())
    {
        body.removeFromTop (22);
        recentLabelR = body.removeFromTop (16);
        body.removeFromTop (8);

        for (auto* row : rows)
        {
            row->setBounds (body.removeFromTop (rowH));
            body.removeFromTop (rowGap);
        }
    }
}

void WelcomeDialog::paint (juce::Graphics& g)
{
    using namespace ptl::theme;

    g.fillAll (window);

    // Header: panel surface + a faint brand-coloured glow, hairline below (same as the licence dialog).
    g.setColour (panel);
    g.fillRect (headerR);
    {
        juce::ColourGradient glow (juce::Colour (0xff5aa9e6).withAlpha (0.10f), 0.0f, 0.0f,
                                   juce::Colours::transparentBlack, 0.0f, (float) headerH * 1.6f, false);
        glow.addColour (0.5, juce::Colour (0xffa392f4).withAlpha (0.04f));
        g.setGradientFill (glow);
        g.fillRect (headerR);
    }
    g.setColour (hairline);
    g.fillRect (0, headerH - 1, getWidth(), 1);

    g.setColour (text);
    g.setFont (font (20.0f, true));
    g.drawFittedText ("Welcome to pt.Live", headlineR, juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (textDim);
    g.setFont (font (szSmall + 0.5f));
    g.drawText ("Live Singing DAW", subR, juce::Justification::centredLeft, false);

    g.setColour (textFaint);
    g.setFont (font (szSmall + 0.5f));
    g.drawText ("Open a saved session (.pt), or start with an empty session.", explainR,
                juce::Justification::centredLeft, true);

    if (! rows.isEmpty())
    {
        g.setColour (textFaint);
        g.setFont (spacedFont (szMicro, 0.35f, true));
        g.drawText ("RECENT", recentLabelR, juce::Justification::centredLeft, false);
    }

    // Window edge
    g.setColour (outlineStrong);
    g.drawRect (getLocalBounds(), 1);
}
