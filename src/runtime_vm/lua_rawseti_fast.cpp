#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_index2adr.h"

extern "C" void Log(const char* fmt, ...);

#define LUA_TSTRING           4
#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
#define TAINT_A0   0x00D413A0
#define TAINT_A4   0x00D413A4

// lua_rawseti at 0x84EA00 — table[n] = value (no metamethods)
// value at L->top-16
typedef int(__cdecl *rawseti_fn)(uintptr_t L, int idx, int n);
static rawseti_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int idx, int n) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, idx, n); }

    uintptr_t top = *(uintptr_t*)(L + 0x0C);
    if (top < 0x10000 || top > 0xFFE00000) { g_misses++; return orig(L, idx, n); }

    uintptr_t val_tv = top - 16;
    if (val_tv < 0x10000) { g_misses++; return orig(L, idx, n); }

    // n must be positive integer
    if (n < 1) { g_misses++; return orig(L, idx, n); }

    __try {
        // Resolve table via index2adr
        uintptr_t table_tv = WowIndex2Adr(idx, L);
        if (table_tv < 0x10000 || *(int*)(table_tv + 8) != 5) { g_misses++; return orig(L, idx, n); }

        uintptr_t table = *(uintptr_t*)table_tv;
        if (table < 0x10000) { g_misses++; return orig(L, idx, n); }

        // Check array bounds
        uint32_t sizearray = *(uint32_t*)(table + 32);
        if ((unsigned)(n - 1) >= sizearray) { g_misses++; return orig(L, idx, n); }

        // Direct array write
        uintptr_t slot = *(uintptr_t*)(table + 16) + (n - 1) * 16;
        uint64_t val0 = *(uint64_t*)(val_tv + 0);
        uint32_t val8 = *(uint32_t*)(val_tv + 8);
        uint32_t val12 = *(uint32_t*)(val_tv + 12);

        *(uint64_t*)(slot + 0) = val0;
        *(uint32_t*)(slot + 8) = val8;
        *(uint32_t*)(slot + 12) = val12;

        // Taint propagation
        if (val12 && *(uint32_t*)TAINT_A0 && !*(uint32_t*)TAINT_A4)
            *(uint32_t*)0x00D4139C = val12;

        // GC write barrier
        if (val8 >= LUA_TSTRING && (*(unsigned char*)(*(uintptr_t*)val_tv + 9) & 3)) {
            if (*(unsigned char*)(table + 9) & 4) {
                typedef void(__cdecl *barrier_fn)(uintptr_t, uintptr_t);
                ((barrier_fn)0x0085BA90)(L, table);
            }
        }

        // Pop value
        *(uintptr_t*)(L + 0x0C) = top - 16;

        g_hits++;
        return (int)top;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, idx, n);
}

bool InstallLuaRawSetIFast() {
    void* t = (void*)0x0084EA00;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[RawSetI] ACTIVE — lua_rawseti inline at 0x84EA00");
    CrashDumper::RegisterFeature("RawSetI");
    CrashDumper::FeatureSetActive("RawSetI", true);
    return true;
}
