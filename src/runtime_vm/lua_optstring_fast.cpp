#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_index2adr.h"

extern "C" void Log(const char* fmt, ...);

typedef const char*(__cdecl *optlstr_fn)(uintptr_t L, int narg, const char* def, uint32_t* len_out);
static optlstr_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static const char* __cdecl hook(uintptr_t L, int narg, const char* def, uint32_t* len_out) {
    if (L < 0x10000 || L > 0xFFE00000) return orig(L, narg, def, len_out);
    uintptr_t tv = WowIndex2Adr(narg, L);
    if (tv < 0x10000) return orig(L, narg, def, len_out);

    int tt = *(int*)(tv + 8);
    if (tt == 4) {
        uintptr_t ts = *(uintptr_t*)(tv + 0);
        if (ts > 0x10000) {
            if (len_out) *len_out = *(uint32_t*)(ts + 16);
            g_hits++;
            return (const char*)(ts + 20);
        }
    }
    g_misses++;
    return orig(L, narg, def, len_out);
}

bool InstallLuaOptstringFast() {
    void* t = (void*)0x0084FA50;
    if (WineSafe_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    if (WO_EnableHook(t) != MH_OK) return false;
    Log("[OptStr] ACTIVE — luaL_optlstring inline at 0x84FA50");
    CrashDumper::RegisterFeature("OptStr");
    CrashDumper::FeatureSetActive("OptStr", true);
    return true;
}
