/*
    pt.Live - Live Singing DAW
    Update dialog - shown ONCE, a few seconds after the main window opened, when the VPS has a newer version.

        [ animated pt.Live brand ]                                              [x]
        ------------------------------------------------------------------------
        Có bản cập nhật mới
        pt.Live 0.1.1 đã sẵn sàng (bạn đang dùng 0.1.0)

        <release notes, plain text, up to 7 lines>

        [  Cập nhật  ]   [   Hủy   ]   [  Bỏ qua bản này  ]

    Cập nhật       -> the installer is DOWNLOADED INSIDE THIS DIALOG (progress bar, MB, %, [Hủy tải]); when it is
                      complete the dialog closes and reports Choice::update + the installer file. The app then
                      closes itself, installs over the old version and starts again (MainApplication).
                      A failed download shows the reason with [Thử lại] / [Đóng] / [Mở trình duyệt].
    Hủy            -> Choice::later  (asked again at the next start)
    Bỏ qua bản này -> Choice::skip   (not asked again for THIS version; a newer one is offered again)
    Enter = Cập nhật, Esc / [x] = Hủy (while downloading: stops the download). The result is reported once, from a
    clean message-loop turn, after the window has closed. Borderless window in the same style as WelcomeDialog /
    LicenseDialog.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"
#include "include/TitleBar.h"
#include "include/UI/UIWidgets.h"
#include "include/Net/UpdateChecker.h"

class UpdateDialog : public juce::Component, private juce::Timer
{
public:
    enum class Choice { update, later, skip };
    // `installer` is only valid for Choice::update: the downloaded setup file, ready to run.
    using Chosen = std::function<void (Choice choice, const juce::File& installer)>;

    static void show (const juce::String& currentVersion, const juce::String& newVersion,
                      const juce::String& notes, const juce::String& url, const juce::String& sha256, Chosen onChosen);

    UpdateDialog (const juce::String& currentVersion, const juce::String& newVersion,
                  const juce::String& notes, const juce::String& url, const juce::String& sha256, Chosen onChosen);
    ~UpdateDialog() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

    static constexpr int dialogWidth = 520;

private:
    enum class Mode { ready, downloading, installing, failed };

    void choose (Choice c);
    void startDownload();
    void downloadFinished (bool ok, const juce::File& installerFile, UpdateDownloader::Error error);
    void applyMode();
    void timerCallback() override;               // repaints the progress bar while downloading

    Chosen onChosen;
    bool finished = false;
    juce::String currentVersion, newVersion, notes, url, sha256;

    Mode mode = Mode::ready;
    UpdateDownloader::Error lastError = UpdateDownloader::Error::none;
    juce::File installerFile;
    UpdateDownloader downloader;

    BrandMark brand;
    juce::TextButton updateBtn, laterBtn, skipBtn;
    juce::ComponentDragger dragger;

    juce::Rectangle<int> headerR, headlineR, subR, notesR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateDialog)
};
