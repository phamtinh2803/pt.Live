/*
    pt.Live - Live Singing DAW
    KeyFetcher implementation.
*/

#include "include/Net/KeyFetcher.h"
#include "include/Audio/PtLog.h"

KeyFetcher::KeyFetcher (std::function<juce::String()> tokenProvider) : juce::Thread ("pt.Live key fetcher")
{
    if (tokenProvider)
        client.setTokenProvider (std::move (tokenProvider));     // before startThread: no race with run()
    startThread (juce::Thread::Priority::low);
}

KeyFetcher::~KeyFetcher()
{
    requestStop();
    stopThread (-1);          // never a forced kill (it can leave a lock held); cancelCurrent aborts the HTTP wait
}

void KeyFetcher::requestStop()
{
    alive->store (false);
    cancelCurrent.store (true);
    signalThreadShouldExit();
    notify();
}

void KeyFetcher::request (const juce::String& videoId, const juce::String& url, const juce::String& title)
{
    {
        const juce::ScopedLock sl (lock);
        pending = { videoId, url, title };
        hasPending = true;
        cancelCurrent.store (true);      // stop waiting for the previous song (inside the lock: run() clears it when it takes the job)
    }
    notify();
}

void KeyFetcher::cancel()
{
    {
        const juce::ScopedLock sl (lock);
        hasPending = false;
    }
    cancelCurrent.store (true);
}

void KeyFetcher::run()
{
    // Open DNS + TLS now, while nobody is waiting: the first real song then reuses the connection.
    client.warmUp();

    while (! threadShouldExit())
    {
        Job job;
        {
            const juce::ScopedLock sl (lock);
            if (hasPending)
            {
                job = pending;
                hasPending = false;
                cancelCurrent.store (false);     // set under the lock: a newer request() sets it again afterwards
            }
        }

        if (job.videoId.isEmpty())
        {
            wait (500);
            continue;
        }

        auto post = [flag = alive] (std::function<void()> fn)
        {
            juce::MessageManager::callAsync ([flag, fn = std::move (fn)] { if (flag->load()) fn(); });
        };

        const auto id = job.videoId;
        ptl::diag::write ("[vps] new song " + id + " | " + job.title + " | configured=" + juce::String (client.isConfigured() ? "yes" : "NO"));

        if (! client.isConfigured())
        {
            post ([this, id] { if (onFailed) onFailed (id, "vps_config.json"); });
            continue;
        }

        // 1. cache
        post ([this, id] { if (onProgress) onProgress (id, Stage::CheckingCache); });

        if (auto hit = client.checkCache (id, cancelCurrent); hit.has_value() && ! cancelCurrent.load())
        {
            auto r = *hit;
            ptl::diag::write ("[vps] cache HIT " + id + " -> " + r.root + " " + r.scale);
            post ([this, id, r] { if (onFound) onFound (id, r, true); });
            continue;
        }
        if (cancelCurrent.load()) continue;
        ptl::diag::write ("[vps] cache MISS (or error, see lines above) -> submitting worker job for " + id);

        // 2. job
        post ([this, id] { if (onProgress) onProgress (id, Stage::Analysing); });

        const auto jobId = client.submitJob (id, job.url, job.title);
        if (cancelCurrent.load()) continue;
        if (jobId.isEmpty())
        {
            ptl::diag::write ("[vps] submit job FAILED for " + id + " (server did not return a job_id)");
            post ([this, id] { if (onFailed) onFailed (id, "worker"); });
            continue;
        }

        ptl::diag::write ("[vps] job accepted: " + jobId + " - waiting for a worker result");

        // 3. wait (long-poll rounds)
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) (totalTimeoutSec * 1000);
        bool done = false;

        while (! cancelCurrent.load() && ! threadShouldExit() && juce::Time::getMillisecondCounter() < deadline)
        {
            if (auto res = client.pollResult (jobId, pollWindowSec, cancelCurrent); res.has_value())
            {
                if (! cancelCurrent.load())
                {
                    auto r = *res;
                    ptl::diag::write ("[vps] worker result " + id + " -> " + r.root + " " + r.scale);
                    post ([this, id, r] { if (onFound) onFound (id, r, false); });
                }
                done = true;
                break;
            }
            if (! cancelCurrent.load()) wait (300);       // tiny pause when the server answered "not yet" at once
        }

        if (! done && ! cancelCurrent.load() && ! threadShouldExit())
        {
            ptl::diag::write ("[vps] no result for " + id + " within " + juce::String (totalTimeoutSec) + " s");
            post ([this, id] { if (onFailed) onFailed (id, "timeout"); });
        }
    }
}
