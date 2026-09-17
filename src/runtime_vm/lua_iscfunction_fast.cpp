#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_iscfunction at 0x84DEF0 — check if value is a C function.
// Engine: index2adr → tt==6(LUA_TFUNCTION) && gcobj[10](isC flag).
// Fast path: direct TValue resolution + type/flag check.

typedef int (__cdecl *iscfunction_fn)(uintptr_t L, int idx);
static iscfunction_fn orig = nullptr;
static volatile long g_hits = 0;

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

static int __cdecl hook(uintptr_t L, int idx) {
    if (L < 0x10000 || L > 0xFFE00000)
        return orig(L, idx);
    __try {
        if (idx < 0 && idx > -10000) {
            uintptr_t tv = index2adr(L, idx);
            if (tv && tv != NIL_ADDR) {
                int tt = *(int*)(tv + 8);
                if (tt == LUA_TFUNCTION) {
                    uintptr_t gc = *(uintptr_t*)(tv + 0);
                    if (gc > 0x10000) {
                        unsigned char isC = *(unsigned char*)(gc + 10);
                        g_hits++;
                        return isC ? 1 : 0;
                    }
                }
                return 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig(L, idx);
}

bool InstallLuaIsCFuncFast() {
    void* t = (void*)0x0084DEF0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[IsCFunc] ACTIVE — lua_iscfunction inline at 0x84DEF0");
    CrashDumper::RegisterFeature("IsCFunc");
    CrashDumper::FeatureSetActive("IsCFunc", true);
    return true;
}
