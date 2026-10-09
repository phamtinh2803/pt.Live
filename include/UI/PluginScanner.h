/*
    pt.Live - Live Singing DAW
    Plugin Scanner for VST3 + VST2 (.dll, incl. jBridge-bridged) plugin discovery

    Task 2.10: 3-tier plugin scanning strategy:
              Layer 1: JUCE scans Windows registry + standard locations automatically
              Layer 2: Manual fallback paths (C:\Program Files\Common Files\VST3 etc.)
              Layer 3: User-configured folders (persisted in config)

              Scans run on a background thread to avoid blocking UI.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/TracktionHeaders.h"

class PluginScanner : public juce::Thread
{
public:
    PluginScanner();
    ~PluginScanner() override;

    // Start scanning (background thread). NEVER clears the existing list: plugins that are already
    // known (and unchanged on disk) are skipped, so a rescan only looks for NEW / updated plugins.
    //   onlyGivenPaths = false : default VST folders + saved user folders + customPaths
    //   onlyGivenPaths = true  : ONLY customPaths (used by "Add Folder")
    void startScan (const juce::StringArray& customPaths = {}, bool onlyGivenPaths = false);

    // Ask a running scan to stop (keeps everything found so far and saves it).
    void cancelScan();

    // Safe mode (Shift held at start): NO external plugin is ever touched. The cached list is not loaded,
    // scans are refused and the cache file is never rewritten, so the engine gets an EMPTY list of known
    // plugins and a saved session's inserts stay as "failed to load" placeholders instead of being
    // instantiated. Set once, right after construction and BEFORE loadCachedPlugins().
    void setSafeMode (bool on) noexcept      { safeMode = on; }
    bool isSafeMode() const noexcept         { return safeMode; }

    // Try to load cached plugin list from disk; returns true if loaded. (Always false in safe mode.)
    bool loadCachedPlugins();

    // Save current plugin list to disk.
    void saveCachedPlugins();

    // Forget the plugins that were skipped because they failed / crashed the scan ("retry bad plugins").
    // Does nothing while a scan is running. The next scan will try them again.
    void clearFailedPlugins();

    // Access results
    juce::Array<juce::PluginDescription> getScannedPlugins() const;
    int getProgressPercent() const noexcept { return progressPercent.load(); }
    bool isScanning() const noexcept { return isThreadRunning(); }
    bool hasCachedPlugins() const noexcept { return cachedPluginsLoaded; }

    // Realtime scan progress (safe to call from the UI thread while scanning)
    enum class Phase { idle = 0, searching, scanning, saving };
    Phase getPhase() const noexcept        { return (Phase) phase.load(); }
    int   getTotalFiles() const noexcept   { return totalFiles.load(); }
    int   getDoneFiles() const noexcept    { return doneFiles.load(); }
    int   getFoundCount() const            { return getScannedPlugins().size(); }      // de-duplicated
    int   getNewCount() const              { return juce::jmax (0, getScannedPlugins().size() - typesBeforeScan.load()); }   // de-duplicated
    int   getFailedCount() const noexcept  { return failedThisScan.load(); }
    bool  wasCancelled() const noexcept    { return lastScanCancelled.load(); }
    juce::String getCurrentItem() const    { const juce::ScopedLock sl (currentLock); return currentItem; }
    juce::String getCurrentFormat() const  { const juce::ScopedLock sl (currentLock); return currentFormat; }

    // Callbacks
    std::function<void()> onScanComplete;

private:
    void run() override;
    static juce::File getCacheFile();
    static juce::File getFoldersFile();   // persisted user-added scan folders

    juce::KnownPluginList knownPluginList;
    juce::AudioPluginFormatManager pluginFormatManager;

    std::atomic<int> progressPercent { 0 };
    std::atomic<int> phase { 0 };
    std::atomic<int> totalFiles { 0 };
    std::atomic<int> doneFiles { 0 };
    std::atomic<int> typesBeforeScan { 0 };
    std::atomic<int> failedThisScan { 0 };
    std::atomic<bool> userCancelled { false };
    std::atomic<bool> lastScanCancelled { false };
    bool scanDefaultLocations { true };
    bool safeMode { false };

    // Files that could not be loaded as a plugin ("path|modTimeMs"): skipped on later rescans
    // unless the file changed on disk, so a rescan never re-loads broken / unsupported DLLs.
    juce::StringArray failedFiles;
    static juce::File getFailedFile();
    static juce::File getScanMarkerFile();   // holds the path being scanned; still there after a crash
    void recoverFromCrashedScan();           // marker left by a crash -> that plugin goes to failedFiles
    void saveFailedFiles();
    static juce::String failKey (const juce::String& path);

    mutable juce::CriticalSection currentLock;
    juce::String currentItem, currentFormat;
    void setCurrent (const juce::String& item, const juce::String& format)
    {
        const juce::ScopedLock sl (currentLock);
        currentItem = item;
        currentFormat = format;
    }
    juce::StringArray pendingPaths;
    juce::StringArray userFolders;        // Layer 3: custom folders (e.g. jBridge output), saved to disk
    bool cachedPluginsLoaded { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanner)
};
