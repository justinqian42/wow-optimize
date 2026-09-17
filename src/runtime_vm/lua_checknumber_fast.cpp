#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_checknumber at 0x84FAB0 — validates Lua stack value is a number.
// 60 static callers, called millions/sec under ElvUI/WeakAuras.
// Calls lua_tonumber (already hooked) → if result != 0, it's a number.
// Only calls lua_isnumber (slow) when lua_tonumber returns 0 (checking
// whether the value is actually 0 vs not-a-number).

typedef double(__cdecl *check_fn)(uintptr_t L, int idx);
static check_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static double __cdecl hook(uintptr_t L, int idx) {
    typedef double(__cdecl *tonumber_fn)(uintptr_t, int);
    double d = ((tonumber_fn)0x0084E030)(L, idx);
    if (d != 0.0) {
        g_hits++;
        return d;
    }
    // Check if it's really 0 or if it's not a number
    typedef int(__cdecl *isnumber_fn)(uintptr_t, int);
    if (((isnumber_fn)0x0084DF20)(L, idx)) {
        g_hits++;
        return d;
    }
    // Note: type mismatch is normal WoW engine behavior. No logging - it caused
    // synchronous disk flushes on every miss, stalling the main thread.
    g_misses++;
    return orig(L, idx);  // Raise error
}

bool InstallLuaCheckNumberFast() {
    void* t = (void*)0x0084FAB0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[CheckNum] ACTIVE — luaL_checknumber inline at 0x84FAB0");
    CrashDumper::RegisterFeature("CheckNum");
    CrashDumper::FeatureSetActive("CheckNum", true);
    return true;
}
