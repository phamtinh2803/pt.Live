/*
    pt.Live - Live Singing DAW
    Main application implementation
*/

#include "include/MainApplication.h"
#include "include/MainWindow.h"
#include "include/FileAssociation.h"
#include "include/InstanceGuard.h"
#include "include/Audio/PtLog.h"
#include "include/CrashLog.h"
#include "include/UI/LicenseDialog.h"
#include "include/UI/UpdateDialog.h"
#include "include/UI/WelcomeDialog.h"
#include "include/RecentFiles.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <commdlg.h>
 #pragma comment (lib, "comdlg32.lib")
#endif

static MainApplication* jassertedAppInstance = nullptr;

MainApplication& MainApplication::getApp()
{
    jassert (jassertedAppInstance != nullptr);
    return *jassertedAppInstance;
}

namespace
{
    // Longest the start waits for the license server. Past it the app starts with the cached token (already
    // verified by checkLocal: signature, machine, expiry, offline window), so a flaky venue network cannot hold
    // the app back for tens of seconds.
    constexpr int kLicenseStartupWaitMs = 8000;

    // The update check starts this long after the main window opened (never competes with the start-up).
    constexpr int kUpdateCheckDelayMs = 5000;

    // Switches given to the downloaded installer. FIXED in the program, never taken from the server.
    // These are Inno Setup's: show only a progress window, close running copies, do not reboot. Another kind
    // of installer simply ignores them and shows its normal wizard, which still installs over the old version.
    const juce::String kInstallerArgs = "/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";

    bool isShiftHeldNow()
    {
       #if JUCE_WINDOWS
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
       #else
        return juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
       #endif
    }
}

MainApplication::MainApplication()
{
    jassertedAppInstance = this;

    // Runs BEFORE JUCE's single-instance check: a pt.Live that is closing or has no window
    // (stuck) is waited for / ended here, so this start never vanishes silently.
    ptl::guard::clearStaleInstance();
}

const juce::String MainApplication::getApplicationName()
{
    return "pt.Live";
}

const juce::String MainApplication::getApplicationVersion()
{
    return ProjectInfo::versionString;       // = VERSION in CMakeLists.txt (juce_add_gui_app)
}

// Looks for a pt.Live session (*.pt) in a command line, e.g.  ptLive.exe "D:\songs\live set.pt"
// (this is what Windows runs when a .pt file is double-clicked).
static juce::File findSessionFileInCommandLine(const juce::String& commandLine)
{
    juce::ArgumentList args("pt.Live", commandLine);

    for (const auto& a : args.arguments)
    {
        const auto path = a.text.unquoted().trim();

        if (path.endsWithIgnoreCase(".pt"))
            return juce::File::getCurrentWorkingDirectory().getChildFile(path);
    }

    return {};
}

// One pt.Live at a time: two instances would fight over the same ASIO driver, and a running
// session cannot be replaced by opening another file on top of it.
bool MainApplication::moreThanOneInstanceAllowed()
{
    return false;
}

void MainApplication::initialise(const juce::String& commandLine)
{
    // Live singing: ask Windows to schedule this process ahead of normal background work.
    // (HIGH, not REALTIME: realtime can starve the mouse / keyboard / the driver itself.)
    juce::Process::setPriority(juce::Process::HighPriority);

    // If the process dies (plugin / driver crash) the reason + the DLL + a mini dump go to %APPDATA%\ptLive.
    juce::SystemStats::setApplicationCrashHandler(ptl::crash::handle);
    ptl::diag::cleanOldDumps();                        // keep the 5 newest crash-/hang- dumps of each kind
    ptl::crash::hangWatchdog().start();                // writes [HANG] + hang-*.dmp if the UI freezes

    ptl::guard::publishInstance();               // from now on a new start can see this one

    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);

    // Make ".pt" open with pt.Live (and show its icon). Per-user, only written when it is missing
    // or points to another exe, so it costs nothing on a normal start.
    ptl::fileassoc::registerIfNeeded();

    const auto file = findSessionFileInCommandLine(commandLine);

    // Safe mode: Shift held while the app starts (or "ptLive.exe --safe"). Read NOW, not later: the license
    // check and the start screen can take seconds and the user lets go of the key. startMainWindow() looks again,
    // so Shift held while picking a session counts too.
    safeModeRequested = isShiftHeldNow() || juce::ArgumentList("pt.Live", commandLine).containsOption("--safe");

    // License gate: no window of the app opens before the key was verified (see Net/LicenseClient.h).
    checkLicenseThen([this, file]
    {
        if (file != juce::File())
            startMainWindow(file);              // started by double-clicking a .pt: no question asked
        else
            showWelcomeDialog();                // started directly (the .exe): ask what to open
    });
}

// Local check only (no network, no wait): a valid license goes straight on. Otherwise the license dialog asks
// for a key / talks to the server, and the app continues only when it reports success; Cancel quits pt.Live.
void MainApplication::checkLicenseThen(std::function<void()> proceed)
{
    licenseVps    = std::make_unique<VpsClient>();
    licenseClient = std::make_unique<LicenseClient>(*licenseVps);

    LicenseClient::Info info;
    const auto gate = licenseClient->checkLocal(&info);
    ptl::diag::write("[license] local check: " + LicenseClient::toString(gate));

    if (gate == LicenseClient::Gate::ok)
    {
        // Re-check with the server so a revoke / expiry done on the web applies on next app open.
        // checkLicenseThen() runs on the message thread, so the blocking /refresh must happen on a worker;
        // the result is applied back on the message thread before the app proceeds.
        // A thread with a handle (joined in shutdown), not a detached one that keeps `this` / licenseClient.
        if (licenseRefreshThread.joinable())
            licenseRefreshThread.join();

        // Whoever comes first - the server's answer or the end of the wait - continues the start; the other one
        // finds `resolved` already set and does nothing (both run on the message thread, so no race).
        auto resolved = std::make_shared<std::atomic<bool>> (false);

        licenseRefreshThread = std::thread ([this, proceed, resolved]
        {
            const bool stillValid = licenseClient->refreshAndWait (&licenseCancel);

            juce::MessageManager::callAsync ([this, proceed, resolved, stillValid]
            {
                if (finished)                                  // app is shutting down
                    return;

                if (resolved->exchange (true))                 // the wait ended first: the app is already running
                {
                    ptl::diag::write ("[license] late answer after the start-up wait: "
                                      + (stillValid ? juce::String ("ok") : licenseClient->getLastRefreshCode())
                                      + " (applies from the next start)");
                    return;
                }

                if (stillValid)
                {
                    updateLicenseStatus();
                    proceed();
                    return;
                }

                // Server says revoked / expired / removed -> show the dialog (or quit on cancel).
                // Revoked on the server: the activation dialog comes back (explaining why). The user can still
                // send an activation request from it (rate limited, see LicenseClient::requestActivation).
                LicenseDialog::show (*licenseClient, licenseClient->gateAfterFailedRefresh(), [this, proceed] (bool activated)
                {
                    if (activated)
                        proceed();
                    else
                        quit();
                });
            });
        });

        // End of the wait: no answer yet -> start with the cached token. (checkLocal() above already said "ok", so
        // the token is valid and inside its offline window; a revoke is then applied at the next start.)
        juce::Timer::callAfterDelay (kLicenseStartupWaitMs, [this, proceed, resolved]
        {
            if (finished || resolved->exchange (true))
                return;

            ptl::diag::write ("[license] could not reach the server within " + juce::String (kLicenseStartupWaitMs / 1000)
                              + " s - starting with the cached token");
            proceed();
        });
        return;
    }

    LicenseDialog::show(*licenseClient, gate, [this, proceed](bool activated)
    {
        if (activated)
            proceed();
        else
            quit();                                     // Cancel / window closed: no license, no pt.Live
    });
}

// Title bar chip: plan and time left of the stored license. checkLocal() is local only (no network).
// Called after the window opens and after every successful activation - not on a timer: the title bar
// itself recounts the remaining time from the end date once a minute.
void MainApplication::updateLicenseStatus()
{
    if (mainWindow == nullptr || licenseClient == nullptr)
        return;

    LicenseClient::Info info;
    licenseClient->checkLocal(&info);

    if (info.expiresAt > 0)
        mainWindow->setLicenseStatus(info.plan == "trial", LicenseClient::planLabel(info),
                                     info.expiresAt, LicenseClient::describe(info));
}

// Same dialog as the start-up gate, opened by the user (double-click on the licence status) while pt.Live runs
// (trial -> real key, renewal).
// Closing it without activating just returns to the app; success refreshes the title bar chip.
void MainApplication::showLicenseDialog()
{
    if (licenseClient == nullptr)
        return;

    LicenseClient::Info info;
    const auto gate = licenseClient->checkLocal(&info);

    LicenseDialog::showFromApp(*licenseClient, gate,
                               info.expiresAt > 0 ? LicenseClient::describe(info) : juce::String(),
                               info.plan == "trial",
                               [this](bool activated)
                               {
                                   if (activated)
                                       updateLicenseStatus();
                               });
}

void MainApplication::startMainWindow(const juce::File& file)
{
    ptl::diag::write("[start] creating main window, file=" + (file == juce::File() ? juce::String("(default session)") : file.getFullPathName()));

    ptl::recent::add(file);                      // opened from Explorer / Open... / Recent: top of the recent list

    // The VPS key lookup authenticates with the license token (X-License-Token). The window is destroyed before
    // licenseClient (see shutdown), so the pointer inside this lambda never dangles.
    const bool safeMode = safeModeRequested || isShiftHeldNow();
    if (safeMode)
        ptl::diag::write("[safe] safe mode requested (Shift / --safe): external plugins stay off");

    mainWindow.reset(new MainWindow("pt.Live", file,
                                    [this] { return licenseClient != nullptr ? licenseClient->currentToken() : juce::String(); },
                                    safeMode));

    // Double-click on the licence status (title bar) = enter / renew a key.
    mainWindow->onLicenseRequested = [this] { showLicenseDialog(); };
    updateLicenseStatus();

    // The window must never stay invisible while the process runs.
    mainWindow->setVisible(true);
    mainWindow->toFront(true);

   #if JUCE_WINDOWS
    // Long shows: keep the machine AND the screen awake while pt.Live runs (given back in shutdown()). Same thread
    // both times. ES_DISPLAY_REQUIRED stops the power plan from turning the monitor off / locking in the middle of a set.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
   #endif

    ptl::diag::write("[start] main window shown");

    // Update check a few seconds AFTER the window is up: start-up (audio engine, plugins) is never slowed by it.
    // Once per run; a failure of any kind is silent (see Net/UpdateChecker.h).
    juce::Timer::callAfterDelay(kUpdateCheckDelayMs, [this]
    {
        if (! finished && mainWindow != nullptr)
            startUpdateCheck();
    });
}

// Starts a tiny hidden PowerShell that WAITS until this process is gone, then runs the installer (install over the
// old version, with kInstallerArgs), then starts pt.Live again. Returns false when it could not be started (the
// caller then keeps the app open and falls back to the browser). The script travels as -EncodedCommand (UTF-16 base64),
// so no quoting problem can come from the path of the installer / the exe (spaces, ', &, accents).
static bool launchInstallerAfterExit(const juce::File& installer)
{
   #if JUCE_WINDOWS
    auto q = [] (const juce::String& v) { return "'" + v.replace("'", "''") + "'"; };     // PowerShell single-quoted string

    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto pid = juce::String((int) GetCurrentProcessId());

    juce::String script;
    script << "$ErrorActionPreference = 'SilentlyContinue'\n"
           << "Wait-Process -Id " << pid << " -Timeout 180\n"
           << "if (Get-Process -Id " << pid << ") { exit }\n"                           // the app did not close (user cancelled): do nothing
           << "Start-Process -FilePath " << q(installer.getFullPathName())
           << " -ArgumentList " << q(kInstallerArgs) << " -Wait\n"
           << "Start-Process -FilePath " << q(exe.getFullPathName()) << "\n"
           << "Remove-Item -LiteralPath " << q(installer.getFullPathName()) << " -Force\n";

    const std::wstring wide = script.toWideCharPointer();
    const auto encoded = juce::Base64::toBase64(wide.data(), wide.size() * sizeof(wchar_t));

    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ";
    cmd += encoded.toWideCharPointer();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (! CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
    {
        ptl::diag::write("[update] could not start the installer helper (error " + juce::String((int) GetLastError()) + ")");
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
   #else
    juce::ignoreUnused(installer);
    return false;
   #endif
}

// Asks the VPS (public manifest, no credentials) whether a newer pt.Live exists. Only when it does - and the
// user did not skip that version - a dialog opens:  Cập nhật (download in the app, then close + install over + reopen) /
// Hủy / Bỏ qua bản này.
void MainApplication::startUpdateCheck()
{
    if (updateChecker != nullptr)
        return;

    updateChecker = std::make_unique<UpdateChecker>();
    updateChecker->checkInBackground(getApplicationVersion(), [this] (const UpdateChecker::Release& rel)
    {
        if (finished || mainWindow == nullptr)          // callback runs on the message thread, maybe during quit
            return;

        UpdateDialog::show(getApplicationVersion(), rel.version, rel.notes, rel.url, rel.sha256,
                           [this, rel] (UpdateDialog::Choice choice, const juce::File& installer)
        {
            if (finished)
                return;

            switch (choice)
            {
                case UpdateDialog::Choice::update:
                    // The installer is already downloaded (progress was shown in the dialog): close, install over, reopen.
                    ptl::diag::write("[update] user chose to update to " + rel.version);
                    installUpdateAndQuit(installer, rel.url);
                    break;

                case UpdateDialog::Choice::skip:
                    UpdateChecker::skipVersion(rel.version);
                    break;

                case UpdateDialog::Choice::later:
                default:
                    ptl::diag::write("[update] user postponed the update to " + rel.version);
                    break;
            }
        });
    });
}

// The new installer is on disk. Normal quit path first (so "Save changes?" still appears when needed; Cancel there keeps
// pt.Live open and nothing is installed), then the helper is started and the app quits. The helper waits for this
// process to end, installs over the old version and starts pt.Live again.
void MainApplication::installUpdateAndQuit(const juce::File& installer, const juce::String& fallbackUrl)
{
    if (! installer.existsAsFile())
    {
        ptl::diag::write("[update] installer file is missing - opening the download link instead");
        juce::URL(fallbackUrl).launchInDefaultBrowser();
        return;
    }

    auto go = [this, installer, fallbackUrl]
    {
        if (finished)
            return;

        if (! launchInstallerAfterExit(installer))
        {
            juce::URL(fallbackUrl).launchInDefaultBrowser();          // could not self-install: the old manual way
            return;
        }

        ptl::diag::write("[update] closing pt.Live to install " + installer.getFileName());
        ptl::guard::markClosing();
        ptl::guard::armShutdownWatchdog(15000);                       // backstop in case the message loop never reaches shutdown()
        quit();
    };

    if (mainWindow == nullptr)
        go();
    else
        mainWindow->confirmClose([go](bool goAhead) { if (goAhead) go(); });
}

// Windows' own "Open" dialog, used synchronously. The start screen is not a normal part of the
// app yet (no main window exists), so the simplest and safest picker is the native one: no object
// that has to outlive an async callback, nothing to delete from inside its own callback.
static juce::File pickSessionFile()
{
   #if JUCE_WINDOWS
    std::vector<wchar_t> buf(32768, L'\0');

    const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getFullPathName();

    OPENFILENAMEW ofn {};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = nullptr;
    ofn.lpstrFilter     = L"pt.Live session (*.pt)\0*.pt\0All files (*.*)\0*.*\0\0";
    ofn.nFilterIndex    = 1;
    ofn.lpstrFile       = buf.data();
    ofn.nMaxFile        = (DWORD) buf.size();
    ofn.lpstrInitialDir = docs.toWideCharPointer();
    ofn.lpstrTitle      = L"Open session";
    ofn.Flags           = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn))
        return juce::File(juce::String(buf.data()));

    return {};
   #else
    juce::FileChooser fc("Open session", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.pt");
    return fc.browseForFileToOpen() ? fc.getResult() : juce::File();
   #endif
}

// Shown only when the .exe is started directly (not through a .pt file).
//   Open...  -> pick a .pt session and start with it   (cancelling the picker returns to this dialog)
//   Empty    -> the default session (BEAT, MICRO, FX 1, FX 2)
//   Recent   -> the 3 newest .pt files that still exist: one click starts with that session
//   Cancel   -> quit pt.Live
void MainApplication::showWelcomeDialog()
{
    ptl::diag::write("[start] welcome dialog");

    // The callback already runs from a clean message-loop turn (the dialog is closed by then).
    WelcomeDialog::show(ptl::recent::get(3),
        [this](WelcomeDialog::Choice choice, const juce::File& file)
        {
            switch (choice)
            {
                case WelcomeDialog::Choice::empty:
                    startMainWindow({});
                    break;

                case WelcomeDialog::Choice::recent:
                    ptl::diag::write("[start] recent: " + file.getFullPathName());

                    if (file.existsAsFile())
                        startMainWindow(file);
                    else
                        showWelcomeDialog();         // deleted in the meantime: back to the choices
                    break;

                case WelcomeDialog::Choice::open:
                {
                    const auto chosen = pickSessionFile();
                    ptl::diag::write("[start] picked: " + chosen.getFullPathName());

                    if (chosen != juce::File() && chosen.existsAsFile())
                        startMainWindow(chosen);
                    else
                        showWelcomeDialog();         // picker cancelled: back to the choices
                    break;
                }

                case WelcomeDialog::Choice::cancel:
                default:
                    ptl::diag::write("[start] cancelled by user");
                    quit();                           // Cancel / window closed
                    break;
            }
        });
}

void MainApplication::shutdown()
{
    finished.store(true);                        // async callbacks must stop opening dialogs

   #if JUCE_WINDOWS
    SetThreadExecutionState(ES_CONTINUOUS);      // give the power plan back
   #endif

    // From here on the process MUST end: if a thread / driver / plugin hangs while the window is
    // destroyed - or the license worker is still inside a slow network call - the watchdog ends the process
    // after 6 s (the session was saved before this point). Armed BEFORE the join below for that reason.
    ptl::guard::markClosing();
    ptl::guard::armShutdownWatchdog(6000);

    updateChecker.reset();                       // cancels a running update check and joins its thread
    licenseCancel.store(true);                   // abort a /refresh still waiting for the network ...
    if (licenseRefreshThread.joinable())
        licenseRefreshThread.join();             // ... and wait for the worker before licenseClient goes away

    mainWindow.reset();
    licenseClient.reset();                       // joins the background refresh thread
    licenseVps.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);

    // Release the instance-state block LAST: the next start then sees pid == 0 and never wastes
    // time waiting for / killing this dead pid (see InstanceGuard::closeOwnHandle()).
    ptl::guard::closeOwnHandle();
}

void MainApplication::systemRequestedQuit()
{
    if (mainWindow == nullptr)
    {
        quit();
        return;
    }

    // Unsaved changes -> "Save changes?" first; Cancel keeps the app open.
    mainWindow->confirmClose([this](bool goAhead)
    {
        if (! goAhead)
            return;

        ptl::guard::markClosing();
        ptl::guard::armShutdownWatchdog(15000);   // backstop in case the message loop never reaches shutdown()
        quit();
    });
}

// A second pt.Live was started while this one is running (typically a .pt double-clicked).
// The second process exits by itself; this one only warns, it never loads over the running session.
void MainApplication::anotherInstanceStarted(const juce::String& commandLine)
{
    if (mainWindow == nullptr)
        return;

    const auto file = findSessionFileInCommandLine(commandLine);

    if (file != juce::File())
        mainWindow->showAlreadyRunningWarning(file);
    else
        mainWindow->bringToFrontForUser();
}
