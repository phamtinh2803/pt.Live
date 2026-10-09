/*
    pt.Live - Live Singing DAW
    InstanceGuard - safe start / close of the process (Windows).

    Problems this solves:
      1. Closing: the window disappears, but the process stays in Task Manager (a background thread, an
         ASIO driver or a plugin does not finish). The next start of pt.Live was then handed to that
         dying process (JUCE single-instance) and nothing ever appeared on screen.
      2. Starting: a previous pt.Live with NO window (stuck while closing, or stuck while starting)
         blocked every following start until the user pressed "End task".

    How:
      - armShutdownWatchdog()  : a tiny thread that ends the process by force if a normal exit takes
                                 longer than the given time. By then the session was already saved.
      - publishInstance()      : the running pt.Live publishes {pid, closing} in a named shared-memory block.
      - markClosing()          : tells a new start that this one is on its way out.
      - clearStaleInstance()   : called by a NEW start BEFORE JUCE's single-instance check:
            healthy pt.Live (has a visible window, not closing)  -> leave it alone (JUCE forwards the start)
            pt.Live closing                                      -> wait until it is gone (bounded)
            pt.Live without a window                             -> wait a little, then end it
*/

#pragma once

#include <JuceHeader.h>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace ptl::guard
{
   #if JUCE_WINDOWS
    namespace detail
    {
        struct State { volatile LONG pid; volatile LONG closing; };

        inline const wchar_t* mappingName() { return L"Local\\ptLive.InstanceState.v1"; }

        inline HANDLE& ownHandle() { static HANDLE h = nullptr; return h; }
        inline State*& ownState()  { static State* s = nullptr; return s; }

        struct WinSearch { DWORD pid; bool visible; };

        inline bool hasVisibleWindow (DWORD pid)
        {
            WinSearch ws { pid, false };

            EnumWindows ([] (HWND hwnd, LPARAM lp) -> BOOL
            {
                auto& s = *reinterpret_cast<WinSearch*> (lp);
                DWORD p = 0;
                GetWindowThreadProcessId (hwnd, &p);

                if (p == s.pid && IsWindowVisible (hwnd))
                {
                    RECT r {};
                    GetWindowRect (hwnd, &r);
                    if (r.right - r.left > 20 && r.bottom - r.top > 20)
                    {
                        s.visible = true;
                        return FALSE;
                    }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM> (&ws));

            return ws.visible;
        }
    }
   #endif

    /** Ends the process by force when a normal exit takes longer than `ms`. Safe to call more than once. */
    inline void armShutdownWatchdog (int ms)
    {
       #if JUCE_WINDOWS
        static std::atomic<bool> armed { false };
        if (armed.exchange (true))
            return;

        std::thread ([ms]
        {
            Sleep ((DWORD) ms);
            TerminateProcess (GetCurrentProcess(), 0);
        }).detach();
       #else
        juce::ignoreUnused (ms);
       #endif
    }

    /** The running pt.Live announces itself (call from initialise()). */
    inline void publishInstance()
    {
       #if JUCE_WINDOWS
        using namespace detail;
        if (ownHandle() != nullptr)
            return;

        ownHandle() = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof (State), mappingName());
        if (ownHandle() == nullptr)
            return;

        ownState() = static_cast<State*> (MapViewOfFile (ownHandle(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof (State)));
        if (ownState() != nullptr)
        {
            InterlockedExchange (&ownState()->closing, 0);
            InterlockedExchange (&ownState()->pid, (LONG) GetCurrentProcessId());
        }
       #endif
    }

    /** This pt.Live is closing (call as early as possible in the close path). */
    inline void markClosing()
    {
       #if JUCE_WINDOWS
        if (detail::ownState() != nullptr)
            InterlockedExchange (&detail::ownState()->closing, 1);
       #endif
    }

    /** Release the shared-memory block this process published (call at the very end of shutdown).
        Without this the block keeps the pid of a process that no longer exists, and the NEXT start
        can waste up to 8 s waiting / killing a dead pid before JUCE's single-instance check runs -
        which is what made "open the .exe again / open a .pt again" appear to fail. */
    inline void closeOwnHandle()
    {
       #if JUCE_WINDOWS
        using namespace detail;

        if (ownState() != nullptr)
        {
            InterlockedExchange (&ownState()->pid, 0);
            InterlockedExchange (&ownState()->closing, 1);
            UnmapViewOfFile (ownState());
            ownState() = nullptr;
        }

        if (ownHandle() != nullptr)
        {
            CloseHandle (ownHandle());
            ownHandle() = nullptr;
        }
       #endif
    }

    /** Called by a NEW start before anything else. Returns when it is safe to go on. */
    inline void clearStaleInstance()
    {
       #if JUCE_WINDOWS
        using namespace detail;

        HANDLE map = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, mappingName());
        if (map == nullptr)
            return;                                                    // no other pt.Live

        auto* st = static_cast<State*> (MapViewOfFile (map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (State)));
        if (st == nullptr) { CloseHandle (map); return; }

        const DWORD pid = (DWORD) st->pid;
        HANDLE proc = pid != 0 ? OpenProcess (SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;

        // Our OWN pid in the block means a previous run published it and never released it
        // (before closeOwnHandle() existed). Treat it as stale and overwrite below - never
        // try to kill ourselves.
        if (proc != nullptr && pid != GetCurrentProcessId())
        {
            constexpr DWORD closingWaitMs = 6000;     // a closing pt.Live is given this long (its own watchdog ends it earlier)
            constexpr DWORD noWindowWaitMs = 4000;    // a pt.Live without window may just be starting up slowly
            const DWORD t0 = GetTickCount();

            const bool stale = (st->closing != 0) || ! hasVisibleWindow (pid);

            if (stale)
            {
                // The previous instance is gone / closing / has no window: wait for it to fully
                // exit, killing it only as a last resort. We block here so the JUCE single-instance
                // mutex/pipe is free by the time we return - that is the real root cause of the
                // "re-open the .exe / re-open a .pt and it silently fails" bug.
                bool dead = false;

                for (int i = 0; i < 80 && ! dead; ++i)                 // up to 8 s
                {
                    if (WaitForSingleObject (proc, 0) == WAIT_OBJECT_0) { dead = true; break; }

                    const DWORD waited = GetTickCount() - t0;
                    const DWORD limit  = (st->closing != 0) ? closingWaitMs : noWindowWaitMs;

                    if (waited > limit)
                    {
                        TerminateProcess (proc, 0);
                        WaitForSingleObject (proc, 3000);
                        dead = true;
                        break;
                    }

                    Sleep (100);
                }

                // Extra settle so Windows fully tears down the old process' handles.
                Sleep (250);
            }
            // else: visible && ! closing -> healthy instance running: leave it alone, JUCE will
            //       forward this start to it (or show the "already running" dialog).
        }

        if (proc != nullptr) CloseHandle (proc);
        UnmapViewOfFile (st);
        CloseHandle (map);
       #endif
    }
}
