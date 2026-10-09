/*
    pt.Live - Live Singing DAW
    License dialog implementation (see include/UI/LicenseDialog.h).
*/

#include "include/UI/LicenseDialog.h"
#include <cmath>
#include "include/Audio/PtLog.h"

using namespace ptl;

#define VI(text) juce::String::fromUTF8 (text)

namespace
{
    constexpr int headerH = 64, pad = 28, phoneH = 46, statusH = 44, btnH = 44;
    constexpr juce::uint32 successHoldMs = 1400;

    // The window: no OS title bar, no buttons - the dialog draws its own header.
    class LicenseWindow : public juce::DialogWindow
    {
    public:
        explicit LicenseWindow (LicenseDialog* content)
            : juce::DialogWindow ("pt.Live", theme::window, false, true)
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
            if (auto* c = dynamic_cast<LicenseDialog*> (getContentComponent()))
                c->requestQuit();
        }
    };

    // Only digits / + / space / . / - for a phone number.
    struct PhoneFilter : public juce::TextEditor::InputFilter
    {
        juce::String filterNewText (juce::TextEditor& ed, const juce::String& newText) override
        {
            juce::String out;
            for (auto c : newText)
                if ((c < 128 && juce::CharacterFunctions::isDigit (c)) || c == '+' || c == ' ' || c == '.' || c == '-')
                    out += c;

            const int room = juce::jmax (0, 20 - (ed.getTotalNumChars() - ed.getHighlightedRegion().getLength()));
            return out.substring (0, room);
        }
    };
}

//==============================================================================
// One worker-thread operation: activate (trial code), claim (key-less), request (phone) or refresh.
class LicenseDialog::Job : public juce::Thread
{
public:
    // key given -> activate;  claimByHwid -> claim;  isRequest -> request;  otherwise refresh.
    Job (LicenseDialog& d, LicenseClient& c, const juce::String& k, bool claimByHwid, bool isRequestOp, juce::String ph)
        : juce::Thread ("ptLive license"), safe (&d), client (c), key (k), claimMode (claimByHwid),
          requestMode (isRequestOp), phone (std::move (ph)) {}

    void run() override
    {
        const LicenseClient::Outcome o =
            requestMode ? client.requestActivation (phone, cancel)
          : claimMode   ? client.claim (cancel)
          : key.isEmpty() ? client.refresh (cancel)
                          : client.activate (key, cancel);

        if (cancel.load())
            return;

        const bool isRefresh = key.isEmpty() && ! claimMode && ! requestMode;
        const bool isRequest = requestMode;

        juce::MessageManager::callAsync ([s = safe, o, isRefresh, isRequest]
        {
            if (s != nullptr)
                s->onOutcome (o, isRefresh, isRequest);
        });
    }

    std::atomic<bool> cancel { false };

private:
    juce::Component::SafePointer<LicenseDialog> safe;
    LicenseClient& client;
    juce::String key;
    bool claimMode = false;
    bool requestMode = false;
    juce::String phone;
};

//==============================================================================
void LicenseDialog::show (LicenseClient& client, LicenseClient::Gate reason, Finished onFinished)
{
    open (client, reason, std::move (onFinished), false, {}, false);
}

void LicenseDialog::showFromApp (LicenseClient& client, LicenseClient::Gate reason,
                                 const juce::String& currentDesc, bool currentTrial, Finished onFinished)
{
    open (client, reason, std::move (onFinished), true, currentDesc, currentTrial);
}

void LicenseDialog::open (LicenseClient& client, LicenseClient::Gate reason, Finished onFinished,
                          bool openedFromApp, const juce::String& currentDesc, bool currentTrial)
{
    auto* content = new LicenseDialog (client, reason, std::move (onFinished), openedFromApp, currentDesc, currentTrial);
    auto* window  = new LicenseWindow (content);

    window->setVisible (true);
    window->toFront (true);
    window->enterModalState (true, nullptr, true);         // deleted by itself when dismissed

    content->phoneInput.grabKeyboardFocus();

    // Stored token needs the server: try it at once, the user only sees a short "connecting" state.
    if (reason == LicenseClient::Gate::needOnline)
        content->startJob ({});
}

LicenseDialog::LicenseDialog (LicenseClient& c, LicenseClient::Gate reason, Finished done,
                              bool openedFromApp, juce::String currentDesc, bool currentTrial)
    : client (c), mode (reason), onFinished (std::move (done)),
      fromApp (openedFromApp), currentDescription (std::move (currentDesc)), currentIsTrial (currentTrial)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setSize (dialogWidth, dialogHeight);

    addAndMakeVisible (brand);


    // Phone input
    phoneInput.setInputFilter (new PhoneFilter(), true);
    phoneInput.setMultiLine (false);
    phoneInput.setReturnKeyStartsNewLine (false);
    phoneInput.setJustification (juce::Justification::centredLeft);
    phoneInput.setTextToShowWhenEmpty (VI ("Số điện thoại của bạn"), theme::textFaint);
    phoneInput.setColour (juce::TextEditor::backgroundColourId, theme::stage);
    phoneInput.setColour (juce::TextEditor::outlineColourId, theme::outline);
    phoneInput.setFont (theme::font (16.0f));
    phoneInput.onReturnKey = [this] { sendRequest(); };
    phoneInput.onTextChange = [this] { if (status == Status::error) setStatus (Status::idle); updateButtons(); };
    addAndMakeVisible (phoneInput);

    // Buttons
    requestBtn.setButtonText (VI ("Gửi yêu cầu"));
    requestBtn.setColour (juce::TextButton::buttonColourId, theme::raised);
    requestBtn.setColour (juce::TextButton::textColourOffId, theme::text);
    requestBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    requestBtn.onClick = [this] { sendRequest(); };
    addAndMakeVisible (requestBtn);

    trialBtn.setButtonText (VI ("Dùng thử"));
    trialBtn.setColour (juce::TextButton::buttonColourId, theme::raised);
    trialBtn.setColour (juce::TextButton::textColourOffId, theme::text);
    trialBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    trialBtn.onClick = [this] { useTrial(); };
    addAndMakeVisible (trialBtn);

    activateBtn.setButtonText (VI ("Kích hoạt"));
    activateBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
    activateBtn.setColour (juce::TextButton::textColourOffId, theme::textOnAccent);
    activateBtn.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    activateBtn.onClick = [this] { primaryAction(); };
    addAndMakeVisible (activateBtn);

    closeBtn2.setButtonText (fromApp ? VI ("Đóng") : VI ("Thoát"));
    closeBtn2.setColour (juce::TextButton::buttonColourId, theme::raised);
    closeBtn2.setColour (juce::TextButton::textColourOffId, theme::textDim);
    closeBtn2.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    closeBtn2.onClick = [this] { requestQuit(); };
    addAndMakeVisible (closeBtn2);

    updateButtons();
}

LicenseDialog::~LicenseDialog()
{
    stopTimer();

    // Never wait for a running request on the message thread: a blocked network call can take several seconds
    // and the window would look frozen. The job is handed to a helper thread that joins it; the dialog is
    // already gone (the job only talks to it through a SafePointer) and LicenseClient waits for in-flight calls.
    if (job != nullptr)
    {
        job->cancel = true;
        std::shared_ptr<Job> keep (std::move (job));
        juce::Thread::launch ([keep] { keep->stopThread (20000); });
    }
}

//==============================================================================
juce::String LicenseDialog::eyebrow() const
{
    return (fromApp && mode == LicenseClient::Gate::ok) ? VI ("BẢN QUYỀN") : VI ("KÍCH HOẠT BẢN QUYỀN");
}

juce::String LicenseDialog::headline() const
{
    if (fromApp && mode == LicenseClient::Gate::ok)
        return (currentDescription.isEmpty() || currentIsTrial) ? VI ("Kích hoạt bản quyền")
                                                                : VI ("Gia hạn bản quyền");

    switch (mode)
    {
        case LicenseClient::Gate::needOnline: return VI ("Cần xác thực lại bản quyền");
        case LicenseClient::Gate::expired:    return VI ("Key đã hết hạn");
        case LicenseClient::Gate::revoked:    return VI ("Bản quyền đã bị thu hồi");
        case LicenseClient::Gate::invalid:    return VI ("Bản quyền không dùng được trên máy này");
        default:                              return VI ("Kích hoạt bản quyền");
    }
}

juce::String LicenseDialog::explanation() const
{
    if (fromApp && mode == LicenseClient::Gate::ok && currentDescription.isNotEmpty())
        return VI ("Hiện tại: ") + currentDescription + VI (". ")
             + (currentIsTrial ? VI ("Điền số điện thoại rồi bấm Gửi yêu cầu để mua bản quyền.")
                               : VI ("Điền số điện thoại rồi bấm Gửi yêu cầu để gia hạn."));

    switch (mode)
    {
        case LicenseClient::Gate::needOnline:
            if (const auto back = client.getClockBackSeconds(); back > 0)
                return VI ("Đồng hồ máy đang lùi khoảng ") + LicenseClient::describeDuration (back)
                     + VI (" so với lần mở trước (pin CMOS yếu hoặc lỗi đồng bộ giờ). "
                           "Hãy chỉnh lại ngày giờ Windows, bật Internet rồi bấm Kích hoạt.");

            return VI ("Máy đã quá thời hạn dùng ngoại tuyến. "
                       "Hãy bật Internet rồi bấm Kích hoạt, pt.Live sẽ tự xác thực lại.");
        case LicenseClient::Gate::expired:
            return VI ("Điền số điện thoại rồi bấm Gửi yêu cầu để gia hạn, đợi nhà cung cấp xác nhận rồi bấm Kích hoạt.");
        case LicenseClient::Gate::revoked:
            return VI ("Bản quyền của máy này đã bị thu hồi. Điền số điện thoại rồi bấm Gửi yêu cầu để được kích hoạt lại, "
                       "đợi nhà cung cấp xác nhận rồi bấm Kích hoạt.");
        case LicenseClient::Gate::invalid:
            return VI ("Bản quyền đã lưu thuộc về máy khác hoặc bị hỏng. "
                       "Điền số điện thoại rồi bấm Gửi yêu cầu, hoặc dùng thử.");
        default:
            return VI ("Điền số điện thoại rồi bấm Gửi yêu cầu để mua bản quyền. "
                       "Muốn dùng thử ngay thì bấm Dùng thử. Đã được duyệt thì bấm Kích hoạt.");
    }
}

//==============================================================================
void LicenseDialog::updateButtons()
{
    const bool busy = (status == Status::busy) || (status == Status::success);

    activateBtn.setEnabled (! busy);
    trialBtn.setEnabled (! busy);
    closeBtn2.setEnabled (! busy);
    phoneInput.setEnabled (! busy);

    // Anti-spam: the request button stays locked (with a countdown) until LicenseClient allows a new request.
    const int wait = client.requestWaitSeconds();
    requestBtn.setEnabled (! busy && wait <= 0);
    requestBtn.setButtonText (wait > 0 ? VI ("Gửi lại sau ") + LicenseClient::waitText (wait) : VI ("Gửi yêu cầu"));

    if (! busy && wait > 0 && ! isTimerRunning())
        startTimerHz (1);                                 // countdown; timerCallback stops it when the wait is over

    repaint (statusR);
}

void LicenseDialog::setStatus (Status s, const juce::String& text)
{
    status = s;
    statusText = text;

    if (s == Status::busy)         startTimerHz (30);
    else if (s == Status::success) { successAt = juce::Time::getMillisecondCounter(); startTimerHz (30); }
    else                           stopTimer();

    updateButtons();
}

void LicenseDialog::primaryAction()          // Kích hoạt: key-less claim (admin approved this machine)
{
    if (status == Status::busy || status == Status::success)
        return;

    // needOnline: the stored token only needs the server -> refresh.
    // Otherwise: no key typed = the admin approved this machine on the VPS -> claim it.
    startJob ({}, mode != LicenseClient::Gate::needOnline);
}

void LicenseDialog::sendRequest()            // Gửi yêu cầu: send hwid + phone to the VPS
{
    if (status == Status::busy || status == Status::success)
        return;

    if (const int wait = client.requestWaitSeconds(); wait > 0)
    {
        setStatus (Status::error, VI ("Bạn vừa gửi yêu cầu rồi. Vui lòng đợi ") + LicenseClient::waitText (wait) + VI (" nữa."));
        return;
    }

    startJob ({}, false, true, phoneInput.getText().trim());
}

void LicenseDialog::useTrial()               // Dùng thử: built-in public trial code
{
    if (status == Status::busy || status == Status::success)
        return;

    startJob (ptl::license::kTrialCode);
}

void LicenseDialog::startJob (const juce::String& keyOrEmpty, bool claimByHwid, bool isRequest,
                              const juce::String& phone)
{
    if (job != nullptr)
    {
        job->stopThread (20000);                          // previous one has finished already (buttons were disabled)
        job.reset();
    }

    setStatus (Status::busy, isRequest ? VI ("Đang gửi yêu cầu…")
                          : (keyOrEmpty.isEmpty() && ! claimByHwid) ? VI ("Đang xác thực với máy chủ…")
                                                                    : VI ("Đang kích hoạt…"));
    job = std::make_unique<Job> (*this, client, keyOrEmpty, claimByHwid, isRequest, phone);
    job->startThread();
}

void LicenseDialog::onOutcome (const LicenseClient::Outcome& o, bool wasRefresh, bool wasRequest)
{
    if (finished)
        return;

    if (o.ok)
    {
        if (wasRequest)
        {
            setStatus (Status::idle, o.message);          // stay open: the user still presses Kích hoạt after approval
            return;
        }
        setStatus (Status::success, (wasRefresh ? VI ("Đã xác thực lại · ") : VI ("Kích hoạt thành công · "))
                                    + LicenseClient::describe (o.info));
        return;
    }

    // The server said the stored license is dead (expired / revoked / removed): LicenseClient deleted it,
    // so from here on this is a normal "enter a key" screen.
    if (wasRefresh && o.code != "offline" && ! o.code.startsWith ("http_"))
    {
        mode = (o.code == "expired") ? LicenseClient::Gate::expired
             : (o.code == "revoked") ? LicenseClient::Gate::revoked : LicenseClient::Gate::needKey;
        resized();
        repaint();
    }

    setStatus (Status::error, o.message);
}

void LicenseDialog::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();

    if (status == Status::success && now - successAt > successHoldMs)
    {
        stopTimer();
        finish (true);
        return;
    }

    if (status != Status::busy && status != Status::success)   // idle / error: only the request countdown needs the timer
    {
        if (client.requestWaitSeconds() <= 0)
            stopTimer();

        updateButtons();
        return;
    }

    if (status == Status::busy || status == Status::success)
        repaint (statusR);
}

void LicenseDialog::requestQuit()
{
    finish (false);
}

void LicenseDialog::finish (bool activated)
{
    if (finished)
        return;
    finished = true;
    stopTimer();

    auto cb = std::move (onFinished);

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);                           // window + this content are deleted a bit later

    // Do the real work from a clean message-loop turn (the dialog may be gone by then).
    juce::MessageManager::callAsync ([cb, activated] { if (cb) cb (activated); });
}

//==============================================================================
bool LicenseDialog::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        requestQuit();
        return true;
    }
    return false;
}

void LicenseDialog::mouseDown (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.startDraggingComponent (getTopLevelComponent(), e);
}

void LicenseDialog::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getMouseDownPosition().y < headerH)
        dragger.dragComponent (getTopLevelComponent(), e, nullptr);
}

//==============================================================================
void LicenseDialog::resized()
{
    auto b = getLocalBounds();

    headerR = b.removeFromTop (headerH);
    brand.setBounds (0, 0, 320, headerH);

    auto body = b.reduced (pad, 0);
    body.removeFromTop (22);
    eyebrowR  = body.removeFromTop (16);   body.removeFromTop (6);
    headlineR = body.removeFromTop (28);   body.removeFromTop (6);
    explainR  = body.removeFromTop (44);   body.removeFromTop (14);

    // Phone row: [ phone input ]  [ Gửi yêu cầu ]
    requestR = body.removeFromTop (phoneH);            body.removeFromTop (10);
    {
        auto row = requestR;
        auto rr = row.removeFromRight (150);
        requestBtn.setBounds (rr);
        row.removeFromRight (10);
        phoneInput.setBounds (row);
    }

    statusR = body.removeFromTop (statusH);            body.removeFromTop (18);

    // Action row: [ Dùng thử ]  [ Kích hoạt ]  [ Đóng ]
    actionsR = body.removeFromTop (btnH);
    {
        auto row = actionsR;
        const int gap = 10;
        const int w = (row.getWidth() - 2 * gap) / 3;
        trialBtn.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (gap);
        activateBtn.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (gap);
        closeBtn2.setBounds (row);
    }
}

void LicenseDialog::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (window);

    // Header: panel surface + a faint brand-coloured glow, hairline below.
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

    // Text block
    g.setColour (textFaint);
    g.setFont (spacedFont (szMicro, 0.35f, true));
    g.drawText (eyebrow(), eyebrowR, juce::Justification::centredLeft, true);

    g.setColour (text);
    g.setFont (font (20.0f, true));
    g.drawFittedText (headline(), headlineR, juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (textDim);
    g.setFont (font (szSmall + 0.5f));
    g.drawFittedText (explanation(), explainR, juce::Justification::topLeft, 3, 1.0f);

    // Status card
    {
        auto card = statusR.toFloat();
        g.setColour (panel);
        g.fillRoundedRectangle (card, radiusLg);

        juce::Colour edge = outline;
        if (status == Status::error)   edge = danger.withAlpha (0.55f);
        if (status == Status::success) edge = success.withAlpha (0.55f);
        if (status == Status::busy)    edge = accent.withAlpha (0.35f);
        g.setColour (edge);
        g.drawRoundedRectangle (card.reduced (0.5f), radiusLg, 1.0f);

        auto inner = statusR.reduced (14, 0);
        auto icon  = inner.removeFromLeft (22).toFloat();
        inner.removeFromLeft (10);
        const auto c = icon.getCentre();

        juce::Colour tint = textFaint;
        juce::String msg  = VI ("Điền số điện thoại rồi bấm Gửi yêu cầu, hoặc bấm Dùng thử / Kích hoạt");

        if (status == Status::busy)
        {
            // Wrap the angle in DOUBLE precision first. getMillisecondCounterHiRes() counts from the PC's boot:
            // after a few days (ms * 0.006) is ~1e7 and a float no longer has room for fractions, so
            // Path::addCentredArc()'s loop (angle += small step) never advanced -> endless loop that ate all
            // memory (seen as a 7 s hang, then a crash inside Path::lineTo).
            const float t = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() * 0.006,
                                               juce::MathConstants<double>::twoPi);
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, 8.0f, 8.0f, 0.0f, t, t + juce::MathConstants<float>::pi * 1.35f, true);
            g.setColour (accent.withAlpha (0.18f));
            g.drawEllipse (c.x - 8.0f, c.y - 8.0f, 16.0f, 16.0f, 2.0f);
            g.setColour (accent);
            g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            tint = text;
            msg  = statusText;
        }
        else if (status == Status::error)
        {
            g.setColour (danger.withAlpha (0.20f));
            g.fillEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f);
            g.setColour (danger);
            const float d = 3.4f;
            g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 1.9f);
            g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, 1.9f);
            tint = text;
            msg  = statusText;
        }
        else if (status == Status::success)
        {
            g.setColour (success.withAlpha (0.20f));
            g.fillEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f);
            juce::Path tick;
            tick.startNewSubPath (c.x - 4.2f, c.y + 0.3f);
            tick.lineTo (c.x - 1.2f, c.y + 3.2f);
            tick.lineTo (c.x + 4.4f, c.y - 3.0f);
            g.setColour (success);
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            tint = text;
            msg  = statusText;
        }
        else
        {
            g.setColour (textFaint);
            g.drawEllipse (c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f, 1.4f);
        }

        g.setColour (tint);
        g.setFont (font (szSmall + 0.5f));
        g.drawFittedText (msg, inner, juce::Justification::centredLeft, 2, 1.0f);
    }

    // Window edge
    g.setColour (outlineStrong);
    g.drawRect (getLocalBounds(), 1);
}
