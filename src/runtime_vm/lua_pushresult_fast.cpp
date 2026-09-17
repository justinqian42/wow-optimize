#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_pushresult at 0x84F5C0 — push buffer content as concatenated string.
// If B->p != B->buffer, pushes remaining bytes, increments lvl, resets p.
// Then calls lua_concat(L, B->lvl) and resets lvl to 1.
// Fast path: if buffer is empty (lvl==1 and p==buffer), skip the work and
// just ensure the result is on the stack.

typedef int(__cdecl *pushresult_fn)(uintptr_t B);
static pushresult_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t B) {
    if (B < 0x10000 || B > 0xFFE00000) {
        g_misses++;
        return orig(B);
    }
    __try {
        uintptr_t p = *(uintptr_t*)(B);
        uint32_t lvl = *(uint32_t*)(B + 4);
        uintptr_t buffer_addr = B + 12;
        if (p == buffer_addr && lvl == 1) {
            // Buffer is empty with only 1 level — result already on stack
            g_hits++;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_misses++;
    return orig(B);
}

bool InstallLuaPushresultFast() {
    void* t = (void*)0x0084F5C0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[PushRes] ACTIVE — luaL_pushresult inline at 0x84F5C0");
    CrashDumper::RegisterFeature("PushRes");
    CrashDumper::FeatureSetActive("PushRes", true);
    return true;
}
