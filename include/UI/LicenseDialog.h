/*
    pt.Live - Live Singing DAW
    License dialog - shown BEFORE any other window when the stored license is missing / expired / needs the server,
    and opened from the title bar (double-click the licence status) while the app runs.

    Layout (borderless window, same theme as the main window, drag it by the header):

        [ animated pt.Live brand ]                                              [x]
        ------------------------------------------------------------------------
        KÍCH HOẠT BẢN QUYỀN
        Nhập key để bắt đầu
        Short explanation of why the dialog is shown (depends on the reason).

        [ SỐ ĐIỆN THOẠI            ]   [ Gửi yêu cầu ]      <- request a paid licence from the admin
        [ status card: idle hint / spinner / error / success ]

        [ Dùng thử ]   [ Kích hoạt ]   [ Đóng ]             <- three action buttons

    - "Gửi yêu cầu" sends {hwid, phone} to the VPS; the admin sees the request (with phone) on /admin.html
      and approves. After approval the user presses "Kích hoạt" (key-less claim).
    - "Dùng thử" activates immediately with the built-in public trial code (033.247.3333).
    - "Kích hoạt" does the key-less claim (admin already approved this machine on the VPS).
    - There is NO key input box and NO "Mã máy" row in this layout: the app already knows its own hwid
      and sends it along with the request; the user never has to copy/paste it.

    Network calls run on a worker thread (never the message thread). The dialog closes itself
    ~1.4 s after a successful activation and then calls onFinished (true); Đóng / [x] / Esc call
    onFinished (false), which the application answers by quitting (when opened at start-up).
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"
#include "include/TitleBar.h"
#include "include/UI/UIWidgets.h"
#include "include/Net/LicenseClient.h"

class LicenseDialog : public juce::Component,
                      private juce::Timer
{
public:
    using Finished = std::function<void (bool activated)>;

    // Opens the dialog (async, returns at once). `reason` is the result of LicenseClient::checkLocal().
    // `client` must outlive the dialog.
    static void show (LicenseClient& client, LicenseClient::Gate reason, Finished onFinished);

    // Opened by the user from inside the running app. `currentDesc` = LicenseClient::describe() of the
    // stored license ("" if unknown), `currentTrial` = the stored license is a trial.
    static void showFromApp (LicenseClient& client, LicenseClient::Gate reason,
                             const juce::String& currentDesc, bool currentTrial, Finished onFinished);

    LicenseDialog (LicenseClient& client, LicenseClient::Gate reason, Finished onFinished,
                   bool openedFromApp = false, juce::String currentDesc = {}, bool currentTrial = false);
    ~LicenseDialog() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

    void requestQuit();                               // Đóng / [x] / Esc

    static constexpr int dialogWidth = 520, dialogHeight = 480;

private:
    class Job;
    enum class Status { idle, busy, error, success };

    static void open (LicenseClient& client, LicenseClient::Gate reason, Finished onFinished,
                      bool openedFromApp, const juce::String& currentDesc, bool currentTrial);

    void primaryAction();                            // Kích hoạt (key-less claim)
    void sendRequest();                              // Gửi yêu cầu (with phone)
    void useTrial();                                 // Dùng thử (built-in trial code)
    void startJob (const juce::String& keyOrEmpty, bool claimByHwid = false, bool isRequest = false,
                   const juce::String& phone = {});
    void onOutcome (const LicenseClient::Outcome& o, bool wasRefresh, bool wasRequest);
    void setStatus (Status s, const juce::String& text = {});
    void updateButtons();
    void finish (bool activated);
    void timerCallback() override;

    juce::String eyebrow() const;
    juce::String headline() const;
    juce::String explanation() const;

    LicenseClient& client;
    LicenseClient::Gate mode;                         // why we are here (updated when the server says the stored key is dead)
    Finished onFinished;
    bool finished = false;

    const bool fromApp;                               // opened from the title bar: cancelling must NOT quit the app
    const juce::String currentDescription;            // "Gói Dùng thử · hết hạn 13/10/2026" (fromApp only)
    const bool currentIsTrial;

    BrandMark brand;

    juce::TextEditor phoneInput;                      // SỐ ĐIỆN THOẠI
    juce::TextButton requestBtn;                      // Gửi yêu cầu
    juce::TextButton trialBtn;                        // Dùng thử
    juce::TextButton activateBtn;                     // Kích hoạt
    juce::TextButton closeBtn2;                       // Đóng

    Status status = Status::idle;
    juce::String statusText;
    juce::uint32 successAt = 0;

    std::unique_ptr<Job> job;
    juce::ComponentDragger dragger;

    juce::Rectangle<int> headerR, eyebrowR, headlineR, explainR, requestR, statusR, actionsR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseDialog)
};
