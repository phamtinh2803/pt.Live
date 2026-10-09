/*
    pt.Live - Live Singing DAW
    VpsClient implementation (WinHTTP + certificate pinning).
*/

#include "include/Net/VpsClient.h"
#include "include/Audio/PtLog.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <winhttp.h>
 #include <wincrypt.h>
 #pragma comment (lib, "winhttp.lib")
 #pragma comment (lib, "crypt32.lib")
#endif

#if JUCE_WINDOWS
struct VpsClient::Pinned
{
    std::vector<PCCERT_CONTEXT> certs;
    ~Pinned() { for (auto* c : certs) CertFreeCertificateContext (c); }
};
#else
struct VpsClient::Pinned {};
#endif

namespace
{
    juce::File exeDir()
    {
        return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    }

    juce::File appDataDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ptLive");
    }

    juce::File findConfigFile (const char* name)
    {
        for (auto dir : { exeDir(), appDataDir() })
        {
            auto f = dir.getChildFile (name);
            if (f.existsAsFile()) return f;
        }
        return {};
    }
}

VpsClient::VpsClient()  { loadConfig(); }
VpsClient::~VpsClient() { closeConnection(); }

void VpsClient::closeConnection()
{
   #if JUCE_WINDOWS
    if (hConnect != nullptr) { WinHttpCloseHandle ((HINTERNET) hConnect); hConnect = nullptr; }
    if (hSession != nullptr) { WinHttpCloseHandle ((HINTERNET) hSession); hSession = nullptr; }
   #endif
}

void VpsClient::setTokenProvider (std::function<juce::String()> provider)
{
    const juce::ScopedLock sl (providerLock);
    tokenProvider = std::move (provider);
}

juce::String VpsClient::currentToken() const
{
    std::function<juce::String()> provider;
    {
        const juce::ScopedLock sl (providerLock);
        provider = tokenProvider;               // copy: the provider runs OUTSIDE the lock
    }
    if (! provider) return {};

    auto token = provider().trim();
    // A token is base64url + '.', nothing else. Anything different must never reach a header line (CR/LF injection).
    if (token.isNotEmpty() && ! token.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.="))
    {
        ptl::diag::write ("[vps] license token has unexpected characters - not sent");
        return {};
    }
    return token;
}

bool VpsClient::isConfigured() const
{
    return host.isNotEmpty() && (apiKey.isNotEmpty() || currentToken().isNotEmpty());
}

void VpsClient::warmUp()
{
    if (! isConfigured()) return;           // no key and no token yet: a request would only be a sure 401
    std::atomic<bool> never { false };
    request ("GET", "/api/tone-cache/check/ptlive-warmup", {}, 4000, never);      // the answer is ignored
}

void VpsClient::loadConfig()
{
    const auto cfg = findConfigFile ("vps_config.json");
    if (! cfg.existsAsFile())
    {
        ptl::diag::write ("[vps] vps_config.json not found (looked next to the exe and in %APPDATA%/ptLive)");
        return;
    }

    auto json = juce::JSON::parse (cfg.loadFileAsString());
    juce::String url = json.getProperty ("url", juce::var()).toString().trim();
    apiKey           = json.getProperty ("key", juce::var()).toString().trim();

    // A "key" in the file shipped to users is readable by anyone who opens it. The license token (X-License-Token)
    // is the credential for a release build: ship vps_config.json with only "url".
    if (apiKey.isNotEmpty())
        ptl::diag::write ("[vps] problem: vps_config.json holds a plain-text \"key\" - remove it from the released file (token mode works without it)");

    // Plain http would leak the key: force https, exactly like ptStudio does.
    if (url.startsWithIgnoreCase ("http://"))
        url = "https://" + url.substring (7);
    if (! url.startsWithIgnoreCase ("https://"))
        url = "https://" + url;

    juce::URL u (url);
    host   = u.getDomain();
    port   = u.getPort() > 0 ? u.getPort() : 443;
    secure = true;

    // (the token provider is not set yet at this point, so only url / key are judged here)
    if (host.isEmpty())
        ptl::diag::write ("[vps] vps_config.json is missing url");
    else if (apiKey.isEmpty())
        ptl::diag::write ("[vps] vps_config.json has no key - token mode (X-License-Token only)");

    if (auto pem = findConfigFile ("vps_cert.pem"); pem.existsAsFile())
        loadPinnedCerts (pem);
    else
        ptl::diag::write ("[vps] vps_cert.pem not found - using normal Windows certificate validation");
}

void VpsClient::loadPinnedCerts (const juce::File& pem)
{
   #if JUCE_WINDOWS
    pinned = std::make_unique<Pinned>();

    const juce::String text = pem.loadFileAsString();
    const juce::String begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";

    int pos = 0;
    while (true)
    {
        const int b = text.indexOf (pos, begin);
        if (b < 0) break;
        const int e = text.indexOf (b, end);
        if (e < 0) break;

        const auto block = text.substring (b, e + end.length()).toStdString();
        pos = e + end.length();

        DWORD derLen = 0;
        if (! CryptStringToBinaryA (block.c_str(), 0, CRYPT_STRING_BASE64HEADER, nullptr, &derLen, nullptr, nullptr))
            continue;

        std::vector<BYTE> der (derLen);
        if (! CryptStringToBinaryA (block.c_str(), 0, CRYPT_STRING_BASE64HEADER, der.data(), &derLen, nullptr, nullptr))
            continue;

        if (auto* ctx = CertCreateCertificateContext (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), derLen))
            pinned->certs.push_back (ctx);
    }

    if (pinned->certs.empty())
    {
        ptl::diag::write ("[vps] vps_cert.pem has no readable certificate - using normal Windows validation");
        pinned.reset();
    }
   #else
    juce::ignoreUnused (pem);
   #endif
}

std::optional<VpsClient::KeyResult> VpsClient::parseKey (const juce::var& v)
{
    if (! v.isObject()) return std::nullopt;

    KeyResult r;
    r.root    = v.getProperty ("root", juce::var()).toString().trim();
    r.scale   = v.getProperty ("scale", juce::var()).toString().trim();
    r.viet    = v.getProperty ("viet", juce::var()).toString().trim();
    r.western = v.getProperty ("western", juce::var()).toString().trim();

    // Some entries only carry "key_scale" / "western" ("C# Minor"): split it.
    if (r.root.isEmpty() || r.scale.isEmpty())
    {
        auto parts = juce::StringArray::fromTokens (r.western.isNotEmpty() ? r.western
                                                    : v.getProperty ("key_scale", juce::var()).toString(), " ", "");
        parts.removeEmptyStrings();
        if (parts.size() >= 2) { r.root = parts[0]; r.scale = parts[1]; }
    }

    if (! r.isValid()) return std::nullopt;
    return r;
}

//==============================================================================
VpsClient::Response VpsClient::request (const char* verb, const juce::String& path, const juce::String& jsonBody,
                                        int timeoutMs, std::atomic<bool>& cancel, bool retried, bool sendApiKey)
{
    Response out;

    // Credentials of this request. Read BEFORE netLock is taken (the provider locks the license store).
    // /api/license/* is authorised by the key / token inside its body: it gets neither header.
    const bool isLicensePath = path.startsWith ("/api/license/");
    const juce::String token = (sendApiKey && ! isLicensePath) ? currentToken() : juce::String();

    if (host.isEmpty() || cancel.load()) return out;
    if (sendApiKey && apiKey.isEmpty() && token.isEmpty()) return out;     // not configured: fail quietly, as before

   #if JUCE_WINDOWS
    struct Handle
    {
        HINTERNET h = nullptr;
        ~Handle() { if (h != nullptr) WinHttpCloseHandle (h); }
    };

    const juce::ScopedLock netGuard (netLock);

    // Diagnostics: say exactly where a request died (never logs the API key).
    const juce::String tag = "[vps] " + juce::String (verb) + " " + path.upToFirstOccurrenceOf ("?", false, false);
    auto fail = [&] (const char* where)
    {
        ptl::diag::write (tag + " FAILED at " + where + " (WinHTTP error " + juce::String ((int) GetLastError()) + ")");
        return out;
    };

    // Session + connection are created once and reused. DEFAULT_PROXY = the system's WinHTTP proxy setting
    // without WPAD auto-detection (which made every first request slow on some PCs).
    if (hSession == nullptr)
    {
        hSession = WinHttpOpen (L"ptLive/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession == nullptr) return fail ("WinHttpOpen");
    }

    if (hConnect == nullptr)
    {
        hConnect = WinHttpConnect ((HINTERNET) hSession, host.toWideCharPointer(), (INTERNET_PORT) port, 0);
        if (hConnect == nullptr) return fail ("WinHttpConnect");
    }

    Handle req;

    req.h = WinHttpOpenRequest ((HINTERNET) hConnect, juce::String (verb).toWideCharPointer(), path.toWideCharPointer(),
                                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req.h == nullptr) { closeConnection(); return fail ("WinHttpOpenRequest"); }

    WinHttpSetTimeouts (req.h, 4000, 4000, timeoutMs, timeoutMs);      // resolve, connect, send, receive

    if (pinned != nullptr)
    {
        // Pinned: skip the Windows CA / host name checks, the pin check below replaces them.
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption (req.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof (flags));
    }

    juce::String headers;
    if (sendApiKey && apiKey.isNotEmpty())
        headers << "X-API-Key: " << apiKey << "\r\n";
    if (token.isNotEmpty())
    {
        headers << "X-License-Token: " << token << "\r\n";
        if (! tokenModeLogged.exchange (true))        // once, never the token itself
            ptl::diag::write (juce::String ("[vps] requests carry X-License-Token (X-API-Key ")
                              + (apiKey.isNotEmpty() ? "also sent" : "not sent - token mode") + ")");
    }
    const auto bodyUtf8 = jsonBody.toStdString();
    if (jsonBody.isNotEmpty())
        headers << "Content-Type: application/json\r\n";

    if (! WinHttpSendRequest (req.h, headers.toWideCharPointer(), (DWORD) -1L,
                              jsonBody.isNotEmpty() ? (LPVOID) bodyUtf8.data() : WINHTTP_NO_REQUEST_DATA,
                              (DWORD) bodyUtf8.size(), (DWORD) bodyUtf8.size(), 0))
    {
        // The server may have closed the idle kept-alive connection: start over once with a fresh one.
        const auto err = (int) GetLastError();
        closeConnection();

        if (! retried && ! cancel.load())
        {
            ptl::diag::write (tag + " connection was closed (WinHTTP error " + juce::String (err) + ") - reconnecting");
            return request (verb, path, jsonBody, timeoutMs, cancel, true, sendApiKey);
        }
        return fail ("WinHttpSendRequest (cannot connect / TLS handshake - 12029=cannot connect, 12175=TLS error, 12002=timeout)");
    }

    // ---- certificate pin check (the handshake is done once the request was sent) ----
    if (pinned != nullptr)
    {
        PCCERT_CONTEXT server = nullptr;
        DWORD len = sizeof (server);
        bool trusted = false;

        if (WinHttpQueryOption (req.h, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &server, &len) && server != nullptr)
        {
            for (auto* pin : pinned->certs)
            {
                if (CertCompareCertificate (X509_ASN_ENCODING, server->pCertInfo, pin->pCertInfo)
                    || CryptVerifyCertificateSignatureEx (0, X509_ASN_ENCODING,
                                                          CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void*) server,
                                                          CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void*) pin, 0, nullptr))
                {
                    trusted = true;
                    break;
                }
            }
            CertFreeCertificateContext (server);
        }

        if (! trusted)
        {
            ptl::diag::write ("[vps] server certificate does not match vps_cert.pem - request refused");
            return out;
        }
    }

    if (cancel.load()) return out;
    if (! WinHttpReceiveResponse (req.h, nullptr))
    {
        const auto err = (int) GetLastError();
        closeConnection();
        if (! retried && ! cancel.load() && err != ERROR_WINHTTP_TIMEOUT)
            return request (verb, path, jsonBody, timeoutMs, cancel, true, sendApiKey);
        return fail ("WinHttpReceiveResponse");
    }

    DWORD status = 0, sz = sizeof (status);
    WinHttpQueryHeaders (req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = (int) status;

    std::string data;
    for (;;)
    {
        if (cancel.load()) { out.status = 0; return out; }

        DWORD avail = 0;
        if (! WinHttpQueryDataAvailable (req.h, &avail) || avail == 0) break;

        std::string chunk (avail, '\0');
        DWORD got = 0;
        if (! WinHttpReadData (req.h, chunk.data(), avail, &got) || got == 0) break;
        data.append (chunk.data(), got);

        if (data.size() > 1024 * 1024) break;       // answers are tiny; refuse anything silly
    }

    out.body = juce::String::fromUTF8 (data.data(), (int) data.size());
    // License answers carry a signed token: log the status only.
    if (path.startsWith ("/api/license/"))
        ptl::diag::write (tag + " -> HTTP " + juce::String (out.status));
    else
        ptl::diag::write (tag + " -> HTTP " + juce::String (out.status) + " | " + out.body.substring (0, 200).replaceCharacters ("\r\n", "  "));
    return out;
   #else
    juce::ignoreUnused (verb, path, jsonBody, timeoutMs);
    return out;
   #endif
}

//==============================================================================
VpsClient::Response VpsClient::postJson (const juce::String& path, const juce::var& body, bool sendApiKey,
                                         std::atomic<bool>& cancel, int timeoutMs)
{
    return request ("POST", path, juce::JSON::toString (body, true), timeoutMs, cancel, false, sendApiKey);
}

VpsClient::Response VpsClient::getPublic (const juce::String& path, std::atomic<bool>& cancel, int timeoutMs)
{
    return request ("GET", path, {}, timeoutMs, cancel, false, false);       // sendApiKey = false: nothing secret leaves
}

std::optional<VpsClient::KeyResult> VpsClient::checkCache (const juce::String& videoId, std::atomic<bool>& cancel)
{
    if (videoId.isEmpty()) return std::nullopt;

    auto r = request ("GET", "/api/tone-cache/check/" + juce::URL::addEscapeChars (videoId, false), {}, 4000, cancel);
    if (r.status != 200) return std::nullopt;

    auto v = juce::JSON::parse (r.body);
    if (! (bool) v.getProperty ("found", false)) return std::nullopt;
    return parseKey (v.getProperty ("entry", juce::var()));
}

juce::String VpsClient::submitJob (const juce::String& videoId, const juce::String& url, const juce::String& title)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("video_id", videoId);
    o->setProperty ("url", url);
    o->setProperty ("title", title);
    o->setProperty ("mode", "standard");

    std::atomic<bool> never { false };
    auto r = request ("POST", "/api/worker/job", juce::JSON::toString (juce::var (o), true), 6000, never);
    if (r.status != 200) return {};

    return juce::JSON::parse (r.body).getProperty ("job_id", juce::var()).toString();
}

std::optional<VpsClient::KeyResult> VpsClient::pollResult (const juce::String& jobId, int timeoutSec, std::atomic<bool>& cancel)
{
    if (jobId.isEmpty()) return std::nullopt;

    auto r = request ("GET", "/api/worker/result/" + juce::URL::addEscapeChars (jobId, false)
                             + "?timeout=" + juce::String (timeoutSec),
                      {}, (timeoutSec + 5) * 1000, cancel);
    if (r.status != 200) return std::nullopt;

    auto v = juce::JSON::parse (r.body);
    if (v.getProperty ("status", juce::var()).toString() != "ok") return std::nullopt;
    return parseKey (v.getProperty ("result", juce::var()));
}
