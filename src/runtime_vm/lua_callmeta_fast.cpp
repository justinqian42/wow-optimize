#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_callmeta at 0x84F350 — call metamethod on object at index
// Fast path: inline the getmetafield + call sequence
// Falls back to original for pseudo-indices and missing metatables.
typedef int(__cdecl *callmeta_fn)(uintptr_t L, int obj, const char* event);
static callmeta_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int obj, const char* event) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, obj, event); }
    if (!event) { g_misses++; return orig(L, obj, event); }

    __try {
        // Mirror the engine (sub_84F350): normalize obj to an absolute index so
        // lua_pushvalue still targets it after getmetafield grows the stack, then
        // luaL_getmetafield(L, obj, event) which leaves ONLY the metamethod on top
        // (it removes the metatable itself — the previous hand-rolled version left
        // the metatable orphaned on the stack, corrupting the stack on every hit).
        typedef int(__cdecl *gettop_fn)(uintptr_t);
        int v3 = obj;
        if ((uint32_t)obj >= 0xFFFFD8F1u || obj == 0)
            v3 = obj + ((gettop_fn)0x0084DBD0)(L) + 1;

        typedef int(__cdecl *getmetafield_fn)(uintptr_t, int, const char*);
        int hasMethod = ((getmetafield_fn)0x0084F2F0)(L, v3, event);
        if (!hasMethod) { g_misses++; return 0; }   // no metafield: nothing pushed

        typedef void(__cdecl *pushvalue_fn)(uintptr_t, int);
        ((pushvalue_fn)0x0084DE50)(L, v3);          // push the object as arg

        typedef void(__cdecl *call_fn)(uintptr_t, int, int);
        ((call_fn)0x0084EBF0)(L, 1, 1);             // method(obj) -> 1 result

        g_hits++;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, obj, event);
}

bool InstallLuaCallMetaFast() {
    void* t = (void*)0x0084F350;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[CallMeta] ACTIVE — luaL_callmeta inline at 0x84F350");
    CrashDumper::RegisterFeature("CallMeta");
    CrashDumper::FeatureSetActive("CallMeta", true);
    return true;
}
