#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "lua_tonumber_fast.h"
#include "version.h"
#include "MinHook.h"
#include "lua_optimize.h"

// Forward declaration for Log
extern "C" void Log(const char* fmt, ...);

#if !TEST_DISABLE_LUA_TONUMBER_FAST

// Original function pointer
typedef double (__cdecl* lua_tonumber_t)(void* L, int idx);
static lua_tonumber_t orig_lua_tonumber = nullptr;

// Statistics (Lua is single-threaded; plain volatile, no atomics)
static volatile LONG64 g_fast_path_count = 0;
static volatile LONG64 g_slow_path_count = 0;

// ----------------------------------------------------------------
// Fast path for lua_tonumber (sub_84E030). Mirrors the engine:
//   o = index2adr(idx, L);  if (o->tt == 3) return o->value;  else <coerce>
// We take the fast path ONLY for an already-numeric value (tt==3) and defer
// everything else (string coercion, nil, errors) to the original.
//
// Verified vs sub_84D9C0 (index2adr) and sub_84E030:
//   L->base at L+0x10, L->top at L+0x0C; TValue is 16 bytes, value (double) at
//   +0, tt at +0x08; LUA_TNUMBER == 3.
//   positive idx: o = base + 16*(idx-1), valid iff o < top
//   negative idx in (-10000,0): o = top + 16*idx
//   idx <= -10000 (pseudo-index) or out-of-range: defer to the engine.
//
// The previous version was triply wrong (hooked 0x84E0E0 = lua_tolstring, tested
// tt==4 = LUA_TSTRING, read top at +0x14) -> the 0xC0000005 crashes.
// ----------------------------------------------------------------
double __cdecl hooked_lua_tonumber(void* L, int idx) {
    uintptr_t La = (uintptr_t)L;
    if (LuaOpt::IsReloading() || LuaOpt::IsSwapping() ||
        La < 0x10000 || La > 0xFFE00000) {
        ++g_slow_path_count;
        return orig_lua_tonumber(L, idx);
    }

    __try {
        uint8_t* top  = *(uint8_t**)(La + 0x0C);
        uint8_t* base = *(uint8_t**)(La + 0x10);
        uint8_t* o = nullptr;

        if (idx > 0) {
            o = base + (idx - 1) * 16;
            if (o >= top) o = nullptr;          // engine returns nilobject -> defer
        } else if (idx < 0 && idx > -10000) {
            o = top + idx * 16;
            if (o < base) o = nullptr;          // invalid negative index -> defer
        }
        // idx <= -10000: pseudo-index, leave o null -> defer to the engine.

        if (o && (uintptr_t)o >= 0x10000 && (uintptr_t)o < 0xFFE00000 &&
            *(int*)(o + 8) == 3) {              // tt == LUA_TNUMBER
            ++g_fast_path_count;
            return *(double*)o;                 // value at +0
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Bad stack pointer during teardown — fall through to the engine.
    }

    ++g_slow_path_count;
    return orig_lua_tonumber(L, idx);
}

bool InstallLuaToNumberFast() {
    // sub_84E030 = lua_tonumber.  (0x84E0E0 is lua_tolstring — the old bug.)
    void* target = (void*)0x0084E030;

    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[LuaToNumberFast] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }

    if (MH_CreateHook(target, (void*)hooked_lua_tonumber, (void**)&orig_lua_tonumber) != MH_OK) {
        Log("[LuaToNumberFast] MH_CreateHook failed");
        return false;
    }

    if (MH_EnableHook(target) != MH_OK) {
        Log("[LuaToNumberFast] MH_EnableHook failed");
        return false;
    }

    Log("[LuaToNumberFast] ACTIVE at 0x%08X (sub_84E030, tt==3 fast path)", (uintptr_t)target);
    return true;
}

void GetLuaToNumberStats(uint64_t* fast_path, uint64_t* slow_path) {
    if (fast_path) *fast_path = (uint64_t)g_fast_path_count;
    if (slow_path) *slow_path = (uint64_t)g_slow_path_count;
}

#else  // TEST_DISABLE_LUA_TONUMBER_FAST

bool InstallLuaToNumberFast() {
    Log("[LuaToNumberFast] Disabled via feature flag");
    return false;
}

void GetLuaToNumberStats(uint64_t* fast_path, uint64_t* slow_path) {
    if (fast_path) *fast_path = 0;
    if (slow_path) *slow_path = 0;
}

#endif
