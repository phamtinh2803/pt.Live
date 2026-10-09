/*
    pt.Live - Live Singing DAW
    UpdateChecker implementation (see include/Net/UpdateChecker.h).
*/

#include "include/Net/UpdateChecker.h"
#include "include/Audio/PtLog.h"
#include <cstring>

namespace
{
    constexpr int kRequestTimeoutMs = 5000;      // below the 6 s shutdown watchdog

    juce::File stateFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ptLive").getChildFile ("update_state.json");
    }

    // "v0.1.2" / "0.1.2-beta" -> {0,1,2}. Empty array = not a version we understand.
    juce::Array<int> parseVersion (const juce::String& text)
    {
        auto s = text.trim().trimCharactersAtStart ("vV").upToFirstOccurrenceOf ("-", false, false);

        juce::Array<int> parts;
        if (s.isEmpty() || ! s.containsOnly ("0123456789."))
            return parts;

        for (const auto& token : juce::StringArray::fromTokens (s, ".", ""))
        {
            if (token.isEmpty())
                return {};                       // "1..2" / "1." / ".1"
            parts.add (token.getIntValue());
        }
        return parts;
    }

    // The url comes from the server and ends up in ShellExecute: https only, no spaces / quotes / control chars.
    bool isSafeDownloadUrl (const juce::String& url)
    {
        if (! url.startsWithIgnoreCase ("https://") || url.length() > 2048)
            return false;

        for (auto c : url)
            if (c <= 32 || c == '"' || c == '<' || c == '>' || c == '^' || c == '`' || c == '|')
                return false;

        return true;
    }
}

//==============================================================================
UpdateChecker::UpdateChecker() : juce::Thread ("ptLive update check") {}

UpdateChecker::~UpdateChecker()
{
    cancel.store (true);                         // aborts the request between its steps
    stopThread (7000);
}

void UpdateChecker::checkInBackground (const juce::String& currentVersion, Found found)
{
    if (isThreadRunning())
        return;

    current = currentVersion;
    onFound = std::move (found);
    startThread();
}

//==============================================================================
bool UpdateChecker::isNewer (const juce::String& candidate, const juce::String& current)
{
    const auto a = parseVersion (candidate), b = parseVersion (current);
    if (a.isEmpty() || b.isEmpty())
        return false;

    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

juce::String UpdateChecker::getSkippedVersion()
{
    const auto f = stateFile();
    if (! f.existsAsFile())
        return {};

    return juce::JSON::parse (f.loadFileAsString()).getProperty ("skipped_version", juce::var()).toString().trim();
}

void UpdateChecker::skipVersion (const juce::String& version)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("skipped_version", version);

    const auto f = stateFile();
    f.getParentDirectory().createDirectory();

    if (! f.replaceWithText (juce::JSON::toString (juce::var (o), true)))
        ptl::diag::write ("[update] could not store the skipped version in " + f.getFullPathName());
    else
        ptl::diag::write ("[update] version " + version + " skipped by the user");
}

//==============================================================================
void UpdateChecker::run()
{
    const auto r = vps.getPublic (kManifestPath, cancel, kRequestTimeoutMs);

    if (threadShouldExit() || cancel.load())
        return;

    if (r.status != 200)
    {
        ptl::diag::write ("[update] no manifest (HTTP " + juce::String (r.status) + ") - skipping the check");
        return;
    }

    const auto json = juce::JSON::parse (r.body);
    if (! json.isObject())
    {
        ptl::diag::write ("[update] manifest is not a JSON object - ignored");
        return;
    }

    Release rel;
    rel.version = json.getProperty ("version", juce::var()).toString().trim();
    rel.url     = json.getProperty ("url",     juce::var()).toString().trim();
    rel.notes   = json.getProperty ("notes",   juce::var()).toString().trim().substring (0, 800);
    rel.sha256  = json.getProperty ("sha256",  juce::var()).toString().trim().toLowerCase();

    if (parseVersion (rel.version).isEmpty() || ! isSafeDownloadUrl (rel.url))
    {
        ptl::diag::write ("[update] manifest has a bad version / url - ignored");
        return;
    }

    if (rel.sha256.isNotEmpty() && (rel.sha256.length() != 64 || ! rel.sha256.containsOnly ("0123456789abcdef")))
    {
        ptl::diag::write ("[update] manifest has a malformed sha256 - ignored");
        return;
    }

    if (! isNewer (rel.version, current))
    {
        ptl::diag::write ("[update] up to date (running " + current + ", server " + rel.version + ")");
        return;
    }

    if (getSkippedVersion() == rel.version)
    {
        ptl::diag::write ("[update] " + rel.version + " is available but the user skipped it");
        return;
    }

    ptl::diag::write ("[update] new version available: " + rel.version);

    // The callback only captures what it needs (no `this`): the checker may be gone when it runs.
    juce::MessageManager::callAsync ([cb = onFound, rel]
    {
        if (cb)
            cb (rel);
    });
}

//==============================================================================
// UpdateDownloader
//==============================================================================
namespace
{
    constexpr int kConnectTimeoutMs = 15000;
    constexpr int kChunkBytes       = 64 * 1024;
    constexpr juce::int64 kMinInstallerBytes = 200 * 1024;      // anything smaller is an error page, not an installer

    juce::File updateFolder()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ptLive_update");
    }

    // SHA-256 (FIPS 180-4), small and self-contained: the project does not link juce_cryptography.
    class Sha256
    {
    public:
        Sha256() { reset(); }

        void add (const void* data, size_t n)
        {
            auto* p = static_cast<const unsigned char*> (data);
            length += (juce::uint64) n;
            while (n > 0)
            {
                const size_t take = juce::jmin (n, (size_t) 64 - fill);
                std::memcpy (block + fill, p, take);
                fill += take; p += take; n -= take;
                if (fill == 64) { compress(); fill = 0; }
            }
        }

        juce::String finishHex()
        {
            const juce::uint64 bits = length * 8;
            const unsigned char one = 0x80, zero = 0;
            add (&one, 1);
            while (fill != 56)
                add (&zero, 1);
            unsigned char lenBytes[8];
            for (int i = 0; i < 8; ++i)
                lenBytes[i] = (unsigned char) (bits >> (56 - 8 * i));
            add (lenBytes, 8);

            juce::String hex;
            for (auto v : h)
                hex << juce::String::toHexString ((int) v).paddedLeft ('0', 8).getLastCharacters (8);
            return hex.toLowerCase();
        }

    private:
        static juce::uint32 rotr (juce::uint32 x, int n) { return (x >> n) | (x << (32 - n)); }

        void reset()
        {
            static const juce::uint32 init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
            std::memcpy (h, init, sizeof (h));
            fill = 0;
            length = 0;
        }

        void compress()
        {
            static const juce::uint32 k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

            juce::uint32 w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = ((juce::uint32) block[i * 4] << 24) | ((juce::uint32) block[i * 4 + 1] << 16)
                     | ((juce::uint32) block[i * 4 + 2] << 8) | (juce::uint32) block[i * 4 + 3];
            for (int i = 16; i < 64; ++i)
            {
                const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
                const auto s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
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

        juce::uint32 h[8];
        unsigned char block[64];
        size_t fill = 0;
        juce::uint64 length = 0;
    };

    juce::String sha256OfFile (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return {};

        Sha256 sha;
        juce::HeapBlock<char> buf (kChunkBytes);
        for (;;)
        {
            const int n = in.read (buf, kChunkBytes);
            if (n <= 0)
                break;
            sha.add (buf, (size_t) n);
        }
        return sha.finishHex();
    }

    bool looksLikeWindowsProgram (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return false;

        char mz[2] = {};
        return in.read (mz, 2) == 2 && mz[0] == 'M' && mz[1] == 'Z';
    }
}

UpdateDownloader::UpdateDownloader() : juce::Thread ("ptLive update download") {}

UpdateDownloader::~UpdateDownloader()
{
    cancelDownload();
    stopThread (5000);
}

void UpdateDownloader::start (const juce::String& u, const juce::String& v, const juce::String& hash, Finished f)
{
    stopThread (2000);                             // a run that is just finishing (retry) must be gone before a new one starts

    url = u;
    version = v;
    sha256 = hash;
    onFinished = std::move (f);
    done.store (0);
    total.store (-1);
    cancelled.store (false);
    startThread();
}

void UpdateDownloader::cancelDownload()
{
    cancelled.store (true);

    const juce::ScopedLock sl (streamLock);
    if (activeStream != nullptr)
        activeStream->cancel();                    // wakes up a read() that is waiting for the network
}

void UpdateDownloader::finish (bool ok, const juce::File& file, Error e)
{
    if (cancelled.load())
        return;                                    // the user left: nobody wants the result any more

    // Captures a copy of the callback only (no `this`): the downloader may be gone when it runs.
    juce::MessageManager::callAsync ([cb = onFinished, ok, file, e]
    {
        if (cb)
            cb (ok, file, e);
    });
}

void UpdateDownloader::run()
{
    // Old leftovers (earlier versions, half-finished downloads) are removed first.
    const auto dir = updateFolder();
    dir.createDirectory();
    for (const auto& old : dir.findChildFiles (juce::File::findFiles, false))
        old.deleteFile();

    const auto safeVersion = juce::String (version).retainCharacters ("0123456789.");
    const auto target = dir.getChildFile ("ptLive-" + safeVersion + "-setup.exe");
    const auto part   = dir.getChildFile (target.getFileName() + ".part");

    ptl::diag::write ("[update] downloading " + url);

    juce::WebInputStream web (juce::URL (url), false);
    web.withNumRedirectsToFollow (5)
       .withConnectionTimeout (kConnectTimeoutMs)
       .withExtraHeaders ("User-Agent: ptLive-updater\r\n");

    {
        const juce::ScopedLock sl (streamLock);
        activeStream = &web;
    }
    struct StreamGuard { UpdateDownloader& d; ~StreamGuard() { const juce::ScopedLock sl (d.streamLock); d.activeStream = nullptr; } } streamGuard { *this };

    if (cancelled.load())
        return;

    if (! web.connect (nullptr))
    {
        ptl::diag::write ("[update] download: could not connect");
        finish (false, {}, Error::connect);
        return;
    }

    const int status = web.getStatusCode();
    if (status != 200)
    {
        ptl::diag::write ("[update] download: HTTP " + juce::String (status));
        finish (false, {}, status == 0 ? Error::connect : Error::http);
        return;
    }

    const auto length = web.getTotalLength();
    total.store (length > 0 ? length : -1);

    std::unique_ptr<juce::FileOutputStream> out (part.createOutputStream (kChunkBytes));
    if (out == nullptr || out->failedToOpen())
    {
        ptl::diag::write ("[update] download: cannot write " + part.getFullPathName());
        finish (false, {}, Error::disk);
        return;
    }
    out->setPosition (0);
    out->truncate();

    juce::HeapBlock<char> buffer (kChunkBytes);
    for (;;)
    {
        if (cancelled.load() || threadShouldExit())
        {
            out.reset();
            part.deleteFile();
            return;
        }

        const int n = web.read (buffer, kChunkBytes);
        if (n < 0)
            break;                                  // network error
        if (n == 0)
            break;                                  // end of data (checked against the announced length below)

        if (! out->write (buffer, (size_t) n))
        {
            out.reset();
            part.deleteFile();
            ptl::diag::write ("[update] download: disk write failed (disk full?)");
            finish (false, {}, Error::disk);
            return;
        }
        done.fetch_add (n);
    }

    out->flush();
    out.reset();

    if (cancelled.load())
    {
        part.deleteFile();
        return;
    }

    const auto got = done.load();
    if ((length > 0 && got != length) || got < kMinInstallerBytes)
    {
        ptl::diag::write ("[update] download: incomplete (" + juce::String (got) + " of " + juce::String (length) + " bytes)");
        part.deleteFile();
        finish (false, {}, got < kMinInstallerBytes && (length <= 0 || got == length) ? Error::notInstaller : Error::interrupted);
        return;
    }

    if (! looksLikeWindowsProgram (part))
    {
        ptl::diag::write ("[update] download: the file is not a Windows program (wrong link?)");
        part.deleteFile();
        finish (false, {}, Error::notInstaller);
        return;
    }

    if (sha256.isNotEmpty())
    {
        const auto actual = sha256OfFile (part);
        if (actual != sha256)
        {
            ptl::diag::write ("[update] download: sha256 mismatch");
            part.deleteFile();
            finish (false, {}, Error::hashMismatch);
            return;
        }
    }

    target.deleteFile();
    if (! part.moveFileTo (target))
    {
        ptl::diag::write ("[update] download: cannot rename the finished file");
        part.deleteFile();
        finish (false, {}, Error::disk);
        return;
    }

    ptl::diag::write ("[update] download complete: " + target.getFullPathName());
    finish (true, target, Error::none);
}
