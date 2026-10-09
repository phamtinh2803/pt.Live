/*
    pt.Live - Live Singing DAW
    SongHistory - persistent list of songs the user has opened.
*/

#pragma once

#include <JuceHeader.h>

namespace ptl
{
    struct SongEntry
    {
        juce::String videoId;
        juce::String url;
        juce::String title;
        juce::String detectedRoot;
        juce::String detectedScale;
        juce::int64  timestamp = 0;
        int displayIndex = 0;
    };

    class SongHistory
    {
    public:
        SongHistory() = default;

        void load();
        void save();
        void add (const juce::String& videoId, const juce::String& url, const juce::String& title);
        void updateKey (const juce::String& videoId, const juce::String& root, const juce::String& scale);
        juce::Array<SongEntry> all() const;
        static juce::File configFile();
        static constexpr int maxEntries = 500;

    private:
        mutable juce::CriticalSection lock;
        juce::Array<SongEntry> entries;
        bool loaded = false;
    };
}
