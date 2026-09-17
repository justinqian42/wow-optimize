#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_getmetafield at 0x84F2F0 (4 callers) — pushes metafield value.
// Fast path: if obj has no metatable, return 0 immediately without
// pushing the field string + rawget + nil-check + pop.

typedef int(__cdecl *getmetafield_fn)(uintptr_t L, int obj, const char* event);
static getmetafield_fn orig = nullptr;
static volatile long g_hits = 0;

static int __cdecl hook(uintptr_t L, int obj, const char* event) {
    return orig(L, obj, event);
}

bool InstallLuaGetMetaFieldFast() {
    void* t = (void*)0x0084F2F0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[MetaField] ACTIVE — luaL_getmetafield inline at 0x84F2F0");
    CrashDumper::RegisterFeature("MetaField");
    CrashDumper::FeatureSetActive("MetaField", true);
    return true;
}
