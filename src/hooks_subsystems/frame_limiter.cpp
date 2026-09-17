// ============================================================================
// Description: Custom high-precision hybrid frame rate limiter.
// Safety & Threading: Thread-safe. Integrates with the main render thread.
// ============================================================================

#include "frame_limiter.h"
#include "MinHook.h"
#include "version.h"
#include <windows.h>

extern "C" void Log(const char* fmt, ...);
extern "C" void WowOpt_MainThreadPump();
extern "C" void ProcessDeferredGC(double idleBudgetMs);

namespace FrameLimiter {

thread_local bool g_bypassSleep = false;

LARGE_INTEGER g_frameStartQpc = {0};
double g_ticksPerSec = 0.0;

double GetActiveFrameElapsedTime() {
    if (g_ticksPerSec == 0.0) return 0.0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_frameStartQpc.QuadPart) / g_ticksPerSec;
}

typedef unsigned int (__thiscall *EngineFrameLimit_fn)(void* This);
static EngineFrameLimit_fn orig_EngineFrameLimit = nullptr;

typedef void (__cdecl *SleepHelper_fn)(DWORD ms);
static SleepHelper_fn orig_SleepHelper = nullptr;

// Detour for sub_86B280
void __cdecl Hooked_SleepHelper(DWORD ms) {
    if (g_bypassSleep) {
        return; // Bypass original sleep inside the frame limiter
    }
    orig_SleepHelper(ms);
}

// One timer per thread, created once and reused. CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
// arrived in Windows 10 1803; on anything older, and on Wine builds that do not
// implement it, the flag makes the call fail and we fall back to a plain timer,
// then to Sleep. A null return is a supported outcome, not an error worth logging
// every frame.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static HANDLE AcquireFrameTimer() {
    static __declspec(thread) HANDLE t_timer = NULL;
    static __declspec(thread) bool    t_tried = false;

    if (!t_tried) {
        t_tried = true;
        t_timer = CreateWaitableTimerExW(NULL, NULL,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
        if (!t_timer) {
            t_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        Log("[FrameLimiter] Wait mode: %s",
            t_timer ? "waitable timer" : "Sleep (no timer available)");
    }
    return t_timer;
}

// Detour for sub_6836D0
unsigned int __fastcall Hooked_EngineFrameLimit(void* This, void* unused) {
    if (!This) {
        return orig_EngineFrameLimit(This);
    }

#if !TEST_DISABLE_FRAME_LIMITER
    // Everything this DLL does per frame - the liveness stamp the freeze watchdog
    // reads, periodic maintenance, dropping caches on a lua_State change, every
    // subsystem's OnFrame - hangs off this call. It must stay here, at the top,
    // before anything can return: put it beside the wait instead and it sits
    // behind two exits, one when no FPS cap is set and one whenever a frame
    // overruns its budget, so for those configurations it never runs at all.
    //
    // This function is the client's own per-frame limiter, so it runs once a
    // frame whatever it decides to do about pacing. The eight-millisecond gate
    // inside the pump is what keeps two callers from doing the work twice.
    WowOpt_MainThreadPump();

    typedef int (__cdecl *GetLimit_fn)();
    GetLimit_fn GetFGLimit = (GetLimit_fn)0x00681780;
    GetLimit_fn GetBGLimit = (GetLimit_fn)0x006817A0;

    unsigned int fg = GetFGLimit();
    unsigned int bg = GetBGLimit();

    unsigned int limit = fg ? fg : -1;
    unsigned int bgLimit = bg ? bg : -1;

    // Check background state. Offset 3940 bytes from This is the active/foreground bool (985 * 4 = 3940)
    bool isBackground = !*((unsigned char*)This + 3940);
    if (isBackground && limit >= bgLimit) {
        limit = bgLimit;
    }

    if (limit <= 8 && limit != -1) {
        limit = 8;
    }

    if (limit == -1) {
        // No limit: bypass high precision wait
        g_bypassSleep = false;
        return orig_EngineFrameLimit(This);
    }

    double targetDuration = 1.0 / limit;

    if (g_ticksPerSec == 0.0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_ticksPerSec = (double)freq.QuadPart;
        QueryPerformanceCounter(&g_frameStartQpc);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - g_frameStartQpc.QuadPart) / g_ticksPerSec;
    double remaining = targetDuration - elapsed;

    if (remaining > 0.0) {
        // Run deferred GC sweeps during remaining idle time, leaving 0.5ms margin
        if (remaining > 0.001) {
            ProcessDeferredGC((remaining - 0.0005) * 1000.0);
            
            // Re-evaluate remaining time
            QueryPerformanceCounter(&now);
            elapsed = (double)(now.QuadPart - g_frameStartQpc.QuadPart) / g_ticksPerSec;
            remaining = targetDuration - elapsed;
        }

        LARGE_INTEGER targetTime;
        targetTime.QuadPart = g_frameStartQpc.QuadPart + (LONGLONG)(targetDuration * g_ticksPerSec);

        // How much of the frame is spent spinning rather than sleeping. Keep it
        // small: the spin is entered every frame, so at a 200 FPS cap a 1.5 ms
        // margin burns 1.5 ms of every 5 in a loop that does nothing, which one
        // tester saw as CPU use going from about 15% to about 50%.
        //
        // SwitchToThread does not help. It yields only to a thread already
        // runnable on the same processor and returns immediately when there is
        // none, so with an idle core the loop is a plain busy-wait.
        //
        // A high-resolution waitable timer does the waiting in the kernel and
        // wakes within a few hundred microseconds, so the spin only has to cover
        // what it cannot. Where the timer is unavailable the old Sleep path
        // remains, with a margin sized to Sleep's actual granularity.
        static const double SPIN_MARGIN_SEC = 0.00025;   // 250 us

        double toWait = (double)(targetTime.QuadPart - now.QuadPart) / g_ticksPerSec;
        if (toWait > SPIN_MARGIN_SEC) {
            double sleepSec = toWait - SPIN_MARGIN_SEC;
            HANDLE timer = AcquireFrameTimer();
            if (timer) {
                LARGE_INTEGER due;
                // Negative means relative, in 100 ns units.
                due.QuadPart = -(LONGLONG)(sleepSec * 10000000.0);
                if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                    // Bounded, never INFINITE. This runs on the main thread, and
                    // an unbounded wait there means that if the timer ever fails
                    // to signal - a driver quirk, an odd Wine build, the handle
                    // being interfered with - the game stops for good rather than
                    // for a frame. The cap is generous next to the longest wait
                    // that can legitimately be asked for here, which is one frame
                    // at the eight FPS floor enforced above, so overshooting it
                    // costs the pacing of a single frame and nothing else.
                    DWORD capMs = (DWORD)(sleepSec * 1000.0) + 50;
                    if (capMs > 250) capMs = 250;
                    WaitForSingleObject(timer, capMs);
                }
            } else if (sleepSec > 0.001) {
                Sleep((DWORD)(sleepSec * 1000.0));
            }
        }

        // Whatever is left after the wait, and it should now be under a
        // millisecond. Pause rather than SwitchToThread: it does not enter the
        // kernel and it tells the core this is a spin, which on a hyperthreaded
        // part lets the sibling have the pipeline.
        while (true) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= targetTime.QuadPart) break;
            YieldProcessor();
        }
        g_frameStartQpc = targetTime;
    } else {
        if (remaining < -0.1) {
            g_frameStartQpc = now;
        } else {
            g_frameStartQpc.QuadPart += (LONGLONG)(targetDuration * g_ticksPerSec);
        }
    }
#endif

    // Run original frame limit updates with internal sleep bypassed
    g_bypassSleep = true;
    unsigned int ret = orig_EngineFrameLimit(This);
    g_bypassSleep = false;

    return ret;
}

bool Init() {
#if !TEST_DISABLE_FRAME_LIMITER
    void* target_limit = (void*)0x006836D0;
    void* target_sleep = (void*)0x0086B280;

    if (MH_CreateHook(target_limit, (void*)Hooked_EngineFrameLimit, (void**)&orig_EngineFrameLimit) != MH_OK ||
        MH_CreateHook(target_sleep, (void*)Hooked_SleepHelper, (void**)&orig_SleepHelper) != MH_OK) 
    {
        Log("[FrameLimiter] Failed to create hooks");
        return false;
    }

    MH_EnableHook(target_limit);
    MH_EnableHook(target_sleep);

    Log("[FrameLimiter] Active - High-precision hybrid frame rate limiter enabled");
#endif
    return true;
}

void Shutdown() {
#if !TEST_DISABLE_FRAME_LIMITER
    MH_DisableHook((void*)0x006836D0);
    MH_DisableHook((void*)0x0086B280);
#endif
}

} // namespace FrameLimiter
