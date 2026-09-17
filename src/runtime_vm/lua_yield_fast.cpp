#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_yield at 0x856150 — yield coroutine execution.
// Engine: check nCcalls at L+56 → error; set L->base=L->top-nresults*16;
// set L->status=1(LUA_YIELD); return -1.
// Fast path: direct struct-field writes, identical to engine.

typedef int (__cdecl *yield_fn)(uintptr_t L, int nresults);
static yield_fn orig = nullptr;
static volatile long g_hits = 0;

static int __cdecl hook(uintptr_t L, int nresults) {
    if (L < 0x10000 || L > 0xFFE00000)
        return orig(L, nresults);
    __try {
        if (*(unsigned short*)(L + 56)) {
            return orig(L, nresults);
        }
        *(uintptr_t*)(L + 16) = *(uintptr_t*)(L + 12) - (uintptr_t)(16 * nresults);
        *(unsigned char*)(L + 10) = 1;
        g_hits++;
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig(L, nresults);
}

bool InstallLuaYieldFast() {
    void* t = (void*)0x00856150;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[YieldFast] ACTIVE — lua_yield inline at 0x856150");
    CrashDumper::RegisterFeature("YieldFast");
    CrashDumper::FeatureSetActive("YieldFast", true);
    return true;
}
