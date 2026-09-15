// ============================================================================
// Module: lua_newkey_safety.cpp
// Description: Accelerates Lua runtime calls in `lua_newkey_safety.cpp`.
// Safety & Threading: Thread-safe under Lua VM execution constraints.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_newkey_safety.h"
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

extern "C" void Log(const char* fmt, ...);
extern void LogFlushImmediate();  // Defined in dllmain.cpp

// int __cdecl sub_85CAB0(lua_State* L, Table* t, TValue* key) -> Node*
typedef void* (__cdecl* newkey_fn)(int L, int t, void* key);
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static newkey_fn g_orig_newkey = nullptr;

// Private throw-away node returned when the original would crash. 40 bytes =
// one Node (10 dwords); 16-byte aligned so the caller's TValue store is happy.
// The caller writes only the value slot (first 16 bytes); nothing reads it back.
__declspec(align(16)) static uint8_t g_scratch_node[40] = {};

static volatile LONG64 g_total_calls = 0;
static volatile LONG64 g_recovered   = 0;
// The invalidations this hook performs. Without them the report said only
// "0 recovered from chain corruption", which reads as a hook that does nothing
// and very nearly got this one deleted as dead weight - the recoveries are the
// SEH guard's half, and the other half is a table-cache invalidation running a
// billion times a session and doing exactly what it was put here to do.
static volatile LONG64 g_invalidated = 0;
static volatile long g_logged        = 0;

extern "C" void InvalidateTableCacheSlot(void* table, void* key_str);

// A fault inside the client's luaH_newkey is noted and passed on, not absorbed.
//
// This used to catch it and hand the caller a private scratch node. luaH_newkey
// rehashes when the table is full: it allocates a new node array, moves every
// entry and frees the old one. A fault part way through that leaves the table
// with half its entries moved, and handing back a scratch node lets the client
// carry on with it. The collector walks every live table, so the next
// traversal of that one reads what was left - and the one tester crash with
// this DLL in the stack is the collector's traversetable reading [0+9], a
// collectable value with a null object, which is what a half-moved table holds.
// Whether the guard ever fired in that session is not known; no log since has
// shown it firing. Crashing where the table breaks names the cause, and
// crashing later in the collector names nothing.
static LONG NoteNewKeyFault(int t, void* ret) {
    ++g_recovered;
    if (InterlockedCompareExchange(&g_logged, 1, 0) == 0) {
        Log("[NewKeySafety] a fault inside luaH_newkey, table 0x%08X, called from %p. "
            "Passed on to the crash handler: carrying on would leave the table "
            "half rebuilt for the collector to walk.", t, ret);
        LogFlushImmediate();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void* __cdecl Safe_newkey(int L, int t, void* key)
{
    ++g_total_calls;
    // Our own cache invalidation, guarded on its own: a fault here is ours and
    // must not stop the client's insert.
    __try {
        if (t && key && (uintptr_t)t >= 0x10000 && (uintptr_t)t < 0xFFE00000 &&
            (uintptr_t)key >= 0x10000 && (uintptr_t)key < 0xFFE00000) {
            int key_tt = *(int*)((char*)key + 8);
            if (key_tt == 4) {
                void* key_str = *(void**)key;
                if (key_str && (uintptr_t)key_str >= 0x10000 && (uintptr_t)key_str < 0xFFE00000) {
                    InvalidateTableCacheSlot((void*)t, key_str);
                    ++g_invalidated;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        return g_orig_newkey(L, t, key);
    } __except (NoteNewKeyFault(t, _ReturnAddress())) {
        return nullptr;
    }
}

bool InstallLuaNewKeySafety()
{
#if TEST_DISABLE_LUA_NEWKEY_SAFETY
    Log("[NewKeySafety] DISABLED via feature flag");
    return false;
#else
    void* target = (void*)0x0085CAB0;

    // Verify prologue: push ebp; mov ebp, esp
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[NewKeySafety] BAD PROLOGUE at 0x%08X (got %02X %02X %02X)",
            (uintptr_t)target, p[0], p[1], p[2]);
        return false;
    }

    if (WineSafe_CreateHook(target, (void*)Safe_newkey, (void**)&g_orig_newkey) != MH_OK) {
        Log("[NewKeySafety] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[NewKeySafety] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    CrashDumper::RegisterFeature("LuaNewKeySafety");
    CrashDumper::FeatureSetActive("LuaNewKeySafety", true);

    Log("[NewKeySafety] ACTIVE on luaH_newkey (sub_85CAB0): invalidates the table "
        "cache slot for each new key, and logs a fault inside the insert before "
        "passing it on rather than letting the client carry on with a half-rebuilt table");
    g_statsInstalled = true;
    return true;
#endif
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void LuaNewKeySafety_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[NewKeySafety] not measured: the guard is not installed.");
        return;
    }
    Log("[NewKeySafety] %lld calls, %lld cache slot(s) invalidated - that is "
        "this hook's actual work - and %lld fault(s) inside luaH_newkey, each "
        "passed on to the crash handler.",
        (long long)g_total_calls, (long long)g_invalidated,
        (long long)g_recovered);

}

void UninstallLuaNewKeySafety()
{
#if !TEST_DISABLE_LUA_NEWKEY_SAFETY
    MH_DisableHook((void*)0x0085CAB0);
    MH_RemoveHook((void*)0x0085CAB0);

    LONG64 total = g_total_calls;
    LONG64 recovered = g_recovered;
    if (recovered > 0) {
        Log("[NewKeySafety] Stats: %lld calls | %lld recovered from chain corruption",
            total, recovered);
    }
    CrashDumper::FeatureSetActive("LuaNewKeySafety", false);
#endif
}
