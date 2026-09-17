#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "lua_pushnumber_fast.h"
#include "lua_optimize.h"

extern "C" void Log(const char* fmt, ...);

// Statistics
static volatile LONG64 g_pushnum_calls = 0;
static volatile LONG64 g_pushnum_hits = 0;

// Original function pointer
typedef int (__cdecl *lua_pushnumber_fn)(void* L, double n);
static lua_pushnumber_fn g_orig_pushnumber = nullptr;

// Taint globals
static constexpr uintptr_t ADDR_taint_global  = 0x00D4139C;
static constexpr uintptr_t ADDR_taint_enabled = 0x00D413A0;
static constexpr uintptr_t ADDR_taint_skip    = 0x00D413A4;

// Optimized replacement
static int __cdecl Optimized_PushNumber(void* L, double n)
{
    ++g_pushnum_calls;

    // Bail out during lua_State swap — L->top is being torn down
    if (LuaOpt::IsReloading() || LuaOpt::IsSwapping()) {
        return g_orig_pushnumber(L, n);
    }

    // Validate L pointer
    uintptr_t L_addr = (uintptr_t)L;
    if (L_addr < 0x10000 || L_addr > 0xFFE00000) {
        return g_orig_pushnumber(L, n);
    }

    __try {
        // Read L->top
        DWORD* top = *(DWORD**)(L_addr + 0x0C);
        if (!top || (uintptr_t)top < 0x10000 || (uintptr_t)top > 0xFFE00000) {
            return g_orig_pushnumber(L, n);
        }
        
        int old_top = (int)top;

        // Write TValue directly: value (8 bytes) + tt (4 bytes) + taint (4 bytes)
        uint64_t value_bits;
        memcpy(&value_bits, &n, sizeof(double));

        top[0] = (DWORD)(value_bits & 0xFFFFFFFF);         // value lo
        top[1] = (DWORD)(value_bits >> 32);                // value hi
        top[2] = 3;                                         // tt = LUA_TNUMBER
        top[3] = *(DWORD*)ADDR_taint_global;               // taint pointer from global

        // Advance L->top
        *(DWORD**)(L_addr + 0x0C) = top + 4;

        ++g_pushnum_hits;
        return old_top;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return g_orig_pushnumber(L, n);
    }
}

bool InstallLuaPushNumberFast()
{
#if TEST_DISABLE_PUSHNUMBER_FAST
    (void)&Optimized_PushNumber;
    Log("[PushNumFast] DISABLED via TEST_DISABLE_PUSHNUMBER_FAST");
    return false;
#else
    // RE-ENABLED after root-causing in disassembly (sub_84E2A0): the direct stack write
    // is byte-exact to the engine --
    //   top = L->top (L+0x0C); top[0..1] = (double)n; top[2] = 3 (LUA_TNUMBER);
    //   top[3] = *(DWORD*)0xD4139C (global taint, single indirection); L->top += 16
    // -- which is exactly what Optimized_PushNumber writes. The original
    // "compare number with nil" corruption was the custom VM interpreter
    // (lua_vm_engine, still disabled); this path was only collateral, disabled so
    // that fix could be tested cleanly. Layout verified, so it is safe again.
    // SEH-guarded + pointer-validated; defers to the engine during a state swap.
    void* target = (void*)0x0084E2A0;

    // Verify prologue: push ebp; mov ebp, esp
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[PushNumFast] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }

    if (MH_CreateHook(target, (void*)Optimized_PushNumber, (void**)&g_orig_pushnumber) != MH_OK) {
        Log("[PushNumFast] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[PushNumFast] MH_EnableHook FAILED");
        return false;
    }

    Log("[PushNumFast] ACTIVE: direct stack write for lua_pushnumber (0x%08X)", (uintptr_t)target);
    return true;
#endif
}

void ShutdownLuaPushNumberFast()
{
    MH_DisableHook((void*)0x0084E2A0);

    LONG64 total = g_pushnum_calls;
    LONG64 hits  = g_pushnum_hits;
    if (total > 0) {
        Log("[PushNumFast] Stats: %lld calls, %lld fast (%.1f%%)",
            total, hits, 100.0 * hits / total);
    }
}