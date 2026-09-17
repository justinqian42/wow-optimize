#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "texcache_tuning.h"
#include "memory_pressure_governor.h"

extern "C" void Log(const char* fmt, ...);
extern bool g_isMultiClient;

// HIGH dword of qword_B49C98 = resident-texture byte budget.
static int* const TEXCACHE_BUDGET = (int*)0x00B49C9C;

// WoW's modern default cap; we only act on a value at or below this so we never
// fight a deliberately-smaller budget or stomp the 0 (null-device) case.
static const int STOCK_MAX_BYTES = 0x04000000;   // 64 MB

// Target resident budget, chosen at init from available 32-bit VA. A larger
// resident set keeps an area's textures from being evicted/reloaded, which cuts
// both stutter and the large-block-heap fragmentation that churn causes -- but it
// holds that much VA. With /3GB (bcdedit /set increaseuserva 3072) there is room
// for 256MB; on a stock 2GB user-VA layout we keep it to 192MB.
// Lowered from 256/192 MB: on VA-tight HD clients the larger resident set
// enlarged the working set that Windows trims on alt-tab, so faulting it back
// stalled the main thread. A smaller budget trades a little eviction churn for
// much less paging pressure.
static const int TARGET_3GB_BYTES = 0x08000000;  // 128 MB (increaseuserva active)
static const int TARGET_2GB_BYTES = 0x06000000;  //  96 MB (stock 2GB user VA)
static int g_targetBytes = TARGET_2GB_BYTES;

static bool g_logged = false;

// True when the OS hands this 32-bit process >2GB of usable user address space.
// lpMaximumApplicationAddress alone is not reliable: it can report a high
// architectural ceiling (~0xBFFEFFFF/0xFFFEFFFF) that the process cannot actually
// map -- a tester's log showed this field reading high while a real reservation at
// 0x80000000 failed and the client ran out of VA at ~2GB. Over-budgeting resident
// textures there enlarges the working set Windows trims on a world transition, so
// faulting it back stalls the main thread. Gate on the cheap field, then CONFIRM
// with a real reservation in the >2GB region (matches the MEMORY_OPT probe).
static bool Has3GBUserVA() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if ((uintptr_t)si.lpMaximumApplicationAddress <= 0x80000000) return false;
    void* probe = VirtualAlloc((void*)0x80000000, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    if (!probe) return false;
    VirtualFree(probe, 0, MEM_RELEASE);
    return true;
}

static void RaiseBudget() {
    __try {
        int cur = *TEXCACHE_BUDGET;
        // Act only once WoW has installed its sane default (>0 and <= stock cap).
        // After we raise it to TARGET (> STOCK_MAX) this test is false, so we stay
        // quiet until a device reset puts the stock 64 MB back -- self-throttling.
        if (cur > 0 && cur <= STOCK_MAX_BYTES) {
            *TEXCACHE_BUDGET = g_targetBytes;
            if (!g_logged) {
                Log("[TexCache] Resident-texture budget %dMB -> %dMB (single-client; eviction logic unchanged)",
                    cur >> 20, g_targetBytes >> 20);
                g_logged = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void TexCache_SetBudget(int bytes) {
    __try {
        *TEXCACHE_BUDGET = bytes;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int TexCache_GetConfiguredTarget() {
    return g_targetBytes;
}

void InitTexCacheTuning() {
    if (g_isMultiClient) {
        Log("[TexCache] Budget tuning OFF (multi-client: preserve stock budget for VA headroom)");
        return;
    }
    bool va3 = Has3GBUserVA();
    g_targetBytes = va3 ? TARGET_3GB_BYTES : TARGET_2GB_BYTES;
    Log("[TexCache] Resident-texture budget tuning armed (target %dMB, %s, re-asserted after device resets)",
        g_targetBytes >> 20, va3 ? "/3GB user VA detected" : "stock 2GB user VA");
    RaiseBudget();   // apply now if the cache is already initialized
}

void TexCacheTuning_Tick() {
    if (g_isMultiClient) return;
    // Only raise/re-assert the texture budget if the memory pressure is GREEN.
    // If it is YELLOW or RED, let the dropped budget persist to free up VA space.
    if (PressureGovernor::GetLevel() == PressureGovernor::PRESSURE_GREEN) {
        RaiseBudget();   // cheap: usually reads one int and returns (see self-throttle note)
    }
}
