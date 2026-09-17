// ============================================================================
// Description: Accelerates table boundary search (the '#' operator) in Lua.
// ============================================================================

#include "lua_getn_fast.h"
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaH_getn address in 3.3.5a
static constexpr uintptr_t ADDR_luaH_getn = 0x0085C690;

typedef int (__cdecl *luaH_getn_fn)(uintptr_t table);
static luaH_getn_fn orig_luaH_getn = nullptr;

static volatile long g_getnCalls = 0;
static volatile long g_getnFast = 0;

static int __cdecl Hooked_luaH_getn(uintptr_t table) {
    g_getnCalls++;
    if (table < 0x10000 || table > 0xFFE00000)
        return 0;

    __try {
        int sizearray = *(int*)(table + 32);
        int* array = *(int**)(table + 16);
        uintptr_t node = *(uintptr_t*)(table + 20); // table->node

        // Fast path 1: Table has array elements
        if (sizearray > 0 && array && (uintptr_t)array >= 0x10000 && (uintptr_t)array <= 0xFFE00000) {
            // TValue is 16 bytes (4 ints), type tt is at offset 8 (index 2)
            int last_tt = array[(sizearray - 1) * 4 + 2];
            if (last_tt != 0) { // 0 is LUA_TNIL
                // If hash part is empty (node == dummynode), length is exactly sizearray
                if (node == 0x00A48280) {
                    g_getnFast++;
                    return sizearray;
                }
            } else {
                // Fast path 2: Binary search boundary directly in array part
                unsigned int i = 0;
                unsigned int j = sizearray;
                while (j - i > 1) {
                    unsigned int m = (i + j) / 2;
                    int tt_m = array[(m - 1) * 4 + 2];
                    if (tt_m == 0) j = m; // LUA_TNIL
                    else i = m;
                }
                g_getnFast++;
                return i;
            }
        } else if (sizearray == 0 && node == 0x00A48280) {
            // Empty table (no array part, empty hash part)
            g_getnFast++;
            return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return orig_luaH_getn(table);
}

bool InstallLuaGetnFast() {
    void* target = (void*)ADDR_luaH_getn;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[LuaGetn] BAD PROLOGUE at 0x%08X", ADDR_luaH_getn);
        return false;
    }

    if (MH_CreateHook(target, (void*)Hooked_luaH_getn, (void**)&orig_luaH_getn) != MH_OK) {
        Log("[LuaGetn] MH_CreateHook FAILED");
        return false;
    }

    if (MH_EnableHook(target) != MH_OK) {
        Log("[LuaGetn] MH_EnableHook FAILED");
        return false;
    }

    Log("[LuaGetn] ACTIVE: luaH_getn table length fast path (0x0085C690)");
    CrashDumper::RegisterFeature("LuaGetn");
    CrashDumper::FeatureSetActive("LuaGetn", true);
    return true;
}

void UninstallLuaGetnFast() {
    MH_DisableHook((void*)ADDR_luaH_getn);
    MH_RemoveHook((void*)ADDR_luaH_getn);
    if (g_getnCalls > 0) {
        Log("[LuaGetn] Stats: %ld calls, %ld fast (%.1f%%)",
            g_getnCalls, g_getnFast, 100.0 * g_getnFast / g_getnCalls);
    }
}
