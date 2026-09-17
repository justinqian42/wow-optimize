#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_buffinit at 0x84F6A0 — initialize string buffer.
// Sets B->L = L, B->p = B->buffer (B+12), B->lvl = 0.
// Fast path: direct struct field writes, bypassing function call overhead.

typedef uintptr_t(__cdecl *buffinit_fn)(uintptr_t L, uintptr_t B);
static buffinit_fn orig = nullptr;
static volatile long g_hits = 0;

static uintptr_t __cdecl hook(uintptr_t L, uintptr_t B) {
    if (L > 0x10000 && L < 0xFFE00000 && B > 0x10000 && B < 0xFFE00000) {
        __try {
            *(uintptr_t*)(B + 8) = L;
            *(uintptr_t*)(B) = B + 12;
            *(uint32_t*)(B + 4) = 0;
            g_hits++;
            return B;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return orig(L, B);
}

bool InstallLuaBuffinitFast() {
    void* t = (void*)0x0084F6A0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[BufInit] ACTIVE — luaL_buffinit inline at 0x84F6A0");
    CrashDumper::RegisterFeature("BufInit");
    CrashDumper::FeatureSetActive("BufInit", true);
    return true;
}
