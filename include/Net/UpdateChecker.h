/*
    pt.Live - Live Singing DAW
    UpdateChecker - asks the VPS once, in the background, whether a newer pt.Live exists.

    The VPS serves a tiny PUBLIC manifest (no key, no token, nothing secret in it):

        GET  <vps url from vps_config.json>/ptlive/update.json
        {
          "version": "0.1.1",                                  required, digits and dots only
          "url":     "https://.../ptLive-0.1.1-setup.exe",     required, https only, DIRECT link to the installer
          "sha256":  "<64 hex chars>",                         optional, checked after the download (recommended)
          "notes":   "- Fixed ...\n- Added ..."                optional, plain text shown in the dialog
        }

    Rules:
      - Newer than the running version (MainApplication::getApplicationVersion()) AND not skipped -> onFound().
      - "Skip this version" is stored in %APPDATA%\ptLive\update_state.json; a version NEWER than the skipped
        one is offered again.
      - Every failure (offline, 404, bad JSON, bad url) is silent for the user and only logged ([update] ...).
        An update check must never stop or delay pt.Live.
      - The HTTP call runs on its own thread; onFound() is called on the MESSAGE thread.

    Lifetime: the destructor cancels the request and joins the thread (call it before the app shuts down).
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include "include/Net/VpsClient.h"

class UpdateChecker : private juce::Thread
{
public:
    struct Release
    {
        juce::String version, url, notes, sha256;      // sha256: "" = the manifest did not give one
    };

    // Called on the message thread, only when there IS an update the user has not skipped.
    using Found = std::function<void (const Release&)>;

    UpdateChecker();
    ~UpdateChecker() override;

    // Starts the check once. `currentVersion` = the running version, e.g. "0.1.0".
    void checkInBackground (const juce::String& currentVersion, Found onFound);

    // true when `candidate` is a higher version than `current` ("0.1.10" > "0.1.9", missing parts count as 0).
    static bool isNewer (const juce::String& candidate, const juce::String& current);

    // "Skip this version" persistence (tiny file in %APPDATA%\ptLive).
    static void skipVersion (const juce::String& version);
    static juce::String getSkippedVersion();

    static constexpr const char* kManifestPath = "/ptlive/update.json";

private:
    void run() override;

    VpsClient vps;                          // own connection: never competes with the license / key traffic
    juce::String current;
    Found onFound;
    std::atomic<bool> cancel { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};


//==============================================================================
/*
    UpdateDownloader - downloads the installer the manifest points to, IN the app, with progress.

        start(url, version, sha256, onFinished)   -> background thread, one download at a time
        getBytesDone() / getBytesTotal()          -> progress (any thread; total = -1 when the server sends no length)
        cancelDownload()                          -> aborts the transfer at once (also done by the destructor)

    The file goes to  %TEMP%\ptLive_update\ptLive-<version>-setup.exe  (first as ".part", renamed when complete).
    After the transfer it is checked: not tiny, starts with "MZ" (a Windows program, not an HTML error page), and -
    when the manifest has a sha256 - the hash must match; otherwise the file is deleted and the download fails.

    onFinished runs on the MESSAGE thread (never after cancelDownload()). It captures nothing from this object.
*/
class UpdateDownloader : private juce::Thread
{
public:
    enum class Error { none, connect, http, interrupted, disk, notInstaller, hashMismatch };

    using Finished = std::function<void (bool ok, const juce::File& installer, Error error)>;

    UpdateDownloader();
    ~UpdateDownloader() override;

    void start (const juce::String& url, const juce::String& version, const juce::String& sha256, Finished onFinished);
    void cancelDownload();

    juce::int64 getBytesDone() const  { return done.load(); }
    juce::int64 getBytesTotal() const { return total.load(); }
    bool isRunning() const            { return isThreadRunning(); }

private:
    void run() override;
    void finish (bool ok, const juce::File& file, Error e);

    juce::String url, version, sha256;
    Finished onFinished;

    std::atomic<juce::int64> done { 0 }, total { -1 };
    std::atomic<bool> cancelled { false };

    juce::CriticalSection streamLock;
    juce::WebInputStream* activeStream = nullptr;     // guarded by streamLock (cancel() may come from another thread)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateDownloader)
};
