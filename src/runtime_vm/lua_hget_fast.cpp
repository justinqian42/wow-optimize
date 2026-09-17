#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define NIL_OBJECT  0x00A46F78

// luaH_get at 0x85C470 — master table read dispatcher.
// Calls luaH_getstr for strings, luaH_getnum for integers,
// or mainposition+chain walk for other types.
// Optimizes integer keys via direct array access.

typedef uintptr_t(__cdecl *get_fn)(uintptr_t table, uintptr_t key_tv);
static get_fn orig_get = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static uintptr_t __cdecl hook(uintptr_t table, uintptr_t key_tv)
{
    uintptr_t node;
    int tt = *(int*)(key_tv + 8);

    // Integer key? Try array part first — most common
    if (tt == 3) {  // LUA_TNUMBER
        int d = (int)*(double*)key_tv;
        if ((double)d == *(double*)key_tv && d >= 1 && (unsigned)(d - 1) < *(uint32_t*)(table + 32)) {
            node = *(uintptr_t*)(table + 16) + (d - 1) * 16;
            if (node > 0x10000) { g_hits++; return node; }
        }
    }
    // String key? use luaH_getstr (already optimized by GetStrInline)
    // luaH_getstr expects (Table*, TString*), not (Table*, TValue*)
    else if (tt == 4) {
        typedef uintptr_t(__cdecl *getstr_fn)(uintptr_t, uintptr_t);
        node = ((getstr_fn)0x0085C430)(table, *(uintptr_t*)key_tv);
        if (node > 0x10000 && node != NIL_OBJECT) { g_hits++; return node; }
    }

    g_misses++;
    return orig_get(table, key_tv);
}

bool InstallLuaHgetFast() {
    void* t = (void*)0x0085C470;
    if (MH_CreateHook(t, hook, (void**)&orig_get) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[HGetFast] ACTIVE — luaH_get inline at 0x85C470");
    CrashDumper::RegisterFeature("HGetFast");
    CrashDumper::FeatureSetActive("HGetFast", true);
    return true;
}
