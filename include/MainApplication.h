/*
    pt.Live - Live Singing DAW
    Main application class
*/

#pragma once

#include <JuceHeader.h>
#include <thread>
#include "include/PtLiveLookAndFeel.h"
#include "include/Net/LicenseClient.h"
#include "include/Net/UpdateChecker.h"

class MainWindow;

class MainApplication : public juce::JUCEApplication
{
public:
    MainApplication();

    const juce::String getApplicationName() override;
    const juce::String getApplicationVersion() override;
    bool moreThanOneInstanceAllowed() override;

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;

    static MainApplication& getApp();

private:
    void startMainWindow(const juce::File& file);
    void showWelcomeDialog();
    void checkLicenseThen(std::function<void()> proceed);      // license gate: nothing else opens before it passes
    void showLicenseDialog();                                  // double-click on the licence status in the title bar (app already running)
    void updateLicenseStatus();                                // plan + time left -> title bar chip
    void installUpdateAndQuit(const juce::File& installer, const juce::String& fallbackUrl);   // update downloaded: close, install over, reopen
    void startUpdateCheck();                                   // background check on the VPS; dialog only when a newer version exists

    PtLiveLookAndFeel lookAndFeel;               // declared BEFORE the window: destroyed after it
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<VpsClient> licenseVps;       // own connection for /api/license/* (declared BEFORE licenseClient)
    std::unique_ptr<LicenseClient> licenseClient;
    std::thread licenseRefreshThread;            // start-up /refresh worker, joined in shutdown()
    std::unique_ptr<UpdateChecker> updateChecker;   // own thread + own connection, stopped first in shutdown()
    std::atomic<bool> licenseCancel { false };
    std::atomic<bool> finished { false };       // set by shutdown(): async license callbacks stop opening dialogs
    bool safeModeRequested = false;             // Shift held (or --safe) at launch: start without external plugins

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainApplication)
};
