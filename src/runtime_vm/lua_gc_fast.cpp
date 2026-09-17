#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_gc at 0x84ED50 (6 callers) — GC control.
// LUA_GCSTEP (what=5) with data<=0 is the hot path (addons polling GC state).
// Fast return: skip the GC step call when no work is requested.

typedef int(__cdecl *gc_fn)(uintptr_t L, int what, int data);
static gc_fn orig = nullptr;
static volatile long g_hits = 0;

static int __cdecl hook(uintptr_t L, int what, int data) {
    if (what == 5 && data <= 0) { g_hits++; return 1; }
    return orig(L, what, data);
}

bool InstallLuaGCFast() {
    void* t = (void*)0x0084ED50;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[GCFast] ACTIVE — lua_gc inline at 0x84ED50");
    CrashDumper::RegisterFeature("GCFast");
    CrashDumper::FeatureSetActive("GCFast", true);
    return true;
}
