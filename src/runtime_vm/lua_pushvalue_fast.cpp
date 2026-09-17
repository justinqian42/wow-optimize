#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "lua_pushvalue_fast.h"

extern "C" void Log(const char* fmt, ...);

static volatile LONG64 g_pushvalue_calls = 0;
static volatile LONG64 g_pushvalue_hits = 0;

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
static const uint32_t TAINT_A0 = 0x00D413A0;
static const uint32_t TAINT_A4 = 0x00D413A4;

typedef int (__cdecl *lua_pushvalue_fn)(uintptr_t L, int idx);
static lua_pushvalue_fn g_orig_pushvalue = nullptr;

static int __cdecl Hooked_PushValue(uintptr_t L, int idx) {
    ++g_pushvalue_calls;

    __try {
        if (L < 0x10000 || L > 0xFFE00000) goto fallback;

        // Read base and top pointers
        uint8_t** base_ptr = (uint8_t**)(L + 0x10);
        uint8_t** top_ptr = (uint8_t**)(L + 0x0C);
        uint8_t* base = *base_ptr;
        uint8_t* top = *top_ptr;

        if (base < (uint8_t*)0x10000 || base > (uint8_t*)0xFFE00000) goto fallback;
        if (top < (uint8_t*)0x10000 || top > (uint8_t*)0xFFE00000) goto fallback;

        // Resolve index to stack slot (TValue = 16 bytes in WoW 3.3.5a)
        uint8_t* src;
        if (idx > 0) {
            src = base + (idx - 1) * 16;
        } else if (idx < 0 && idx > -10000) {
            src = top + idx * 16;
        } else {
            goto fallback;
        }

        // Validate source is within stack bounds
        if (src < base || src >= top) goto fallback;

        // Copy 16-byte TValue directly to top
        uint64_t* dst64 = (uint64_t*)top;
        const uint64_t* src64 = (const uint64_t*)src;
        dst64[0] = src64[0];  // value (8 bytes)
        dst64[1] = src64[1];  // tt + taint (8 bytes)

        // Propagate taint cell and return result exactly like sub_84DE50
        uint32_t src_taint = *(uint32_t*)(src + 12);
        uint32_t result_val;
        if (src_taint != 0) {
            result_val = src_taint;
            uint32_t a0 = *(uint32_t*)TAINT_A0;
            uint32_t a4 = *(uint32_t*)TAINT_A4;
            if (a0 && !a4) {
                *(uint32_t*)0x00D4139C = src_taint;
            }
        } else {
            uint32_t global_taint = *(uint32_t*)0x00D4139C;
            *(uint32_t*)(top + 12) = global_taint;
            result_val = global_taint;
        }

        // Advance top by 16 bytes
        *top_ptr = top + 16;

        ++g_pushvalue_hits;
        return (int)result_val;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

fallback:
    return g_orig_pushvalue(L, idx);
}

static void* const ADDR_LUA_PUSHVALUE = (void*)0x0084DE50;

bool InstallLuaPushValueFast(void) {
    MH_STATUS st = WineSafe_CreateHook(
        ADDR_LUA_PUSHVALUE, (void*)Hooked_PushValue, (void**)&g_orig_pushvalue);
    if (st == MH_OK) {
        WO_EnableHook(ADDR_LUA_PUSHVALUE);
        Log("[PushValueFast] lua_pushvalue hook at 0x84DE50 ACTIVE (inline fast path)");
        return true;
    } else {
        Log("[PushValueFast] lua_pushvalue hook FAILED (status %d)", (int)st);
        return false;
    }
}

void ShutdownLuaPushValueFast(void) {
    MH_DisableHook(ADDR_LUA_PUSHVALUE);

    LONG64 calls = g_pushvalue_calls;
    LONG64 hits = g_pushvalue_hits;
    if (calls > 0) {
        Log("[PushValueFast] Stats: %lld calls, %lld fast (%.1f%%)",
            calls, hits, 100.0 * hits / calls);
    }
}