#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_addvalue at 0x84F610 — adds top-of-stack value to buffer as string.
// Calls lua_tolstring(L, -1, &len), then memcpy's to buffer if fits, or
// flushes partial buffer first if string is too large.
// Fast path: when string fits in remaining buffer space, do direct memcpy
// and advance p + pop the stack copy.

typedef int(__cdecl *addvalue_fn)(uintptr_t B);
static addvalue_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t B) {
    if (B < 0x10000 || B > 0xFFE00000) {
        g_misses++;
        return orig(B);
    }
    __try {
        uintptr_t L = *(uintptr_t*)(B + 8);
        uintptr_t p = *(uintptr_t*)(B);
        if (L < 0x10000 || L > 0xFFE00000 || p < B + 12 || p > B + 512 + 12) {
            g_misses++;
            return orig(B);
        }
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t tv = top - 16;
        if (tv < 0x10000 || tv > 0xFFE00000) {
            g_misses++;
            return orig(B);
        }
        uint32_t tt = *(uint32_t*)(tv + 8);
        const char* s;
        size_t len;
        if (tt == 4) {
            uintptr_t ts = *(uintptr_t*)(tv);
            if (!ts || ts < 0x10000 || ts > 0xFFE00000) {
                g_misses++;
                return orig(B);
            }
            len = *(uint32_t*)(ts + 16);
            s = (const char*)(ts + 20);
        } else if (tt == 3) {
            // Number: skip fast path, let original handle via lua_tolstring
            g_misses++;
            return orig(B);
        } else {
            g_misses++;
            return orig(B);
        }
        if (s && len > 0) {
            size_t remaining = (B + 524) - p;
            if (len <= remaining) {
                memcpy((void*)p, s, len);
                *(uintptr_t*)(B) = p + len;
                *(uintptr_t*)(L + 0x0C) = top - 16;
                g_hits++;
                return (int)len;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_misses++;
    return orig(B);
}

bool InstallLuaAddlstringFast() {
    void* t = (void*)0x0084F610;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[AddLStr] ACTIVE — luaL_addvalue inline at 0x84F610");
    CrashDumper::RegisterFeature("AddLStr");
    CrashDumper::FeatureSetActive("AddLStr", true);
    return true;
}
