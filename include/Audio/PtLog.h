/*
    pt.Live - Live Singing DAW
    Tiny diagnostic logger.

    Writes timestamped lines to  %APPDATA%\ptLive\ptLive.log  (re-created on every start) and to
    the debugger Output window (DBG). Thread-safe. Used to find out WHEN and WHY a mono track
    ends up stereo after inserting a plugin (see TrackManager::applyTrackChannelLayout,
    InspectorPanel::insertPlugin and TrackRow::updateMeter).
*/

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include "include/Audio/TracktionHeaders.h"

namespace ptl::diag
{
    inline juce::File getLogDir()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ptLive");
        dir.createDirectory();
        return dir;
    }

    inline juce::File getLogFile()
    {
        return getLogDir().getChildFile ("ptLive.log");
    }

    /** Keeps the log of the run that just ended. A crash leaves the reason ([CRASH] / [HANG] line
        and the dump name) at the END of ptLive.log; that line is exactly what the next start would
        destroy, because a new run truncates the log. So before the first write() of a new run we copy
        the previous file to ptLive.prev.log (overwriting the older copy), ONLY if it actually holds
        something worth keeping. Called once, on the first write() of the process. */
    inline void keepPreviousLog()
    {
        const auto current = getLogFile();
        if (! current.existsAsFile())
            return;

        const auto content = current.loadFileAsString();
        if (content.isEmpty() || ! content.containsChar ('['))
            return;                               // empty or a bare header: nothing to preserve

        const auto prev = getLogDir().getChildFile ("ptLive.prev.log");
        prev.deleteFile();                        // drop the older .prev (overwrite it)
        current.copyFileTo (prev);
    }

    /** Startup housekeeping: ptLive.log and the dumps (crash-*.dmp / hang-*.dmp) grow forever.
        Keep the 5 newest dumps of each kind, drop the rest. Called once, early in the app's life. */
    inline void cleanOldDumps()
    {
        const auto dir = getLogDir();
        if (! dir.isDirectory())
            return;

        for (const auto* prefix : { "crash-", "hang-" })
        {
            juce::Array<juce::File> dumps;

            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, juce::String (prefix) + "*.dmp"))
                dumps.add (f);

            // Oldest first, so the front of the list is what gets deleted.
            std::sort (dumps.begin(), dumps.end(),
                       [] (const juce::File& a, const juce::File& b)
                       { return a.getLastModificationTime() < b.getLastModificationTime(); });

            while (dumps.size() > 5)
                dumps.removeAndReturn (0).deleteFile();
        }
    }

    /** TRUE when the line reports a problem (failure, refusal, missing file, xrun...). Only these
        lines are kept in ptLive.log; routine progress lines are dropped. */
    inline bool isErrorLine (const juce::String& message)
    {
        static const char* const keywords[] =
        {
            "fail", "error", "could not", "cannot", "can't", "refused", "rejected", "blocked",
            "crash", "hang", "xrun", "not found", "not available", "problem", "invalid",
            "exception", "no active driver input",
            "[asio]",                     // driver open / reload / latency lines (few, needed to debug start-up latency)
            "[safe]",                     // safe mode (Shift at start): what was refused / what is still loaded
            "[health]"                    // hourly "still running fine" line (see MainWindow::logHealthLine)
        };

        for (auto* k : keywords)
            if (message.containsIgnoreCase (k))
                return true;

        return false;
    }

    /** Set the environment variable PTLIVE_VERBOSE=1 to keep EVERY line again (for development). */
    inline bool isVerbose()
    {
        static const bool verbose = juce::SystemStats::getEnvironmentVariable ("PTLIVE_VERBOSE", {}) == "1";
        return verbose;
    }

    inline void write (const juce::String& message)
    {
        static juce::CriticalSection lock;
        static bool started = false;
        static bool headerWritten = false;

        const juce::ScopedLock sl (lock);
        const auto file = getLogFile();

        if (! started)                       // a new run starts with a clean log
        {
            started = true;
            keepPreviousLog();               // ptLive.prev.log keeps the last run (its [CRASH] line)
            file.deleteFile();
        }

        const bool keep = isVerbose() || isErrorLine (message);

        const auto now = juce::Time::getCurrentTime();
        const auto line = now.formatted ("%H:%M:%S") + "."
                        + juce::String (now.getMilliseconds()).paddedLeft ('0', 3)
                        + "  " + message;

        DBG (line);                          // the debugger still sees everything (Debug builds)

        if (! keep)
            return;                          // routine line: not written to the file

        if (! headerWritten)                 // the file only exists when something went wrong
        {
            headerWritten = true;
            file.appendText ("=== pt.Live log started " + now.toString (true, true) + " ===\n");
        }

        file.appendText (line + "\n");
    }

    inline juce::String describeBuses (juce::AudioProcessor& p, bool isInput)
    {
        juce::String s;

        for (int i = 0, n = p.getBusCount (isInput); i < n; ++i)
            if (auto* bus = p.getBus (isInput, i))
                s << (i > 0 ? "," : "") << bus->getName() << "="
                  << bus->getCurrentLayout().getDescription()
                  << (bus->isEnabled() ? "" : "(off)");

        return s.isEmpty() ? juce::String ("none") : s;
    }

    /** One-line description of a plugin: name, type, enabled, bus layouts, channels it reports. */
    inline juce::String describePlugin (te::Plugin& plugin)
    {
        juce::String s;
        s << "'" << plugin.getName() << "' <" << plugin.getPluginType() << "> "
          << (plugin.isEnabled() ? "on" : "BYPASSED");

        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&plugin))
        {
            if (auto* pi = ext->getAudioPluginInstance())
                s << " | loaded | in[" << describeBuses (*pi, true) << "] out[" << describeBuses (*pi, false) << "]";
            else
                s << " | NOT LOADED";
        }

        s << " | reportsOut(1in)=" << plugin.getNumOutputChannelsGivenInputs (1)
          << " | latency=" << juce::String (plugin.getLatencySeconds() * 1000.0, 2) << "ms";

        return s;
    }
}
