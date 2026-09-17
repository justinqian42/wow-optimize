#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_index2adr.h"

extern "C" void Log(const char* fmt, ...);

typedef double(__cdecl *optnum_fn)(uintptr_t L, int narg, double def);
static optnum_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static double __cdecl hook(uintptr_t L, int narg, double def) {
    if (L < 0x10000 || L > 0xFFE00000) return orig(L, narg, def);
    uintptr_t tv = WowIndex2Adr(narg, L);
    if (tv < 0x10000) return orig(L, narg, def);

    int tt = *(int*)(tv + 8);
    if (tt == 3) {
        double d = *(double*)(tv + 0);
        g_hits++;
        return d;
    }
    g_misses++;
    return orig(L, narg, def);
}

bool InstallLuaOptnumberFast() {
    void* t = (void*)0x0084FB30;
    if (WineSafe_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    if (WO_EnableHook(t) != MH_OK) return false;
    Log("[OptNum] ACTIVE — luaL_optnumber inline at 0x84FB30");
    CrashDumper::RegisterFeature("OptNum");
    CrashDumper::FeatureSetActive("OptNum", true);
    return true;
}
