/*
    pt.Live - Live Singing DAW
    Windows file association for pt.Live session files (.pt).

    Registers, for the CURRENT USER only (HKEY_CURRENT_USER\Software\Classes - no admin rights):
      .pt                 -> ptLive.Session
      ptLive.Session      -> "pt.Live Session"
      ...\DefaultIcon     -> "<ptLive.exe>",-101        the SAME icon as the application
      ...\shell\open\command -> "<ptLive.exe>" "%1"     double-click opens the file in pt.Live

    The icon is the application's own ptlive.ico, referenced by its resource ID (101, APP_ICON_ID
    in resources/ptlive.rc). A negative number in a DefaultIcon value means "resource ID", so it does
    not depend on the order of the icons inside the exe. Nothing is embedded in the .pt files
    themselves: Explorer takes the icon of a file type from this registry entry.

    Both paths are quoted (JUCE's own registerFileAssociation() does not quote the command, which
    breaks for an install folder such as "C:\Program Files\...").
*/

#pragma once

#include <JuceHeader.h>

#if JUCE_WINDOWS
 #include <shlobj.h>
 #pragma comment (lib, "shell32.lib")
#endif

namespace ptl::fileassoc
{
    inline constexpr int iconResourceId = 101;          // APP_ICON_ID in resources/ptlive.rc

   #if JUCE_WINDOWS
    namespace detail
    {
        inline juce::String classesRoot()      { return "HKEY_CURRENT_USER\\Software\\Classes\\"; }
        inline juce::String progId()           { return "ptLive.Session"; }
        inline juce::String exePath()          { return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(); }
        inline juce::String expectedCommand()  { return "\"" + exePath() + "\" \"%1\""; }
        inline juce::String expectedIcon()     { return "\"" + exePath() + "\"," + juce::String (-iconResourceId); }
    }
   #endif

    /** True when .pt already points at THIS executable (path, command and icon all match). */
    inline bool isRegistered()
    {
       #if JUCE_WINDOWS
        using namespace detail;
        const auto root = classesRoot();

        return juce::WindowsRegistry::getValue (root + ".pt\\")                                  == progId()
            && juce::WindowsRegistry::getValue (root + progId() + "\\shell\\open\\command\\")    == expectedCommand()
            && juce::WindowsRegistry::getValue (root + progId() + "\\DefaultIcon\\")             == expectedIcon();
       #else
        return false;
       #endif
    }

    /** Writes the association for the current user and tells Explorer to refresh its icons. */
    inline bool registerForCurrentUser()
    {
       #if JUCE_WINDOWS
        using namespace detail;
        const auto root = classesRoot();
        const auto id   = progId();

        const bool ok = juce::WindowsRegistry::setValue (root + ".pt\\",                              id)
                     && juce::WindowsRegistry::setValue (root + id + "\\",                            "pt.Live Session")
                     && juce::WindowsRegistry::setValue (root + id + "\\DefaultIcon\\",               expectedIcon())
                     && juce::WindowsRegistry::setValue (root + id + "\\shell\\open\\command\\",      expectedCommand());

        if (ok)
            SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        return ok;
       #else
        return false;
       #endif
    }

    /** Called at startup: (re)registers only when needed, e.g. the first run or after the exe moved. */
    inline void registerIfNeeded()
    {
        if (! isRegistered())
            registerForCurrentUser();
    }
}
