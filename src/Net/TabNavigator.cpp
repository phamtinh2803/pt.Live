/*
    pt.Live - Live Singing DAW
    TabNavigator implementation.

    Only ONE YouTube song may play at a time, so a link is never opened in a second tab:
      - a browser already has a YouTube tab (active tab of a window, or a background tab) ->
        that tab is brought to the front and its ADDRESS BAR gets the new link + Enter (same tab reused);
      - no browser has YouTube open -> the link is opened in the default browser (ShellExecute).
    If a YouTube tab exists but cannot be driven, nothing is opened (returns false) so two songs never play together.
*/

#include "include/Net/TabNavigator.h"
#include "include/Audio/PtLog.h"

#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>
#include <shellapi.h>
#include <atomic>
#include <thread>

#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "oleaut32.lib")

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

        operator T*() const noexcept { return p; }
        T* operator->() const noexcept { return p; }
        T* get() const noexcept { return p; }
        bool operator== (std::nullptr_t) const noexcept { return p == nullptr; }
        bool operator!= (std::nullptr_t) const noexcept { return p != nullptr; }

    private:
        T* p = nullptr;
    };

    bool isBrowserWindowClass (HWND hwnd)
    {
        wchar_t cls[128] = {};
        GetClassNameW (hwnd, cls, 128);
        const juce::String c (cls);
        return c == "Chrome_WidgetWin_1"          // Chrome, Edge, Coc Coc, Brave, Opera
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

    bool containsYouTube (const juce::String& title)
    {
        return title.containsIgnoreCase ("youtube");
    }

    juce::String readValue (IUIAutomationValuePattern* vp)
    {
        if (vp == nullptr) return {};
        BSTR b = nullptr;
        if (FAILED (vp->get_CurrentValue (&b)) || b == nullptr) return {};
        const juce::String value (b);
        SysFreeString (b);
        return value.trim();
    }

    // Takes an extra reference of `src` into `dst` (ComPtr is not copyable).
    template <typename T>
    void assignRef (ComPtr<T>& dst, T* src)
    {
        if (src != nullptr) src->AddRef();
        *dst.resetAndGetPointerAddress() = src;
    }

    // Brings a browser window to the front (restores it when minimised). True when it is the foreground window afterwards.
    bool bringToFront (HWND hwnd)
    {
        if (IsIconic (hwnd)) ShowWindow (hwnd, SW_RESTORE);

        const DWORD me   = GetCurrentThreadId();
        const DWORD fgTh = GetWindowThreadProcessId (GetForegroundWindow(), nullptr);
        const bool attach = fgTh != 0 && fgTh != me;
        if (attach) AttachThreadInput (me, fgTh, TRUE);

        BringWindowToTop (hwnd);
        SetForegroundWindow (hwnd);

        if (attach) AttachThreadInput (me, fgTh, FALSE);
        Sleep (80);
        return GetForegroundWindow() == hwnd;
    }

    // The best YouTube tab (a TabItem whose name contains "youtube") of a browser window; the selected one wins.
    bool findYouTubeTab (IUIAutomation* uia, IUIAutomationElement* root,
                         ComPtr<IUIAutomationElement>& outEl, bool& selected)
    {
        VARIANT v;
        VariantInit (&v);
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;

        ComPtr<IUIAutomationCondition> cond;
        if (FAILED (uia->CreatePropertyCondition (UIA_ControlTypePropertyId, v, cond.resetAndGetPointerAddress())) || cond == nullptr)
            return false;

        ComPtr<IUIAutomationElementArray> tabs;
        if (FAILED (root->FindAll (TreeScope_Descendants, cond.get(), tabs.resetAndGetPointerAddress())) || tabs == nullptr)
            return false;

        int n = 0;
        tabs->get_Length (&n);

        bool found = false;
        selected = false;

        for (int i = 0; i < n; ++i)
        {
            ComPtr<IUIAutomationElement> el;
            if (FAILED (tabs->GetElement (i, el.resetAndGetPointerAddress())) || el == nullptr) continue;

            BSTR nm = nullptr;
            if (FAILED (el->get_CurrentName (&nm)) || nm == nullptr) continue;
            const juce::String name (nm);
            SysFreeString (nm);
            if (! containsYouTube (name)) continue;

            bool sel = false;
            ComPtr<IUIAutomationSelectionItemPattern> sp;
            if (SUCCEEDED (el->GetCurrentPatternAs (UIA_SelectionItemPatternId, __uuidof (IUIAutomationSelectionItemPattern),
                                                    (void**) sp.resetAndGetPointerAddress())) && sp != nullptr)
            {
                BOOL b = FALSE;
                sp->get_CurrentIsSelected (&b);
                sel = b != FALSE;
            }

            if (! found || (sel && ! selected))
            {
                assignRef (outEl, el.get());
                selected = sel;
                found = true;
            }
        }
        return found;
    }

    // The browser's address bar: the editable, visible Edit control that sits highest in the window
    // (the page's own inputs, e.g. the YouTube search box, are lower).
    bool findAddressBar (IUIAutomation* uia, IUIAutomationElement* root, ComPtr<IUIAutomationElement>& outEl)
    {
        VARIANT v;
        VariantInit (&v);
        v.vt = VT_I4;
        v.lVal = UIA_EditControlTypeId;

        ComPtr<IUIAutomationCondition> cond;
        if (FAILED (uia->CreatePropertyCondition (UIA_ControlTypePropertyId, v, cond.resetAndGetPointerAddress())) || cond == nullptr)
            return false;

        ComPtr<IUIAutomationElementArray> edits;
        if (FAILED (root->FindAll (TreeScope_Descendants, cond.get(), edits.resetAndGetPointerAddress())) || edits == nullptr)
            return false;

        int n = 0;
        edits->get_Length (&n);

        bool found = false;
        long bestTop = 0;
        bool bestYt = false;

        for (int i = 0; i < n; ++i)
        {
            ComPtr<IUIAutomationElement> el;
            if (FAILED (edits->GetElement (i, el.resetAndGetPointerAddress())) || el == nullptr) continue;

            BOOL off = FALSE;
            el->get_CurrentIsOffscreen (&off);
            if (off) continue;

            ComPtr<IUIAutomationValuePattern> vp;
            if (FAILED (el->GetCurrentPatternAs (UIA_ValuePatternId, __uuidof (IUIAutomationValuePattern),
                                                (void**) vp.resetAndGetPointerAddress())) || vp == nullptr)
                continue;

            BOOL ro = FALSE;
            vp->get_CurrentIsReadOnly (&ro);
            if (ro) continue;

            RECT rc {};
            if (FAILED (el->get_CurrentBoundingRectangle (&rc))) continue;

            const auto value = readValue (vp.get());
            const bool yt = value.containsIgnoreCase ("youtube.com") || value.containsIgnoreCase ("youtu.be");

            if (! found || rc.top < bestTop || (rc.top == bestTop && yt && ! bestYt))
            {
                assignRef (outEl, el.get());
                bestTop = rc.top;
                bestYt = yt;
                found = true;
            }
        }
        return found;
    }

    void sendKey (WORD vk, bool up)
    {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        SendInput (1, &in, sizeof (INPUT));
    }

    void sendEnterKey()
    {
        sendKey (VK_RETURN, false);
        sendKey (VK_RETURN, true);
    }

    // Fallback when the address bar refuses SetValue: Ctrl+A then type the link character by character.
    // Keys only go out while `expected` (the browser window we picked) still has the foreground: a plugin
    // window that popped up in between must never receive Ctrl+A and the link. False = cancelled.
    bool typeText (const juce::String& s, HWND expected)
    {
        if (GetForegroundWindow() != expected)
        {
            ptl::diag::write ("[navigator] browser lost the foreground - typing cancelled");
            return false;
        }

        sendKey (VK_CONTROL, false);
        sendKey ('A', false);
        sendKey ('A', true);
        sendKey (VK_CONTROL, true);
        Sleep (30);

        int typed = 0;

        for (auto p = s.toWideCharPointer(); *p != 0; ++p)
        {
            if ((typed++ % 8) == 0 && GetForegroundWindow() != expected)
            {
                ptl::diag::write ("[navigator] browser lost the foreground - typing cancelled");
                return false;
            }

            INPUT in[2] = {};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wScan = *p;
            in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1] = in[0];
            in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput (2, in, sizeof (INPUT));
        }

        return true;
    }

    // The browser window that took the last link we launched. Used when no window TITLE says "YouTube"
    // (so the song is still sent to the same window instead of opening another tab).
    std::atomic<uintptr_t> lastLaunchedBrowser { 0 };

    void openInDefaultBrowser (const juce::String& url)
    {
        const auto w = url.toWideCharPointer();
        ShellExecuteW (nullptr, L"open", w, nullptr, nullptr, SW_SHOWNORMAL);

        // Within ~4 s the browser window that shows the new page becomes the foreground window: remember it.
        std::thread ([]
        {
            for (int i = 0; i < 40; ++i)
            {
                Sleep (100);
                const HWND fg = GetForegroundWindow();
                if (fg != nullptr && isBrowserWindowClass (fg))
                {
                    lastLaunchedBrowser = (uintptr_t) fg;
                    return;
                }
            }
        }).detach();
    }

    // True when the window exposes at least one browser tab. A YouTube "app" (PWA / --app window) has no tab strip
    // and no address bar, so a window without any TabItem is safe to close without losing other tabs.
    bool hasAnyTab (IUIAutomation* uia, IUIAutomationElement* root)
    {
        VARIANT v;
        VariantInit (&v);
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;

        ComPtr<IUIAutomationCondition> cond;
        if (FAILED (uia->CreatePropertyCondition (UIA_ControlTypePropertyId, v, cond.resetAndGetPointerAddress())) || cond == nullptr)
            return true;                         // cannot tell -> assume a real browser (never close)

        ComPtr<IUIAutomationElement> first;
        if (FAILED (root->FindFirst (TreeScope_Descendants, cond.get(), first.resetAndGetPointerAddress())))
            return true;
        return first != nullptr;
    }

    // Closes the YouTube app window and waits (up to ~2.5 s) until it is really gone.
    void closeAppWindow (HWND hwnd)
    {
        PostMessageW (hwnd, WM_CLOSE, 0, 0);
        for (int i = 0; i < 25 && IsWindow (hwnd); ++i)
            Sleep (100);
    }

    struct Candidate { HWND hwnd; juce::String title; bool fg; bool activeIsYouTube; };
}

bool navigateToYouTubeTab (const juce::String& url)
{
   #if JUCE_WINDOWS
    if (url.isEmpty())
        return false;

    const bool comOk = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
    struct ComGuard { bool ok; ~ComGuard() { if (ok) CoUninitialize(); } } comGuard { comOk };

    ComPtr<IUIAutomation> uia;
    if (FAILED (uia.CoCreateInstance (__uuidof (CUIAutomation))) || uia == nullptr)
    {
        ptl::diag::write ("[navigator] UI Automation init failed -> default browser");
        openInDefaultBrowser (url);
        return true;
    }

    // All visible browser windows (the active tab's title is the window title).
    struct Ctx { std::vector<Candidate> out; HWND fg; } ctx { {}, GetForegroundWindow() };

    EnumWindows ([] (HWND hwnd, LPARAM lp) -> BOOL
    {
        auto& c = *reinterpret_cast<Ctx*> (lp);
        if (! IsWindowVisible (hwnd) || ! isBrowserWindowClass (hwnd)) return TRUE;

        const auto title = windowTitle (hwnd);
        if (title.isEmpty()) return TRUE;

        c.out.push_back ({ hwnd, title, hwnd == c.fg, containsYouTube (title) });
        return TRUE;
    }, reinterpret_cast<LPARAM> (&ctx));

    // 1) a window whose ACTIVE tab is YouTube (foreground one first)
    const Candidate* pick = nullptr;
    for (auto& c : ctx.out)
        if (c.activeIsYouTube && (pick == nullptr || c.fg))
            pick = &c;

    ComPtr<IUIAutomationElement> root, bgTab;
    bool needSelect = false;

    if (pick != nullptr)
    {
        uia->ElementFromHandle (pick->hwnd, root.resetAndGetPointerAddress());
    }
    else
    {
        // 2) a YouTube tab sitting in the background of some browser window
        for (auto& c : ctx.out)
        {
            ComPtr<IUIAutomationElement> r, tab;
            if (FAILED (uia->ElementFromHandle (c.hwnd, r.resetAndGetPointerAddress())) || r == nullptr) continue;

            bool sel = false;
            if (findYouTubeTab (uia.get(), r.get(), tab, sel))
            {
                pick = &c;
                assignRef (root, r.get());
                assignRef (bgTab, tab.get());
                needSelect = ! sel;
                if (c.fg) break;
            }
        }
    }

    if (pick == nullptr)
    {
        // 3) no title mentions YouTube: reuse the browser window we launched the previous song in.
        const HWND last = (HWND) lastLaunchedBrowser.load();
        if (last != nullptr && IsWindow (last) && IsWindowVisible (last))
        {
            for (auto& c : ctx.out)
            {
                if (c.hwnd == last)
                {
                    pick = &c;
                    uia->ElementFromHandle (last, root.resetAndGetPointerAddress());
                    ptl::diag::write ("[navigator] no YouTube title; reusing the window the last song was opened in");
                    break;
                }
            }
        }
    }

    if (pick == nullptr)
    {
        // Worded with "not found" so the line is kept in ptLive.log (error lines only) - it lists what was scanned.
        juce::String seen;
        for (auto& c : ctx.out)
        {
            wchar_t cls[64] = {};
            GetClassNameW (c.hwnd, cls, 64);
            seen << "\n    [" << juce::String (cls) << "] \"" << c.title.substring (0, 80) << "\"";
        }
        ptl::diag::write ("[navigator] YouTube tab not found among " + juce::String ((int) ctx.out.size())
                          + " browser window(s) -> opening default browser" + seen);
        openInDefaultBrowser (url);
        return true;
    }

    // From here a YouTube tab EXISTS: never open a second one. Any failure returns false.
    ptl::diag::write ("[navigator] reusing YouTube tab: " + pick->title);

    if (root == nullptr)
        return false;

    if (! bringToFront (pick->hwnd))
        ptl::diag::write ("[navigator] could not take the foreground (continuing)");

    if (needSelect && bgTab != nullptr)
    {
        ComPtr<IUIAutomationSelectionItemPattern> sp;
        if (SUCCEEDED (bgTab->GetCurrentPatternAs (UIA_SelectionItemPatternId, __uuidof (IUIAutomationSelectionItemPattern),
                                                   (void**) sp.resetAndGetPointerAddress())) && sp != nullptr)
        {
            sp->Select();
            Sleep (180);                         // let the browser switch tabs
        }
    }

    ComPtr<IUIAutomationElement> addr;
    if (! findAddressBar (uia.get(), root.get(), addr) || addr == nullptr)
    {
        // No address bar. If the window also has no tab strip it is the YouTube APP (not a browser):
        // close it and open the link in the default browser so only one song ever plays.
        if (! hasAnyTab (uia.get(), root.get()))
        {
            ptl::diag::write ("[navigator] YouTube app window (no address bar) -> closing it, opening default browser");
            closeAppWindow (pick->hwnd);
            openInDefaultBrowser (url);
            return true;
        }

        ptl::diag::write ("[navigator] address bar not found");
        return false;
    }

    addr->SetFocus();
    Sleep (60);

    bool valueSet = false;
    ComPtr<IUIAutomationValuePattern> vp;
    if (SUCCEEDED (addr->GetCurrentPatternAs (UIA_ValuePatternId, __uuidof (IUIAutomationValuePattern),
                                              (void**) vp.resetAndGetPointerAddress())) && vp != nullptr)
    {
        std::vector<wchar_t> wide ((size_t) url.length() * 2 + 8, L'\0');
        if (MultiByteToWideChar (CP_UTF8, 0, url.toRawUTF8(), -1, wide.data(), (int) wide.size()) > 0
            && SUCCEEDED (vp->SetValue (wide.data())))
        {
            const auto expected = url.fromFirstOccurrenceOf ("://", false, false);
            valueSet = readValue (vp.get()).containsIgnoreCase (expected);
        }
    }

    if (! valueSet)
    {
        ptl::diag::write ("[navigator] SetValue not accepted -> typing the link");
        if (! typeText (url, pick->hwnd))
            return false;
    }

    Sleep (70);

    if (GetForegroundWindow() != pick->hwnd)
    {
        ptl::diag::write ("[navigator] browser lost the foreground - Enter not sent");
        return false;
    }

    sendEnterKey();
    ptl::diag::write ("[navigator] link sent to the existing tab");
    return true;
   #else
    return false;
   #endif
}
