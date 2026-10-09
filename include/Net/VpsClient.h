/*
    pt.Live - Live Singing DAW
    VpsClient - talks to the SAME VPS and the SAME API that the released ptStudio uses.
    Nothing on the server changes, ptStudio keeps working as before.

    Endpoints used (all with header  X-API-Key and/or X-License-Token):
      GET  /api/tone-cache/check/{video_id}          -> {"found": true, "entry": {root, scale, viet, western, ...}}
      POST /api/worker/job   {video_id,url,title,mode} -> {"job_id": "..."}
      GET  /api/worker/result/{job_id}?timeout=N      -> {"status":"ok","result":{root,scale,viet,western}}

    NOT used on purpose: /api/tone-cache/sync (ptLive only reads, it never writes into the shared cache).

    Config files (same format as ptStudio), looked up next to ptLive.exe, then in %APPDATA%/ptLive:
      vps_config.json   {"url": "https://host[:port]", "key": "..."}      ("key" is OPTIONAL, see token mode)

    Token mode: when a token provider is set (setTokenProvider) every non-license request also carries
      X-License-Token: <ed25519 license token>
    so a release build can ship vps_config.json with only "url" - the license token is the credential.
    If the config still has a "key", BOTH headers are sent (the server accepts either).
      vps_cert.pem      pinned certificate / CA (same file ptStudio ships in _internal)

    TLS: when vps_cert.pem exists the server certificate is accepted only if it IS one of the pinned
    certificates or is SIGNED by one of them (host name is not checked - the server is reached by IP,
    like ptStudio does).  Without vps_cert.pem normal Windows validation is used.

    All calls are blocking: call them from a worker thread, never from the message thread.

    Speed: ONE WinHTTP session + ONE TLS connection are kept and reused (keep-alive) for every call, so a song
    costs a single TLS handshake (or none, when warmUp() already made it) instead of three. The system proxy is
    used WITHOUT automatic proxy detection (WPAD), which can add seconds to every first request.
*/

#pragma once

#include <JuceHeader.h>
#include <optional>

class VpsClient
{
public:
    struct KeyResult
    {
        juce::String root, scale, viet, western;
        bool isValid() const { return root.isNotEmpty() && scale.isNotEmpty(); }
    };

    VpsClient();
    ~VpsClient();

    // Opens the connection (DNS + TLS) in advance, so the first real song does not pay for it.
    void warmUp();

    // Source of the license token (called on every request, from any thread: must be thread-safe and cheap).
    // Returns "" when this machine has no license token. Set once, before the first request.
    void setTokenProvider (std::function<juce::String()> provider);

    // Ready to talk: url known AND (a "key" in vps_config.json OR the token provider returns a token).
    bool isConfigured() const;

    // Cache of results other users / earlier sessions already analysed.
    std::optional<KeyResult> checkCache (const juce::String& videoId, std::atomic<bool>& cancel);

    // Ask the worker to analyse the song. Returns job id or {} (worker offline / error).
    juce::String submitJob (const juce::String& videoId, const juce::String& url, const juce::String& title);

    // Long-poll one window (server holds the request up to `timeoutSec`). nullopt = not ready yet.
    std::optional<KeyResult> pollResult (const juce::String& jobId, int timeoutSec, std::atomic<bool>& cancel);

    // Reply of postJson(). status 0 = no answer at all (offline, TLS refused, cancelled).
    struct Response { int status = 0; juce::String body; };

    // POST a JSON body and return the raw status + body (blocking, worker thread only).
    // sendApiKey = false for /api/license/*: those calls are authorised by the key / token inside the
    // body, not by the shared X-API-Key, and must also work when vps_config.json has no "key".
    Response postJson (const juce::String& path, const juce::var& body, bool sendApiKey,
                       std::atomic<bool>& cancel, int timeoutMs = 8000);

    // GET a PUBLIC path (no X-API-Key, no X-License-Token), e.g. the update manifest. Same pinned TLS connection.
    // Blocking, worker thread only. status 0 = no answer at all.
    Response getPublic (const juce::String& path, std::atomic<bool>& cancel, int timeoutMs = 5000);

private:
    Response request (const char* verb, const juce::String& path, const juce::String& jsonBody,
                      int timeoutMs, std::atomic<bool>& cancel, bool retried = false, bool sendApiKey = true);

    juce::String currentToken() const;       // "" = none / provider not set / token with unexpected characters

    static std::optional<KeyResult> parseKey (const juce::var& v);
    void loadConfig();
    void loadPinnedCerts (const juce::File& pem);

    void closeConnection();

    juce::CriticalSection netLock;       // one request at a time on the shared connection
    void* hSession = nullptr;            // HINTERNET (Windows)
    void* hConnect = nullptr;            // HINTERNET (Windows)

    juce::String host, apiKey;
    mutable juce::CriticalSection providerLock;
    std::function<juce::String()> tokenProvider;         // guarded by providerLock
    std::atomic<bool> tokenModeLogged { false };
    int port = 443;
    bool secure = true;

    struct Pinned;                       // CERT_CONTEXT list (Windows only)
    std::unique_ptr<Pinned> pinned;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VpsClient)
};
