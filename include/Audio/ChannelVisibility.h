/*
    pt.Live - Live Singing DAW
    ChannelVisibility - which ASIO channels the user ticked in Devices > Audio Settings.

    The ticks decide which inputs / outputs the track combos OFFER and which channels the driver
    is opened with: exactly the ticked ones (sparse, like Cubase / ASIO Link Pro's routing, e.g.
    In 1-2 + In 11-12 + Out 1-2) and nothing else. Tracktion indexes audio buffers by the physical
    channel index, JUCE hands over only the active channels packed from 0:
    AudioEngine::SparseChannelAdapter converts between the two.

    First sight of a driver = In 1-2 + Out 1-2 only (like Cubase); the user ticks anything else.
    Stored per driver name in  %APPDATA%\ptLive\channel_visibility.xml  (written on every change).
    Message thread only.
*/

#pragma once

#include <JuceHeader.h>
#include <map>

namespace ptl
{
class ChannelVisibility : public juce::ChangeBroadcaster
{
public:
    static ChannelVisibility& get()
    {
        static ChannelVisibility instance;
        return instance;
    }

    bool hasEntry (const juce::String& driver) const    { return entries.find (driver) != entries.end(); }

    juce::BigInteger getInputs (const juce::String& driver) const
    {
        auto it = entries.find (driver);
        return it != entries.end() ? it->second.in : juce::BigInteger();
    }

    juce::BigInteger getOutputs (const juce::String& driver) const
    {
        auto it = entries.find (driver);
        return it != entries.end() ? it->second.out : juce::BigInteger();
    }

    // First time we see a driver: Cubase-style default = the first stereo pair, In 1-2 + Out 1-2.
    // Everything else stays OFF until the user ticks it (a virtual driver such as ASIO Link Pro
    // exposes 64 channels - enabling them all makes the driver stream and mix channels nobody uses).
    // Channels the device does not have are ignored by the driver / hidden by the dialog.
    // No broadcast - nothing the user could notice has changed.
    void initialise (const juce::String& driver)
    {
        if (hasEntry (driver))
            return;

        Entry e;
        e.in.setRange  (0, kDefaultChannels, true);
        e.out.setRange (0, kDefaultChannels, true);
        entries[driver] = e;
        save();
    }

    void set (const juce::String& driver, const juce::BigInteger& in, const juce::BigInteger& out)
    {
        auto it = entries.find (driver);
        if (it != entries.end() && it->second.in == in && it->second.out == out)
            return;

        entries[driver] = { in, out };
        save();
        sendChangeMessage();          // track combos refresh
    }

private:
    static constexpr int kDefaultChannels = 2;     // default: In 1-2 / Out 1-2
    static constexpr int kFileVersion     = 2;     // v1 files stored "whatever the device streamed" (up to 64)
    static constexpr int kLegacyMaxKept   = 8;     // v1 entries with more channels than this are dropped

    struct Entry { juce::BigInteger in, out; };
    std::map<juce::String, Entry> entries;

    ChannelVisibility() { load(); }

    static juce::File getFile()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ptLive");
        dir.createDirectory();
        return dir.getChildFile ("channel_visibility.xml");
    }

    void load()
    {
        if (auto xml = juce::XmlDocument::parse (getFile()))
        {
            if (! xml->hasTagName ("ChannelVisibility"))
                return;

            const bool legacy = xml->getIntAttribute ("version", 1) < kFileVersion;

            for (auto* e : xml->getChildIterator())
            {
                if (! e->hasTagName ("Driver"))
                    continue;

                Entry en;
                en.in.parseString  (e->getStringAttribute ("in"),  16);
                en.out.parseString (e->getStringAttribute ("out"), 16);

                // Old builds saved every channel the device happened to stream (ASIO Link Pro: 64 in /
                // 64 out). Drop those so the driver gets the 2 in / 2 out default instead.
                if (legacy && (en.in.countNumberOfSetBits() > kLegacyMaxKept
                               || en.out.countNumberOfSetBits() > kLegacyMaxKept))
                    continue;

                entries[e->getStringAttribute ("name")] = en;
            }

            if (legacy)
                save();                // stamp the new version so this runs only once
        }
    }

    void save() const
    {
        juce::XmlElement root ("ChannelVisibility");
        root.setAttribute ("version", kFileVersion);

        for (const auto& kv : entries)
        {
            auto* e = root.createNewChildElement ("Driver");
            e->setAttribute ("name", kv.first);
            e->setAttribute ("in",   kv.second.in.toString (16));
            e->setAttribute ("out",  kv.second.out.toString (16));
        }

        root.writeTo (getFile());
    }
};
} // namespace ptl
