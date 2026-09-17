#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_xpcall at 0x84EBF0 (7 callers) — protected call with error handler.
// Fast path: when errfunc is 0 (nil), the engine converts to -1 internally.
// Skip the errfunc setup and call the underlying pcall directly.

typedef int(__cdecl *xpcall_fn)(uintptr_t L, int nargs, int errfunc);
static xpcall_fn orig = nullptr;
static volatile long g_hits = 0;

static int __cdecl hook(uintptr_t L, int nargs, int errfunc) {
    if (errfunc == 0) { g_hits++; return orig(L, nargs, -1); }
    return orig(L, nargs, errfunc);
}

bool InstallLuaXPCallFast() {
    void* t = (void*)0x0084EBF0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[XPCall] ACTIVE — lua_xpcall inline at 0x84EBF0");
    CrashDumper::RegisterFeature("XPCall");
    CrashDumper::FeatureSetActive("XPCall", true);
    return true;
}
