/*
    pt.Live - Live Singing DAW
    LicenseClient - key activation + offline token check against the ptlicense server (server/ptlicense.py).

    How it works (same rules as ptlicense.local_state()):
      - The user types a key (PTL-XXXXX-XXXXX-XXXXX-XXXXX) or a public trial code (e.g. 033.247.3333).
      - POST /api/license/activate  ->  the server answers with a TOKEN signed with Ed25519.
      - The token is verified here with the server's PUBLIC key (kPublicKeyBase64), then stored encrypted with
        Windows DPAPI in %APPDATA%\ptLive\license.dat, together with the last local time the app was seen.
      - Every start: checkLocal() decides WITHOUT network:
            wrong machine -> invalid | past expires_at -> expired | clock moved back > 5 min -> needOnline
            | past offline_until (7 days) -> needOnline | else ok
      - A token older than 24 h is refreshed in the background (POST /api/license/refresh).

    The license endpoints are sent WITHOUT the shared X-API-Key (VpsClient::postJson, sendApiKey = false).
    Every online call is blocking: call them from a worker thread, never from the message thread.
    checkLocal() does no network and is cheap: it may run on the message thread at start-up.

    Build requirements (see CMakeLists.txt):
      - third_party/tweetnacl.c + tweetnacl.h (public domain, https://tweetnacl.cr.yp.to/). Without them the
        project still builds, but no token can be verified, so no key can be activated (fails closed).
      - kPublicKeyBase64 below = output of  `python3 ptlicense.py init`.
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include "include/Net/VpsClient.h"

namespace ptl::license
{
    // Ed25519 PUBLIC key of the license server, base64 (32 bytes). Paste the line printed by
    //   python3 ptlicense.py init        ("Public key (base64, 32 byte) - dan vao client")
    inline constexpr const char* kPublicKeyBase64 = "WKJYi5V72Qt03f0FzK2hvrESWRN/WK/JDhpn9nHuxZM=";

    // Public trial code shown in the dialog (also printed next to the logo in the title bar).
    inline constexpr const char* kTrialCode = "033.247.3333";
}

class LicenseClient
{
public:
    // What the application has to do at start-up.
    enum class Gate
    {
        ok,            // run
        needKey,       // nothing stored: ask for a key
        needOnline,    // stored token needs the server (offline window over, or the clock moved back)
        expired,       // stored token is past its end date
        invalid,       // stored token belongs to another machine / is damaged
        revoked        // the server revoked this license (only used to pick the dialog text, never returned by checkLocal)
    };

    struct Info
    {
        juce::String plan;                       // trial | m1 | m6 | y1 | custom
        juce::int64  issuedAt = 0, expiresAt = 0, offlineUntil = 0;     // epoch seconds
    };

    struct Outcome
    {
        bool ok = false;
        juce::String code;                       // "" when ok; else server error code or offline / bad_signature / no_crypto ...
        juce::String message;                    // text for the user (Vietnamese)
        Info info;                               // filled when ok
    };

    explicit LicenseClient (VpsClient& vpsClient);
    ~LicenseClient();

    //== Start-up gate (no network) ===========================================
    Gate checkLocal (Info* info = nullptr);

    // When the last checkLocal() answered needOnline BECAUSE the Windows clock is earlier than the last time the
    // app was seen (by more than 5 minutes): how many seconds it went back. 0 = that was not the reason (e.g. the
    // offline window is simply over). A weak CMOS battery or a broken time sync causes this; the dialog names it.
    juce::int64 getClockBackSeconds() const noexcept   { return clockBackSeconds.load(); }
    // "1 giờ 20 phút" / "35 phút" / "2 ngày": the amount above, for the dialog text.
    static juce::String describeDuration (juce::int64 seconds);

    // Token older than 24 h and still valid: ask the server for a fresh one on a background thread.
    void refreshInBackground (const Info& current);

    // Blocking refresh used at start-up so revoke / extend made on the web take effect on the next app open.
    // Returns true if the token is still valid after the server answer; false if revoked / expired / removed
    // (storage is cleared in that case). Offline returns false but keeps the stored token (offline window still rules).
    bool refreshAndWait (std::atomic<bool>* externalCancel = nullptr);   // externalCancel: set by the app on quit
    // Server verdict of the last refreshAndWait() ("revoked", "expired", "device_removed", "offline", "ok"...).
    juce::String getLastRefreshCode();
    // The dialog text to show when refreshAndWait() returned false.
    Gate gateAfterFailedRefresh();

    //== Online (blocking, worker thread) =====================================
    Outcome activate   (const juce::String& keyOrTrialCode, std::atomic<bool>& cancel);
    // Key-less activation: the admin approved this machine's hwid on the VPS web page, the user only presses
    // "Kích hoạt". POST /api/license/claim {hwid, hw_label, app_version}; the answer has the SAME shape as activate
    // (a signed token). Not approved yet -> error code "not_approved" (the server should list the hwid as pending).
    Outcome claim      (std::atomic<bool>& cancel);
    // User pressed "Gửi yêu cầu" from the dialog: send hwid + phone number to the VPS so the admin can contact
    // and approve. POST /api/license/request {hwid, phone, hw_label, app_version}; the answer is {ok:true}.
    // Anti-spam (enforced HERE, so every caller is covered, and persisted so restarting the app does not reset it):
    //   - at least kRequestCooldownSec between two requests (kFailedCooldownSec after a failed attempt),
    //   - at most kRequestMaxPerHour requests per rolling hour.
    // A blocked call returns code "rate_limited" at once, WITHOUT touching the network.
    Outcome requestActivation (const juce::String& phone, std::atomic<bool>& cancel);
    static constexpr int kRequestCooldownSec = 60, kFailedCooldownSec = 15, kRequestMaxPerHour = 3;
    // 0 = a request may be sent now; otherwise the seconds still to wait. Local only (tiny file read).
    int requestWaitSeconds();
    static juce::String waitText (int seconds);        // "45 giây" / "3 phút"
    Outcome refresh    (std::atomic<bool>& cancel);
    Outcome deactivate (std::atomic<bool>& cancel);       // user removes THIS machine (uses one of the key's resets)

    // The license token currently stored (the same one checkLocal() verifies), or "" when this machine is not
    // activated. Thread-safe and cheap: kept in memory, refreshed by every save / clear of license.dat, so the
    // network code can call it for every request without touching DPAPI. VpsClient sends it as X-License-Token.
    juce::String currentToken();

    //== Helpers ==============================================================
    const juce::String& getHwid() const noexcept { return hwid; }
    static juce::String toString (Gate g);
    static juce::String planLabel (const Info& info);      // "Dùng thử" / "1 tháng" / "6 tháng" / "1 năm" / "Tùy chỉnh"
    static juce::String describe (const Info& info);       // "Gói 1 tháng · hết hạn 07/11/2026"
    static juce::String messageFor (const juce::String& code);

private:
    struct Stored { juce::String token; juce::int64 lastSeen = 0; };

    std::optional<Stored> loadStored();
    void saveStored (const Stored& s);
    void clearStored();

    Outcome acceptReply (const juce::var& reply);
    Outcome failure (const VpsClient::Response& r);
    // true + `reply` = the server answered 2xx with a JSON object; false + `failed` = anything else.
    bool post (const char* path, const juce::var& body, std::atomic<bool>& cancel, juce::var& reply, Outcome& failed);
    static juce::String cryptoProblem();                   // "" when verification is possible

    class BackgroundRefresh;
    struct InFlight;

    juce::File requestLogFile() const;
    std::vector<juce::int64> loadRequestTimes();
    void recordRequest();

    VpsClient& vps;
    juce::String hwid;
    juce::CriticalSection storeLock;                       // load-modify-save of license.dat (also guards cachedToken)
    juce::String cachedToken;                              // in-memory copy of the stored token, see currentToken()
    juce::CriticalSection requestLock;                     // request log + last verdict
    juce::String lastRefreshCode;
    juce::int64 lastAttemptSec = 0;                        // last activation request attempt (success or not)
    std::atomic<int> inFlight { 0 };                       // network calls running right now (see post())
    std::atomic<bool> shuttingDown { false };
    std::atomic<juce::int64> clockBackSeconds { 0 };       // see getClockBackSeconds()
    std::unique_ptr<BackgroundRefresh> background;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseClient)
};
