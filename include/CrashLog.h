/*
    pt.Live - Live Singing DAW
    Crash log: when the process dies from an unhandled exception (access violation inside a VST,
    a driver, ...), write WHAT / WHERE into %APPDATA%\ptLive\ptLive.log and a mini dump next to it
    (%APPDATA%\ptLive\crash-<time>.dmp). Without this the log simply stops at the last normal line.

    Install once at start:  juce::SystemStats::setApplicationCrashHandler (ptl::crash::handle);
    (dbghelp.dll is loaded only when a crash happens, so no extra library to link.)
*/

#pragma once

#include <JuceHeader.h>
#include "include/Audio/PtLog.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace ptl::crash
{
    // Writes <appdata>\ptLive\<prefix>-<time>.dmp. ep may be null (hang dump: no exception, all threads).
    inline void writeDump (void* info, const char* prefix)
    {
       #if JUCE_WINDOWS
        auto* ep = static_cast<EXCEPTION_POINTERS*> (info);

        if (auto dbg = LoadLibraryW (L"dbghelp.dll"))
        {
            using MiniDumpWriteDumpFn = BOOL (WINAPI*) (HANDLE, DWORD, HANDLE, int, void*, void*, void*);

            if (auto fn = (MiniDumpWriteDumpFn) GetProcAddress (dbg, "MiniDumpWriteDump"))
            {
                const auto dmp = ptl::diag::getLogFile().getParentDirectory()
                                     .getChildFile (juce::String (prefix) + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".dmp");

                HANDLE file = CreateFileW (dmp.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

                if (file != INVALID_HANDLE_VALUE)
                {
                    // MINIDUMP_EXCEPTION_INFORMATION is declared with 4-byte packing in dbghelp.h.
                    // Without it the EXCEPTION_POINTERS pointer lands at the wrong offset on x64,
                    // MiniDumpWriteDump fails and the .dmp stays 0 bytes.
                   #pragma pack(push, 4)
                    struct ExceptionInfo { DWORD threadId; EXCEPTION_POINTERS* pointers; BOOL clientPointers; };
                   #pragma pack(pop)
                    ExceptionInfo ei { GetCurrentThreadId(), ep, FALSE };

                    // 0x1000 = MiniDumpWithThreadInfo (normal dumps already hold the stacks of ALL threads).
                    const BOOL ok = fn (GetCurrentProcess(), GetCurrentProcessId(), file, 0x00001000 /* MiniDumpWithThreadInfo */,
                                        ep != nullptr ? &ei : nullptr, nullptr, nullptr);
                    CloseHandle (file);

                    ptl::diag::getLogFile().appendText (juce::String ("[dump] ") + (ok ? "written " : "FAILED ") + dmp.getFullPathName() + "\n");
                }
            }
        }
       #else
        juce::ignoreUnused (info, prefix);
       #endif
    }
}

namespace ptl::crash
{
    inline void handle (void* info)
    {
       #if JUCE_WINDOWS
        auto* ep = static_cast<EXCEPTION_POINTERS*> (info);

        juce::String text;
        {
            const auto now = juce::Time::getCurrentTime();
            text << now.formatted ("%H:%M:%S") << "  [CRASH] ";

            if (ep != nullptr && ep->ExceptionRecord != nullptr)
            {
                auto* r = ep->ExceptionRecord;
                text << "code=0x" << juce::String::toHexString ((juce::int64) r->ExceptionCode)
                     << " address=0x" << juce::String::toHexString ((juce::int64) (uintptr_t) r->ExceptionAddress);

                if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                    text << (r->ExceptionInformation[0] == 0 ? " (read of 0x" : r->ExceptionInformation[0] == 1 ? " (write to 0x" : " (execute at 0x")
                         << juce::String::toHexString ((juce::int64) r->ExceptionInformation[1]) << ")";

                HMODULE mod = nullptr;
                if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR) r->ExceptionAddress, &mod) && mod != nullptr)
                {
                    wchar_t path[MAX_PATH] = {};
                    GetModuleFileNameW (mod, path, MAX_PATH);
                    text << " module=" << juce::String (path);                 // <-- the DLL that crashed (plugin / driver / pt.Live)
                }
            }
            text << "\n" << juce::SystemStats::getStackBacktrace() << "\n";
        }

        // Written directly (no lock): the crashing thread may be holding the logger's lock.
        ptl::diag::getLogFile().appendText (text);

        writeDump (ep, "crash");
       #else
        juce::ignoreUnused (info);
       #endif
    }
}

namespace ptl::crash
{
    // Detects a frozen message thread ("Not Responding"). The message thread bumps a counter every
    // 500 ms; if it stops for 6 s a [HANG] line and a dump (stacks of every thread) are written once.
    // Open the .dmp in Visual Studio -> Debug with Native Only -> Threads window shows where it waits.
    class HangWatchdog : private juce::Thread, private juce::Timer
    {
    public:
        HangWatchdog() : juce::Thread ("ptLive hang watchdog") {}
        ~HangWatchdog() override { stopThread (2000); stopTimer(); }

        void start()                                     // call on the message thread
        {
            beat.store (juce::Time::getMillisecondCounter());
            startTimer (500);
            startThread (juce::Thread::Priority::low);
        }

        void setPhase (const char* p) noexcept { phase.store (p); }   // optional: what the app is doing now

    private:
        void timerCallback() override { beat.store (juce::Time::getMillisecondCounter()); }

        void run() override
        {
            bool dumped = false;

            while (! threadShouldExit())
            {
                wait (1000);
                const auto stalled = juce::Time::getMillisecondCounter() - beat.load();

                if (stalled > 6000 && ! dumped)
                {
                    dumped = true;
                    ptl::diag::getLogFile().appendText (juce::Time::getCurrentTime().formatted ("%H:%M:%S")
                        + "  [HANG] message thread has not answered for " + juce::String ((int) stalled)
                        + " ms (phase=" + juce::String (phase.load() != nullptr ? phase.load() : "?") + ")\n");
                    writeDump (nullptr, "hang");
                }
                else if (stalled < 1500)
                    dumped = false;                       // it recovered: arm again
            }
        }

        std::atomic<juce::uint32> beat { 0 };
        std::atomic<const char*> phase { nullptr };
    };

    inline HangWatchdog& hangWatchdog() { static HangWatchdog w; return w; }
}
