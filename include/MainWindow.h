/*
    pt.Live - Live Singing DAW
    Main window with custom dark title bar and mixing infrastructure.

    Layout (spec section 4.1):
        title bar (42)                      Plugins File Devices  [_][o][x]
        inspector (300) | track list (lanes) | master meter (44)
        control bar (56)                    BEAT MICRO VANG | TONE | KEY | key readout | [collapse]
        bottom bar (40)                     Record, timecode, format, device status

    The collapse button of the control bar shrinks the window to title bar + control bar only.
    NOTE: native title bar stays disabled; custom TitleBar is used.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Theme.h"
#include "include/TitleBar.h"
#include "include/Audio/AudioEngine.h"
#include "include/Audio/TrackManager.h"
#include "include/UI/TransportBar.h"
#include "include/UI/MixerBoard.h"
#include "include/UI/InspectorPanel.h"
#include "include/UI/UIWidgets.h"
#include "include/UI/PluginBrowserDialog.h"
#include "include/UI/AudioSettingsDialog.h"
#include "include/Audio/LiveController.h"
#include "include/UI/ControlBar.h"
#include "include/Net/KeyFetcher.h"
#include "include/Net/SongDetector.h"
#include "include/Audio/SongHistory.h"
#include "include/Ktv.h"

class MainWindow : public juce::ResizableWindow,
                   private juce::Timer
{
public:
    // fileToOpen: a .pt session (double-clicked in Explorer / command line). Empty = default session.
    // tokenProvider: where the VPS client gets the license token (X-License-Token). Empty = key-only mode.
    // startInSafeMode: Shift was held at start. NO external plugin (VST3 / VST2) is loaded or scanned, so a
    // plugin that crashes while the session opens can still be removed. See PluginScanner::setSafeMode().
    explicit MainWindow(juce::String name, const juce::File& fileToOpen = {},
                        std::function<juce::String()> tokenProvider = {},
                        bool startInSafeMode = false);
    ~MainWindow() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;       // Ctrl+O / Ctrl+S / Ctrl+Shift+S

    // Closing the app: asks "Save changes?" when the session has unsaved changes.
    // done(true) = go ahead and quit, nothing is called when the user cancels.
    void confirmClose(std::function<void(bool)> done);

    // A second pt.Live was started (e.g. a .pt double-clicked while this one runs): this window
    // comes to the front and says that the running app must be closed first.
    void showAlreadyRunningWarning(const juce::File& requestedFile);
    void bringToFrontForUser();

    // License UI. The application owns the LicenseClient: it shows the dialog when `onLicenseRequested` fires
    // (double-click on the licence status on the title bar) and feeds the plan + end date back with setLicenseStatus().
    std::function<void()> onLicenseRequested;
    void setLicenseStatus(bool isTrial, const juce::String& planLabel, juce::int64 expiresAtSec, const juce::String& tooltip);

    // Scale management (Phase 6)
    float getScaleFactor() const { return scaleFactor; }
    void setScaleFactor(float factor);
    void applyScale();

private:
    // Window size / position persistence + DPI-aware default size
    static juce::File getWindowStateFile();
    juce::Rectangle<int> getDefaultWindowBounds() const;
    void saveWindowState();
    void restoreWindowState();

    // Startup lock: from the moment the window appears until the audio device has been opened
    // (once, see AudioEngine::openSavedASIO in initialize()) nothing can be clicked or typed, so no
    // user action can race the driver. Released by a timer, or by a safety timeout.
    void lockUiForStartup();
    void unlockUiAfterStartup();
    std::unique_ptr<juce::Component> startupGate;
    std::unique_ptr<juce::Component> deviceBanner;      // "audio device lost" strip with a Reopen button
    void showDeviceBanner (const juce::String& text);
    void hideDeviceBannerAsync();
    void reopenAudioDevice (bool forceReopen = false);   // forceReopen = close + open even when already open
    void scheduleDriverReloadAfterInputChange();         // ONE reload per input change (changes close together share it)
    bool inputReloadPending = false;
    bool startupUnlocked = false;

    // Safe mode (see the constructor parameter). The first Save over an existing .pt keeps a copy of the
    // file as "<name>.before-safe.pt": a session opened without its plugins must never destroy the original.
    bool safeMode = false;
    juce::StringArray safeBackedUp;                     // targets already backed up in this run
    int countLoadedExternalPlugins (int* total = nullptr);   // external inserts in the session / how many really loaded
    void showSafeModeNotice();
    void backupBeforeSafeSave (const juce::File& target);

    void showPluginBrowser();
    void showAudioSettings();
    void setPDCEnabled(bool enabled);

    // File menu / session (.pt)
    void showFileMenu(juce::Component& target);
    void openSession();                                             // File > Open: pick a .pt, restart pt.Live with it
    void relaunchWithFile(const juce::File& file);
    void saveSession(std::function<void(bool)> done = nullptr);     // no file yet -> Save As
    void saveSessionAs(std::function<void(bool)> done = nullptr);
    void writeSession(const juce::File& file, std::function<void(bool)> done);
    void associateFiles();

    //=== KTV (technician) mode (see include/Ktv.h) ============================
    // The KTV copy lives inside the .pt, so it belongs to that one file. The password only guards CHANGING
    // the KTV data; the title bar RESET (resetToKtvCopy) needs no password.
    void ktvAuthenticate(std::function<void()> onOk);   // asks the KTV password (or creates it the first time)
    void ktvSaveBase();                                 // save the file AND keep this state as the KTV copy
    void resetToKtvCopy();                              // title bar RESET: file back to the KTV copy, then restart
    void ktvChangePassword();
    void ktvRemove();                                   // remove KTV data from this file
    bool ktvAuthed = false;                             // the KTV password was entered in this session

    // Full / collapsed: the app starts the way it was closed.
    static juce::File getUiModeFile();
    void saveUiMode() const;
    void applyStartupMode();

    void updateDocumentTitle();
    void timerCallback() override;
    void addTrack(TrackManager::TrackType type);
    void deleteTrackDeferred(te::Track* track);

    //=== Live control (control bar, song detection, VPS key) ================
    void setupLive();
    juce::Rectangle<int> getWorkArea() const;                      // screen without the taskbar
    juce::Rectangle<int> centredFullBounds(int w, int h) const;     // full app: always the middle of the work area
    void setCompact(bool compact);        // collapse to title bar + control bar / bring everything back
    void onSongDetected(const SongDetector::Song& song);
    void onKeyFound(const juce::String& videoId, const VpsClient::KeyResult& r, bool fromCache);
    void onKeyFailed(const juce::String& videoId, const juce::String& reason);

    // DANH SÁCH history popup and Record handling
    void showHistoryPopup();
    void addToHistory(const juce::String& id, const juce::String& url, const juce::String& title);
    void openFromHistory(const juce::String& url);
    void updateHistoryKey(const juce::String& videoId, const juce::String& root, const juce::String& scale);

    bool compact = false;
    juce::Rectangle<int> normalBounds;
    int normalMinW = 0, normalMinH = 0;
    int normalBaseMinW = 0;               // normal minimum width without the control bar's extra need (toggles)
    int compactMinW = 0;                  // minimum width of the collapsed panel right now (follows the toggles)
    int compactMinWidth() const;          // what the collapsed panel needs for its current toggles
    void onControlBarWidthChanged();      // a toggle was added / removed: stretch (or shrink) the window to fit
    juce::String currentVideoId, currentTitle, currentUrl;

    juce::TooltipWindow tooltipWindow { this, 600 };
    TitleBar titleBar { *this };
    TransportBar transportBar;                 // bottom bar
    ptl::ui::MasterMeter masterMeter;
    AudioEngine audioEngine;

    // Declared BEFORE the inspector (which keeps a pointer to it) so it is destroyed AFTER it.
    std::unique_ptr<PluginScanner> pluginScanner;

    std::unique_ptr<TrackManager> trackManager;
    std::unique_ptr<MixerBoard> mixerBoard;
    std::unique_ptr<InspectorPanel> inspectorPanel;

    // Declared AFTER the track manager (they use it) so they are destroyed BEFORE it.
    std::unique_ptr<LiveController> liveController;
    std::unique_ptr<ControlBar> controlBar;
    std::function<juce::String()> licenseTokenProvider;   // handed to keyFetcher in setupLive()
    std::unique_ptr<KeyFetcher> keyFetcher;
    std::unique_ptr<SongDetector> songDetector;
    std::unique_ptr<ptl::SongHistory> songHistory;       // persistent history of opened songs

    float scaleFactor = 1.0f;

    std::unique_ptr<juce::FileChooser> fileChooser;   // kept alive while the Save As dialog is open
    bool closePromptShowing = false;
    juce::String shownTitle;
    bool shownDirty = false;
    int timerTicks = 0;

    // xrun / NaN / over: only written to ptLive.log (never shown in the UI), see logSignalStats().
    unsigned int lastXruns = 0, lastNan = 0, lastOver = 0;
    void logSignalStats();
    // One line per hour (kept in ptLive.log): the app is still running and what grew in that hour, so a long
    // session's log can be read at a glance ("+0 / +0 / +0" = healthy).
    unsigned int hourXruns = 0, hourNan = 0, hourOver = 0;
    void logHealthLine();
    void serviceRecording();            // disk space + 4 GB WAV limit while recording
    void chooseRecordingFolder();       // File > Recording folder...

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
