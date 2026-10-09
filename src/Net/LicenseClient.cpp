/*
    pt.Live - Live Singing DAW
    LicenseClient implementation (see include/Net/LicenseClient.h).
*/

#include "include/Net/LicenseClient.h"
#include "include/Audio/PtLog.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <wincrypt.h>
 #include <dpapi.h>
 #pragma comment (lib, "crypt32.lib")
 #pragma comment (lib, "advapi32.lib")
#endif

#if JUCE_WINDOWS && ! defined (RRF_SUBKEY_WOW64_64KEY)
 #define RRF_SUBKEY_WOW64_64KEY 0x00010000      // not declared by winreg.h for this _WIN32_WINNT: same value as the SDK
#endif

#ifndef PTLIVE_HAS_NACL
 #define PTLIVE_HAS_NACL 0          // set to 1 by CMake when third_party/tweetnacl.c exists
#endif

#if PTLIVE_HAS_NACL
 extern "C"
 {
  #include "third_party/tweetnacl.h"
 }

 // tweetnacl references randombytes() for key generation / box functions. pt.Live only VERIFIES
 // signatures (crypto_sign_open), so this is never used for anything secret; it exists for the linker.
 extern "C" void randombytes (unsigned char* x, unsigned long long n)
 {
     juce::Random::getSystemRandom().fillBitsRandomly (x, (size_t) n);
 }
#endif

#define VI(text) juce::String::fromUTF8 (text)

namespace
{
    constexpr juce::int64 kDay          = 86400;
    constexpr juce::int64 kClockSlack   = 300;       // = CLOCK_SLACK in ptlicense.py
    constexpr juce::int64 kRefreshAfter = kDay;      // refresh a token older than this

    juce::int64 nowSec() { return juce::Time::currentTimeMillis() / 1000; }

    //== SHA-256 (self-contained: juce::SHA256 lives in juce_cryptography, which pt.Live does not link) ==
    std::string sha256Hex (const std::string& data)
    {
        static const std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

        std::uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };

        std::string msg = data;
        const std::uint64_t bitLen = (std::uint64_t) data.size() * 8u;
        msg.push_back ((char) 0x80);
        while (msg.size() % 64 != 56)
            msg.push_back ((char) 0);
        for (int i = 7; i >= 0; --i)
            msg.push_back ((char) ((bitLen >> (8 * i)) & 0xff));

        auto rotr = [] (std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

        for (size_t off = 0; off < msg.size(); off += 64)
        {
            std::uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4]     << 24)
                     | ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 1] << 16)
                     | ((std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 2] << 8)
                     |  (std::uint32_t) (unsigned char) msg[off + (size_t) i * 4 + 3];
            for (int i = 16; i < 64; ++i)
            {
                const auto s0 = rotr (w[i - 15], 7)  ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
                const auto s1 = rotr (w[i - 2],  17) ^ rotr (w[i - 2],  19) ^ (w[i - 2]  >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                const auto S1  = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
                const auto ch  = (e & f) ^ (~e & g);
                const auto t1  = hh + S1 + ch + k[i] + w[i];
                const auto S0  = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
                const auto maj = (a & b) ^ (a & c) ^ (b & c);
                const auto t2  = S0 + maj;
                hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        static const char* hex = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i)
            for (int sh = 28; sh >= 0; sh -= 4)
                out.push_back (hex[(h[i] >> sh) & 0xf]);
        return out;
    }

    //== base64 / base64url -> bytes ===========================================
    [[maybe_unused]] std::vector<unsigned char> decodeBase64 (juce::String s)
    {
        s = s.trim().replaceCharacter ('-', '+').replaceCharacter ('_', '/');       // url-safe -> standard
        while (s.length() % 4 != 0)
            s += "=";

        juce::MemoryOutputStream out;
        if (s.isEmpty() || ! juce::Base64::convertFromBase64 (out, s))
            return {};

        const auto* p = static_cast<const unsigned char*> (out.getData());
        return std::vector<unsigned char> (p, p + out.getDataSize());
    }

    //== Token ==================================================================
    struct Payload
    {
        int version = 0;
        juce::String hwid, plan;
        juce::int64 issuedAt = 0, expiresAt = 0, offlineUntil = 0;
    };

    // token = base64url(JSON) "." base64url(Ed25519 signature of those JSON bytes)   (Signer.sign in ptlicense.py)
    std::optional<Payload> verifyToken (const juce::String& token)
    {
       #if PTLIVE_HAS_NACL
        const int dot = token.indexOfChar ('.');
        if (dot <= 0 || dot >= token.length() - 1)
            return std::nullopt;

        const auto data = decodeBase64 (token.substring (0, dot));
        const auto sig  = decodeBase64 (token.substring (dot + 1));
        const auto pub  = decodeBase64 (ptl::license::kPublicKeyBase64);

        if (data.empty() || sig.size() != 64 || pub.size() != 32)
            return std::nullopt;

        // tweetnacl wants the "signed message" = signature (64 bytes) followed by the message.
        std::vector<unsigned char> sm (64 + data.size()), m (sm.size());
        std::memcpy (sm.data(), sig.data(), 64);
        std::memcpy (sm.data() + 64, data.data(), data.size());

        unsigned long long mlen = 0;
        if (crypto_sign_open (m.data(), &mlen, sm.data(), (unsigned long long) sm.size(), pub.data()) != 0)
            return std::nullopt;

        const auto json = juce::JSON::parse (juce::String::fromUTF8 (reinterpret_cast<const char*> (m.data()), (int) mlen));
        if (! json.isObject())
            return std::nullopt;

        Payload p;
        p.version      = (int) json.getProperty ("v", 0);
        p.hwid         = json.getProperty ("hwid", juce::var()).toString();
        p.plan         = json.getProperty ("plan", juce::var()).toString();
        p.issuedAt     = (juce::int64) json.getProperty ("issued_at", 0);
        p.expiresAt    = (juce::int64) json.getProperty ("expires_at", 0);
        p.offlineUntil = (juce::int64) json.getProperty ("offline_until", 0);

        if (p.version != 1 || p.hwid.isEmpty() || p.expiresAt <= 0)
            return std::nullopt;

        return p;
       #else
        juce::ignoreUnused (token);
        return std::nullopt;
       #endif
    }

    // Same order as local_state() in ptlicense.py.
    LicenseClient::Gate decide (const Payload& p, const juce::String& myHwid, juce::int64 now, juce::int64 lastSeen)
    {
        if (p.hwid != myHwid)                    return LicenseClient::Gate::invalid;
        if (now >= p.expiresAt)                  return LicenseClient::Gate::expired;
        if (now < lastSeen - kClockSlack)        return LicenseClient::Gate::needOnline;     // clock moved back
        if (now >= p.offlineUntil)               return LicenseClient::Gate::needOnline;     // offline window over
        return LicenseClient::Gate::ok;
    }

    LicenseClient::Info toInfo (const Payload& p)
    {
        LicenseClient::Info i;
        i.plan = p.plan;
        i.issuedAt = p.issuedAt;
        i.expiresAt = p.expiresAt;
        i.offlineUntil = p.offlineUntil;
        return i;
    }

    //== Machine id =============================================================
    juce::File licenseDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ptLive");
    }

    // hwid = SHA-256 ("ptlive|" + lower-case MachineGuid), 64 hex chars (ptlicense._check_hwid).
    // If the registry value cannot be read, a random id is created once and kept in install.id.
    juce::String computeHwid()
    {
        juce::String raw;

       #if JUCE_WINDOWS
        wchar_t buf[128] = {};
        DWORD size = (DWORD) sizeof (buf);
        if (RegGetValueW (HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                          RRF_RT_REG_SZ | RRF_SUBKEY_WOW64_64KEY, nullptr, buf, &size) == ERROR_SUCCESS)
            raw = juce::String (buf).trim();
       #else
        raw = juce::SystemStats::getUniqueDeviceID();
       #endif

        if (raw.isEmpty())
        {
            const auto f = licenseDir().getChildFile ("install.id");
            raw = f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();

            if (raw.isEmpty())
            {
                raw = juce::Uuid().toString();
                f.getParentDirectory().createDirectory();
                f.replaceWithText (raw);
            }
        }

        return juce::String (sha256Hex (("ptlive|" + raw.toLowerCase()).toStdString()));
    }

    //== Storage: license.dat = DPAPI (current Windows user) ====================
    juce::File storeFile() { return licenseDir().getChildFile ("license.dat"); }

    bool protectBytes (const std::string& plain, juce::MemoryBlock& out, bool encrypt)
    {
       #if JUCE_WINDOWS
        static BYTE entropyBytes[] = "ptLive-license-v1";
        DATA_BLOB entropy { (DWORD) (sizeof (entropyBytes) - 1), entropyBytes };
        DATA_BLOB in  { (DWORD) plain.size(), (BYTE*) plain.data() };
        DATA_BLOB res {};

        const BOOL ok = encrypt ? CryptProtectData (&in, L"pt.Live license", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res)
                                : CryptUnprotectData (&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res);
        if (! ok)
            return false;

        out.replaceAll (res.pbData, res.cbData);
        SecureZeroMemory (res.pbData, res.cbData);
        LocalFree (res.pbData);
        return true;
       #else
        juce::ignoreUnused (encrypt);
        out.replaceAll (plain.data(), plain.size());
        return true;
       #endif
    }
}

//==============================================================================
// Background refresh: one short-lived thread, joined in ~LicenseClient.
class LicenseClient::BackgroundRefresh : public juce::Thread
{
public:
    explicit BackgroundRefresh (LicenseClient& c) : juce::Thread ("ptLive license refresh"), owner (c) {}

    void run() override
    {
        outcome = owner.refresh (cancel);
        ptl::diag::write ("[license] background refresh: " + (outcome.ok ? juce::String ("ok") : outcome.code));
    }

    std::atomic<bool> cancel { false };
    Outcome outcome;                              // filled by run(); read after the thread has been joined

private:
    LicenseClient& owner;
};

//==============================================================================
LicenseClient::LicenseClient (VpsClient& vpsClient) : vps (vpsClient), hwid (computeHwid()) {}

LicenseClient::~LicenseClient()
{
    // A detached dialog job may still be inside a request: new calls are refused, running ones end by their
    // own timeout (a few seconds). Waiting here keeps `vps` / `this` alive for them.
    shuttingDown = true;
    for (int i = 0; i < 600 && inFlight.load() > 0; ++i)
        juce::Thread::sleep (20);

    if (background != nullptr)
    {
        background->cancel = true;
        background->stopThread (15000);
    }
}

juce::String LicenseClient::toString (Gate g)
{
    switch (g)
    {
        case Gate::ok:         return "ok";
        case Gate::needKey:    return "needKey";
        case Gate::needOnline: return "needOnline";
        case Gate::expired:    return "expired";
        case Gate::invalid:    return "invalid";
        case Gate::revoked:    return "revoked";
    }
    return "?";
}

juce::String LicenseClient::planLabel (const Info& i)
{
    if (i.plan == "trial")  return VI ("Dùng thử");
    if (i.plan == "m1")     return VI ("1 tháng");
    if (i.plan == "m6")     return VI ("6 tháng");
    if (i.plan == "y1")     return VI ("1 năm");
    return VI ("Tùy chỉnh");
}

juce::String LicenseClient::describeDuration (juce::int64 seconds)
{
    if (seconds < 90 * 60)                                          // up to 1.5 h: minutes
        return juce::String (juce::jmax ((juce::int64) 1, (seconds + 30) / 60)) + VI (" phút");

    if (seconds < 48 * 3600)                                        // up to 2 days: hours + minutes
    {
        const auto h = seconds / 3600, m = (seconds % 3600) / 60;
        return juce::String (h) + VI (" giờ") + (m > 0 ? " " + juce::String (m) + VI (" phút") : juce::String());
    }

    return juce::String ((seconds + 43200) / 86400) + VI (" ngày");
}

juce::String LicenseClient::describe (const Info& i)
{
    return VI ("Gói ") + planLabel (i) + VI (" · hết hạn ") + juce::Time (i.expiresAt * 1000).formatted ("%d/%m/%Y");
}

juce::String LicenseClient::messageFor (const juce::String& code)
{
    if (code == "offline")                  return VI ("Không kết nối được máy chủ. Hãy kiểm tra Internet rồi thử lại.");
    if (code == "invalid_key")              return VI ("Key không đúng. Hãy kiểm tra lại rồi nhập lại.");
    if (code == "expired")                  return VI ("Key đã hết hạn.");
    if (code == "revoked")                  return VI ("Key đã bị thu hồi. Vui lòng liên hệ nhà cung cấp.");
    if (code == "activation_window_closed") return VI ("Key đã quá hạn kích hoạt.");
    if (code == "device_limit")             return VI ("Key này đã dùng hết số máy cho phép.");
    if (code == "trial_used")               return VI ("Máy này đã dùng thử trước đó.");
    if (code == "trial_closed")             return VI ("Đã ngừng cấp dùng thử. Vui lòng liên hệ nhà cung cấp.");
    if (code == "trial_limit")              return VI ("Mạng này đã nhận quá nhiều gói dùng thử. Vui lòng liên hệ nhà cung cấp.");
    if (code == "device_removed" || code == "bad_token")
                                            return VI ("Máy này đã bị gỡ khỏi key. Hãy nhập lại key để kích hoạt.");
    if (code == "no_resets_left")           return VI ("Đã hết lượt tự gỡ máy. Vui lòng liên hệ nhà cung cấp.");
    if (code == "bad_request")              return VI ("Yêu cầu không hợp lệ. Hãy kiểm tra lại key.");
    if (code == "empty_key")                return VI ("Hãy nhập key hoặc mã dùng thử.");
    if (code == "not_approved")             return VI ("Máy này chưa được kích hoạt. Hãy gửi Mã máy cho nhà cung cấp, đợi xác nhận rồi bấm Kích hoạt lại.");
    if (code == "rate_limited")             return VI ("Bạn đã gửi yêu cầu quá nhiều lần. Vui lòng đợi một lúc rồi thử lại.");
    if (code == "request_sent")             return VI ("Đã gửi yêu cầu kích hoạt. Nhà cung cấp sẽ liên hệ với bạn để xác nhận, sau đó bấm Kích hoạt.");
    if (code == "http_404")                 return VI ("Máy chủ chưa hỗ trợ kích hoạt bằng mã máy. Hãy nhập key hoặc liên hệ nhà cung cấp.");
    if (code == "bad_signature")            return VI ("Phản hồi của máy chủ không hợp lệ (sai chữ ký).");
    if (code == "no_crypto")                return VI ("Bản build này chưa có third_party/tweetnacl.c nên không kiểm tra được chữ ký.");
    if (code == "no_public_key")            return VI ("Chưa gắn public key của máy chủ vào LicenseClient.h.");
    if (code.startsWith ("http_5"))         return VI ("Máy chủ đang bận. Hãy thử lại sau ít phút.");
    return VI ("Lỗi không xác định (") + code + ")";
}

//== Storage ====================================================================
std::optional<LicenseClient::Stored> LicenseClient::loadStored()
{
    const juce::ScopedLock sl (storeLock);

    const auto f = storeFile();
    juce::MemoryBlock enc;
    if (! f.existsAsFile() || ! f.loadFileAsData (enc) || enc.isEmpty())
        return std::nullopt;

    juce::MemoryBlock plain;
    if (! protectBytes (std::string (static_cast<const char*> (enc.getData()), enc.getSize()), plain, false))
        return std::nullopt;                               // other Windows user / damaged file

    const auto json = juce::JSON::parse (juce::String::fromUTF8 (static_cast<const char*> (plain.getData()), (int) plain.getSize()));
    if (! json.isObject())
        return std::nullopt;

    Stored s;
    s.token    = json.getProperty ("token", juce::var()).toString();
    s.lastSeen = (juce::int64) json.getProperty ("last_seen", 0);
    if (s.token.isEmpty())
        return std::nullopt;
    cachedToken = s.token;
    return s;
}

juce::String LicenseClient::currentToken()
{
    const juce::ScopedLock sl (storeLock);
    if (cachedToken.isEmpty())
        loadStored();                                      // fills cachedToken; cheap when license.dat does not exist
    return cachedToken;
}

void LicenseClient::saveStored (const Stored& s)
{
    const juce::ScopedLock sl (storeLock);
    cachedToken = s.token;                                 // new token (activate / refresh) is used by the next request

    auto* o = new juce::DynamicObject();
    o->setProperty ("token", s.token);
    o->setProperty ("last_seen", s.lastSeen);

    juce::MemoryBlock enc;
    if (! protectBytes (juce::JSON::toString (juce::var (o), true).toStdString(), enc, true))
    {
        ptl::diag::write ("[license] could not encrypt license.dat");
        return;
    }

    const auto f = storeFile();
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithData (enc.getData(), enc.getSize()))          // temp file + rename: never half-written
        ptl::diag::write ("[license] could not write license.dat");
}

void LicenseClient::clearStored()
{
    const juce::ScopedLock sl (storeLock);
    cachedToken = {};
    storeFile().deleteFile();
}

//== Start-up gate ================================================================
LicenseClient::Gate LicenseClient::checkLocal (Info* info)
{
    const auto stored = loadStored();
    if (! stored.has_value())
        return Gate::needKey;

    const auto payload = verifyToken (stored->token);
    if (! payload.has_value())                             // bad signature / other key / no tweetnacl
    {
        ptl::diag::write ("[license] stored token rejected (signature)");
        clearStored();
        return Gate::needKey;
    }

    if (info != nullptr)
        *info = toInfo (*payload);

    const auto now  = nowSec();
    const auto gate = decide (*payload, hwid, now, stored->lastSeen);

    // Same condition as in decide(), kept apart so the dialog can say WHY an online check is needed.
    const bool clockBack = (gate == Gate::needOnline) && (now < stored->lastSeen - kClockSlack);
    clockBackSeconds = clockBack ? (stored->lastSeen - now) : 0;

    if (clockBack)
        ptl::diag::write ("[license] blocked: the clock moved back by " + juce::String (stored->lastSeen - now) + " s"
                          " (last seen " + juce::Time (stored->lastSeen * 1000).toString (true, true)
                          + ", now " + juce::Time (now * 1000).toString (true, true) + ")");

    if (gate == Gate::ok)
    {
        if (now - stored->lastSeen > 60)                   // remember the latest local time seen (clock-rollback check)
            saveStored ({ stored->token, juce::jmax (stored->lastSeen, now) });
    }
    else if (gate == Gate::expired || gate == Gate::invalid)
        clearStored();

    return gate;
}

// Refresh the stored token in the background (non-blocking). If revoked/expired the app will notice at next start-up.
// To make revoke/extend take effect immediately, you must call refreshAndWait() instead at startup.
void LicenseClient::refreshInBackground (const Info& current)
{
    // Removed the "token older than 24h" gate here — that's only relevant for how often we *need* to sync.
    // We don't wait anyway, so revokes may not be seen until next restart / manual checkLocal().
    if (background != nullptr && background->isThreadRunning())
        return;

    background = std::make_unique<BackgroundRefresh> (*this);
    background->startThread();
}

// Blocking refresh at startup: waits for server answer. If revoked/expired then the caller should close or show dialog.
// Returns true if token is still valid after refresh, false otherwise (caller should handle revoke/expired).
// Runs synchronously on the CALLING thread — do NOT call this from the message thread (it does a blocking HTTP request);
// checkLicenseThen() runs it off the message thread before the main window is created.
bool LicenseClient::refreshAndWait (std::atomic<bool>* externalCancel)
{
    const auto stored = loadStored();
    if (! stored.has_value())
        return false;

    if (const auto problem = cryptoProblem(); problem.isNotEmpty())
    {
        ptl::diag::write ("[license] refresh blocked by " + problem);
        return false;
    }

    std::atomic<bool> ownCancel { false };
    auto& cancel = externalCancel != nullptr ? *externalCancel : ownCancel;
    const auto o = refresh (cancel);              // blocking; clears storage itself on expired / revoked / removed

    ptl::diag::write ("[license] refreshAndWait: " + (o.ok ? juce::String ("ok") : o.code));

    {
        const juce::ScopedLock sl (requestLock);
        lastRefreshCode = o.ok ? juce::String ("ok") : o.code;
    }

    if (o.ok)
        return true;

    // Offline or server-down: the cached token (still inside its offline window by the local check) keeps working.
    if (o.code == "offline" || o.code.startsWith ("http_"))
        return true;

    return false;
}

juce::String LicenseClient::getLastRefreshCode()
{
    const juce::ScopedLock sl (requestLock);
    return lastRefreshCode;
}

LicenseClient::Gate LicenseClient::gateAfterFailedRefresh()
{
    const auto code = getLastRefreshCode();

    if (code == "revoked")  return Gate::revoked;
    if (code == "expired")  return Gate::expired;
    return Gate::needKey;
}

//== Activation request anti-spam ====================================================
juce::File LicenseClient::requestLogFile() const
{
    return licenseDir().getChildFile ("request.log");
}

// Epoch seconds of the requests sent in the last 24 h (oldest first). A time in the future (clock set back to
// dodge the limit) is clamped to "now", so changing the clock can only make the wait longer, never shorter.
std::vector<juce::int64> LicenseClient::loadRequestTimes()
{
    std::vector<juce::int64> out;
    const auto now = nowSec();

    juce::StringArray lines;
    lines.addLines (requestLogFile().loadFileAsString());

    for (const auto& line : lines)
    {
        const auto t = line.trim().getLargeIntValue();
        if (t > 0 && now - t < kDay)
            out.push_back (juce::jmin (t, now));
    }

    std::sort (out.begin(), out.end());
    return out;
}

void LicenseClient::recordRequest()
{
    const juce::ScopedLock sl (requestLock);

    auto times = loadRequestTimes();
    times.push_back (nowSec());

    if (times.size() > 20)
        times.erase (times.begin(), times.end() - 20);

    juce::String text;
    for (auto t : times)
        text << juce::String (t) << "\n";

    requestLogFile().getParentDirectory().createDirectory();
    requestLogFile().replaceWithText (text);
}

int LicenseClient::requestWaitSeconds()
{
    const juce::ScopedLock sl (requestLock);

    const auto now = nowSec();
    const auto times = loadRequestTimes();
    juce::int64 wait = 0;

    if (! times.empty())
        wait = juce::jmax (wait, times.back() + kRequestCooldownSec - now);

    // Rolling hour: once kRequestMaxPerHour requests are inside it, the next one is allowed when the oldest of
    // the last kRequestMaxPerHour leaves the window.
    std::vector<juce::int64> inHour;
    for (auto t : times)
        if (now - t < 3600)
            inHour.push_back (t);

    if ((int) inHour.size() >= kRequestMaxPerHour)
        wait = juce::jmax (wait, inHour[inHour.size() - (size_t) kRequestMaxPerHour] + 3600 - now);

    // A failed attempt (no Internet, server busy) is not stored, but it must not be hammered either.
    if (lastAttemptSec > 0)
        wait = juce::jmax (wait, juce::jmin (lastAttemptSec, now) + kFailedCooldownSec - now);

    return (int) juce::jmax ((juce::int64) 0, wait);
}

juce::String LicenseClient::waitText (int seconds)
{
    if (seconds < 90)
        return juce::String (seconds) + VI (" giây");

    return juce::String ((seconds + 59) / 60) + VI (" phút");
}

//== Online ========================================================================
juce::String LicenseClient::cryptoProblem()
{
   #if PTLIVE_HAS_NACL
    if (decodeBase64 (ptl::license::kPublicKeyBase64).size() != 32)
        return "no_public_key";
    return {};
   #else
    return "no_crypto";
   #endif
}

LicenseClient::Outcome LicenseClient::failure (const VpsClient::Response& r)
{
    Outcome o;
    if (r.status == 0)
        o.code = "offline";
    else if (r.status == 429)
        o.code = "rate_limited";                            // the server throttles too
    else
    {
        o.code = juce::JSON::parse (r.body).getProperty ("error", juce::var()).toString();
        if (o.code.isEmpty())
            o.code = "http_" + juce::String (r.status);
    }
    o.message = messageFor (o.code);
    return o;
}

struct LicenseClient::InFlight
{
    explicit InFlight (std::atomic<int>& c) : counter (c) { ++counter; }
    ~InFlight() { --counter; }
    std::atomic<int>& counter;
};

bool LicenseClient::post (const char* path, const juce::var& body, std::atomic<bool>& cancel,
                          juce::var& reply, Outcome& failed)
{
    InFlight guard (inFlight);

    if (shuttingDown.load())
    {
        failed.code = "offline";
        failed.message = messageFor (failed.code);
        return false;
    }

    const auto r = vps.postJson (path, body, false, cancel);

    if (r.status >= 200 && r.status < 300)
    {
        reply = juce::JSON::parse (r.body);
        if (reply.isObject())
            return true;

        failed.code = "bad_signature";
        failed.message = messageFor (failed.code);
        return false;
    }

    failed = failure (r);
    return false;
}

// Verifies the token of an activate / refresh answer and stores it.
LicenseClient::Outcome LicenseClient::acceptReply (const juce::var& reply)
{
    Outcome o;
    const auto token = reply.getProperty ("token", juce::var()).toString();
    const auto payload = verifyToken (token);

    if (! payload.has_value() || payload->hwid != hwid)
    {
        o.code = "bad_signature";
        o.message = messageFor (o.code);
        ptl::diag::write ("[license] token from the server was rejected");
        return o;
    }

    // last_seen = latest LOCAL time seen; an online answer proves the clock, so it may reset it.
    saveStored ({ token, nowSec() });

    o.ok = true;
    o.info = toInfo (*payload);
    return o;
}

LicenseClient::Outcome LicenseClient::activate (const juce::String& keyOrTrialCode, std::atomic<bool>& cancel)
{
    Outcome o;
    const auto key = keyOrTrialCode.trim();

    if (key.isEmpty())
    {
        o.code = "empty_key";
        o.message = messageFor (o.code);
        return o;
    }

    if (const auto problem = cryptoProblem(); problem.isNotEmpty())
    {
        o.code = problem;
        o.message = messageFor (problem);
        return o;
    }

    auto* b = new juce::DynamicObject();
    b->setProperty ("key", key);
    b->setProperty ("hwid", hwid);
    b->setProperty ("hw_label", juce::SystemStats::getComputerName());
    b->setProperty ("app_version", juce::JUCEApplicationBase::getInstance() != nullptr
                                       ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String());

    juce::var reply;
    if (! post ("/api/license/activate", juce::var (b), cancel, reply, o))
    {
        ptl::diag::write ("[license] activate failed: " + o.code);
        return o;
    }

    o = acceptReply (reply);
    ptl::diag::write ("[license] activate: " + (o.ok ? juce::String ("ok") : o.code));
    return o;
}

LicenseClient::Outcome LicenseClient::claim (std::atomic<bool>& cancel)
{
    Outcome o;

    if (const auto problem = cryptoProblem(); problem.isNotEmpty())
    {
        o.code = problem;
        o.message = messageFor (problem);
        return o;
    }

    auto* b = new juce::DynamicObject();
    b->setProperty ("hwid", hwid);
    b->setProperty ("hw_label", juce::SystemStats::getComputerName());
    b->setProperty ("app_version", juce::JUCEApplicationBase::getInstance() != nullptr
                                       ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String());

    juce::var reply;
    if (! post ("/api/license/claim", juce::var (b), cancel, reply, o))
    {
        ptl::diag::write ("[license] claim failed: " + o.code);
        return o;
    }

    o = acceptReply (reply);                       // verifies the Ed25519 signature + hwid, then stores the token
    ptl::diag::write ("[license] claim: " + (o.ok ? juce::String ("ok") : o.code));
    return o;
}

LicenseClient::Outcome LicenseClient::refresh (std::atomic<bool>& cancel)
{
    Outcome o;
    const auto stored = loadStored();

    if (! stored.has_value())
    {
        o.code = "bad_token";
        o.message = messageFor (o.code);
        return o;
    }

    if (const auto problem = cryptoProblem(); problem.isNotEmpty())
    {
        o.code = problem;
        o.message = messageFor (problem);
        return o;
    }

    auto* b = new juce::DynamicObject();
    b->setProperty ("token", stored->token);

    juce::var reply;
    if (! post ("/api/license/refresh", juce::var (b), cancel, reply, o))
    {
        // The server is the authority: a key that was revoked / expired / removed from this machine is dead.
        // (Offline or a busy server must NOT delete anything.)
        if (o.code == "expired" || o.code == "revoked" || o.code == "device_removed" || o.code == "bad_token")
            clearStored();

        return o;
    }

    return acceptReply (reply);
}

LicenseClient::Outcome LicenseClient::deactivate (std::atomic<bool>& cancel)
{
    Outcome o;
    const auto stored = loadStored();

    if (! stored.has_value())
    {
        o.ok = true;                                        // nothing to remove
        return o;
    }

    auto* b = new juce::DynamicObject();
    b->setProperty ("token", stored->token);

    juce::var reply;
    if (! post ("/api/license/deactivate", juce::var (b), cancel, reply, o))
        return o;

    clearStored();
    o.ok = true;
    return o;
}

LicenseClient::Outcome LicenseClient::requestActivation (const juce::String& phone, std::atomic<bool>& cancel)
{
    Outcome o;

    if (const auto problem = cryptoProblem(); problem.isNotEmpty())
    {
        o.code = problem;
        o.message = messageFor (problem);
        return o;
    }

    if (const int wait = requestWaitSeconds(); wait > 0)
    {
        o.code = "rate_limited";
        o.message = VI ("Bạn vừa gửi yêu cầu rồi. Vui lòng đợi ") + waitText (wait) + VI (" nữa rồi gửi lại.");
        ptl::diag::write ("[license] request blocked (anti-spam), wait " + juce::String (wait) + " s");
        return o;
    }

    {
        const juce::ScopedLock sl (requestLock);
        lastAttemptSec = nowSec();
    }

    auto* b = new juce::DynamicObject();
    b->setProperty ("hwid", hwid);
    b->setProperty ("phone", phone.trim());
    b->setProperty ("hw_label", juce::SystemStats::getComputerName());
    b->setProperty ("app_version", juce::JUCEApplicationBase::getInstance() != nullptr
                                       ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String());

    juce::var reply;
    if (! post ("/api/license/request", juce::var (b), cancel, reply, o))
    {
        ptl::diag::write ("[license] request failed: " + o.code);

        if (o.code == "rate_limited")                       // the server refused: lock the button here too
            recordRequest();

        return o;
    }

    recordRequest();

    o.ok = true;
    o.code = "request_sent";
    o.message = messageFor (o.code);
    ptl::diag::write ("[license] request sent (phone=" + (phone.trim().isEmpty() ? juce::String ("(none)") : phone.trim()) + ")");
    return o;
}
