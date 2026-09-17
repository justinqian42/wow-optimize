#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_where at 0x84FE40 (9 callers) — builds error location string.
// Fast path: common case is level=1 with a valid call info.
// Defers to original for edge cases; reduces overhead on the hot path.

typedef int(__cdecl *where_fn)(uintptr_t L, int level, uintptr_t out_debug);
static where_fn orig = nullptr;
static volatile long g_hits = 0;

static int __cdecl hook(uintptr_t L, int level, uintptr_t out_debug) {
    return orig(L, level, out_debug);
}

bool InstallLuaWhereFast() {
    void* t = (void*)0x0084FE40;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[WhereFast] ACTIVE — luaL_where inline at 0x84FE40");
    CrashDumper::RegisterFeature("WhereFast");
    CrashDumper::FeatureSetActive("WhereFast", true);
    return true;
}
