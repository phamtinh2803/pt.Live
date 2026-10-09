/*
    pt.Live - Live Singing DAW
    SongHistory implementation.
*/

#include "include/Audio/SongHistory.h"
#include "include/Audio/PtLog.h"

using namespace ptl;

juce::File SongHistory::configFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive");
    dir.createDirectory();
    return dir.getChildFile ("song_history.json");
}

void SongHistory::load()
{
    const juce::ScopedLock sl (lock);

    if (loaded)
        return;
    loaded = true;

    entries.clearQuick();

    const auto f = configFile();
    if (! f.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (f.loadFileAsString());
    if (! parsed.isArray())
        return;

    auto* arr = parsed.getArray();
    if (arr == nullptr)
        return;

    for (const auto& item : *arr)
    {
        if (! item.isObject())
            continue;

        SongEntry e;
        e.videoId       = item.getProperty ("videoId", {}).toString();
        e.url           = item.getProperty ("url", {}).toString();
        e.title         = item.getProperty ("title", {}).toString();
        e.detectedRoot  = item.getProperty ("root", {}).toString();
        e.detectedScale = item.getProperty ("scale", {}).toString();
        e.timestamp     = (juce::int64) item.getProperty ("time", (juce::int64) 0);

        if (e.url.isEmpty() && e.videoId.isEmpty())
            continue;

        entries.add (e);
    }

    ptl::diag::write ("[history] loaded " + juce::String (entries.size()) + " song(s)");
}

void SongHistory::save()
{
    juce::Array<SongEntry> copy;
    {
        const juce::ScopedLock sl (lock);
        copy = entries;
    }

    juce::Array<juce::var> arr;

    for (const auto& e : copy)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("videoId", e.videoId);
        o->setProperty ("url",     e.url);
        o->setProperty ("title",   e.title);
        o->setProperty ("root",    e.detectedRoot);
        o->setProperty ("scale",   e.detectedScale);
        o->setProperty ("time",    e.timestamp);
        arr.add (juce::var (o));
    }

    // One entry per line: readable when it grows large, and cheap to rewrite.
    juce::String json = "[\n";
    for (int i = 0; i < arr.size(); ++i)
    {
        json << "  " << juce::JSON::toString (arr.getReference (i), true);
        if (i < arr.size() - 1)
            json << ",";
        json << "\n";
    }
    json << "]\n";

    configFile().replaceWithText (json);
}

void SongHistory::add (const juce::String& videoId, const juce::String& url, const juce::String& title)
{
    {
        const juce::ScopedLock sl (lock);

        // Promote an existing entry (same video id, or same url when the id is unknown).
        for (int i = 0; i < entries.size(); ++i)
        {
            auto& e = entries.getReference (i);
            const bool same = (videoId.isNotEmpty() && e.videoId == videoId)
                           || (videoId.isEmpty() && e.url == url);

            if (same)
            {
                e.url       = url.isNotEmpty() ? url : e.url;
                e.title     = title.isNotEmpty() ? title : e.title;
                e.videoId   = videoId.isNotEmpty() ? videoId : e.videoId;
                e.timestamp = juce::Time::currentTimeMillis();
                entries.move (i, 0);                    // newest goes to the front (#1)
                save();
                return;
            }
        }

        SongEntry e;
        e.videoId   = videoId;
        e.url       = url;
        e.title     = title;
        e.timestamp = juce::Time::currentTimeMillis();
        entries.insert (0, e);

        while (entries.size() > maxEntries)
            entries.removeLast();
    }

    save();
}

void SongHistory::updateKey (const juce::String& videoId, const juce::String& root, const juce::String& scale)
{
    if (videoId.isEmpty())
        return;

    bool changed = false;
    {
        const juce::ScopedLock sl (lock);

        for (auto& e : entries)
        {
            if (e.videoId == videoId)
            {
                if (root.isNotEmpty())  e.detectedRoot  = root;
                if (scale.isNotEmpty()) e.detectedScale = scale;
                changed = true;
                break;
            }
        }
    }

    if (changed)
        save();
}

juce::Array<SongEntry> SongHistory::all() const
{
    const juce::ScopedLock sl (lock);
    auto copy = entries;

    // Newest first; assign the display number (1 = newest).
    std::sort (copy.begin(), copy.end(),
               [] (const SongEntry& a, const SongEntry& b) { return a.timestamp > b.timestamp; });

    for (int i = 0; i < copy.size(); ++i)
        copy.getReference (i).displayIndex = i + 1;

    return copy;
}
