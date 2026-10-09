/*
    pt.Live - Live Singing DAW
    SongDetector implementation.
*/

#include "include/Net/SongDetector.h"
#include "include/Audio/PtLog.h"
#include <algorithm>

#if JUCE_WINDOWS
 #include <map>
 #include <memory>
 #include <windows.h>
 #include <objbase.h>
 #include <uiautomation.h>
 #include <winhttp.h>
 #pragma comment (lib, "winhttp.lib")
 #pragma comment (lib, "ole32.lib")
 #pragma comment (lib, "oleaut32.lib")
#endif

#if JUCE_WINDOWS
namespace
{
    // Minimal COM smart pointer (juce::ComSmartPtr is not exposed in this JUCE version).
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() = default;
        ~ComPtr() { reset(); }
        ComPtr (const ComPtr&) = delete;
        ComPtr& operator= (const ComPtr&) = delete;

        void reset() { if (p != nullptr) { p->Release(); p = nullptr; } }
        T** resetAndGetPointerAddress() { reset(); return &p; }
        HRESULT CoCreateInstance (REFCLSID clsid, DWORD ctx = CLSCTX_INPROC_SERVER)
        {
            reset();
            return ::CoCreateInstance (clsid, nullptr, ctx, __uuidof (T), (void**) &p);
        }

        ComPtr& operator= (std::nullptr_t) { reset(); return *this; }
        operator T*() const noexcept { return p; }
        T* operator->() const noexcept { return p; }
        bool operator== (std::nullptr_t) const noexcept { return p == nullptr; }
        bool operator!= (std::nullptr_t) const noexcept { return p != nullptr; }

    private:
        T* p = nullptr;
    };
}
#endif

namespace
{
    // "liên khúc" (medley) written as UTF-8 bytes
    const juce::String kLienKhuc = juce::String::fromUTF8 ("li\xc3\xaan kh\xc3\xba" "c");

    // "[01:23 / 10:45] " written by the Violentmonkey script: digits ':' '/' ' ' only.
    bool hasTimePrefix (const juce::String& t, int& endIndex)
    {
        if (! t.startsWithChar ('[')) return false;
        const int close = t.indexOfChar (']');
        if (close < 4) return false;

        const auto inside = t.substring (1, close);
        if (! inside.containsChar (':') || ! inside.containsChar ('/')) return false;
        if (! inside.containsOnly ("0123456789:/ ")) return false;

        endIndex = close + 1;
        return true;
    }

    // "(3) " notification counter YouTube puts in front of the title.
    int counterPrefixEnd (const juce::String& t)
    {
        if (! t.startsWithChar ('(')) return 0;
        const int close = t.indexOfChar (')');
        if (close < 2 || ! t.substring (1, close).containsOnly ("0123456789")) return 0;
        return close + 1;
    }

    bool isYouTubeTitle (const juce::String& title)
    {
        int dummy = 0;
        return title.containsIgnoreCase ("youtube") || hasTimePrefix (title.trim(), dummy);
    }

   #if JUCE_WINDOWS
    bool isBrowserWindowClass (HWND hwnd)
    {
        wchar_t cls[128] = {};
        GetClassNameW (hwnd, cls, 128);
        const juce::String c (cls);
        return c == "Chrome_WidgetWin_1"          // Chrome, Edge, Coc Coc, Brave, Opera, YouTube app (PWA)
            || c == "MozillaWindowClass";         // Firefox
    }

    juce::String windowTitle (HWND hwnd)
    {
        const int len = GetWindowTextLengthW (hwnd);
        if (len <= 0) return {};
        std::vector<wchar_t> buf ((size_t) len + 1, L'\0');
        GetWindowTextW (hwnd, buf.data(), len + 1);
        return juce::String (buf.data());
    }

    struct Candidate { HWND hwnd; juce::String title; bool foreground; bool playing; };

    // All visible browser windows that show YouTube. Cheap: only Win32 calls.
    std::vector<Candidate> collectYouTubeWindows()
    {
        struct Ctx { std::vector<Candidate> out; HWND fg; } ctx { {}, GetForegroundWindow() };

        EnumWindows ([] (HWND hwnd, LPARAM lp) -> BOOL
        {
            auto& c = *reinterpret_cast<Ctx*> (lp);
            if (! IsWindowVisible (hwnd) || ! isBrowserWindowClass (hwnd)) return TRUE;

            const auto title = windowTitle (hwnd);
            if (title.isEmpty() || ! isYouTubeTitle (title)) return TRUE;

            int dummy = 0;
            c.out.push_back ({ hwnd, title, hwnd == c.fg, hasTimePrefix (title.trim(), dummy) });
            return TRUE;
        }, reinterpret_cast<LPARAM> (&ctx));

        return ctx.out;
    }

    bool isWatchUrl (const juce::String& v)
    {
        return v.containsIgnoreCase ("youtube.com/watch") || v.containsIgnoreCase ("youtu.be/");
    }

    juce::String readValue (IUIAutomationElement* el)
    {
        if (el == nullptr) return {};

        ComPtr<IUIAutomationValuePattern> vp;
        if (FAILED (el->GetCurrentPatternAs (UIA_ValuePatternId, __uuidof (IUIAutomationValuePattern),
                                             (void**) vp.resetAndGetPointerAddress())) || vp == nullptr)
            return {};

        BSTR b = nullptr;
        if (FAILED (vp->get_CurrentValue (&b)) || b == nullptr) return {};
        const juce::String value (b);
        SysFreeString (b);
        return value.trim();
    }

    // The address bar element of each browser window is remembered: reading it again is ONE cheap call
    // (a few ms). Searching the whole UI tree of a browser window for it is the slow part (hundreds of ms
    // up to seconds), so that is only done the first time, or when the browser rebuilt its UI.
    using BarCache = std::map<HWND, std::shared_ptr<ComPtr<IUIAutomationElement>>>;

    // Reads the address bar of one browser window through UI Automation.
    // Returns {} when the window has no address bar (PWA) or it does not show a watch URL.
    juce::String readWatchUrl (IUIAutomation* uia, HWND hwnd, BarCache& cache, bool allowSearch = true)
    {
        if (auto it = cache.find (hwnd); it != cache.end())
        {
            const auto v = readValue (*it->second);
            if (isWatchUrl (v)) return v;
            if (v.isNotEmpty() && v.containsChar ('.')) return {};      // the bar is fine, it just shows another page
            cache.erase (it);                                           // element went stale: search again below
        }

        if (! allowSearch) return {};                                   // cheap poll: never walk the whole UI tree

        ComPtr<IUIAutomationElement> root;
        if (FAILED (uia->ElementFromHandle (hwnd, root.resetAndGetPointerAddress())) || root == nullptr)
            return {};

        VARIANT v;
        VariantInit (&v);
        v.vt = VT_I4;
        v.lVal = UIA_EditControlTypeId;

        ComPtr<IUIAutomationCondition> cond;
        if (FAILED (uia->CreatePropertyCondition (UIA_ControlTypePropertyId, v, cond.resetAndGetPointerAddress())))
            return {};

        ComPtr<IUIAutomationElementArray> edits;
        if (FAILED (root->FindAll (TreeScope_Descendants, cond, edits.resetAndGetPointerAddress())) || edits == nullptr)
            return {};

        int n = 0;
        edits->get_Length (&n);

        for (int i = 0; i < n; ++i)
        {
            auto el = std::make_shared<ComPtr<IUIAutomationElement>>();
            if (FAILED (edits->GetElement (i, el->resetAndGetPointerAddress())) || *el == nullptr) continue;

            const auto value = readValue (*el);
            if (isWatchUrl (value))
            {
                cache[hwnd] = el;
                return value;
            }
        }
        return {};
    }

    // The EXACT title of a video, asked by video id (YouTube oEmbed, no API key).
    // The window title can not be trusted for this: YouTube changes the address a moment BEFORE it changes
    // the page title, so reading both together gives the new video id with the PREVIOUS video's title.
    juce::String fetchOEmbedTitle (const juce::String& videoId)
    {
        if (videoId.isEmpty() || ! videoId.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"))
            return {};

        struct H { HINTERNET h = nullptr; ~H() { if (h != nullptr) WinHttpCloseHandle (h); } } ses, con, req;

        ses.h = WinHttpOpen (L"ptLive/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses.h == nullptr) return {};
        WinHttpSetTimeouts (ses.h, 3000, 3000, 3000, 3000);

        con.h = WinHttpConnect (ses.h, L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (con.h == nullptr) return {};

        const auto path = "/oembed?format=json&url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3D" + videoId;
        req.h = WinHttpOpenRequest (con.h, L"GET", path.toWideCharPointer(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req.h == nullptr) return {};

        if (! WinHttpSendRequest (req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || ! WinHttpReceiveResponse (req.h, nullptr))
            return {};

        DWORD status = 0, sz = sizeof (status);
        WinHttpQueryHeaders (req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) return {};

        std::string body;
        for (;;)
        {
            DWORD avail = 0;
            if (! WinHttpQueryDataAvailable (req.h, &avail) || avail == 0) break;
            std::vector<char> buf (avail);
            DWORD got = 0;
            if (! WinHttpReadData (req.h, buf.data(), avail, &got) || got == 0) break;
            body.append (buf.data(), got);
            if (body.size() > 64 * 1024) break;
        }

        const auto json = juce::JSON::parse (juce::String::fromUTF8 (body.data(), (int) body.size()));
        return json.getProperty ("title", juce::var()).toString().trim();
    }
   #endif
}

//==============================================================================
SongDetector::SongDetector() : juce::Thread ("pt.Live song detector") {}

SongDetector::~SongDetector()
{
    alive->store (false);
    stop();
}

void SongDetector::start() { if (! isThreadRunning()) startThread (juce::Thread::Priority::normal); }
void SongDetector::stop()
{
    // Never time out into a forced kill: killing a thread inside a COM / UI Automation call can leave
    // a lock held and hang the process. The thread checks threadShouldExit() every poll; the process-level
    // watchdog (ptl::guard) is the backstop if a browser makes one call very slow.
    signalThreadShouldExit();
    notify();
    stopThread (-1);
}
void SongDetector::requestStop() { signalThreadShouldExit(); notify(); }
void SongDetector::forget() { forgetRequested.store (true); notify(); }

juce::String SongDetector::extractVideoId (const juce::String& rawUrl)
{
    auto url = rawUrl.trim();
    if (url.isEmpty()) return {};
    if (! url.containsIgnoreCase ("://")) url = "https://" + url;

    juce::URL u (url);
    const auto domain = u.getDomain().toLowerCase();

    if (domain.contains ("youtu.be"))
        return u.getSubPath().trimCharactersAtStart ("/").upToFirstOccurrenceOf ("/", false, false).trim();

    if (domain.contains ("youtube.com"))
    {
        auto names = u.getParameterNames();
        auto values = u.getParameterValues();
        for (int i = 0; i < names.size(); ++i)
            if (names[i] == "v") return values[i].trim();
    }
    return {};
}

juce::String SongDetector::cleanTitle (const juce::String& windowTitle)
{
    auto t = windowTitle.trim();

    int e = 0;
    if (hasTimePrefix (t, e)) t = t.substring (e).trim();
    if (const int c = counterPrefixEnd (t); c > 0) t = t.substring (c).trim();

    // Everything after the LAST " - YouTube" is browser chrome ("- Google Chrome", "and 2 more pages - Edge",
    // "- Cốc Cốc" ...): cut there so the title is exactly the video title, whatever the browser is.
    if (const int yt = t.lastIndexOfIgnoreCase (" - YouTube"); yt > 0)
        t = t.substring (0, yt).trim();
    else
    // Fallback (no " - YouTube" in the window title): remove the browser suffix from the end.
    for (int pass = 0; pass < 2; ++pass)
        for (auto suffix : { "- YouTube", "- Google Chrome", "- Chrome", "- C\xe1\xbb\x91" "c C\xe1\xbb\x91" "c", "- Coc Coc", "- Brave",
                             "- Microsoft Edge", "- Edge", "- Mozilla Firefox", "- Firefox", "- Opera" })
        {
            const auto s = juce::String (juce::CharPointer_UTF8 (suffix));
            if (t.endsWithIgnoreCase (s)) { t = t.dropLastCharacters (s.length()).trim(); break; }
        }

    if (t.startsWithIgnoreCase ("YouTube -")) t = t.substring (9).trim();
    return t;
}

void SongDetector::run()
{
   #if JUCE_WINDOWS
    const bool comOk = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));

    ComPtr<IUIAutomation> uia;
    if (FAILED (uia.CoCreateInstance (__uuidof (CUIAutomation))) || uia == nullptr)
        ptl::diag::write ("[song] UI Automation is not available - songs cannot be detected");

    juce::String lastTitle, lastVideoId;
    juce::String candidateTitle;          // a new window title must be seen on 2 polls in a row before we look for the address
    juce::String candidateId;             // a new video id must be seen on 2 polls in a row before we report it
    int urlRetries = 0;
    int titleWaits = 0;
    BarCache barCache;

    // Polling is cheap (EnumWindows + GetWindowText, and ONE cached address-bar read), so it runs often:
    // a new song is noticed within about 0.3 - 0.6 s. The slow UI Automation tree search only happens
    // when the window title changed and the address bar is not known yet.
    constexpr int pollMs = 300;

    while (! threadShouldExit())
    {
        wait (pollMs);
        if (threadShouldExit()) break;
        if (uia == nullptr) continue;

        if (forgetRequested.exchange (false))
        {
            lastTitle.clear();
            lastVideoId.clear();
            candidateTitle.clear();
            candidateId.clear();
            urlRetries = 0;
            titleWaits = 0;
        }

        try
        {
            auto windows = collectYouTubeWindows();

            // Forget the address bars of windows that are gone.
            for (auto it = barCache.begin(); it != barCache.end();)
            {
                const bool stillOpen = std::any_of (windows.begin(), windows.end(), [&] (const Candidate& c) { return c.hwnd == it->first; });
                it = stillOpen ? std::next (it) : barCache.erase (it);
            }

            if (windows.empty()) continue;

            // Preferred: the foreground YouTube window, then one that is playing, then the first.
            const Candidate* pick = &windows.front();
            for (auto& w : windows) if (w.playing) { pick = &w; break; }
            for (auto& w : windows) if (w.foreground) { pick = &w; break; }

            const auto windowTitleNow = cleanTitle (pick->title);

            // 1) The ADDRESS decides when the song changed (it changes first and is always right).
            //    Cheap read: only uses the remembered address-bar element.
            juce::String url = readWatchUrl (uia, pick->hwnd, barCache, false);

            // 2) Address bar not known yet (first time / rebuilt UI): search for it, but only when the title
            //    changed and stayed the same for one more poll.
            if (url.isEmpty())
            {
                if (windowTitleNow.isEmpty() || windowTitleNow == lastTitle) { candidateTitle.clear(); continue; }

                if (windowTitleNow != candidateTitle)
                {
                    candidateTitle = windowTitleNow;
                    urlRetries = 0;
                    continue;
                }

                url = readWatchUrl (uia, pick->hwnd, barCache, true);
                for (auto& w : windows)
                    if (url.isEmpty() && w.hwnd != pick->hwnd && cleanTitle (w.title) == windowTitleNow)
                        url = readWatchUrl (uia, w.hwnd, barCache, true);

                // The browser may not have the address yet: try again on the next polls (about 2 s in total).
                if (url.isEmpty())
                {
                    if (++urlRetries >= 7) { lastTitle = windowTitleNow; candidateTitle.clear(); urlRetries = 0; }
                    continue;
                }
                candidateTitle.clear();
                urlRetries = 0;
            }

            const auto id = extractVideoId (url);
            if (id.isEmpty()) continue;

            if (id == lastVideoId)                       // still the same video
            {
                candidateId.clear();
                titleWaits = 0;
                if (windowTitleNow.isNotEmpty()) lastTitle = windowTitleNow;
                continue;
            }

            // A new video id must stay the same for one more poll (the address can flicker while the page loads).
            if (id != candidateId)
            {
                candidateId = id;
                titleWaits = 0;
                continue;
            }

            // 3) The TITLE: ask YouTube for the title of exactly this video id. The window title is only a fallback,
            //    and then only after it has changed (right after a switch it still shows the PREVIOUS video).
            juce::String title = fetchOEmbedTitle (id);
            if (title.isEmpty())
            {
                const bool titleStillOld = windowTitleNow.isEmpty() || windowTitleNow == lastTitle;
                if (titleStillOld && ++titleWaits < 8) continue;          // wait up to ~2.4 s for the page title to follow
                title = windowTitleNow;
            }

            lastVideoId = id;
            lastTitle = windowTitleNow;
            candidateId.clear();
            titleWaits = 0;

            if (! url.containsIgnoreCase ("://")) url = "https://" + url;
            Song song { id, url, title };

            const auto lower = title.toLowerCase();
            const bool isMedley = lower.contains (kLienKhuc) || lower.contains ("lien khuc");
            const bool noKeyword = filterEnabled.load() && ! (lower.contains ("karaoke") || lower.contains ("beat"));

            if (isMedley || noKeyword)
            {
                // Not a song for pt.Live (no key lookup / history), but the title must still follow the video.
                ptl::diag::write (juce::String (isMedley ? "[song] medley, title only: " : "[song] no karaoke/beat, title only: ") + title);

                juce::MessageManager::callAsync ([this, song, flag = alive]
                {
                    if (flag->load() && onTitleOnly) onTitleOnly (song);
                });
                continue;
            }

            ptl::diag::write ("[song] detected " + id + " | " + title);

            juce::MessageManager::callAsync ([this, song, flag = alive]
            {
                if (flag->load() && onSong) onSong (song);
            });
        }
        catch (...) { /* never let a browser oddity kill the thread */ }
    }

    barCache.clear();                      // release the COM elements before COM goes away
    uia = nullptr;
    if (comOk) CoUninitialize();
   #else
    while (! threadShouldExit()) wait (2000);
   #endif
}
