/*
    pt.Live - Live Singing DAW
    Recently used sessions (.pt): a small text file, one full path per line, newest first.

        %APPDATA%\ptLive\recent.txt

    add()  is called when pt.Live opens a .pt (double-click, Open..., Recent) and when a session is saved to a file.
    get()  returns the newest files that STILL EXIST (a deleted / moved file is simply skipped), for the start screen.
    Message thread only.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/PtLog.h"

namespace ptl::recent
{
    constexpr int keepCount = 10;                      // remembered on disk; the start screen shows the newest 3

    inline juce::File listFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive").getChildFile ("recent.txt");
    }

    inline juce::StringArray readPaths()
    {
        juce::StringArray lines, paths;
        lines.addLines (listFile().loadFileAsString());

        for (const auto& l : lines)
            if (l.trim().isNotEmpty())
                paths.add (l.trim());

        return paths;
    }

    // Newest first, existing files only.
    inline juce::Array<juce::File> get (int maxCount = 3)
    {
        juce::Array<juce::File> out;

        for (const auto& path : readPaths())
        {
            const juce::File f (path);

            if (f.existsAsFile() && ! out.contains (f))
                out.add (f);

            if (out.size() >= maxCount)
                break;
        }

        return out;
    }

    // Puts `file` at the top (moves it there when it is already in the list). Every outcome goes to ptLive.log
    // as "[recent] ..." so a missing entry on the start screen can be traced.
    inline bool add (const juce::File& file)
    {
        if (file == juce::File())
            return false;                                  // default (unnamed) session: nothing to remember

        if (! file.existsAsFile() || ! file.hasFileExtension ("pt"))
        {
            ptl::diag::write ("[recent] NOT added (missing or not .pt): " + file.getFullPathName());
            return false;
        }

        juce::StringArray paths;
        paths.add (file.getFullPathName());

        for (const auto& path : readPaths())
            if (juce::File (path) != file && paths.size() < keepCount)
                paths.add (path);

        const auto list = listFile();
        list.getParentDirectory().createDirectory();

        const auto text = paths.joinIntoString ("\n");
        bool ok = list.replaceWithText (text, false, false, "\n");

        if (! ok)                                          // replace = temp file + rename; plain overwrite as a fallback
            ok = list.deleteFile() && list.appendText (text + "\n", false, false, "\n");

        ptl::diag::write (juce::String ("[recent] ") + (ok ? "added " : "WRITE FAILED for ")
                          + file.getFullPathName() + "  -> " + list.getFullPathName());
        return ok;
    }
}
