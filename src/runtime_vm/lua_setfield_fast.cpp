#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_index2adr.h"

extern "C" void Log(const char* fmt, ...);

#define LUA_TSTRING           4
#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
#define TAINT_A0   0x00D413A0
#define TAINT_A4   0x00D413A4

// lua_setfield at 0x84E900 — table.k = value (C-string key)
// value at L->top-16, pops value after write
typedef int(__cdecl *setfield_fn)(uintptr_t L, int idx, const char* k);
static setfield_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

extern "C" void InvalidateTableCacheSlot(void* table, void* key_str);

static int __cdecl hook(uintptr_t L, int idx, const char* k) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, idx, k); }

    if (k < (const char*)0x10000 || k > (const char*)0xFFE00000) {
        g_misses++; return orig(L, idx, k);
    }

    __try {
        // Resolve table via index2adr
        uintptr_t table_tv = WowIndex2Adr(idx, L);
        if (table_tv < 0x10000 || *(int*)(table_tv + 8) != 5) { g_misses++; return orig(L, idx, k); }

        uintptr_t table = *(uintptr_t*)table_tv;
        if (table < 0x10000) { g_misses++; return orig(L, idx, k); }

        // Check for metatable — if present, __newindex may fire
        uintptr_t mt = *(uintptr_t*)(table + 12);
        if (mt >= 0x10000 && mt < 0xFFE00000) { g_misses++; return orig(L, idx, k); }

        // Create TString for the key
        size_t klen = strlen(k);
        typedef uintptr_t(__cdecl *newlstr_fn)(uintptr_t, const void*, size_t);
        uintptr_t key_ts = ((newlstr_fn)0x00856C80)(L, k, klen);
        if (key_ts < 0x10000) { g_misses++; return orig(L, idx, k); }

        InvalidateTableCacheSlot((void*)table, (void*)key_ts);

        // Re-read L->top and resolve new val_tv in case GC reallocated stack!
        uintptr_t new_top = *(uintptr_t*)(L + 0x0C);
        if (new_top < 0x10000 || new_top > 0xFFE00000) { g_misses++; return orig(L, idx, k); }
        uintptr_t val_tv = new_top - 16;
        if (val_tv < 0x10000) { g_misses++; return orig(L, idx, k); }

        // luaH_getstr: find existing node
        typedef uintptr_t(__cdecl *getstr_fn)(uintptr_t, uintptr_t);
        uintptr_t node = ((getstr_fn)0x0085C430)(table, key_ts);
        if (node < 0x10000 || node == 0x00A46F78) { g_misses++; return orig(L, idx, k); }

        // Direct write
        uint64_t val0 = *(uint64_t*)(val_tv + 0);
        uint32_t val8 = *(uint32_t*)(val_tv + 8);
        uint32_t val12 = *(uint32_t*)(val_tv + 12);
        *(uint64_t*)(node + 0) = val0;
        *(uint32_t*)(node + 8) = val8;
        *(uint32_t*)(node + 12) = val12;

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
        *(uintptr_t*)(L + 0x0C) = new_top - 16;

        g_hits++;
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, idx, k);
}

bool InstallLuaSetFieldFast() {
    void* t = (void*)0x0084E900;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[SetField] ACTIVE — lua_setfield inline at 0x84E900");
    CrashDumper::RegisterFeature("SetField");
    CrashDumper::FeatureSetActive("SetField", true);
    return true;
}
