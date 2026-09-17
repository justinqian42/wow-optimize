#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_index2adr.h"

extern "C" void Log(const char* fmt, ...);

// luaL_checklstring at 0x84F9F0 — validates string argument.
// 23 static callers. Common case: TValue is already LUA_TSTRING.
// Skip lua_tolstring conversion for direct string access.

typedef const char*(__cdecl *chklstr_fn)(uintptr_t L, int idx, uint32_t* len_out);
static chklstr_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static const char* __cdecl hook(uintptr_t L, int idx, uint32_t* len_out) {
    if (L < 0x10000 || L > 0xFFE00000) return orig(L, idx, len_out);
    uintptr_t tv = WowIndex2Adr(idx, L);
    if (tv < 0x10000) return orig(L, idx, len_out);

    int tt = *(int*)(tv + 8);
    if (tt == 4) {  // LUA_TSTRING — direct read
        uintptr_t ts = *(uintptr_t*)(tv + 0);
        if (ts > 0x10000) {
            if (len_out) *len_out = *(uint32_t*)(ts + 16);  // len may be NULL
            g_hits++;
            return (const char*)(ts + 20);
        }
    }
    // Note: type mismatch (e.g. number passed where string expected) is normal WoW
    // engine behavior via auto-coercion. No logging - it was causing hundreds of
    // SetEvent() calls per second, saturating the log ring and stalling main thread.
    g_misses++;
    return orig(L, idx, len_out);
}

bool InstallLuaCheckStringFast() {
    void* t = (void*)0x0084F9F0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[CheckStr] ACTIVE — luaL_checklstring inline at 0x84F9F0");
    CrashDumper::RegisterFeature("CheckStr");
    CrashDumper::FeatureSetActive("CheckStr", true);
    return true;
}
