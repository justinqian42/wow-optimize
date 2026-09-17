#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_getupvalue at 0x84F150 — gets closure upvalue.
// Fast path: direct upvalue array access, bypassing index2adr + type checks.
// For C closures: reads c.upvals[n-1] at cl+16*(n+1).
// For Lua closures: reads upvals[n-1]->v at cl+4*n+24+12.

typedef const char*(__cdecl *getupvalue_fn)(uintptr_t L, int funcindex, int n);
static getupvalue_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static const char* __cdecl hook(uintptr_t L, int funcindex, int n) {
    if (L < 0x10000 || L > 0xFFE00000 || n < 1 || n > 255) {
        g_misses++;
        return orig(L, funcindex, n);
    }
    __try {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (base < 0x10000 || top < 0x10000 || base > 0xFFE00000 || top > 0xFFE00000) {
            g_misses++;
            return orig(L, funcindex, n);
        }
        if (funcindex < 0) {
            uintptr_t tv = top + 16 * funcindex;
            if (tv < base || tv >= top) {
                g_misses++;
                return orig(L, funcindex, n);
            }
            uint32_t tt = *(uint32_t*)(tv + 8);
            if (tt != 6) {
                g_misses++;
                return orig(L, funcindex, n);
            }
            uintptr_t cl = *(uintptr_t*)(tv);
            if (!cl || cl < 0x10000 || cl > 0xFFE00000) {
                g_misses++;
                return orig(L, funcindex, n);
            }
            uint8_t isC = *(uint8_t*)(cl + 10);
            uintptr_t val_ptr;
            const char* name;
            if (isC) {
                uint8_t nupvalues = *(uint8_t*)(cl + 11);
                if (n < 1 || n > nupvalues) {
                    g_misses++;
                    return orig(L, funcindex, n);
                }
                val_ptr = cl + 16 * (n + 1);
                name = "";  // C closures have empty name
            } else {
                uintptr_t proto = *(uintptr_t*)(cl + 24);
                if (!proto || proto < 0x10000 || proto > 0xFFE00000) {
                    g_misses++;
                    return orig(L, funcindex, n);
                }
                int32_t nups = *(int32_t*)(proto + 40);
                if (n < 1 || n > nups) {
                    g_misses++;
                    return orig(L, funcindex, n);
                }
                uintptr_t upval = *(uintptr_t*)(cl + 4 * n + 24);
                if (!upval || upval < 0x10000 || upval > 0xFFE00000) {
                    g_misses++;
                    return orig(L, funcindex, n);
                }
                val_ptr = *(uintptr_t*)(upval + 12);
                if (!val_ptr || val_ptr < 0x10000 || val_ptr > 0xFFE00000) {
                    g_misses++;
                    return orig(L, funcindex, n);
                }
                uintptr_t ts = *(uintptr_t*)(proto + 32);
                name = ts && n <= nups ? (const char*)(*(uintptr_t*)(ts + 4 * n - 4) + 20) : "";
            }
            uint32_t taint_val = *(uint32_t*)(val_ptr + 12);
            *(uint64_t*)(top) = *(uint64_t*)(val_ptr);
            *(uint32_t*)(top + 8) = *(uint32_t*)(val_ptr + 8);
            *(uint32_t*)(top + 12) = taint_val;
            uint32_t taint_cell = *(uint32_t*)0x00D4139C;
            uint32_t taint_flag = *(uint32_t*)0x00D413A0;
            if (taint_val && taint_flag && !*(uint32_t*)0x00D413A4)
                *(uint32_t*)taint_cell = taint_val;
            *(uintptr_t*)(L + 0x0C) = top + 16;
            g_hits++;
            return name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_misses++;
    return orig(L, funcindex, n);
}

bool InstallLuaGetUpvalueFast() {
    void* t = (void*)0x0084F150;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[GetUpval] ACTIVE — lua_getupvalue inline at 0x84F150");
    CrashDumper::RegisterFeature("GetUpval");
    CrashDumper::FeatureSetActive("GetUpval", true);
    return true;
}
