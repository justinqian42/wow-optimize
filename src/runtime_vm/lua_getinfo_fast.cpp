#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// Correct signature: luaL_findfield(L, idx, field, level)
typedef char*(__cdecl *getinfo_fn)(uintptr_t L, int idx, const char* field, int level);
static getinfo_fn orig = nullptr;
static volatile long g_calls = 0;

static char* __cdecl hook(uintptr_t L, int idx, const char* field, int level) {
    g_calls++;
    return orig(L, idx, field, level);
}

bool InstallLuaGetInfoFast() {
    void* t = (void*)0x0084F3B0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[GetInfo] ACTIVE — luaL_findfield inline at 0x84F3B0");
    CrashDumper::RegisterFeature("GetInfo");
    CrashDumper::FeatureSetActive("GetInfo", true);
    return true;
}
