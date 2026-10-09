/*
    pt.Live - Live Singing DAW
    SongDetector - the C++ version of ptStudio's lay_link.py.

    Every 0.3 s a background thread looks (Win32, very cheap) for a browser window that shows YouTube.
    The address bar element of each window is remembered, so each poll reads the address with ONE cheap call;
    the video id in the address decides when the song changed (the window title lags behind it).
    The slow UI Automation tree search only runs when the title changed and the bar is not known yet.  UI Automation lives on this one thread only (its own COM
    apartment), never on the message thread - the same rule that fixed the ptStudio crashes.

    Same rules as ptStudio:
      - title containing "liên khúc" is NOT reported as a song (no key lookup, no history) - see onTitleOnly
      - with the filter ON (default) only titles containing "karaoke" or "beat" are reported as a song
      - every song that is not reported (medley, no karaoke/beat) still goes to onTitleOnly, so the app can SHOW its
        title and nothing else (the title never stays stuck on the previous song)
      - a video id is reported once; forget() lets it be reported again
    The title is fetched by video id from YouTube oEmbed, so it always belongs to the right video. If that fails,
    the window title is the fallback (Violentmonkey "[01:23 / 10:45]" prefix, "(3)" counter and
    " - YouTube - Browser" suffix are removed), used only after it has changed from the previous song.

    onSong and onTitleOnly are called on the MESSAGE thread.
*/

#pragma once

#include <JuceHeader.h>

class SongDetector : private juce::Thread
{
public:
    struct Song
    {
        juce::String videoId, url, title;
    };

    SongDetector();
    ~SongDetector() override;

    void start();
    void stop();
    void requestStop();      // only asks the thread to finish (does not wait) - lets several threads close in parallel

    // Report the current song again even though it was already reported (user pressed "dò lại").
    void forget();

    void setFilterEnabled (bool on) noexcept { filterEnabled.store (on); }
    bool isFilterEnabled() const noexcept    { return filterEnabled.load(); }

    std::function<void (const Song&)> onSong;

    // A new video whose title was NOT accepted as a song (medley, or no "karaoke" / "beat" with the filter on):
    // only the title should be shown, no key lookup, no history entry, no reset.
    std::function<void (const Song&)> onTitleOnly;

    static juce::String extractVideoId (const juce::String& url);
    static juce::String cleanTitle (const juce::String& windowTitle);

private:
    void run() override;

    std::atomic<bool> filterEnabled { true };
    std::atomic<bool> forgetRequested { false };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongDetector)
};
