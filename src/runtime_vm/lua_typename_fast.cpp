#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_typename at 0x84DED0 — returns type name string for a given type code.
// Tiny wrapper over a lookup table. Inline the lookup.

typedef const char*(__cdecl *tname_fn)(uintptr_t L, int tt);
static tname_fn orig = nullptr;

static const char* g_tnames[] = {
    "nil", "boolean", "userdata", "number",
    "string", "table", "function", "userdata",
    "thread", "proto", "upval", "no value"
};

static const char* __cdecl hook(uintptr_t L, int tt) {
    if ((unsigned)tt < 12) return g_tnames[tt];
    return orig(L, tt);
}

bool InstallLuaTypeNameFast() {
    void* t = (void*)0x0084DED0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[TypeName] ACTIVE — luaL_typename inline");
    CrashDumper::RegisterFeature("TypeName");
    CrashDumper::FeatureSetActive("TypeName", true);
    return true;
}
