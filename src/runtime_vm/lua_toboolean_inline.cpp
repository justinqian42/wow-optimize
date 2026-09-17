#include "lua_toboolean_inline.h"
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// Statistics (plain increments: Lua is single-threaded)
static volatile long g_tobooleanCalls = 0;
static volatile long g_tobooleanFast  = 0;

// Original function pointer
typedef int (__cdecl *lua_toboolean_fn)(uintptr_t L, int idx);
static lua_toboolean_fn orig_toboolean = nullptr;

static int __cdecl Hooked_Toboolean(uintptr_t L, int idx) {
    ++g_tobooleanCalls;
    __try {
        if (idx > 0) {
            uintptr_t base = *(uintptr_t*)(L + 0x10);
            if (base > 0x10000 && base < 0xFFE00000) {
                uintptr_t tv = base + (uintptr_t)(idx - 1) * 16;
                uintptr_t top = *(uintptr_t*)(L + 0x0C);
                if (tv < top) {
                    int tt = *(int*)(tv + 8);
                    if (tt == 0) { // LUA_TNIL
                        ++g_tobooleanFast;
                        return 0;
                    }
                    if (tt == 1) { // LUA_TBOOLEAN
                        ++g_tobooleanFast;
                        return *(int*)(tv + 0) != 0;
                    }
                    ++g_tobooleanFast;
                    return 1; // all other types are truthy
                }
            }
        } else if (idx < 0 && idx > -10000) {
            uintptr_t top = *(uintptr_t*)(L + 0x0C);
            if (top > 0x10000 && top < 0xFFE00000) {
                uintptr_t tv = top + (uintptr_t)idx * 16;
                uintptr_t base = *(uintptr_t*)(L + 0x10);
                if (tv >= base) {
                    int tt = *(int*)(tv + 8);
                    if (tt == 0) {
                        ++g_tobooleanFast;
                        return 0;
                    }
                    if (tt == 1) {
                        ++g_tobooleanFast;
                        return *(int*)(tv + 0) != 0;
                    }
                    ++g_tobooleanFast;
                    return 1;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return orig_toboolean(L, idx);
}

bool InstallLuaTobooleanInline() {
    void* target = (void*)0x0084E0B0;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[LuaTBool] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }
    if (MH_CreateHook(target, (void*)Hooked_Toboolean, (void**)&orig_toboolean) != MH_OK) {
        Log("[LuaTBool] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[LuaTBool] MH_EnableHook FAILED");
        return false;
    }
    Log("[LuaTBool] ACTIVE: inline lua_toboolean (0x84E0B0)");
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void LuaTobooleanInline_LogStats(void) {
    if (!orig_toboolean) {
        Log("[LuaTBool] not measured: the hook is not installed.");
        return;
    }
    const LONG64 total = g_tobooleanCalls, fast = g_tobooleanFast;
    if (total == 0) {
        Log("[LuaTBool] measured and zero: nothing asked the VM for a boolean.");
        return;
    }
    Log("[LuaTBool] %lld calls, %lld inline (%.1f%%).",
        (long long)total, (long long)fast,
        100.0 * (double)fast / (double)total);
}

void UninstallLuaTobooleanInline() {
    MH_DisableHook((void*)0x0084E0B0);
    MH_RemoveHook((void*)0x0084E0B0);
    LONG64 total = g_tobooleanCalls;
    LONG64 fast  = g_tobooleanFast;
    if (total > 0) {
        Log("[LuaTBool] Stats: %lld calls, %lld inline (%.1f%%)",
            (long long)total, (long long)fast,
            100.0 * (double)fast / (double)total);
    }
}