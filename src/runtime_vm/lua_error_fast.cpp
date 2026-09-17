#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);
extern void LogFlushImmediate();
extern void CrashDumper_DumpHookTrace(int count);

typedef int(__cdecl *error_fn)(uintptr_t L);
static error_fn orig = nullptr;
static volatile LONG g_errorCount = 0;
static volatile LONG g_hits = 0;

#define MAX_LOG_ERRORS 100

static const char* ReadErrorString(uintptr_t L) {
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (top < 0x10000 || top > 0xFFE00000) return nullptr;
        uintptr_t tv = top - 16;
        if (tv < 0x10000 || tv > 0xFFE00000) return nullptr;
        uint32_t tt = *(uint32_t*)(tv + 8);
        if (tt != 4) return nullptr;
        uintptr_t ts = *(uintptr_t*)tv;
        if (ts < 0x10000 || ts > 0xFFE00000) return nullptr;
        size_t len = *(size_t*)(ts + 16);
        if (len > 4000) len = 4000;
        return (const char*)(ts + 20);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) static void IncrementHitsSafe(uintptr_t L) {
    if (L > 0x10000 && L < 0xFFE00000) {
        __try {
            uintptr_t top = *(uintptr_t*)(L + 0x0C);
            if (top > 0x10000 && top < 0xFFE00000) {
                uintptr_t tv = top - 16;
                if (tv > 0x10000 && tv < 0xFFE00000) {
                    uint32_t tt = *(uint32_t*)(tv + 8);
                    if (tt == 4) {
                        g_hits++;
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static int __cdecl hook(uintptr_t L) {
    IncrementHitsSafe(L);

    LONG errNum = InterlockedIncrement(&g_errorCount);
    if (errNum <= MAX_LOG_ERRORS) {
        const char* msg = ReadErrorString(L);
        Log("");
        Log("=== LUA ERROR #%d ===", (int)errNum);
        Log("  lua_State: 0x%08X", (unsigned)L);
        if (msg && msg[0]) {
            Log("  Message: %s", msg);
        } else {
            Log("  Message: <unable to read>");
        }
        Log("  Last 32 hook calls:");
        CrashDumper_DumpHookTrace(32);
        Log("=== END LUA ERROR #%d ===", (int)errNum);
        Log("");
        if (errNum <= 10) LogFlushImmediate();
    }

    return orig(L);
}

bool InstallLuaErrorFast() {
    Log("[ErrorFast] DISABLED — duplicates lua_error_diag");
    return false;
}
