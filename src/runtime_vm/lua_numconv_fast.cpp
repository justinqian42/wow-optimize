#include "lua_numconv_fast.h"
#include <windows.h>
#include <stdint.h>
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

// Shared helpers & constants
#define LUA_TNUMBER 3

// Pseudo-index boundary
#define LUA_REGISTRYINDEX (-10000)

// WoW taint cell
#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

#include "../allocators/loading_defrag.h"

// Teardown state helper
static inline bool IsTeardownState() {
    if (LoadingDefrag::IsLoadingActive()) return true;
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000 || gL > 0xFFE00000);
}

// Pointer sanity: must be in usermode range
static __forceinline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

// Read the type tag from a resolved TValue pointer.
static __forceinline int TValue_tt(uintptr_t tv) {
    return *(int*)(tv + 8);
}

// Read the double value from a resolved TValue pointer.
static __forceinline double TValue_nvalue(uintptr_t tv) {
    return *(double*)(tv);
}

// Resolve a Lua stack index to a raw TValue pointer.
static __forceinline uintptr_t ResolveIndex(uintptr_t L, int idx) {
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 16);
        if (!IsValidPtr(base))
            return 0;
        uintptr_t tv = base + (uintptr_t)(idx - 1) * 16;
        uintptr_t top = *(uintptr_t*)(L + 12);
        if (tv >= top)
            return 0;
        return tv;
    }
    if (idx < 0 && idx > LUA_REGISTRYINDEX) {
        uintptr_t top = *(uintptr_t*)(L + 12);
        if (!IsValidPtr(top))
            return 0;
        uintptr_t tv = top + (uintptr_t)idx * 16;
        uintptr_t base = *(uintptr_t*)(L + 16);
        if (tv < base)
            return 0;
        return tv;
    }
    return 0;
}

// ================================================================
// 1. lua_gettop hook (0x84DBD0)
// ================================================================
typedef int (__cdecl* lua_gettop_t)(uintptr_t L);
static lua_gettop_t orig_lua_gettop = nullptr;

static int __cdecl hook_lua_gettop(uintptr_t L) {
    __try {
        uintptr_t top  = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (top >= base && (top - base) < 0x100000)
            return (int)((top - base) >> 4);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_gettop(L);
}

// ================================================================
// 2. lua_isnumber hook (0x84DF20)
// ================================================================
typedef int (__cdecl* lua_isnumber_t)(uintptr_t L, int idx);
static lua_isnumber_t orig_lua_isnumber = nullptr;

static int __cdecl hook_lua_isnumber(uintptr_t L, int idx) {
    __try {
        uintptr_t tv = ResolveIndex(L, idx);
        if (tv) {
            if (TValue_tt(tv) == LUA_TNUMBER)
                return 1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return orig_lua_isnumber(L, idx);
}

// ================================================================
// 3. lua_tonumber hook (0x84E030)
// ================================================================
typedef double (__cdecl* lua_tonumber_t)(uintptr_t L, int idx);
static lua_tonumber_t orig_lua_tonumber = nullptr;

static double __cdecl hook_lua_tonumber(uintptr_t L, int idx) {
    __try {
        uintptr_t tv = ResolveIndex(L, idx);
        if (tv && TValue_tt(tv) == LUA_TNUMBER) {
            return TValue_nvalue(tv);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
    return orig_lua_tonumber(L, idx);
}

// Install / Shutdown
static void* const ADDR_LUA_GETTOP   = (void*)0x0084DBD0;
static void* const ADDR_LUA_ISNUMBER = (void*)0x0084DF20;
static void* const ADDR_LUA_TONUMBER = (void*)0x0084E030;

bool InstallLuaNumConvFast() {
    int installed = 0;

    MH_STATUS st = WineSafe_CreateHook(
        ADDR_LUA_GETTOP, (void*)hook_lua_gettop, (void**)&orig_lua_gettop);
    if (st == MH_OK) {
        WO_EnableHook(ADDR_LUA_GETTOP);
        installed++;
        Log("[LuaNumConvFast] lua_gettop hook at 0x84DBD0 (inline fast path)");
    } else {
        Log("[LuaNumConvFast] lua_gettop hook FAILED (status %d)", (int)st);
    }

    st = WineSafe_CreateHook(
        ADDR_LUA_ISNUMBER, (void*)hook_lua_isnumber, (void**)&orig_lua_isnumber);
    if (st == MH_OK) {
        WO_EnableHook(ADDR_LUA_ISNUMBER);
        installed++;
        Log("[LuaNumConvFast] lua_isnumber hook at 0x84DF20 (1194 xrefs)");
    } else {
        Log("[LuaNumConvFast] lua_isnumber hook FAILED (status %d)", (int)st);
    }

    st = WineSafe_CreateHook(
        ADDR_LUA_TONUMBER, (void*)hook_lua_tonumber, (void**)&orig_lua_tonumber);
    if (st == MH_OK) {
        WO_EnableHook(ADDR_LUA_TONUMBER);
        installed++;
        Log("[LuaNumConvFast] lua_tonumber hook at 0x84E030 (1165 xrefs)");
    } else {
        Log("[LuaNumConvFast] lua_tonumber hook FAILED (status %d)", (int)st);
    }

    return installed > 0;
}

void ShutdownLuaNumConvFast() {
    MH_DisableHook(ADDR_LUA_GETTOP);
    MH_DisableHook(ADDR_LUA_ISNUMBER);
    MH_DisableHook(ADDR_LUA_TONUMBER);
}
