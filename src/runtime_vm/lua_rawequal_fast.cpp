#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_rawequal at 0x84DF90 — compare two values without metamethods.
// Engine: index2adr ×2 → nil-guard → sub_84D420(compare).
// Fast path: direct tt+value compare for simple types; fallback for tables/userdata.

typedef int (__cdecl *rawequal_fn)(uintptr_t L, int a, int b);
static rawequal_fn orig = nullptr;
static volatile long g_hits = 0;

#define LUA_TNIL      0
#define LUA_TBOOLEAN  1
#define LUA_TNUMBER   3
#define LUA_TSTRING   4
#define LUA_TFUNCTION 6
#define NIL_ADDR 0x00A46F78

static inline uintptr_t index2adr(uintptr_t L, int idx) {
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (base < 0x10000) return 0;
        uintptr_t tv = base + (idx - 1) * 16;
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (tv >= top) return 0;
        return tv;
    }
    if (idx < 0 && idx > -10000) {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (top < 0x10000) return 0;
        uintptr_t tv = top + idx * 16;
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (tv < base) return 0;
        return tv;
    }
    return 0;
}

static int __cdecl hook(uintptr_t L, int a, int b) {
    if (L < 0x10000 || L > 0xFFE00000)
        return orig(L, a, b);
    __try {
        if (a < 0 && a > -10000 && b < 0 && b > -10000) {
            uintptr_t tv1 = index2adr(L, a);
            uintptr_t tv2 = index2adr(L, b);
            if (tv1 && tv2 && tv1 != NIL_ADDR && tv2 != NIL_ADDR) {
                int tt1 = *(int*)(tv1 + 8);
                int tt2 = *(int*)(tv2 + 8);
                if (tt1 != tt2)
                    return 0;
                if (tt1 == LUA_TNIL || tt1 == LUA_TBOOLEAN || tt1 == LUA_TNUMBER) {
                    g_hits++;
                    return *(uintptr_t*)(tv1 + 0) == *(uintptr_t*)(tv2 + 0);
                }
                if (tt1 == LUA_TSTRING || tt1 == LUA_TFUNCTION) {
                    g_hits++;
                    return (*(uintptr_t*)(tv1 + 0) == *(uintptr_t*)(tv2 + 0)) ? 1 : 0;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig(L, a, b);
}

bool InstallLuaRawEqualFast() {
    void* t = (void*)0x0084DF90;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[RawEqual] ACTIVE — lua_rawequal inline at 0x84DF90");
    CrashDumper::RegisterFeature("RawEqual");
    CrashDumper::FeatureSetActive("RawEqual", true);
    return true;
}
