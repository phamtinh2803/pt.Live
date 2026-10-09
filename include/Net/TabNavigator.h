/*
    pt.Live - Live Singing DAW
    TabNavigator - find and navigate the active YouTube browser tab.

    Reuses UI Automation infrastructure from SongDetector to:
      1) Find a visible browser window (Chrome, Edge, Firefox) showing YouTube
      2) Locate its address bar edit control
      3) Set the URL value in that field + send Enter key to load it

    This is how we "insert a link into the same tab" instead of opening a new one.
    If no suitable tab is found, we fall back to ShellExecute (open default browser).
*/

#pragma once

#include <JuceHeader.h>

// Plays `url` without ever starting a second YouTube tab:
//   - a browser already has YouTube open -> the link replaces that tab's address (same tab, brought to the front);
//   - the YouTube window is an app (no address bar, no tabs) -> that app is closed and the default browser opens;
//   - no browser has YouTube open        -> opens the default browser.
// Returns false only when a YouTube tab exists but could not be driven (nothing is opened in that case).
bool navigateToYouTubeTab (const juce::String& url);
