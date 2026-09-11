// ============================================================================
// Module: freeze_catcher
// Description: Samples the main thread only while a frame is already long, so
//              a freeze says what it was doing.
// Safety & Threading: A watchdog thread; the main thread is touched only during
//                     a frame that has already overrun.
// ============================================================================
//
// The worst thing in the field data is a frame that took 1918 ms - three hundred
// and forty-three times the median - and this is the whole of what the log could
// say about it:
//
//     slow frame: 1918.3 ms (342.6x the 5.60 ms median) - events within it:
//     (nothing traced in this window)
//
// The flight recorder has columns for archive opens, our own hooks, file reads,
// file writes and socket receives, and not one of them moved. The same session
// has a 1068 ms frame, an 880 ms frame and a hundred and forty frames over a
// hundred milliseconds. Whatever they are, they are CPU work nobody instruments,
// and they are the only thing in this project a player can actually feel.
//
// The sampling profiler could answer it and is switched off in every field log,
// because sampling a thousand times a second all session is a real cost and one
// reporter traced longer loading screens to having it on. Their conclusion was
// right for a profiler and wrong for this question: nothing needs sampling while
// frames are fine.
//
// So this watches instead. A thread wakes every few milliseconds, reads how long
// the current frame has been running, and does nothing at all unless that is
// already past the arm threshold. Inside a frame that has overrun it samples
// hard, and when the frame finally ends it prints where the main thread was.
// Normal frames cost one clock read and one comparison per wake-up, on a thread
// that is asleep the rest of the time; the main thread is never suspended during
// a frame that is behaving.
//
// ---------------------------------------------------------------------------
// The stamp, and why it is milliseconds
//
// The watchdog has to read the frame's start from another thread. A 64-bit QPC
// value read across threads on x86 can tear between its halves, and a torn
// start time would arm the catcher inside a frame that is fine or hide one that
// is not. A 32-bit millisecond count cannot tear on this architecture, and a
// millisecond is four hundred times finer than the events being caught.
//
// ---------------------------------------------------------------------------
// What it prints
//
// The addresses, grouped, most-sampled first. Not symbols: this project reads
// wow.exe in IDA and an address is what it wants - the profiler's own reports
// name their hottest entries the same way, and the one that found the UI layout
// relink at 9% did it by printing wow!0x00489763 and nothing else.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "freeze_catcher.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

namespace FreezeCatcher {

namespace {

// A frame past this is not a frame any more, it is a stall worth explaining.
// The field median is 5.6 to 9 ms and the events being caught are 100 ms and up.
constexpr long kArmMs   = 60;
constexpr DWORD kWakeMs = 4;     // how often the watchdog looks
constexpr DWORD kFastMs = 1;     // how often it samples once armed

constexpr int kRing = 512;

HANDLE g_main    = nullptr;
HANDLE g_thread  = nullptr;
volatile LONG g_running = 0;

// Milliseconds since init, stamped by the main thread at every frame boundary.
// 32-bit so a cross-thread read cannot tear.
volatile LONG g_frameStartMs = 0;
LARGE_INTEGER g_freq = {};
LARGE_INTEGER g_base = {};

uintptr_t g_eip[kRing];
long      g_atMs[kRing];
volatile LONG g_count = 0;       // samples in the current armed window

// Totals for the report.
unsigned long g_frames   = 0;
unsigned long g_caught   = 0;    // frames that armed it
unsigned long g_samples  = 0;
unsigned long g_worstMs  = 0;
unsigned long g_wakes    = 0;

long NowMs() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    if (!g_freq.QuadPart) return 0;
    return (long)(((n.QuadPart - g_base.QuadPart) * 1000) / g_freq.QuadPart);
}

const char* Where(uintptr_t a) {
    if (a >= 0x00400000u && a <= 0x00BFFFFFu) return "wow";
    if (a >= 0x10000000u && a <= 0x11000000u) return "wowopt";
    return "other";
}

DWORD WINAPI WatchdogProc(LPVOID) {
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        const long start = g_frameStartMs;
        const long age   = NowMs() - start;
        ++g_wakes;

        if (age < kArmMs) {
            Sleep(kWakeMs);
            continue;
        }

        // This frame has already overrun. Sample it until it ends or the ring
        // fills; the main thread is only suspended from here.
        while (InterlockedCompareExchange(&g_running, 1, 1) &&
               g_frameStartMs == start) {
            const long at = NowMs() - start;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(g_main) != (DWORD)-1) {
                uintptr_t eip = 0;
                if (GetThreadContext(g_main, &ctx)) eip = (uintptr_t)ctx.Eip;
                ResumeThread(g_main);
                const LONG i = InterlockedIncrement(&g_count) - 1;
                if (i < kRing && eip) { g_eip[i] = eip; g_atMs[i] = at; }
                else if (i >= kRing) break;
            }
            Sleep(kFastMs);
        }
    }
    return 0;
}

}  // namespace

void OnFrame() {
    ++g_frames;

    const long now = NowMs();
    const long was = g_frameStartMs;
    const long len = now - was;

    // Stamp the new frame BEFORE reading the ring, not after. The watchdog's
    // inner loop runs while g_frameStartMs still equals the frame it armed on,
    // so leaving the stamp until the end of this function meant it went on
    // writing samples into the ring being read here. One statement, and the
    // only place in this module where two threads touch the same memory.
    g_frameStartMs = now;

    // The frame that just ended. If the watchdog armed on it, say what it saw.
    const LONG n = InterlockedExchange(&g_count, 0);
    if (n > 0 && len >= kArmMs) {
        ++g_caught;
        g_samples += (unsigned long)(n < kRing ? n : kRing);
        if ((unsigned long)len > g_worstMs) g_worstMs = (unsigned long)len;

        const int take = (int)(n < kRing ? n : kRing);
        Log("[FreezeCatcher] a frame of %ld ms, %d sample(s) of where the main "
            "thread was while it ran:", len, take);

        // Group by address without sorting in place - the ring is small and this
        // runs once per caught frame, not per sample.
        bool done[kRing];
        memset(done, 0, sizeof(done));
        for (int printed = 0; printed < 8; ++printed) {
            int best = -1, bestCount = 0;
            for (int i = 0; i < take; ++i) {
                if (done[i]) continue;
                int c = 0;
                for (int j = 0; j < take; ++j)
                    if (!done[j] && g_eip[j] == g_eip[i]) ++c;
                if (c > bestCount) { bestCount = c; best = i; }
            }
            if (best < 0) break;
            Log("[FreezeCatcher]   %s!0x%08X  %d sample(s), first at %ld ms in",
                Where(g_eip[best]), (unsigned)g_eip[best], bestCount,
                g_atMs[best]);
            for (int j = 0; j < take; ++j)
                if (g_eip[j] == g_eip[best]) done[j] = true;
        }
    }
}

bool Init(HANDLE mainThread) {
    if (!Config::g_settings.OptFreezeCatcher) return true;
    if (!mainThread) {
        Log("[FreezeCatcher] NOT active: no handle to the main thread.");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_base);
    if (!g_freq.QuadPart) {
        Log("[FreezeCatcher] NOT active: no performance counter.");
        return false;
    }

    // The caller closes its handle as soon as Init returns, the way it does
    // for the sampling profiler, so this keeps its own.
    if (!DuplicateHandle(GetCurrentProcess(), mainThread, GetCurrentProcess(),
                         &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log("[FreezeCatcher] NOT active: could not duplicate the main thread "
            "handle (error %lu).", GetLastError());
        return false;
    }
    g_frameStartMs = NowMs();
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        Log("[FreezeCatcher] NOT active: the watchdog thread would not start.");
        return false;
    }

    Log("[FreezeCatcher] ACTIVE. The worst frame in the field data is 1918 ms "
        "and the log's whole account of it is \"nothing traced in this window\" "
        "- the flight recorder's columns are archive opens, our hooks, file "
        "reads and writes and socket receives, and none of them moved. This "
        "watches instead of sampling: a thread wakes every %u ms, and does "
        "nothing until the frame in progress is already past %ld ms. Only then "
        "is the main thread touched, and only until that frame ends. A frame "
        "that behaves costs one clock read and one comparison on a sleeping "
        "thread.", kWakeMs, kArmMs);
    return true;
}

void Shutdown() {
    InterlockedExchange(&g_running, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

void LogStats() {
    if (!Config::g_settings.OptFreezeCatcher) return;
    if (!g_thread) {
        Log("[FreezeCatcher] switched on but not watching, so nothing here was "
            "measured.");
        return;
    }
    if (g_caught == 0) {
        Log("[FreezeCatcher] %lu frames watched over %lu wake-ups and none of "
            "them ran past %ld ms. Measured and zero: there was nothing to "
            "catch, not nothing looking.", g_frames, g_wakes, kArmMs);
        return;
    }
    Log("[FreezeCatcher] %lu frames watched, %lu of them ran past %ld ms and "
        "were sampled, %lu samples in total, worst frame %lu ms. Each one is "
        "printed above with the addresses it was caught at.",
        g_frames, g_caught, kArmMs, g_samples, g_worstMs);
}

}  // namespace FreezeCatcher
