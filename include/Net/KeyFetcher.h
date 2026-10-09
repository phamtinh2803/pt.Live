/*
    pt.Live - Live Singing DAW
    KeyFetcher - asks the VPS for the key/scale of a song, in the background.

    Flow for one song (same order ptStudio uses on the server side):
      1. cache check          GET  /api/tone-cache/check/{id}   (instant when someone already analysed it)
      2. submit worker job    POST /api/worker/job
      3. wait for the result  GET  /api/worker/result/{job}     (long-poll, 3 s per round, 20 s in total)
    A newer song cancels the one being waited for (latest wins).
    On failure nothing changes in the mixer: the old key stays and onFailed tells the panel to say so.

    Callbacks run on the MESSAGE thread.
*/

#pragma once

#include <JuceHeader.h>
#include "include/Net/VpsClient.h"

class KeyFetcher : private juce::Thread
{
public:
    // tokenProvider: returns the current license token ("" = none). Given to the VpsClient BEFORE the thread
    // starts, so the very first request (warm-up) already carries it. Must be thread-safe (see LicenseClient::currentToken).
    explicit KeyFetcher (std::function<juce::String()> tokenProvider = {});
    ~KeyFetcher() override;

    bool isConfigured() const { return client.isConfigured(); }

    // Start (or restart) the lookup for this song. Cancels the previous one.
    void request (const juce::String& videoId, const juce::String& url, const juce::String& title);
    void cancel();
    void requestStop();      // asks the thread to finish and aborts a running request (does not wait)

    enum class Stage { CheckingCache, Analysing };

    std::function<void (const juce::String& videoId, Stage)>                         onProgress;
    std::function<void (const juce::String& videoId, const VpsClient::KeyResult&, bool fromCache)> onFound;
    std::function<void (const juce::String& videoId, const juce::String& reason)>     onFailed;

    static constexpr int totalTimeoutSec = 20;
    static constexpr int pollWindowSec   = 3;      // short rounds: a newer song is noticed within ~3 s while waiting

private:
    void run() override;

    struct Job { juce::String videoId, url, title; };

    VpsClient client;

    juce::CriticalSection lock;
    Job pending;                         // newest request, not picked up yet
    bool hasPending = false;
    std::atomic<bool> cancelCurrent { false };

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyFetcher)
};
