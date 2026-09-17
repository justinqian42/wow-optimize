#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_isnumber at 0x84DF20 — check if value is a number or convertible.
// Engine: index2adr → tt==3(LUA_TNUMBER)?1 : sub_856E50(try conversion)?1:0.
// Fast path: direct tt==3 match; defer conversions to original.

typedef int (__cdecl *isnumber_fn)(uintptr_t L, int idx);
static isnumber_fn orig = nullptr;
static volatile long g_hits = 0;

#define LUA_TNUMBER 3
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

static int __cdecl hook(uintptr_t L, int idx) {
    if (L < 0x10000 || L > 0xFFE00000)
        return orig(L, idx);
    __try {
        if (idx < 0 && idx > -10000) {
            uintptr_t tv = index2adr(L, idx);
            if (tv && tv != NIL_ADDR) {
                if (*(int*)(tv + 8) == LUA_TNUMBER) {
                    g_hits++;
                    return 1;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig(L, idx);
}

bool InstallLuaIsNumberFast() {
    void* t = (void*)0x0084DF20;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[IsNumber] ACTIVE — lua_isnumber inline at 0x84DF20");
    CrashDumper::RegisterFeature("IsNumber");
    CrashDumper::FeatureSetActive("IsNumber", true);
    return true;
}
