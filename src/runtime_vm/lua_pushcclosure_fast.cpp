#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

// lua_pushcclosure at 0x84E400 — push C closure (L, fn, nupvals)
// Fast path: nupvals==0 (equivalent to lua_pushcfunction)
typedef int(__cdecl *pushcclosure_fn)(uintptr_t L, uintptr_t fn, int nupvals);
static pushcclosure_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, uintptr_t fn, int nupvals) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, fn, nupvals); }

    if (nupvals != 0) { g_misses++; return orig(L, fn, nupvals); }

    __try {
        // GC check: if GC threshold exceeded, run a step (same test as the engine).
        uintptr_t g = *(uintptr_t*)(L + 0x14);
        if (*(uintptr_t*)(g + 0x44) >= *(uintptr_t*)(g + 0x40)) {
            typedef void(__cdecl *gcstep_fn)(uintptr_t);
            ((gcstep_fn)0x0085B950)(L);
        }

        // Closure environment, exactly as sub_84E400: the base CallInfo uses the
        // thread's global table (L+0x48); otherwise the currently-running
        // function's environment.
        uintptr_t env = *(uintptr_t*)(L + 0x48);
        uintptr_t ci = *(uintptr_t*)(L + 0x18);
        if (ci && ci != *(uintptr_t*)(L + 0x2C)) {
            uintptr_t func_tv = *(uintptr_t*)(ci + 4);
            if (func_tv >= 0x10000 && func_tv < 0xFFE00000) {
                uintptr_t cl = *(uintptr_t*)func_tv;
                if (cl >= 0x10000 && cl < 0xFFE00000) {
                    env = *(uintptr_t*)(cl + 16);
                }
            }
        }

        // Allocate via the real luaF_newCclosure so the closure header (isC=1,
        // nupvalues, env, gclist) is set up correctly; then store the C function
        // pointer at +24 (result[6]) like the engine does. The old hand-rolled
        // path wrote f at +16 (that slot is env) and marked isC=0 (a Lua closure),
        // producing a malformed closure that crashed when first called.
        typedef uintptr_t(__cdecl *newcclosure_fn)(uintptr_t, int, uintptr_t);
        uintptr_t cl = ((newcclosure_fn)0x0085CC10)(L, 0, env);
        if (cl < 0x10000) { g_misses++; return orig(L, fn, nupvals); }
        *(uintptr_t*)(cl + 24) = fn;       // f

        // Push the closure value onto the stack.
        uintptr_t new_top = *(uintptr_t*)(L + 0x0C);
        if (new_top < 0x10000 || new_top > 0xFFE00000) { g_misses++; return orig(L, fn, nupvals); }
        uint32_t taint = *(uint32_t*)0x00D4139C;
        *(uintptr_t*)(new_top + 0) = cl;
        *(uint32_t*)(new_top + 4) = 0;
        *(uint32_t*)(new_top + 8) = 6;         // LUA_TFUNCTION
        *(uint32_t*)(new_top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = new_top + 16;

        g_hits++;
        return (int)cl;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, fn, nupvals);
}

bool InstallLuaPushCClosureFast() {
    void* t = (void*)0x0084E400;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[PushCClos] ACTIVE — lua_pushcclosure inline at 0x84E400");
    CrashDumper::RegisterFeature("PushCClos");
    CrashDumper::FeatureSetActive("PushCClos", true);
    return true;
}
