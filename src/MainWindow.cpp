/*
    pt.Live - Live Singing DAW
    Main window implementation

    A fresh session starts with BEAT (stereo in), MICRO (mono in), FX 1, FX 2 and the Master.
    More tracks are added with the "+" button of each mixer group.
*/

#include "include/MainWindow.h"
#include "include/Audio/PluginWindow.h"
#include "include/FileAssociation.h"
#include "include/RecentFiles.h"
#include "include/Audio/PtLog.h"
#include "include/UI/MasterRecorder.h"
#include "include/Net/TabNavigator.h"
#include "include/UI/HistoryPanel.h"

#include <map>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

using namespace ptl;

MainWindow::MainWindow(juce::String name, const juce::File& fileToOpen, std::function<juce::String()> tokenProvider,
                       bool startInSafeMode)
    : juce::ResizableWindow(name, false),
      titleBar(*this),
      licenseTokenProvider(std::move(tokenProvider))
{
    safeMode = startInSafeMode;

    //=== Plugin scanner (also feeds the inspector's "Add insert" menu) =======
    // Created first: its cached VST3 list is handed to the engine so that a saved session's
    // inserts can be re-instantiated while the session loads.
    // Safe mode: the list stays EMPTY, so the engine cannot find any plugin to instantiate and the session's
    // inserts load as "failed to load" placeholders (the same state a missing plugin has).
    pluginScanner = std::make_unique<PluginScanner>();
    pluginScanner->setSafeMode(safeMode);
    pluginScanner->loadCachedPlugins();

    if (safeMode)
        ptl::diag::write("[safe] SAFE MODE on: external plugins are not registered, not loaded, not scanned");

    // Plugin editor windows are owned by this window (always above it, never system-wide topmost).
    ptl::pluginhost::setMainWindow(this);

    //=== Audio engine (loads the .pt we were started with, otherwise the default session) ====
    audioEngine.initialize(pluginScanner->getScannedPlugins(), fileToOpen);

    // Second chance for the start screen's RECENT list: remember the session that really loaded
    // (also covers a path that reached the engine some other way than the command line).
    ptl::recent::add(audioEngine.getCurrentSessionFile());
    transportBar.setAudioEngine(&audioEngine);
    addAndMakeVisible(&transportBar);
    masterMeter.setShowScale(true);                 // dB scale + wider LEDs: the strip is no longer a thin line in an empty column
    addAndMakeVisible(&masterMeter);

    // The audio device is NOT touched here. It is opened exactly once, by
    // audioEngine.beginStartupOpen(), after this window is completely built and visible and a
    // short quiet moment has passed (see the end of this constructor), while the UI is locked.
    // Playback (live monitoring) is started when that open has finished.
    juce::Component::SafePointer<MainWindow> safe(this);

    //=== Track manager + Mixer + Inspector ===================================
    auto* edit = audioEngine.getEdit();
    if (edit != nullptr)
    {
        trackManager   = std::make_unique<TrackManager>(*audioEngine.getEngine(), *edit);

        // Default session -> BEAT, MICRO, FX 1, FX 2 (does nothing when a .pt restored its tracks).
        trackManager->ensureDefaultTracks();

        // Safe mode relies on the engine NOT instantiating a plugin it has no description for. Check it and say
        // so in the log: "0 of N loaded" = the assumption holds; anything else is shown to the user below.
        if (safeMode)
        {
            int total = 0;
            const int loaded = countLoadedExternalPlugins(&total);
            ptl::diag::write("[safe] external inserts in the session: " + juce::String(total)
                             + ", loaded by the engine: " + juce::String(loaded)
                             + (loaded == 0 ? juce::String(" (ok)") : juce::String(" - WARNING: safe mode did not stop them")));
        }
        mixerBoard     = std::make_unique<MixerBoard>(*trackManager);
        inspectorPanel = std::make_unique<InspectorPanel>(*trackManager, pluginScanner.get());

        if (auto* dm = audioEngine.getDeviceManager())
            mixerBoard->setDeviceManager(dm);

        // NOTE: applyAllTrackChannelLayouts() is DELIBERATELY NOT called here.
        // It would call edit->restartPlayback() while restored VST3 plugins are still
        // in their lazy initialiseFully() phase, causing a deadlock in Tracktion's
        // graph builder (waitForFinalNode() never returns). Instead, it is called once
        // in unlockUiAfterStartup(), after the audio device is open and the UI is ready
        // (plugins have had ~1.5s to fully initialise). See unlockUiAfterStartup().

        addAndMakeVisible(mixerBoard.get());
        addAndMakeVisible(inspectorPanel.get());

        mixerBoard->onTrackSelected = [this](te::Track* t)
        {
            if (inspectorPanel != nullptr)
                inspectorPanel->setSelectedTrack(t);
        };
        // The user picked another input on a track: reload the driver once (same reload as the one after start-up).
        mixerBoard->onTrackInputChanged = [this](te::Track*) { scheduleDriverReloadAfterInputChange(); };
        mixerBoard->onTrackRenamed = [this](te::Track*)
        {
            if (inspectorPanel != nullptr)
                inspectorPanel->refresh();
        };
        mixerBoard->onAddTrackRequested = [this](TrackManager::TrackType type) { addTrack(type); };
        mixerBoard->onMasterLevels      = [this](float l, float r) { masterMeter.setLevels(l, r); };
        mixerBoard->onDeleteRequested   = [this](te::Track* t) { deleteTrackDeferred(t); };

        // Select the first track (BEAT on a fresh session) so the inspector is not empty.
        mixerBoard->syncWithTrackManager();
        mixerBoard->selectTrack(trackManager->getNumTracks() > 0 ? static_cast<te::Track*>(trackManager->getTrackAt(0))
                                                                 : static_cast<te::Track*>(trackManager->getMasterTrack()),
                                true);
    }

    //=== Custom title bar (native stays disabled) ==========================
    setUsingNativeTitleBar(false);
    addAndMakeVisible(&titleBar);

    audioEngine.onDeviceProblem = [this] (const juce::String& message)
    {
        showDeviceBanner (juce::String::fromUTF8 ("Thi\xE1\xBA\xBFt b\xE1\xBB\x8B \xC3\xA2m thanh b\xE1\xBB\x8B m\xE1\xBA\xA5t: ") + message);
    };

    titleBar.onDevices  = [this] { showAudioSettings(); };
    titleBar.onDevicesReload = [this] { return audioEngine.openSavedASIO(); };       // double-click on Devices
    titleBar.onPlugins  = [this] { showPluginBrowser(); };
    titleBar.onReset    = [this] { resetToKtvCopy(); };         // no password
    titleBar.onLicense  = [this] { if (onLicenseRequested) onLicenseRequested(); };
    // PDC is OFF until the user ticks the checkbox in Devices > Audio Settings.
    titleBar.onFileMenu = [this](juce::Component& button) { showFileMenu(button); };

    //=== Live control panel (BEAT / MICRO / VANG, tone, key, collapse) ========
    setupLive();

    // Wire up the new DANH SÁCH and Record controls. Song history is loaded once here.
    if (songHistory == nullptr)
        songHistory = std::make_unique<ptl::SongHistory>();
    songHistory->load();
    controlBar->onShowHistory   = [this] { showHistoryPopup(); };
    controlBar->isRecording = [this]
    {
        auto* rec = audioEngine.getMasterRecorder();
        return rec != nullptr && rec->isRecording();
    };
    controlBar->getRecordSeconds = [this]
    {
        auto* rec = audioEngine.getMasterRecorder();
        return rec != nullptr && rec->isRecording() ? rec->getDurationSeconds() : 0.0;
    };
    controlBar->onRecordPressed = [this]
    {
        if (audioEngine.getMasterRecorder() == nullptr) return;

        auto* rec = audioEngine.getMasterRecorder();
        const bool wantRec = !(rec->isRecording());

        if (wantRec)
        {
            if (! audioEngine.isPlaying())
                audioEngine.play();

            juce::String err;
            if (! rec->beginRecording (err))
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record", err, "OK");
                return;
            }

            if (rec->getStartWarning().isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record", rec->getStartWarning(), "OK");
        }
        else
            rec->stopRecording();

        controlBar->refresh();
    };

    //=== Styling ============================================================
    setColour(juce::ResizableWindow::backgroundColourId, theme::window);

    //=== Size limits: never larger than the screen's work area (high OS scale, e.g. 200-300%) ===
    const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                          ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                          : juce::Rectangle<int>(0, 0, 1920, 1080);

    const int minW = juce::jmin(theme::minWindowWidth,  juce::jmax(480, area.getWidth()));
    const int minH = juce::jmin(theme::minWindowHeight, juce::jmax(320, area.getHeight()));
    normalMinW = minW;
    normalMinH = minH;
    normalBaseMinW = minW;
    setResizeLimits(minW, minH, 16384, 16384);

    // Borderless window (custom title bar) => JUCE must supply the resize edges itself.
    // Without this call the window could not be resized by dragging its edges.
    setResizable(true, false);

    //=== Initial size: scales with the display, clamped to the work area =====
    const auto def = getDefaultWindowBounds();
    setSize(def.getWidth(), def.getHeight());
    centreWithSize(def.getWidth(), def.getHeight());
    addToDesktop(juce::ComponentPeer::windowHasTitleBar
                 | juce::ComponentPeer::windowIsResizable);

    // Always start at the default size (no state from the previous session).
    restoreWindowState();

    // The app starts the way it was closed: collapsed panel or full (before the window is shown).
    applyStartupMode();

    setVisible(true);

    // Document state: the title shows the file name (and * when unsaved). Startup work (default
    // tracks, driver warm-up) must not count as "unsaved changes", so the clean baseline is taken
    // now and once more after the driver has settled (see timerCallback).
    audioEngine.markSessionClean();
    updateDocumentTitle();
    startTimer(500);

    if (audioEngine.didRequestedFileFail())
    {
        const auto bad = audioEngine.getRequestedFile();
        juce::MessageManager::callAsync([safe, bad]
        {
            if (safe != nullptr)
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "pt.Live",
                    "Could not open:\n" + bad.getFullPathName()
                    + "\n\nThe file is missing, unreadable or not a pt.Live session.\n"
                      "pt.Live started with the default session. The file was not changed.", "OK");
        });
    }

    //=== Startup: lock the UI, wait for the window to settle, then open the device ONCE ======
    lockUiForStartup();

    // The device is already open (audioEngine.initialize() -> createOrLoadEdit() -> openSavedASIO()), so there is
    // nothing left to wait for except the one-time graph rebuild that attaches the master tap (AudioEngine::
    // ensureTapActive). That rebuild now runs during start-up, BEFORE play(), so it cannot cut the sound.
    //
    // 1) kHideStartupNoticeMs after the window is built: the "pt.Live dang khoi dong..." veil disappears.
    // 2) Then wait (poll every 50 ms, at most kMaxSettleWaitMs) until the graph is final, and only then
    //    unlock + play(). Before: a fixed 2 s + 1 s wait, THEN play(), THEN a rebuild = sound came 3.5 s+ late.
    constexpr int kHideStartupNoticeMs = 600;
    constexpr int kMaxSettleWaitMs     = 2500;

    struct SettleWaiter
    {
        static void poll (juce::Component::SafePointer<MainWindow> w, int waitedMs)
        {
            if (w == nullptr)
                return;

            if (w->audioEngine.isStartupGraphSettled() || waitedMs >= kMaxSettleWaitMs)
            {
                w->audioEngine.unlockStartupPhase();
                w->unlockUiAfterStartup();          // UI unlocked, layouts applied, play() running

                // ONE extra driver reload per app start. Without it the mic is late until the user reloads the
                // driver by hand (double-click Devices). It must happen the way the manual reload does: AFTER
                // the app is running and the first device-change events of the start-up open have been handled.
                // (Doing it before play() made the sound start and then vanish: the driver's late "device
                // changed" event rebuilt the wave devices right when play() started.)
                juce::Timer::callAfterDelay (1500, [w]
                {
                    if (w == nullptr)
                        return;

                    auto& eng = w->audioEngine;
                    auto* rec = eng.getMasterRecorder();

                    if (rec != nullptr && rec->isRecording())
                        return;                     // never cut a recording

                    eng.openSavedASIO (true);

                    if (! eng.isPlaying())
                        eng.play();

                    juce::Timer::callAfterDelay (1500, [w] { if (w != nullptr) w->audioEngine.markSessionClean(); });
                });
                return;
            }

            juce::Timer::callAfterDelay (50, [w, waitedMs] { poll (w, waitedMs + 50); });
        }
    };

    juce::Timer::callAfterDelay(kHideStartupNoticeMs, [safe]
    {
        if (safe == nullptr)
            return;

        safe->startupGate.reset();                     // notice gone
        SettleWaiter::poll (safe, 0);
    });

    // Safety net: never leave the app locked, whatever a driver does.
    juce::Timer::callAfterDelay(15000, [safe]
    {
        if (safe != nullptr)
        {
            safe->audioEngine.unlockStartupPhase();
            safe->unlockUiAfterStartup();
        }
    });
}

//==============================================================================
namespace
{
    // Dark veil over everything below the title bar; swallows mouse and keyboard.
    class StartupGate : public juce::Component
    {
    public:
        StartupGate()
        {
            setInterceptsMouseClicks(true, true);
            setWantsKeyboardFocus(true);
            setMouseCursor(juce::MouseCursor::WaitCursor);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::black.withAlpha(0.55f));
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(18.0f)).boldened());
            g.drawFittedText(juce::String::fromUTF8("pt.Live \xC4\x91" "ang kh\xE1\xBB\x9F" "i \xC4\x91\xE1\xBB\x99ng\xE2\x80\xA6"),
                             getLocalBounds().withSizeKeepingCentre(getWidth(), 40),
                             juce::Justification::centred, 1);
        }

        bool keyPressed(const juce::KeyPress&) override { return true; }
    };
}

void MainWindow::setLicenseStatus(bool isTrial, const juce::String& planLabel, juce::int64 expiresAtSec, const juce::String& tooltip)
{
    titleBar.setLicenseStatus(isTrial, planLabel, expiresAtSec, tooltip);
}

namespace
{
    // Non-blocking strip over the top of the work area: what went wrong + a "Reopen" button.
    class DeviceBanner : public juce::Component
    {
    public:
        DeviceBanner()
        {
            reopen.setButtonText (juce::String::fromUTF8 ("M\xE1\xBB\x9F l\xE1\xBA\xA1i"));
            close.setButtonText ("X");
            addAndMakeVisible (reopen);
            addAndMakeVisible (close);
            reopen.onClick = [this] { if (onReopen) onReopen(); };
            close.onClick  = [this] { if (onClose)  onClose(); };
        }

        void setMessage (const juce::String& m) { message = m; repaint(); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::danger.darker (0.35f));
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawFittedText (message, getLocalBounds().withTrimmedLeft (12).withTrimmedRight (160),
                              juce::Justification::centredLeft, 1);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (4);
            close.setBounds (r.removeFromRight (28));
            r.removeFromRight (6);
            reopen.setBounds (r.removeFromRight (110));
        }

        std::function<void()> onReopen, onClose;

    private:
        juce::TextButton reopen, close;
        juce::String message;
    };
}

void MainWindow::showDeviceBanner (const juce::String& text)
{
    if (deviceBanner == nullptr)
    {
        auto b = std::make_unique<DeviceBanner>();
        juce::Component::SafePointer<MainWindow> safe (this);
        b->onReopen = [safe] { if (safe != nullptr) safe->reopenAudioDevice(); };
        b->onClose  = [safe] { if (safe != nullptr) safe->hideDeviceBannerAsync(); };
        deviceBanner = std::move (b);
        addAndMakeVisible (deviceBanner.get());
    }

    static_cast<DeviceBanner*> (deviceBanner.get())->setMessage (text);
    resized();
    deviceBanner->toFront (false);
}

// Deleting the banner from inside its own button callback is not safe: do it a moment later.
void MainWindow::hideDeviceBannerAsync()
{
    juce::Component::SafePointer<MainWindow> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->deviceBanner.reset(); });
}

void MainWindow::scheduleDriverReloadAfterInputChange()
{
    if (inputReloadPending)
        return;                                    // the waiting reload will pick up this change too

    inputReloadPending = true;

    juce::Component::SafePointer<MainWindow> safe (this);
    juce::Timer::callAfterDelay (300, [safe]
    {
        if (safe == nullptr)
            return;

        safe->inputReloadPending = false;

        auto* rec = safe->audioEngine.getMasterRecorder();

        if (rec != nullptr && rec->isRecording())
        {
            safe->scheduleDriverReloadAfterInputChange();   // never cut a recording: try again a bit later
            return;
        }

        safe->reopenAudioDevice (true);
    });
}

void MainWindow::reopenAudioDevice (bool forceReopen)
{
    const bool ok = audioEngine.openSavedASIO (forceReopen);
    const auto problem = audioEngine.getDriverOpenProblem();

    if (ok && problem.isEmpty())
    {
        if (! audioEngine.isPlaying())
            audioEngine.play();

        hideDeviceBannerAsync();
        return;
    }

    showDeviceBanner (problem.isNotEmpty() ? juce::String::fromUTF8 ("Kh\xC3\xB4ng m\xE1\xBB\x9F \xC4\x91\xC6\xB0\xE1\xBB\xA3" "c driver '") + problem + "'"
                                           : juce::String::fromUTF8 ("V\xE1\xBA\xABn ch\xC6\xB0" "a m\xE1\xBB\x9F \xC4\x91\xC6\xB0\xE1\xBB\xA3" "c thi\xE1\xBA\xBFt b\xE1\xBB\x8B \xC3\xA2m thanh."));
}

void MainWindow::lockUiForStartup()
{
    titleBar.setMenusEnabled(false);       // Devices / File / Plugins / License (window buttons stay usable)

    startupGate = std::make_unique<StartupGate>();
    addAndMakeVisible(startupGate.get());
    startupGate->setBounds(getLocalBounds().withTrimmedTop(theme::titleBarH));
    startupGate->toFront(false);
    startupGate->grabKeyboardFocus();
}

void MainWindow::unlockUiAfterStartup()
{
    if (startupUnlocked)
        return;

    startupUnlocked = true;

    startupGate.reset();
    titleBar.setMenusEnabled(true);

    // The saved driver could not be opened at start-up (never reported before).
    if (const auto problem = audioEngine.getDriverOpenProblem(); problem.isNotEmpty())
        showDeviceBanner (juce::String::fromUTF8 ("Kh\xC3\xB4ng m\xE1\xBB\x9F \xC4\x91\xC6\xB0\xE1\xBB\xA3" "c driver '") + problem + "'");

    // Sessions saved before this fix may hold stereo-layout plugins on MONO IN tracks.
    // Apply mono channel layouts AFTER the device is open and plugins have time to fully
    // initialize (avoiding deadlock in Tracktion's waitForFinalNode() during graph build).
    if (trackManager != nullptr)
        trackManager->applyAllTrackChannelLayouts();

    // Live monitoring simply runs while the app is open (the Monitor button is gone). Started only
    // now: the graph is built once, against the final device.
    audioEngine.play();

    // The startup work (default tracks, device open) must not count as "unsaved changes".
    audioEngine.markSessionClean();

    if (safeMode)
        showSafeModeNotice();
}

// Counts the external (VST3 / VST2) inserts of the session and, through `total`, how many exist.
// Returns how many of them the engine really instantiated (a plugin DLL is in memory).
int MainWindow::countLoadedExternalPlugins(int* total)
{
    int loaded = 0, all = 0;

    auto visit = [&] (te::PluginList& list)
    {
        for (auto* p : list.getPlugins())
            if (auto* ext = dynamic_cast<te::ExternalPlugin*>(p))
            {
                ++all;
                if (ext->getAudioPluginInstance() != nullptr)
                    ++loaded;
            }
    };

    if (trackManager != nullptr)
    {
        for (int i = 0; i < trackManager->getNumTracks(); ++i)
            if (auto* t = trackManager->getTrackAt(i))
                visit(trackManager->getPluginList(t));

        if (auto* m = trackManager->getMasterTrack())
            visit(trackManager->getPluginList(m));
    }

    if (total != nullptr)
        *total = all;

    return loaded;
}

void MainWindow::showSafeModeNotice()
{
    int total = 0;
    const int loaded = countLoadedExternalPlugins(&total);

    juce::String text = juce::String::fromUTF8(
        "pt.Live đang chạy ở Safe mode: KHÔNG nạp plugin bên ngoài (VST3/VST2).\n\n"
        "• Các insert plugin trong session hiện là chỗ trống \"không nạp được\". Bạn có thể xóa insert gây lỗi rồi lưu lại.\n"
        "• Không thêm được plugin mới và không quét plugin ở chế độ này.\n"
        "• Lần Lưu đầu tiên đè lên một file .pt sẽ tạo bản sao \"<tên>.before-safe.pt\" bên cạnh file đó.\n\n"
        "Muốn chạy bình thường: đóng pt.Live rồi mở lại, không giữ Shift.");

    if (loaded > 0)
        text += juce::String::fromUTF8("\n\nLƯU Ý: ") + juce::String(loaded) + "/" + juce::String(total)
              + juce::String::fromUTF8(" plugin vẫn được nạp. Hãy gửi file ptLive.log cho người hỗ trợ.");

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           juce::String::fromUTF8("Safe mode: plugin bị tắt"), text, "OK");
}

// First Save over an existing file in safe mode: keep what was on disk. Tracktion should keep the state of an
// insert that never loaded, but "should" is not good enough for a file that holds a whole live set.
void MainWindow::backupBeforeSafeSave(const juce::File& target)
{
    if (! safeMode || ! target.existsAsFile() || safeBackedUp.contains(target.getFullPathName()))
        return;

    safeBackedUp.add(target.getFullPathName());

    const auto backup = target.getSiblingFile(target.getFileNameWithoutExtension() + ".before-safe" + target.getFileExtension());

    if (backup.existsAsFile())                       // never overwrite an older backup: it may be the only good copy
    {
        ptl::diag::write("[safe] backup already exists, kept: " + backup.getFullPathName());
        return;
    }

    if (target.copyFileTo(backup))
        ptl::diag::write("[safe] backup written: " + backup.getFullPathName());
    else
        ptl::diag::write("[safe] could not write backup: " + backup.getFullPathName());      // the save still goes on
}

MainWindow::~MainWindow()
{
    // Hide FIRST. Closing takes a moment (threads are stopped, state is saved) and a window that is
    // resized / half destroyed during that time shows up as a big white rectangle. Everything below
    // happens on a window nobody can see any more.
    setVisible(false);

    audioEngine.onDeviceProblem = nullptr;         // no more banners while closing

    titleBar.brandMark.levelProvider = nullptr;    // releases the master meter client before the live controller goes

    stopTimer();

    {
        auto& st = ptl::signalStats();
        const auto xr   = st.xruns.load     (std::memory_order_relaxed);
        const auto nan  = st.nanCount.load  (std::memory_order_relaxed);
        const auto over = st.overCount.load (std::memory_order_relaxed);

        if (xr > 0 || nan > 0 || over > 0)
            ptl::diag::write("[xrun] session summary: total " + juce::String(xr)
                             + " | nan " + juce::String(nan) + " | over " + juce::String(over));
    }

    // Stop the background threads FIRST: their callbacks point at this window.
    // Ask ALL of them to finish first, then wait: the waits overlap instead of adding up
    // (before: song detector + key fetcher + ... one after the other = many seconds with no window).
    if (songDetector != nullptr) songDetector->requestStop();
    if (keyFetcher   != nullptr) keyFetcher->requestStop();
    if (pluginScanner != nullptr) pluginScanner->signalThreadShouldExit();

    keyFetcher.reset();
    songDetector.reset();

    // The window size is deliberately NOT saved: every start uses the default size (see restoreWindowState).
}

//==============================================================================
// Default size. Logical units (JUCE is per-monitor DPI aware, so 200% OS scale on a 4K
// screen reports a 1920x1080 logical work area -> 1280x720 as designed).
// Large logical areas (2K / 4K at 100%) get a proportionally larger window instead of a
// small 1280x720 island; small ones (1080p at 200-300%) are clamped to fit.
juce::Rectangle<int> MainWindow::getDefaultWindowBounds() const
{
    juce::Rectangle<int> area(0, 0, 1920, 1080);

    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        area = display->userArea;

    const double k = juce::jlimit(1.0, 2.0, juce::jmin(area.getWidth()  / 1920.0,
                                                       area.getHeight() / 1080.0));

    int w = juce::roundToInt(theme::baseWindowWidth  * k);
    int h = juce::roundToInt(theme::baseWindowHeight * k);

    w = juce::jmin(w, juce::roundToInt(area.getWidth()  * 0.92f));
    h = juce::jmin(h, juce::roundToInt(area.getHeight() * 0.92f));

    const int minW = juce::jmin(theme::minWindowWidth,  area.getWidth());
    const int minH = juce::jmin(theme::minWindowHeight, area.getHeight());

    w = juce::jmax(minW, w);
    h = juce::jmax(minH, h);

    return juce::Rectangle<int>(w, h).withCentre(area.getCentre());
}

//==============================================================================
juce::File MainWindow::getWindowStateFile()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("ptLive");
    dir.createDirectory();
    return dir.getChildFile("window_state.txt");
}

void MainWindow::saveWindowState()
{
    getWindowStateFile().replaceWithText(getWindowStateAsString());
}

// Work area (screen WITHOUT the taskbar) of the display the window is on - the primary one when unknown.
juce::Rectangle<int> MainWindow::getWorkArea() const
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    if (isOnDesktop())
        if (auto* d = displays.getDisplayForRect(getScreenBounds()))
            return d->userArea;

    if (auto* d = displays.getPrimaryDisplay())
        return d->userArea;

    return { 0, 0, 1920, 1080 };
}

// The FULL app is always placed in the middle of the work area (never partly below the screen / under the
// taskbar), whatever position was saved or wherever the collapsed panel was.
juce::Rectangle<int> MainWindow::centredFullBounds(int w, int h) const
{
    const auto work = getWorkArea();

    w = juce::jlimit(juce::jmin(normalMinW > 0 ? normalMinW : theme::minWindowWidth,  work.getWidth()),  work.getWidth(),  w);
    h = juce::jlimit(juce::jmin(normalMinH > 0 ? normalMinH : theme::minWindowHeight, work.getHeight()), work.getHeight(), h);

    return juce::Rectangle<int>(w, h).withCentre(work.getCentre());
}

// Every start opens the window at its DEFAULT size, in the middle of the work area, with every part of the
// app visible (never too small, never bigger than the screen). Nothing from the last session is restored;
// the user can still resize the window freely while the app runs, and the next start is the default again.
void MainWindow::restoreWindowState()
{
    const auto def = getDefaultWindowBounds();

    if (isFullScreen())
        setFullScreen(false);

    setBounds(centredFullBounds(def.getWidth(), def.getHeight()));
    ptl::diag::write("[window] opened at default size " + getBounds().toString());
}

void MainWindow::addTrack(TrackManager::TrackType type)
{
    if (trackManager == nullptr || mixerBoard == nullptr)
        return;

    if (auto* t = trackManager->createTrack(type))
    {
        mixerBoard->syncWithTrackManager();
        mixerBoard->selectTrack(t, true);                     // show the new track in the inspector
    }
}

// Deleting from inside the row's own button callback would destroy that button while it
// is still executing, so the real work is deferred to the next message-loop iteration.
void MainWindow::deleteTrackDeferred(te::Track* track)
{
    juce::Component::SafePointer<MainWindow> safe(this);

    juce::MessageManager::callAsync([safe, track]
    {
        if (safe == nullptr || safe->trackManager == nullptr || safe->mixerBoard == nullptr)
            return;

        auto* audioTrack = dynamic_cast<te::AudioTrack*>(track);
        if (audioTrack == nullptr)
            return;                                           // Master can't be deleted

        // Drop every reference to the track BEFORE it is destroyed.
        if (safe->mixerBoard->getSelectedTrack() == track)
            safe->mixerBoard->selectTrack(nullptr, true);

        safe->trackManager->deleteTrack(audioTrack);
        safe->mixerBoard->syncWithTrackManager();

        if (safe->inspectorPanel != nullptr)
            safe->inspectorPanel->refresh();                  // send lists may have changed
    });
}

void MainWindow::paint(juce::Graphics& g)
{
    g.fillAll(ptl::theme::window);
}

void MainWindow::resized()
{
    ResizableWindow::resized();     // lays out the resize border (was never called before)

    auto r = getLocalBounds();

    if (startupGate != nullptr)
        startupGate->setBounds(r.withTrimmedTop(theme::titleBarH));

    if (deviceBanner != nullptr)
        deviceBanner->setBounds(r.withTrimmedTop(theme::titleBarH).removeFromTop(36));

    titleBar.setBounds(r.removeFromTop(theme::titleBarH));

    // The control bar is taller when the user added so many sliders / toggles that they need extra rows.
    const int controlBarHeight = controlBar != nullptr ? controlBar->getIdealHeightForWidth(r.getWidth())
                                                       : theme::controlBarH;

    if (compact)
    {
        if (controlBar != nullptr)
            controlBar->setBounds(r.removeFromTop(controlBarHeight));
        return;
    }

    // Order matters: the bottom bar is full width (under inspector and meter too),
    // the control bar sits right above it.
    transportBar.setBounds(r.removeFromBottom(theme::bottomBarH));
    if (controlBar != nullptr)
        controlBar->setBounds(r.removeFromBottom(controlBarHeight));

    auto inspectorArea = r.removeFromLeft(theme::inspectorW);
    auto meterArea     = r.removeFromRight(theme::masterMeterW).reduced(6, 6);

    masterMeter.setBounds(meterArea);

    if (inspectorPanel != nullptr)
        inspectorPanel->setBounds(inspectorArea);
    if (mixerBoard != nullptr)
        mixerBoard->setBounds(r);
}

//==============================================================================
// File menu / session (.pt).
// A .pt holds the whole mixer session (tracks, faders, inserts + plugin state, sends, input
// routing) AND the audio device setup (driver, sample rate, buffer size, ticked channels).
void MainWindow::showFileMenu(juce::Component& target)
{
    const auto file = audioEngine.getCurrentSessionFile();

    juce::PopupMenu menu;

    auto add = [&menu](int id, const juce::String& text, const juce::String& shortcut, bool enabled)
    {
        juce::PopupMenu::Item item(text);
        item.itemID = id;
        item.shortcutKeyDescription = shortcut;
        item.isEnabled = enabled;
        menu.addItem(item);
    };

    auto* edit = audioEngine.getEdit();
    const bool hasK = edit != nullptr && ptl::ktv::hasKtv(edit->state);

    add(6, "Open...",                           "Ctrl+O",       true);
    menu.addSeparator();
    add(1, "Save",                              "Ctrl+S",       true);
    add(2, "Save As...",                        "Ctrl+Shift+S", true);
    menu.addSeparator();
    add(3, "Show in Explorer",                  {},             file.existsAsFile());
    add(4, "Associate .pt files with pt.Live",  {},             true);
    add(30, "Recording folder...",              {},             true);
    menu.addSeparator();

    juce::PopupMenu ktvMenu;
    ktvMenu.addItem(20, juce::String::fromUTF8("Lưu bản gốc KTV..."), edit != nullptr);
    ktvMenu.addItem(23, juce::String::fromUTF8("Đổi password KTV..."), hasK);
    ktvMenu.addItem(24, juce::String::fromUTF8("Gỡ chế độ KTV khỏi file này..."), hasK);
    menu.addSubMenu(juce::String::fromUTF8("Kỹ thuật viên (KTV)"), ktvMenu);

    menu.addSeparator();
    add(5, "Exit",                              "Ctrl+Q",       true);

    juce::Component::SafePointer<MainWindow> safe(this);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(230),
                       [safe](int result)
    {
        if (safe == nullptr)
            return;

        switch (result)
        {
            case 6: safe->openSession();    break;
            case 1: safe->saveSession();    break;
            case 2: safe->saveSessionAs();  break;
            case 3: safe->audioEngine.getCurrentSessionFile().revealToUser(); break;
            case 4: safe->associateFiles(); break;
            case 30: safe->chooseRecordingFolder(); break;
            case 5: if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit(); break;
            case 20: safe->ktvSaveBase();        break;
            case 23: safe->ktvChangePassword();  break;
            case 24: safe->ktvRemove();          break;
            default: break;
        }
    });
}

// File > Open: for the user who started pt.Live directly (not by double-clicking a .pt).
// A running session cannot be swapped under the mixer / inspector / live panel, so the chosen file is
// opened the same way a double-click opens it: after the "Save changes?" question pt.Live closes and
// starts again with that file.
void MainWindow::openSession()
{
    auto start = audioEngine.getCurrentSessionFile();
    if (start == juce::File())
        start = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    fileChooser = std::make_unique<juce::FileChooser>("Open session", start,
                                                      juce::String("*") + AudioEngine::sessionExtension);

    juce::Component::SafePointer<MainWindow> safe(this);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
                             [safe](const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;

        const auto chosen = fc.getResult();
        if (chosen == juce::File() || ! chosen.existsAsFile())
            return;                                                    // cancelled

        // Same file as the open session and nothing changed: nothing to do.
        if (chosen == safe->audioEngine.getCurrentSessionFile() && ! safe->audioEngine.hasUnsavedChanges())
            return;

        safe->confirmClose([safe, chosen](bool goAhead)
        {
            if (goAhead && safe != nullptr)
                safe->relaunchWithFile(chosen);
        });
    });
}

// Starts a NEW pt.Live with `file` once this one has exited, then quits this one. A helper cmd.exe waits
// ~3 s (the single-instance check of the new process must not see this one) and then runs
//     start "" "<ptLive.exe>" "<file.pt>"
void MainWindow::relaunchWithFile(const juce::File& file)
{
   #if JUCE_WINDOWS
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();

    // cmd /c "<command>" : the outer quotes are stripped by cmd, the inner ones stay.
    const juce::String cmd = "cmd.exe /c \"ping -n 4 127.0.0.1 >nul & start \"\" \""
                             + exe + "\" \"" + file.getFullPathName() + "\"\"";

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};

    std::wstring wide = cmd.toWideCharPointer();
    wide.push_back(L'\0');

    if (CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ptl::diag::write("[session] relaunching with " + file.getFullPathName());

        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->quit();                              // the "Save changes?" question was already answered
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "pt.Live",
            "Could not restart pt.Live to open:\n" + file.getFullPathName(), "OK");
    }
   #else
    juce::ignoreUnused(file);
   #endif
}

void MainWindow::saveSession(std::function<void(bool)> done)
{
    const auto file = audioEngine.getCurrentSessionFile();

    if (file == juce::File())                 // default session: no file yet -> ask where
    {
        saveSessionAs(std::move(done));
        return;
    }

    writeSession(file, std::move(done));
}

void MainWindow::saveSessionAs(std::function<void(bool)> done)
{
    auto start = audioEngine.getCurrentSessionFile();

    if (start == juce::File())
        start = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.pt");

    fileChooser = std::make_unique<juce::FileChooser>("Save session as", start,
                                                      juce::String("*") + AudioEngine::sessionExtension);

    juce::Component::SafePointer<MainWindow> safe(this);

    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe, done](const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;

        const auto chosen = fc.getResult();

        if (chosen == juce::File())           // cancelled
        {
            if (done)
                done(false);
            return;
        }

        safe->writeSession(chosen, done);
    });
}

void MainWindow::writeSession(const juce::File& file, std::function<void(bool)> done)
{
    backupBeforeSafeSave(file);

    const bool ok = audioEngine.saveSessionTo(file);
    if (ok)
        ptl::recent::add(file);              // a session saved under a new name belongs in "Recent" too
    titleBar.flashSaveResult(ok);
    updateDocumentTitle();

    if (! ok)
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Save",
            "Could not save the session to:\n" + file.getFullPathName(), "OK");

    if (done)
        done(ok);
}

//==============================================================================
// KTV (technician) mode. The password, the lock flag and the KTV copy are properties of the .pt itself
// (see include/Ktv.h), so they belong to that one file and travel with it.
//==============================================================================
void MainWindow::ktvAuthenticate(std::function<void()> onOk)
{
    auto* edit = audioEngine.getEdit();
    if (edit == nullptr || ! onOk)
        return;

    if (ktvAuthed)                                   // already entered in this session
    {
        onOk();
        return;
    }

    juce::Component::SafePointer<MainWindow> safe(this);

    if (! ptl::ktv::hasKtv(edit->state))             // first time for this file: create the password
    {
        ptl::ktv::askPassword(this, juce::String::fromUTF8("Tạo password KTV"),
            juce::String::fromUTF8("File này chưa có chế độ kỹ thuật viên.\nTạo password KTV cho file này. "
                                   "Giữ kỹ: mất password thì không lưu lại / đổi được bản gốc KTV."),
            true, [safe, onOk](const juce::String& pw)
        {
            if (safe == nullptr) return;
            auto* e = safe->audioEngine.getEdit();
            if (e == nullptr) return;

            ptl::ktv::setPassword(e->state, pw);
            safe->ktvAuthed = true;
            onOk();
        });
        return;
    }

    ptl::ktv::askPassword(this, "Password KTV", juce::String::fromUTF8("Nhập password KTV của file này."),
        false, [safe, onOk](const juce::String& pw)
    {
        if (safe == nullptr) return;
        auto* e = safe->audioEngine.getEdit();
        if (e == nullptr) return;

        if (! ptl::ktv::checkPassword(e->state, pw))
        {
            ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Sai password."), true);
            return;
        }

        safe->ktvAuthed = true;
        onOk();
    });
}

// Saves the file normally, then keeps exactly what was saved as the KTV copy (and saves again so the copy is in the file).
void MainWindow::ktvSaveBase()
{
    juce::Component::SafePointer<MainWindow> safe(this);

    ktvAuthenticate([safe]
    {
        if (safe == nullptr) return;

        safe->saveSession([safe](bool ok)                    // no file yet -> Save As first
        {
            if (! ok || safe == nullptr) return;

            auto* edit = safe->audioEngine.getEdit();
            const auto file = safe->audioEngine.getCurrentSessionFile();
            if (edit == nullptr || file == juce::File()) return;

            if (! ptl::ktv::storeBase(edit->state, file))
            {
                ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Không đọc lại được file vừa lưu để tạo bản gốc KTV."), true);
                return;
            }

            safe->writeSession(file, [safe](bool ok2)
            {
                if (ok2 && safe != nullptr)
                    ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Đã lưu file và bản gốc KTV."));
            });
        });
    });
}

// The running session cannot be swapped under the mixer, so the file on disk is replaced by the KTV copy
// and pt.Live restarts with it (nothing of the current session is saved over it). No password: this is the
// title bar RESET the user presses after changing things; the lock flag and the KTV data stay as they were.
void MainWindow::resetToKtvCopy()
{
    auto* edit = audioEngine.getEdit();
    const auto file = audioEngine.getCurrentSessionFile();

    if (edit == nullptr || ! ptl::ktv::hasBase(edit->state) || ! file.existsAsFile())
    {
        ptl::ktv::info(this, juce::String::fromUTF8("File này chưa có bản gốc KTV. Dùng \"Lưu bản gốc KTV\" trước."), true);
        return;
    }

    juce::Component::SafePointer<MainWindow> safe(this);

    ptl::ktv::confirm(this, "Reset",
        juce::String::fromUTF8("Đưa pt.Live về đúng cài đặt kỹ thuật viên đã lưu? Mọi thay đổi sau đó (kể cả chưa lưu) "
                               "sẽ mất, và pt.Live sẽ khởi động lại."),
        "Reset", [safe, file]
    {
        if (safe == nullptr) return;
        auto* e = safe->audioEngine.getEdit();
        if (e == nullptr) return;

        if (! ptl::ktv::writeRestoredFile(e->state, file))
        {
            ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Không reset được. File cũ vẫn giữ nguyên."), true);
            return;
        }

        safe->relaunchWithFile(file);
    });
}

void MainWindow::ktvChangePassword()
{
    juce::Component::SafePointer<MainWindow> safe(this);

    ktvAuthenticate([safe]
    {
        if (safe == nullptr) return;

        ptl::ktv::askPassword(safe.getComponent(), juce::String::fromUTF8("Đổi password KTV"),
            juce::String::fromUTF8("Nhập password KTV mới."), true, [safe](const juce::String& pw)
        {
            if (safe == nullptr) return;
            auto* e = safe->audioEngine.getEdit();
            if (e == nullptr) return;

            ptl::ktv::setPassword(e->state, pw);
            safe->saveSession([safe](bool ok)
            {
                if (ok && safe != nullptr)
                    ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Đã đổi password KTV."));
            });
        });
    });
}

void MainWindow::ktvRemove()
{
    juce::Component::SafePointer<MainWindow> safe(this);

    ktvAuthenticate([safe]
    {
        if (safe == nullptr) return;

        ptl::ktv::confirm(safe.getComponent(), juce::String::fromUTF8("Gỡ chế độ KTV"),
            juce::String::fromUTF8("Xoá password, khoá và bản gốc KTV khỏi file này? Sau đó ai cũng sửa được file."),
            juce::String::fromUTF8("Gỡ"), [safe]
        {
            if (safe == nullptr) return;
            auto* e = safe->audioEngine.getEdit();
            if (e == nullptr) return;

            ptl::ktv::clearAll(e->state);
            safe->ktvAuthed = false;

            safe->saveSession([safe](bool ok)
            {
                if (ok && safe != nullptr)
                    ptl::ktv::info(safe.getComponent(), juce::String::fromUTF8("Đã gỡ chế độ KTV khỏi file."));
            });
        });
    });
}

//==============================================================================
// Full / collapsed is remembered per machine (not per file): closing the app collapsed opens it collapsed.
//==============================================================================
juce::File MainWindow::getUiModeFile()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("ptLive");
    dir.createDirectory();
    return dir.getChildFile("ui_mode.txt");
}

void MainWindow::saveUiMode() const
{
    getUiModeFile().replaceWithText(compact ? "compact" : "full");
}

void MainWindow::applyStartupMode()
{
    if (controlBar == nullptr)
        return;

    if (getUiModeFile().loadFileAsString().trim() == "compact")
        setCompact(true);
}

void MainWindow::associateFiles()
{
    const bool ok = ptl::fileassoc::registerForCurrentUser();

    juce::AlertWindow::showMessageBoxAsync(ok ? juce::MessageBoxIconType::InfoIcon
                                              : juce::MessageBoxIconType::WarningIcon,
        "pt.Live",
        ok ? "pt.Live session files (.pt) now open with pt.Live and show its icon.\n\n"
             "If Explorer still shows the old icon, restart Explorer or sign out and in again."
           : "Could not register the .pt file type for this user.", "OK");
}

void MainWindow::updateDocumentTitle()
{
    const auto file = audioEngine.getCurrentSessionFile();
    juce::String name = file == juce::File() ? juce::String("Untitled") : file.getFileName();
    if (safeMode)
        name += "  [SAFE MODE]";                       // always visible: plugins are off in this run
    const bool dirty = audioEngine.hasUnsavedChanges();

    if (name == shownTitle && dirty == shownDirty)
        return;

    shownTitle = name;
    shownDirty = dirty;
    titleBar.setDocumentTitle(name, dirty);
    setName("pt.Live - " + name + (dirty ? " *" : ""));    // taskbar / Alt-Tab text
}

void MainWindow::timerCallback()
{
    // About 6 s after start the driver has been reloaded and verified: take the "clean" baseline
    // again so that warm-up work is never reported as unsaved changes.
    if (++timerTicks == 12)
        audioEngine.markSessionClean();

    updateDocumentTitle();

    // Every 20 ticks (10 s) while recording: stop on low disk space, start the next WAV before the 4 GB limit.
    if (timerTicks % 20 == 10)
        serviceRecording();

    // Every 20 ticks (10 s): log xrun / NaN / over counters, only when they grew.
    if (timerTicks % 20 == 0)
        logSignalStats();

    // Every 7200 ticks (1 h at 500 ms): one "still healthy" line.
    if (timerTicks % 7200 == 0)
        logHealthLine();

    // RESET shows up only when the open file holds a KTV copy.
    auto* edit = audioEngine.getEdit();
    titleBar.setResetVisible(edit != nullptr && ptl::ktv::hasBase(edit->state));
}

void MainWindow::logHealthLine()
{
    auto& st = ptl::signalStats();
    const auto xr   = st.xruns.load     (std::memory_order_relaxed);
    const auto nan  = st.nanCount.load  (std::memory_order_relaxed);
    const auto over = st.overCount.load (std::memory_order_relaxed);

    ptl::diag::write("[health] " + juce::String(timerTicks / 7200) + "h check: xrun +" + juce::String(xr - hourXruns)
                     + " | nan +" + juce::String(nan - hourNan) + " | over +" + juce::String(over - hourOver)
                     + " | cpu " + juce::String(juce::roundToInt(audioEngine.getCpuUsage() * 100.0f)) + "%"
                     + " | buffer " + juce::String(audioEngine.getBlockSize())
                     + " @ " + juce::String(juce::roundToInt(audioEngine.getSampleRate())));

    hourXruns = xr;  hourNan = nan;  hourOver = over;
}

void MainWindow::serviceRecording()
{
    auto* rec = audioEngine.getMasterRecorder();

    if (rec == nullptr || ! rec->isRecording())
        return;

    const auto result = rec->serviceRecording();

    if (result == ptl::MasterRecorder::ServiceResult::ok)
        return;

    if (controlBar != nullptr)
        controlBar->refresh();                      // the Record button shows "stopped"

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record",
        result == ptl::MasterRecorder::ServiceResult::stoppedDiskLow
            ? juce::String::fromUTF8 ("Ổ đĩa gần đầy (còn dưới 500 MB) nên đã dừng thu. File đã được đóng đúng cách.")
            : juce::String::fromUTF8 ("Không ghi tiếp được sang file mới nên đã dừng thu. File trước đó đã được đóng đúng cách."),
        "OK");
}

void MainWindow::chooseRecordingFolder()
{
    fileChooser = std::make_unique<juce::FileChooser>(juce::String::fromUTF8("Thư mục lưu bản thu"),
                                                      ptl::MasterRecorder::getRecordingsFolder());

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [](const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();

        if (folder != juce::File() && folder.isDirectory())
            ptl::MasterRecorder::setRecordingsFolder(folder);
    });
}

void MainWindow::logSignalStats()
{
    // Message thread only. Relaxed loads: nothing is added to the audio thread.
    auto& st = ptl::signalStats();
    const auto xr   = st.xruns.load     (std::memory_order_relaxed);
    const auto nan  = st.nanCount.load  (std::memory_order_relaxed);
    const auto over = st.overCount.load (std::memory_order_relaxed);

    const auto dXr = xr - lastXruns, dNan = nan - lastNan, dOver = over - lastOver;

    lastXruns = xr;  lastNan = nan;  lastOver = over;

    if (dXr == 0 && dNan == 0 && dOver == 0)
        return;

    ptl::diag::write("[xrun] +" + juce::String(dXr) + " in 10s (total " + juce::String(xr) + ")"
                     + " | nan +" + juce::String(dNan) + " (total " + juce::String(nan) + ")"
                     + " | over +" + juce::String(dOver) + " (total " + juce::String(over) + ")"
                     + " | cpu " + juce::String(juce::roundToInt(audioEngine.getCpuUsage() * 100.0f)) + "%"
                     + " | buffer " + juce::String(audioEngine.getBlockSize())
                     + " @ " + juce::String(juce::roundToInt(audioEngine.getSampleRate())));
}

bool MainWindow::keyPressed(const juce::KeyPress& key)
{
    if (! startupUnlocked)                  // locked until the audio device is up
        return true;

    const auto mods = key.getModifiers();

    if (mods.isCommandDown() && (key.getKeyCode() == 'O' || key.getKeyCode() == 'o'))
    {
        openSession();
        return true;
    }

    // Ctrl+Q = same path as File > Exit / the close button: asks "Save changes?" first when the session changed
    // (Save / Don't Save / Cancel, see confirmClose()).
    if (mods.isCommandDown() && ! mods.isShiftDown() && (key.getKeyCode() == 'Q' || key.getKeyCode() == 'q'))
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();

        return true;
    }

    if (mods.isCommandDown() && (key.getKeyCode() == 'S' || key.getKeyCode() == 's'))
    {
        if (mods.isShiftDown())
            saveSessionAs();
        else
            saveSession();

        return true;
    }

    return juce::ResizableWindow::keyPressed(key);
}

void MainWindow::confirmClose(std::function<void(bool)> done)
{
    if (! audioEngine.hasUnsavedChanges())
    {
        done(true);
        return;
    }

    if (closePromptShowing)
    {
        bringToFrontForUser();
        return;
    }

    closePromptShowing = true;

    const auto file = audioEngine.getCurrentSessionFile();
    const juce::String name = file == juce::File() ? juce::String("Untitled") : file.getFileName();

    auto* box = new juce::AlertWindow("pt.Live",
                                      "Save changes to \"" + name + "\" before closing?",
                                      juce::MessageBoxIconType::QuestionIcon, this);
    box->addButton("Save",       1, juce::KeyPress(juce::KeyPress::returnKey));
    box->addButton("Don't Save", 2);
    box->addButton("Cancel",     0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainWindow> safe(this);

    box->enterModalState(true, juce::ModalCallbackFunction::create([safe, done](int r)
    {
        if (safe == nullptr)
            return;

        safe->closePromptShowing = false;

        if (r == 1)                                   // Save, then quit (a failed / cancelled save keeps the app open)
            safe->saveSession([done](bool ok) { if (ok) done(true); });
        else if (r == 2)                              // Don't Save
            done(true);
        // r == 0: Cancel -> the app stays open
    }), true);

    // No answer after 5 s -> behave as "Don't Save" and close. The button shows the countdown.
    // (SafePointers: if the user already answered, the box is gone and the tick does nothing.)
    struct AutoDontSave
    {
        static void tick(juce::Component::SafePointer<juce::AlertWindow> w,
                         juce::Component::SafePointer<juce::Button> btn, int secondsLeft)
        {
            if (w == nullptr)
                return;

            if (secondsLeft <= 0)
            {
                w->exitModalState(2);                 // 2 = Don't Save (runs the callback above)
                return;
            }

            if (btn != nullptr)
                btn->setButtonText("Don't Save (" + juce::String(secondsLeft) + ")");

            juce::Timer::callAfterDelay(1000, [w, btn, secondsLeft] { tick(w, btn, secondsLeft - 1); });
        }
    };

    AutoDontSave::tick(juce::Component::SafePointer<juce::AlertWindow>(box),
                       juce::Component::SafePointer<juce::Button>(box->getButton("Don't Save")), 5);
}

void MainWindow::bringToFrontForUser()
{
    if (isMinimised())
        setMinimised(false);

    toFront(true);
}

void MainWindow::showAlreadyRunningWarning(const juce::File& requestedFile)
{
    bringToFrontForUser();

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "pt.Live",
        "pt.Live is already running.\n\n"
        "To open \"" + requestedFile.getFileName() + "\", close pt.Live completely first, "
        "then open the file again.", "OK");
}

// PDC checkbox: ON = the engine compensates plugin latency so tracks stay aligned,
// OFF = compensation is completely disabled (heavy plugins or not).
void MainWindow::setPDCEnabled(bool enabled)
{
    audioEngine.setPDCEnabled(enabled);
    transportBar.updateStatus();

    // Heavy plugins are only FLAGGED (red border), never disabled.
    if (mixerBoard != nullptr)
        mixerBoard->refreshHeavyIndicators();
    if (inspectorPanel != nullptr)
        inspectorPanel->repaint();
}

void MainWindow::showPluginBrowser()
{
    if (safeMode)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8("Safe mode: plugin bị tắt"),
            juce::String::fromUTF8("Ở Safe mode pt.Live không quét và không nạp plugin.\n"
                                   "Đóng pt.Live rồi mở lại, không giữ Shift, để dùng Plugin Browser."), "OK");
        return;
    }

    // The saved list is already loaded at start-up (and may be mid-scan), so never reload it here.
    // Very first open (nothing known yet): scan automatically.
    if (! pluginScanner->isScanning() && pluginScanner->getScannedPlugins().isEmpty())
        pluginScanner->startScan();

    auto* dlg = new PluginBrowserDialog(*pluginScanner);

    juce::Component::SafePointer<MainWindow> safeWin(this);
    dlg->isAudioRunning = [safeWin]
    {
        if (safeWin == nullptr)
            return false;

        auto* jdm = safeWin->audioEngine.getJuceDeviceManager();
        auto* dev = jdm != nullptr ? jdm->getCurrentAudioDevice() : nullptr;
        return dev != nullptr && dev->isOpen() && dev->isPlaying();
    };

    // Never taller / wider than the screen's work area (high OS scale on small screens).
    const auto work = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds())->userArea;
    dlg->setSize(juce::jmin(1000, juce::roundToInt(work.getWidth()  * 0.9f)),
                 juce::jmin(640,  juce::roundToInt(work.getHeight() * 0.85f)));

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(dlg);
    opts.dialogTitle = "pt.Live - Plugin Browser";
    opts.dialogBackgroundColour = theme::window;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = true;

    theme::launchDialog(opts, 820, 480, 1800, 1400);
}

void MainWindow::showAudioSettings()
{
    // PDC checkbox lives in this dialog; the main window refreshes its heavy-plugin flags when it changes.
    juce::Component::SafePointer<MainWindow> safe(this);
    AudioSettingsDialog::show(audioEngine, this, [safe](bool on) { if (safe != nullptr) safe->setPDCEnabled(on); });
}

void MainWindow::setScaleFactor(float factor)
{
    scaleFactor = factor;
    applyScale();
}

void MainWindow::applyScale()
{
    const int w = juce::roundToInt(theme::baseWindowWidth  * scaleFactor);
    const int h = juce::roundToInt(theme::baseWindowHeight * scaleFactor);
    setSize(w, h);
    centreWithSize(w, h);
    resized();
    repaint();
}

//==============================================================================
// Live control: control bar + song detection + key from the VPS
//==============================================================================
void MainWindow::setupLive()
{
    if (trackManager == nullptr)
        return;

    liveController = std::make_unique<LiveController>(*trackManager);
    // Toggles the user adds with "+" can switch PDC: the panel gets the two ends from here.
    liveController->isPdcOn  = [] { return AudioEngine::isPDCEnabled(); };
    liveController->setPdcOn = [this](bool on) { setPDCEnabled(on); };
    controlBar     = std::make_unique<ControlBar>(*liveController);

    // The title bar's wave shows the master level (works in the full app AND in the collapsed panel).
    {
        struct MasterTap
        {
            LiveController* live = nullptr;
            juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> plugin;
            te::LevelMeasurer::Client client;

            ~MasterTap() { detach(); }
            void detach() { if (plugin != nullptr) { plugin->measurer.removeClient(client); plugin = nullptr; } }

            float readDb()
            {
                juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> current;
                if (live != nullptr)
                    current = live->getMasterLevelMeter();

                if (current != plugin)                       // first call, or the session was reloaded: new meter plugin
                {
                    detach();
                    plugin = current;
                    if (plugin != nullptr)
                        plugin->measurer.addClient(client);
                }

                if (plugin == nullptr)
                    return -100.0f;

                float peak = -100.0f;
                for (int c = 0; c < juce::jmin(2, client.getNumChannelsUsed()); ++c)
                    peak = juce::jmax(peak, client.getAndClearAudioLevel(c).dB);
                return peak;
            }
        };

        auto tap = std::make_shared<MasterTap>();
        tap->live = liveController.get();
        titleBar.brandMark.levelProvider = [tap] { return tap->readDb(); };
    }
    addAndMakeVisible(controlBar.get());
    controlBar->onToggleCompact = [this] { setCompact(! compact); };
    controlBar->onIdealWidthChanged = [this] { onControlBarWidthChanged(); };

    // Maximise button of the title bar while collapsed: open the full app, do not stretch the control panel.
    titleBar.onMaximizeRequest = [this]
    {
        if (! compact)
            return false;
        setCompact(false);
        return true;
    };
    controlBar->onRetry = [this]
    {
        // THỬ LẠI: same song, same lookup again
        if (keyFetcher == nullptr || controlBar == nullptr || currentVideoId.isEmpty()) return;
        controlBar->setLoading(true);
        keyFetcher->request(currentVideoId, currentUrl, currentTitle);
    };

    //=== VPS key lookup (same API as the released ptStudio) ==================
    keyFetcher = std::make_unique<KeyFetcher>(licenseTokenProvider);

    keyFetcher->onProgress = [this](const juce::String& id, KeyFetcher::Stage stage)
    {
        if (id != currentVideoId || controlBar == nullptr) return;
        juce::ignoreUnused(stage);                 // progress is shown by the analysing animation only
    };
    keyFetcher->onFound  = [this](const juce::String& id, const VpsClient::KeyResult& r, bool fromCache) { onKeyFound(id, r, fromCache); };
    keyFetcher->onFailed = [this](const juce::String& id, const juce::String& reason) { onKeyFailed(id, reason); };

    //=== Song detection (lay_link in C++) ====================================
    songDetector = std::make_unique<SongDetector>();
    songDetector->onSong = [this](const SongDetector::Song& s) { onSongDetected(s); };

    // Any other video (not karaoke / beat, or a medley): only the title is shown. No key lookup, no history entry,
    // no reset of the pitch shifts, and currentVideoId stays on the last real song (THỬ LẠI keeps working on it).
    songDetector->onTitleOnly = [this](const SongDetector::Song& s)
    {
        if (controlBar != nullptr)
            controlBar->setSongTitle(s.title, true);      // shown as "[Bỏ qua] <title>"
    };
    songDetector->start();

    if (! keyFetcher->isConfigured())
        controlBar->setStatus(juce::String::fromUTF8("Chưa có vps_config.json - chọn key bằng tay"), ControlBar::StatusKind::Warn);
}

void MainWindow::onSongDetected(const SongDetector::Song& song)
{
    if (liveController == nullptr || controlBar == nullptr || keyFetcher == nullptr)
        return;

    currentVideoId = song.videoId;
    currentTitle   = song.title;
    currentUrl     = song.url;
    controlBar->setSongTitle(song.title);          // exact title of the focused YouTube video

    // Remember the song in the DANH SÁCH list (key/scale is filled in later by onKeyFound).
    addToHistory(song.videoId, song.url, song.title);

    // New song: the music goes back to its original pitch right away.
    liveController->resetShifts();

    if (! keyFetcher->isConfigured())
    {
        controlBar->setLoading(false);
        controlBar->setStatus(juce::String::fromUTF8("Chưa có vps_config.json - chọn key bằng tay"), ControlBar::StatusKind::Warn);
        return;
    }

    controlBar->setLoading(true);                  // spinner + moving bar until onKeyFound / onKeyFailed
    keyFetcher->request(song.videoId, song.url, song.title);
}

void MainWindow::onKeyFound(const juce::String& videoId, const VpsClient::KeyResult& r, bool fromCache)
{
    juce::ignoreUnused(fromCache);
    if (videoId != currentVideoId || liveController == nullptr || controlBar == nullptr)
        return;                                          // an older song answered late

    liveController->setDetectedKey(r.root, r.scale);
    controlBar->setLoading(false);
    controlBar->setStatus({}, ControlBar::StatusKind::Idle);
    controlBar->flashResult();                       // key text blinks for 2 s

    // Update the history entry's key/scale (if this video is in the list).
    updateHistoryKey(videoId, r.root, r.scale);
}

void MainWindow::onKeyFailed(const juce::String& videoId, const juce::String& reason)
{
    if (videoId != currentVideoId || controlBar == nullptr)
        return;

    // Nothing is changed in the mixer: the old key stays. The user can click the key box to pick one.
    controlBar->setLoading(false);

    juce::String shortMsg, why;
    if (reason == "worker")
    {
        shortMsg = juce::String::fromUTF8 ("Không gửi được lên VPS");
        why      = juce::String::fromUTF8 (" (không gửi được job lên VPS)");
    }
    else if (reason == "timeout")
    {
        shortMsg = juce::String::fromUTF8 ("Quá 20 giây, chưa có key");
        why      = juce::String::fromUTF8 (" (worker không trả kết quả sau 20s)");
    }
    else
    {
        shortMsg = juce::String::fromUTF8 ("Không dò được key");
        if (reason.isNotEmpty()) why = " (" + reason + ")";
    }

    controlBar->setFailure(shortMsg,
                           juce::String::fromUTF8("Không dò được key") + why
                               + juce::String::fromUTF8(" - bấm THỬ LẠI, hoặc bấm vào ô key để chọn tay"));
}

//==============================================================================
// DANH SÁCH - persistent song history + Record integration
//==============================================================================
void MainWindow::addToHistory(const juce::String& id, const juce::String& url, const juce::String& title)
{
    if (songHistory == nullptr)
        return;
    songHistory->add(id, url, title);
}

void MainWindow::updateHistoryKey(const juce::String& videoId, const juce::String& root, const juce::String& scale)
{
    if (songHistory == nullptr || videoId.isEmpty())
        return;
    songHistory->updateKey(videoId, root, scale);
}

void MainWindow::openFromHistory(const juce::String& url)
{
    if (url.isEmpty())
        return;

    // If a YouTube tab is already open, navigate it to the new URL (same tab, no new tab).
    // Otherwise open in the default browser.
    juce::String urlToOpen = url;
    if (! urlToOpen.containsIgnoreCase ("://"))
        urlToOpen = "https://" + urlToOpen;

    // Only one YouTube song may play at a time: the link goes into the tab that is already playing YouTube
    // (or the default browser when none is open). false = a YouTube tab exists but could not be driven.
    if (! navigateToYouTubeTab(urlToOpen) && controlBar != nullptr)
        controlBar->setStatus(juce::String::fromUTF8("Không gán được link vào tab YouTube đang mở"),
                              ControlBar::StatusKind::Warn);
}

void MainWindow::showHistoryPopup()
{
    if (songHistory == nullptr || controlBar == nullptr)
        return;

    juce::Component::SafePointer<MainWindow> safe (this);

    auto panel = std::make_unique<HistoryPanel> (
        songHistory->all(),
        [safe] (const juce::String& url) { if (safe != nullptr) safe->openFromHistory (url); },
        [safe]
        {
            if (safe == nullptr || safe->songHistory == nullptr) return;
            // Remove all entries: clear the file by writing empty JSON
            ptl::SongHistory::configFile().replaceWithText ("[]\n");
            safe->songHistory->load();    // reload empty
        });

    // Pointing at the DANH SÁCH button; the window may be always-on-top (collapsed), so the call-out is too.
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (panel), controlBar->getHistoryButtonScreenBounds(), nullptr);
    box.setAlwaysOnTop (true);
}

//==============================================================================
// Width the control bar needs for the toggles it has now. With no toggle the collapsed panel is short; every
// toggle that is added makes it wider (and removing one makes it narrower again).
//==============================================================================
int MainWindow::compactMinWidth() const
{
    const int need = controlBar != nullptr ? controlBar->getIdealWidth() : theme::minWindowWidth;
    return juce::jmin(need, getWorkArea().getWidth());
}

void MainWindow::onControlBarWidthChanged()
{
    if (controlBar == nullptr)
        return;

    const auto work = getWorkArea();

    if (compact)
    {
        const int oldMin = compactMinW;
        const int newMin = compactMinWidth();
        compactMinW = newMin;

        // At its minimum (or too narrow now): follow the content. A window the user made wider is left alone.
        auto b = getBounds();
        int w = b.getWidth();
        if (w <= oldMin || w < newMin)
            w = newMin;

        // Taller / shorter when user-made sliders need more / fewer rows: the window keeps its BOTTOM edge
        // (it is docked above the taskbar), so the panel grows upwards.
        const int h = theme::titleBarH + controlBar->getIdealHeightForWidth(w);
        setResizeLimits(newMin, h, 16384, h);

        const int x = juce::jlimit(work.getX(), juce::jmax(work.getX(), work.getRight() - w), b.getCentreX() - w / 2);
        setBounds(x, b.getBottom() - h, w, h);
    }
    else
    {
        // Full app: never narrower than the control bar needs (more toggles = wider minimum).
        normalMinW = juce::jmin(juce::jmax(normalBaseMinW, controlBar->getIdealWidth()), work.getWidth());
        setResizeLimits(normalMinW, normalMinH, 16384, 16384);

        if (getWidth() < normalMinW)
            setSize(normalMinW, getHeight());

        resized();                           // the control bar may need more / fewer rows now
    }
}

//==============================================================================
// Collapse: title bar + control bar only. Audio keeps running, only the views are hidden.
//==============================================================================
void MainWindow::setCompact(bool on)
{
    if (on == compact || controlBar == nullptr)
        return;

    if (on)
    {
        if (isFullScreen())
            setFullScreen(false);

        normalBounds = getBounds();
        compact = true;
        controlBar->setCompact(true);            // first: its ideal width / height now include the master meter

        for (auto* c : std::initializer_list<juce::Component*>{ &transportBar, &masterMeter, mixerBoard.get(), inspectorPanel.get() })
            if (c != nullptr)
                c->setVisible(false);

        // userArea = the screen WITHOUT the taskbar: the window sits on its bottom edge, never over the taskbar.
        const auto work = juce::Desktop::getInstance().getDisplays().getDisplayForRect(normalBounds)->userArea;
        const int w = juce::jmin(compactMinWidth(), work.getWidth());
        compactMinW = w;
        const int h = theme::titleBarH + controlBar->getIdealHeightForWidth(w);     // + the rows of user-made sliders / toggles

        const int x = work.getCentreX() - w / 2;                             // centred horizontally
        constexpr int taskbarGap = 2;                                       // free space between the collapsed panel and the taskbar
        const int y = work.getBottom() - h - taskbarGap;                    // docked above the taskbar, 2 px clear of it

        setAlwaysOnTop(true);                                               // control panel stays above every other app
        setResizeLimits(w, h, 16384, h);                                    // never narrower than the panel needs; only the width can grow
        setBounds(x, y, w, h);
    }
    else
    {
        compact = false;
        controlBar->setCompact(false);
        setAlwaysOnTop(false);                                              // full app: normal window again

        // Children first, then the big size: the window never shows an empty (white) area.
        for (auto* c : std::initializer_list<juce::Component*>{ &transportBar, &masterMeter, mixerBoard.get(), inspectorPanel.get() })
            if (c != nullptr)
                c->setVisible(true);

        // Toggles may have been added while collapsed: the full app's minimum width follows them.
        normalMinW = juce::jmin(juce::jmax(normalBaseMinW, controlBar->getIdealWidth()), getWorkArea().getWidth());
        setResizeLimits(normalMinW, normalMinH, 16384, 16384);

        // The full app comes back in the CENTRE of the screen (not where the panel was, at the bottom).
        const int nw = normalBounds.getWidth()  > 0 ? normalBounds.getWidth()  : getDefaultWindowBounds().getWidth();
        const int nh = normalBounds.getHeight() > 0 ? normalBounds.getHeight() : getDefaultWindowBounds().getHeight();
        setBounds(centredFullBounds(nw, nh));
    }

    controlBar->setCompact(compact);
    resized();
    repaint();

    saveUiMode();                            // remembered for the next start
}
