#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_argcheck at 0x84F560 (7 callers) — asserts condition.
// If condition is true, no-op. If false, calls argerror.
// Inline the fast path: skip the function call entirely when cond is true.

typedef void(__cdecl *argcheck_fn)(uintptr_t L, int cond, int arg, const char* msg);
static argcheck_fn orig = nullptr;
static volatile long g_hits = 0;

static void __cdecl hook(uintptr_t L, int cond, int arg, const char* msg) {
    if (cond) { g_hits++; return; }
    orig(L, cond, arg, msg);
}

bool InstallLuaArgcheckFast() {
    Log("[ArgCheck] DISABLED — macro function, address 0x84F560 is actually luaL_addlstring");
    return false;
}
