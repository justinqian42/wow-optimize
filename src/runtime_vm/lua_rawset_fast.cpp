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

// lua_rawset at 0x84E970 — table[idx] = value (no metamethods)
// key at L->top-32, value at L->top-16
typedef int(__cdecl *rawset_fn)(uintptr_t L, int idx);
static rawset_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

extern "C" void InvalidateTableCacheSlot(void* table, void* key_str);

static int __cdecl hook(uintptr_t L, int idx) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, idx); }

    uintptr_t top = *(uintptr_t*)(L + 0x0C);
    if (top < 0x10000 || top > 0xFFE00000) { g_misses++; return orig(L, idx); }

    uintptr_t key_tv = top - 32;
    uintptr_t val_tv = top - 16;
    if (key_tv < 0x10000 || val_tv < 0x10000) { g_misses++; return orig(L, idx); }

    __try {
        int key_tt = *(int*)(key_tv + 8);
        if (key_tt != LUA_TSTRING) { g_misses++; return orig(L, idx); }

        uintptr_t table_tv = WowIndex2Adr(idx, L);
        if (table_tv < 0x10000 || *(int*)(table_tv + 8) != 5) { g_misses++; return orig(L, idx); }

        uintptr_t table = *(uintptr_t*)table_tv;
        if (table < 0x10000) { g_misses++; return orig(L, idx); }

        uintptr_t key_str = *(uintptr_t*)key_tv;
        if (key_str < 0x10000) { g_misses++; return orig(L, idx); }

        InvalidateTableCacheSlot((void*)table, (void*)key_str);

        // luaH_getstr: find existing node
        typedef uintptr_t(__cdecl *getstr_fn)(uintptr_t, uintptr_t);
        uintptr_t node = ((getstr_fn)0x0085C430)(table, key_str);
        if (node < 0x10000 || node == 0x00A46F78) { g_misses++; return orig(L, idx); }

        // Direct write: copy value TValue into the node
        uint64_t val0 = *(uint64_t*)(val_tv + 0);
        uint32_t val8 = *(uint32_t*)(val_tv + 8);
        uint32_t val12 = *(uint32_t*)(val_tv + 12);
        *(uint64_t*)(node + 0) = val0;
        *(uint32_t*)(node + 8) = val8;
        *(uint32_t*)(node + 12) = val12;

        // Taint propagation
        if (val12 && *(uint32_t*)TAINT_A0 && !*(uint32_t*)TAINT_A4)
            *(uint32_t*)0x00D4139C = val12;

        // GC write barrier: white value into black table
        if (val8 >= LUA_TSTRING && (*(unsigned char*)(*(uintptr_t*)val_tv + 9) & 3)) {
            if (*(unsigned char*)(table + 9) & 4) {
                typedef void(__cdecl *barrier_fn)(uintptr_t, uintptr_t);
                ((barrier_fn)0x0085BA90)(L, table);
            }
        }

        *(uint8_t*)(table + 10) = 0;

        *(uintptr_t*)(L + 0x0C) = top - 32;

        g_hits++;
        return (int)top;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, idx);
}

bool InstallLuaRawSetFast() {
    void* t = (void*)0x0084E970;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[RawSet] ACTIVE — lua_rawset inline at 0x0084E970");
    CrashDumper::RegisterFeature("RawSet");
    CrashDumper::FeatureSetActive("RawSet", true);
    return true;
}
