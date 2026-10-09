/*
    pt.Live - Live Singing DAW
    PluginScanner implementation

    Task 2.10: Background-threaded VST3/VST2 discovery, 3-tier strategy.
*/

#include <JuceHeader.h>
#include <set>
#include "include/UI/PluginScanner.h"
#include "include/Audio/PtLog.h"

using namespace juce;
using namespace te;

PluginScanner::PluginScanner()
    : Thread ("ptLive Plugin Scanner")
{
    // Register available plugin formats.
    // Use raw pointers instead of make_unique for compatibility (manager takes ownership).
    pluginFormatManager.addFormat (new VST3PluginFormat());

   #if JUCE_PLUGINHOST_VST
    // VST2 (.dll). This also covers 32-bit plugins bridged to 64-bit with jBridge,
    // which are ordinary 64-bit VST2 .dll files from the host's point of view.
    pluginFormatManager.addFormat (new VSTPluginFormat());
   #endif

    // Layer 3: restore user-added folders from the previous session.
    if (auto f = getFoldersFile(); f.existsAsFile())
        userFolders.addLines (f.loadFileAsString());

    userFolders.trim();
    userFolders.removeEmptyStrings();

    if (auto f = getFailedFile(); f.existsAsFile())
        failedFiles.addLines (f.loadFileAsString());

    failedFiles.removeEmptyStrings();

    recoverFromCrashedScan();
}

PluginScanner::~PluginScanner()
{
    // If the thread had to be killed inside a plugin DLL, do not blame that plugin next time.
    if (! stopThread (5000))
        getScanMarkerFile().deleteFile();
}

juce::File PluginScanner::getScanMarkerFile()
{
    return getCacheFile().getSiblingFile ("plugin_scanning.txt");
}

void PluginScanner::saveFailedFiles()
{
    getFailedFile().replaceWithText (failedFiles.joinIntoString ("\n"));
}

// The previous run died while scanning one plugin (marker file still there): never load it again.
void PluginScanner::recoverFromCrashedScan()
{
    const auto marker = getScanMarkerFile();

    if (! marker.existsAsFile())
        return;

    const auto path = marker.loadFileAsString().trim();
    marker.deleteFile();

    if (path.isEmpty())
        return;

    failedFiles.addIfNotAlreadyThere (failKey (path));
    saveFailedFiles();
    ptl::diag::write ("[scan] plugin crashed last time, skipped: " + path);
}

void PluginScanner::clearFailedPlugins()
{
    if (isThreadRunning())
        return;

    failedFiles.clear();
    getFailedFile().deleteFile();
}

void PluginScanner::startScan (const juce::StringArray& customPaths, bool onlyGivenPaths)
{
    if (isThreadRunning())
        return;

    if (safeMode)                                   // safe mode never loads a plugin DLL, not even to scan it
    {
        ptl::diag::write ("[safe] plugin scan refused (safe mode)");
        return;
    }

    // Remember folders added by the user so they survive "Rescan" and app restarts.
    // (Format default locations - registry + standard folders - are added in run().)
    for (const auto& p : customPaths)
        if (p.isNotEmpty())
            userFolders.addIfNotAlreadyThere (p);

    getFoldersFile().replaceWithText (userFolders.joinIntoString ("\n"));

    if (onlyGivenPaths)
    {
        // "Add Folder": scan ONLY what the user picked, nothing else.
        pendingPaths = customPaths;
        scanDefaultLocations = false;
    }
    else
    {
        pendingPaths = userFolders;
        scanDefaultLocations = true;
    }

    progressPercent = 0;
    userCancelled = false;
    lastScanCancelled = false;
    failedThisScan = 0;

    startThread (juce::Thread::Priority::low);       // never competes with the audio threads
}

void PluginScanner::cancelScan()
{
    if (! isThreadRunning())
        return;

    userCancelled = true;
    signalThreadShouldExit();
}

juce::File PluginScanner::getCacheFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive");
    dir.createDirectory();
    return dir.getChildFile ("plugin_cache.xml");
}

juce::File PluginScanner::getFoldersFile()
{
    return getCacheFile().getSiblingFile ("plugin_folders.txt");
}

juce::File PluginScanner::getFailedFile()
{
    return getCacheFile().getSiblingFile ("plugin_failed.txt");
}

juce::String PluginScanner::failKey (const juce::String& path)
{
    return path + "|" + juce::String (juce::File (path).getLastModificationTime().toMilliseconds());
}

bool PluginScanner::loadCachedPlugins()
{
    if (safeMode)                                   // empty list on purpose: see setSafeMode()
        return false;

    auto f = getCacheFile();
    if (! f.exists())
        return false;

    juce::XmlElement xml ("pluginlist");
    if (auto loaded = juce::XmlDocument::parse (f))
    {
        knownPluginList.recreateFromXml (*loaded);
        cachedPluginsLoaded = true;
        progressPercent = 100;
        return true;
    }
    return false;
}

void PluginScanner::saveCachedPlugins()
{
    if (safeMode)                                   // the list is empty here: never overwrite the real cache
        return;

    if (auto xml = knownPluginList.createXml())
    {
        auto f = getCacheFile();
        f.create();
        xml->writeTo (f);
    }
}

void PluginScanner::run()
{
    // NOTE: the plugin list is NOT cleared. Known + unchanged plugins stay, we only add new ones.
    typesBeforeScan = getScannedPlugins().size();      // de-duplicated, same count the list shows
    doneFiles = 0;
    totalFiles = 0;
    setCurrent ({}, {});

    bool cancelled = false;
    juce::uint32 lastCacheSaveMs = juce::Time::getMillisecondCounter();

    //=== Phase 1: search folders for candidate files, keep only the ones we do not know yet ===
    phase = (int) Phase::searching;

    struct Job { juce::AudioPluginFormat* format; juce::StringArray files; };
    std::vector<Job> jobs;

    for (auto* format : pluginFormatManager.getFormats())
    {
        if (format == nullptr || ! format->canScanForPlugins())
            continue;

        if (threadShouldExit())
        {
            if (! userCancelled) return;          // app is quitting: leave silently
            cancelled = true;
            break;
        }

        setCurrent ("Searching folders...", format->getName());

        // Directories to search: format defaults (registry + standard folders) + user folders.
        juce::FileSearchPath searchPath;
        if (scanDefaultLocations)
            searchPath = format->getDefaultLocationsToSearch();

        for (const auto& p : pendingPaths)
            if (p.isNotEmpty())
                searchPath.addPath (p);

        auto candidates = format->searchPathsForPlugins (searchPath, true);
        juce::StringArray todo;

        for (const auto& file : candidates)
        {
            if (auto known = knownPluginList.getTypeForFile (file))
            {
                if (! format->pluginNeedsRescanning (*known))      // already known + unchanged: skip
                    continue;
            }
            else if (failedFiles.contains (failKey (file)))        // failed before, unchanged: skip
                continue;

            todo.add (file);
        }

        totalFiles += todo.size();
        jobs.push_back ({ format, std::move (todo) });
    }

    //=== Phase 2: scan every NEW file, reporting the current one ===
    phase = (int) Phase::scanning;

    for (auto& job : jobs)
    {
        if (cancelled)
            break;

        juce::OwnedArray<juce::PluginDescription> results;

        for (const auto& file : job.files)
        {
            if (threadShouldExit())
            {
                if (! userCancelled) return;
                cancelled = true;
                break;
            }

            setCurrent (juce::File (file).getFileName().isNotEmpty()
                            ? juce::File (file).getFileName() : file,
                        job.format->getName());

            results.clear();

            // Written BEFORE touching the DLL: if the process dies inside it, the next start sees
            // this file and skips that plugin (see recoverFromCrashedScan).
            getScanMarkerFile().replaceWithText (file);
            knownPluginList.scanAndAddFile (file, true, results, *job.format);
            getScanMarkerFile().deleteFile();

            if (results.isEmpty() && knownPluginList.getTypeForFile (file) == nullptr)
            {
                failedFiles.addIfNotAlreadyThere (failKey (file));
                ++failedThisScan;
                saveFailedFiles();                       // tiny file: save at once
            }
            else if (juce::Time::getMillisecondCounter() - lastCacheSaveMs > 2000)
            {
                saveCachedPlugins();                     // a crash later in the scan keeps what was found
                lastCacheSaveMs = juce::Time::getMillisecondCounter();
            }

            const int done = ++doneFiles;
            const int total = juce::jmax (1, totalFiles.load());
            progressPercent = juce::jlimit (0, 99, (int) ((double) done * 100.0 / (double) total));
        }
    }

    //=== Phase 3: save ===
    phase = (int) Phase::saving;
    setCurrent ("Saving plugin list...", {});
    saveCachedPlugins();
    saveFailedFiles();

    lastScanCancelled = cancelled;
    progressPercent = 100;
    setCurrent ({}, {});
    phase = (int) Phase::idle;

    // Notify listener on the message thread
    if (onScanComplete)
        juce::MessageManager::callAsync ([this] { if (onScanComplete) onScanComplete(); });
}

// Two files are "the same plugin" when format, name, manufacturer and unique ID all match,
// even if they live in different folders (JUCE only treats identical file paths as duplicates).
static juce::String makeDedupKey (const juce::PluginDescription& d)
{
    return d.pluginFormatName.toLowerCase()                 + "|"
         + d.name.trim().toLowerCase()                      + "|"
         + d.manufacturerName.trim().toLowerCase()          + "|"
         + juce::String (d.uniqueId)                        + "|"
         + juce::String (d.deprecatedUid)                   + "|"
         + (d.isInstrument ? "i" : "fx");
}

juce::Array<juce::PluginDescription> PluginScanner::getScannedPlugins() const
{
    juce::Array<juce::PluginDescription> result;
    std::set<juce::String> seen;

    // Types are in discovery order (format default folders first, then user folders),
    // so the first copy found wins and later copies are dropped.
    for (const auto& d : knownPluginList.getTypes())
    {
        const auto key = makeDedupKey (d);

        if (! seen.insert (key).second)      // same plugin found in another folder: count it once
            continue;

        result.add (d);
    }

    return result;
}
