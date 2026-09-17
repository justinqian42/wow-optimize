#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// Correct 3-argument signature: (lua_State* L, const lua_Debug* ar, int n)
typedef const char*(__cdecl *getlocal_fn)(uintptr_t L, void* ar, int n);
static getlocal_fn orig = nullptr;
static volatile long g_calls = 0;

static const char* __cdecl hook(uintptr_t L, void* ar, int n) {
    g_calls++;
    return orig(L, ar, n);
}

bool InstallLuaGetLocalFast() {
    void* t = (void*)0x0084FF30;
    if (MH_CreateHook(t, (void*)hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[GetLocal] ACTIVE — lua_getlocal inline at 0x84FF30");
    CrashDumper::RegisterFeature("GetLocal");
    CrashDumper::FeatureSetActive("GetLocal", true);
    return true;
}

