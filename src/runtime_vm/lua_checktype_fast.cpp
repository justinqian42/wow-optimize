#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_checktype at 0x84F960 — assert that value at index has expected type
// Fast path: inline the type check, skip the function call when matching
// Falls back to original for mismatches (which raise an error).
typedef int(__cdecl *checktype_fn)(uintptr_t L, int arg, int expectedType);
static checktype_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int arg, int expectedType) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, arg, expectedType); }

    __try {
        // Fast inline type resolution
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);

        if ((uint32_t)arg >= 0x0000270F && arg > 0) {
            // Pseudo-index — defer to engine
            g_misses++;
            return orig(L, arg, expectedType);
        }

        uintptr_t slot;
        if (arg > 0) {
            slot = base + 16 * (arg - 1);
        } else if (arg < 0 && arg > -10000) {
            slot = top + 16 * arg;
        } else {
            g_misses++;
            return orig(L, arg, expectedType);
        }

        if (slot < base || slot >= top) { g_misses++; return orig(L, arg, expectedType); }

        int actualType = *(int32_t*)(slot + 8);

        if (actualType == expectedType) {
            g_hits++;
            return actualType;
        }
        // Type mismatch — let original handle the error
        g_misses++;
        return orig(L, arg, expectedType);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, arg, expectedType);
}

bool InstallLuaCheckTypeFast() {
    void* t = (void*)0x0084F960;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[CheckType] ACTIVE — luaL_checktype inline at 0x84F960");
    CrashDumper::RegisterFeature("CheckType");
    CrashDumper::FeatureSetActive("CheckType", true);
    return true;
}
