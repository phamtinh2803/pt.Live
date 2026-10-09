/*
    pt.Live - Live Singing DAW
    Update dialog implementation (see include/UI/UpdateDialog.h).
*/

#include "include/UI/UpdateDialog.h"

namespace
{
    constexpr int headerH = 64, pad = 28, btnH = 42, btnGap = 10, notesLineH = 17, maxNotesLines = 7;

    // The window: no OS title bar, no buttons - the dialog draws its own header (same as WelcomeDialog).
    class UpdateWindow : public juce::DialogWindow
    {
    public:
        explicit UpdateWindow (UpdateDialog* content)
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
            if (auto* c = dynamic_cast<UpdateDialog*> (getContentComponent()))
                c->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));      // [x] = Hủy
        }
    };

    constexpr int panelH = 56;                // the notes area when there are no notes: still room for the progress bar

    int heightFor (const juce::String& notes)
    {
        const int notesPart = 16 + (notes.isNotEmpty() ? maxNotesLines * notesLineH : panelH);
        return headerH + 22 + 28 + 4 + 18 + notesPart + 24 + btnH + 28;
    }

    juce::String megabytes (juce::int64 bytes)
    {
        return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
    }

    void styleButton (juce::TextButton& b, const juce::String& label, juce::Colour fill, juce::Colour textCol)
    {
        b.setButtonText (label);
        b.setColour (juce::TextButton::buttonColourId, fill);
        b.setColour (juce::TextButton::textColourOffId, textCol);
        b.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
}

//==============================================================================
void UpdateDialog::show (const juce::String& currentVersion, const juce::String& newVersion,
                         const juce::String& notes, const juce::String& url, const juce::String& sha256, Chosen onChosen)
{
    auto* content = new UpdateDialog (currentVersion, newVersion, notes, url, sha256, std::move (onChosen));
    auto* window  = new UpdateWindow (content);

    window->setVisible (true);
    window->toFront (true);
    window->enterModalState (true, nullptr, true);         // deleted by itself when dismissed

    content->grabKeyboardFocus();
}

UpdateDialog::UpdateDialog (const juce::String& current, const juce::String& latest, const juce::String& releaseNotes,
                            const juce::String& downloadUrl, const juce::String& hash, Chosen done)
    : onChosen (std::move (done)), currentVersion (current), newVersion (latest), notes (releaseNotes),
      url (downloadUrl), sha256 (hash)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    addAndMakeVisible (brand);

    styleButton (updateBtn, juce::String::fromUTF8 ("C\xe1\xba\xadp nh\xe1\xba\xadt"),
                 ptl::theme::accent, ptl::theme::textOnAccent);
    styleButton (laterBtn,  juce::String::fromUTF8 ("H\xe1\xbb\xa7y"),
                 ptl::theme::raised, ptl::theme::text);
    styleButton (skipBtn,   juce::String::fromUTF8 ("B\xe1\xbb\x8f qua b\xe1\xba\xa3n n\xc3\xa0y"),
                 ptl::theme::raised, ptl::theme::textDim);

    updateBtn.onClick = [this] { startDownload(); };                 // "Cập nhật" / "Thử lại"
    laterBtn.onClick  = [this] { choose (Choice::later); };          // "Hủy" / "Hủy tải" / "Đóng"
    skipBtn.onClick   = [this]
    {
        if (mode == Mode::failed)                                    // "Mở trình duyệt": the old way, by hand
        {
            juce::URL (url).launchInDefaultBrowser();
            choose (Choice::later);
        }
        else
            choose (Choice::skip);
    };

    for (auto* b : { &updateBtn, &laterBtn, &skipBtn })
        addAndMakeVisible (b);

    // setSize LAST: it triggers resized(), which needs the buttons to exist.
    setSize (dialogWidth, heightFor (notes));
}

UpdateDialog::~UpdateDialog()
{
    stopTimer();
    downloader.cancelDownload();                          // (the downloader's destructor also does it, then joins)
}

void UpdateDialog::choose (Choice c)
{
    if (finished)
        return;
    finished = true;

    stopTimer();
    if (c != Choice::update)
        downloader.cancelDownload();                      // Hủy while downloading: stop the transfer right now

    auto cb = std::move (onChosen);
    const auto file = installerFile;

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);                           // window + this content are deleted a bit later

    // Do the real work from a clean message-loop turn (the dialog may be gone by then).
    juce::MessageManager::callAsync ([cb, c, file] { if (cb) cb (c, file); });
}

//==============================================================================
void UpdateDialog::startDownload()
{
    if (mode != Mode::ready && mode != Mode::failed)
        return;

    mode = Mode::downloading;
    lastError = UpdateDownloader::Error::none;
    applyMode();
    startTimerHz (20);

    juce::Component::SafePointer<UpdateDialog> safe (this);       // the dialog may be closed while bytes are still coming
    downloader.start (url, newVersion, sha256, [safe] (bool ok, const juce::File& f, UpdateDownloader::Error e)
    {
        if (auto* d = safe.getComponent())
            d->downloadFinished (ok, f, e);
    });
}

void UpdateDialog::downloadFinished (bool ok, const juce::File& file, UpdateDownloader::Error error)
{
    if (finished)
        return;

    stopTimer();

    if (! ok)
    {
        mode = Mode::failed;
        lastError = error;
        applyMode();
        return;
    }

    installerFile = file;
    mode = Mode::installing;
    applyMode();

    // A moment to read "đang cài đặt", then the app takes over (close -> install over -> start again).
    juce::Component::SafePointer<UpdateDialog> safe (this);
    juce::Timer::callAfterDelay (800, [safe]
    {
        if (auto* d = safe.getComponent())
            d->choose (Choice::update);
    });
}

void UpdateDialog::timerCallback()
{
    repaint (notesR.expanded (12, 8));
}

void UpdateDialog::applyMode()
{
    switch (mode)
    {
        case Mode::ready:
            updateBtn.setButtonText (juce::String::fromUTF8 ("C\xe1\xba\xad" "p nh\xe1\xba\xad" "t"));
            laterBtn.setButtonText  (juce::String::fromUTF8 ("H\xe1\xbb\xa7" "y"));
            skipBtn.setButtonText   (juce::String::fromUTF8 ("B\xe1\xbb\x8f" " qua b\xe1\xba\xa3" "n n\xc3\xa0" "y"));
            break;

        case Mode::downloading:
            laterBtn.setButtonText (juce::String::fromUTF8 ("H\xe1\xbb\xa7" "y t\xe1\xba\xa3" "i"));
            break;

        case Mode::failed:
            updateBtn.setButtonText (juce::String::fromUTF8 ("Th\xe1\xbb\xad" " l\xe1\xba\xa1" "i"));
            laterBtn.setButtonText  (juce::String::fromUTF8 ("\xc4\x90\xc3\xb3" "ng"));
            skipBtn.setButtonText   (juce::String::fromUTF8 ("M\xe1\xbb\x9f" " tr\xc3\xac" "nh duy\xe1\xbb\x87" "t"));
            break;

        case Mode::installing:
            break;
    }

    updateBtn.setVisible (mode == Mode::ready || mode == Mode::failed);
    skipBtn.setVisible   (mode == Mode::ready || mode == Mode::failed);
    laterBtn.setVisible  (mode != Mode::installing);

    resized();
    repaint();
}

//==============================================================================
bool UpdateDialog::keyPressed (const juce::KeyPress& key)
{
    if (mode == Mode::installing)
        return true;                                      // the app is about to close: nothing to answer any more

    if (key == juce::KeyPress::escapeKey)
    {
        choose (Choice::later);                           // while downloading this stops the download
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        startDownload();                                  // ignored while a download is already running
        return true;
    }

    return false;
}

void UpdateDialog::mouseDown (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.startDraggingComponent (getTopLevelComponent(), e);
}

void UpdateDialog::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.dragComponent (getTopLevelComponent(), e, nullptr);
}

//==============================================================================
void UpdateDialog::resized()
{
    auto b = getLocalBounds();

    headerR = b.removeFromTop (headerH);
    brand.setBounds (0, 0, 320, headerH);

    auto body = b.reduced (pad, 0);
    body.removeFromTop (22);
    headlineR = body.removeFromTop (28);   body.removeFromTop (4);
    subR      = body.removeFromTop (18);

    body.removeFromTop (16);
    notesR = body.removeFromTop (notes.isNotEmpty() ? maxNotesLines * notesLineH : panelH);   // notes / progress / error

    body.removeFromTop (24);

    auto buttons = body.removeFromTop (btnH);

    if (mode == Mode::downloading)
    {
        laterBtn.setBounds (buttons);                     // one wide [Hủy tải]
        return;
    }

    const int w = (buttons.getWidth() - 2 * btnGap) / 3;
    updateBtn.setBounds (buttons.removeFromLeft (w));   buttons.removeFromLeft (btnGap);
    laterBtn.setBounds  (buttons.removeFromLeft (w));   buttons.removeFromLeft (btnGap);
    skipBtn.setBounds   (buttons);
}

void UpdateDialog::paint (juce::Graphics& g)
{
    using namespace ptl::theme;

    g.fillAll (window);

    // Header: panel surface + a faint brand-coloured glow, hairline below (same as Welcome / License dialogs).
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

    // ---- headline + sub line, by mode -------------------------------------------------------------------
    juce::String headline, sub;
    switch (mode)
    {
        case Mode::ready:
            headline = juce::String::fromUTF8 ("C\xc3\xb3" " b\xe1\xba\xa3" "n c\xe1\xba\xad" "p nh\xe1\xba\xad" "t m\xe1\xbb\x9b" "i");
            sub = juce::String ("pt.Live ") + newVersion
                    + juce::String::fromUTF8 (" \xc4\x91\xc3\xa3" " s\xe1\xba\xb5" "n s\xc3\xa0" "ng (b\xe1\xba\xa1" "n \xc4\x91" "ang d\xc3\xb9" "ng ") + currentVersion + ")";
            break;
        case Mode::downloading:
            headline = juce::String::fromUTF8 ("\xc4\x90" "ang t\xe1\xba\xa3" "i b\xe1\xba\xa3" "n c\xe1\xba\xad" "p nh\xe1\xba\xad" "t");
            sub = juce::String ("pt.Live ") + newVersion + juce::String::fromUTF8 (" - t\xe1\xba\xa3" "i xong, \xe1\xbb\xa9" "ng d\xe1\xbb\xa5" "ng s\xe1\xba\xbd" " t\xe1\xbb\xb1" " \xc4\x91\xc3\xb3" "ng v\xc3\xa0" " c\xc3\xa0" "i \xc4\x91\xc3\xa8");
            break;
        case Mode::installing:
            headline = juce::String::fromUTF8 ("\xc4\x90" "ang c\xc3\xa0" "i \xc4\x91\xe1\xba\xb7" "t");
            sub = juce::String::fromUTF8 ("pt.Live s\xe1\xba\xbd" " \xc4\x91\xc3\xb3" "ng, c\xc3\xa0" "i b\xe1\xba\xa3" "n m\xe1\xbb\x9b" "i r\xe1\xbb\x93" "i t\xe1\xbb\xb1" " m\xe1\xbb\x9f" " l\xe1\xba\xa1" "i");
            break;
        case Mode::failed:
            headline = juce::String::fromUTF8 ("Kh\xc3\xb4" "ng t\xe1\xba\xa3" "i \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c b\xe1\xba\xa3" "n c\xe1\xba\xad" "p nh\xe1\xba\xad" "t");
            sub = juce::String ("pt.Live ") + newVersion;
            break;
    }

    g.setColour (mode == Mode::failed ? danger : text);
    g.setFont (font (20.0f, true));
    g.drawFittedText (headline, headlineR, juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (textDim);
    g.setFont (font (szSmall + 0.5f));
    g.drawText (sub, subR, juce::Justification::centredLeft, true);

    // ---- the area under the sub line: notes / hint (ready), progress (downloading, installing), reason (failed)
    if (mode == Mode::ready)
    {
        if (notes.isNotEmpty())
        {
            // Release notes: plain text only (it comes from the server, so it is never interpreted as markup).
            g.setColour (raised);
            g.fillRoundedRectangle (notesR.expanded (10, 6).toFloat(), radius);

            g.setColour (textFaint);
            g.setFont (font (szSmall + 0.5f));
            g.drawFittedText (notes, notesR, juce::Justification::topLeft, maxNotesLines, 1.0f);
        }
        else
        {
            g.setColour (textFaint);
            g.setFont (font (szSmall + 0.5f));
            g.drawFittedText (juce::String::fromUTF8 ("B\xe1\xba\xa3" "n c\xe1\xba\xad" "p nh\xe1\xba\xad" "t \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c t\xe1\xba\xa3" "i ngay trong \xe1\xbb\xa9" "ng d\xe1\xbb\xa5" "ng. T\xe1\xba\xa3" "i xong, pt.Live t\xe1\xbb\xb1" " \xc4\x91\xc3\xb3" "ng, c\xc3\xa0" "i \xc4\x91\xc3\xa8" " l\xc3\xaa" "n b\xe1\xba\xa3" "n c\xc5\xa9" " v\xc3\xa0" " m\xe1\xbb\x9f" " l\xe1\xba\xa1" "i."),
                              notesR, juce::Justification::topLeft, 3, 1.0f);
        }
    }
    else if (mode == Mode::downloading || mode == Mode::installing)
    {
        const auto done  = downloader.getBytesDone();
        const auto total = downloader.getBytesTotal();
        const bool known = total > 0;
        const float frac = mode == Mode::installing ? 1.0f
                         : known ? juce::jlimit (0.0f, 1.0f, (float) ((double) done / (double) total)) : 0.0f;

        const auto bar = juce::Rectangle<float> ((float) notesR.getX(), (float) notesR.getY() + 10.0f,
                                                 (float) notesR.getWidth(), 10.0f);
        g.setColour (raised);
        g.fillRoundedRectangle (bar, 5.0f);

        if (mode == Mode::downloading && ! known)
        {
            // Size unknown: a segment that slides back and forth instead of a percentage.
            const float seg = bar.getWidth() * 0.28f;
            const float t   = (float) (juce::Time::getMillisecondCounter() % 1400) / 1400.0f;
            const float x   = bar.getX() + (bar.getWidth() - seg) * (0.5f - 0.5f * std::cos (t * juce::MathConstants<float>::twoPi));
            g.setColour (accent);
            g.fillRoundedRectangle (x, bar.getY(), seg, bar.getHeight(), 5.0f);
        }
        else if (frac > 0.0f)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (10.0f, bar.getWidth() * frac)), 5.0f);
        }

        g.setColour (textDim);
        g.setFont (font (szSmall + 0.5f));
        const auto line = juce::Rectangle<int> (notesR.getX(), notesR.getY() + 28, notesR.getWidth(), 18);

        if (mode == Mode::installing)
            g.drawText (juce::String::fromUTF8 ("\xc4\x90\xc3\xa3" " t\xe1\xba\xa3" "i xong - \xc4\x91" "ang chu\xe1\xba\xa9" "n b\xe1\xbb\x8b" " c\xc3\xa0" "i \xc4\x91\xe1\xba\xb7" "t..."), line, juce::Justification::centredLeft, true);
        else
        {
            g.drawText (known ? megabytes (done) + " / " + megabytes (total) : megabytes (done), line,
                        juce::Justification::centredLeft, true);
            if (known)
                g.drawText (juce::String (juce::roundToInt (frac * 100.0f)) + "%", line, juce::Justification::centredRight, true);
        }
    }
    else    // failed
    {
        juce::String why;
        switch (lastError)
        {
            case UpdateDownloader::Error::connect:      why = juce::String::fromUTF8 ("Kh\xc3\xb4" "ng k\xe1\xba\xbf" "t n\xe1\xbb\x91" "i \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c t\xe1\xbb\x9b" "i m\xc3\xa1" "y ch\xe1\xbb\xa7" " t\xe1\xba\xa3" "i v\xe1\xbb\x81" ". Ki\xe1\xbb\x83" "m tra m\xe1\xba\xa1" "ng r\xe1\xbb\x93" "i th\xe1\xbb\xad" " l\xe1\xba\xa1" "i."); break;
            case UpdateDownloader::Error::http:         why = juce::String::fromUTF8 ("M\xc3\xa1" "y ch\xe1\xbb\xa7" " kh\xc3\xb4" "ng tr\xe1\xba\xa3" " v\xe1\xbb\x81" " file c\xc3\xa0" "i \xc4\x91\xe1\xba\xb7" "t (li\xc3\xaa" "n k\xe1\xba\xbf" "t c\xc3\xb3" " th\xe1\xbb\x83" " \xc4\x91\xc3\xa3" " h\xe1\xbb\x8f" "ng)."); break;
            case UpdateDownloader::Error::interrupted:  why = juce::String::fromUTF8 ("Vi\xe1\xbb\x87" "c t\xe1\xba\xa3" "i b\xe1\xbb\x8b" " gi\xc3\xa1" "n \xc4\x91" "o\xe1\xba\xa1" "n n\xc3\xaa" "n file ch\xc6\xb0" "a \xc4\x91\xe1\xba\xa7" "y \xc4\x91\xe1\xbb\xa7" ". H\xc3\xa3" "y th\xe1\xbb\xad" " l\xe1\xba\xa1" "i."); break;
            case UpdateDownloader::Error::disk:         why = juce::String::fromUTF8 ("Kh\xc3\xb4" "ng ghi \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c file v\xc3\xa0" "o th\xc6\xb0" " m\xe1\xbb\xa5" "c t\xe1\xba\xa1" "m (\xe1\xbb\x95" " \xc4\x91\xc4\xa9" "a \xc4\x91\xe1\xba\xa7" "y ho\xe1\xba\xb7" "c b\xe1\xbb\x8b" " ch\xe1\xba\xb7" "n)."); break;
            case UpdateDownloader::Error::notInstaller: why = juce::String::fromUTF8 ("File t\xe1\xba\xa3" "i v\xe1\xbb\x81" " kh\xc3\xb4" "ng ph\xe1\xba\xa3" "i b\xe1\xbb\x99" " c\xc3\xa0" "i \xc4\x91\xe1\xba\xb7" "t (li\xc3\xaa" "n k\xe1\xba\xbf" "t sai). B\xe1\xba\xa1" "n c\xc3\xb3" " th\xe1\xbb\x83" " m\xe1\xbb\x9f" " tr\xc3\xac" "nh duy\xe1\xbb\x87" "t \xc4\x91\xe1\xbb\x83" " t\xe1\xba\xa3" "i."); break;
            case UpdateDownloader::Error::hashMismatch: why = juce::String::fromUTF8 ("File t\xe1\xba\xa3" "i v\xe1\xbb\x81" " kh\xc3\xb4" "ng kh\xe1\xbb\x9b" "p m\xc3\xa3" " ki\xe1\xbb\x83" "m tra n\xc3\xaa" "n \xc4\x91\xc3\xa3" " b\xe1\xbb\x8b" " x\xc3\xb3" "a v\xc3\xac" " an to\xc3\xa0" "n. H\xc3\xa3" "y th\xe1\xbb\xad" " l\xe1\xba\xa1" "i sau."); break;
            case UpdateDownloader::Error::none:         break;
        }

        g.setColour (textFaint);
        g.setFont (font (szSmall + 0.5f));
        g.drawFittedText (why, notesR, juce::Justification::topLeft, 3, 1.0f);
    }

    // Window edge
    g.setColour (outlineStrong);
    g.drawRect (getLocalBounds(), 1);
}
