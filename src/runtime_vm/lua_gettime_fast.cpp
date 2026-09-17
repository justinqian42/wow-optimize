// ============================================================================
// Description: Accelerates Lua GetTime API calls via high-precision caching.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_gettime_fast.h"

extern "C" void Log(const char* fmt, ...);

// Address of the Lua GetTime CFunction
#define ADDR_LUA_GETTIME 0x006081F0

// lua_pushnumber helper address
#define ADDR_LUA_PUSHNUMBER 0x0084E2A0

typedef int (__cdecl *orig_gettime_fn)(int L);
static orig_gettime_fn orig_LuaGetTime = nullptr;

static volatile double g_cachedGetTimeValue = 0.0;
static LARGE_INTEGER g_lastQpc = { 0 };
static LARGE_INTEGER g_qpcFreq = { 0 };
static bool g_hasCache = false;

static int __cdecl Hooked_LuaGetTime(int L) {
    if (L < 0x10000 || L > 0xFFE00000) {
        return orig_LuaGetTime(L);
    }

    if (g_qpcFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpcFreq);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double elapsed = 0.0;
    if (g_qpcFreq.QuadPart > 0 && g_lastQpc.QuadPart > 0) {
        elapsed = (double)(now.QuadPart - g_lastQpc.QuadPart) / (double)g_qpcFreq.QuadPart;
    }

    // Cache hit: if we have a cached value and less than 1 millisecond has elapsed
    if (g_hasCache && elapsed < 0.001) {
        typedef void (__cdecl *pushnumber_fn)(int, double);
        __try {
            ((pushnumber_fn)ADDR_LUA_PUSHNUMBER)(L, g_cachedGetTimeValue);
            return 1;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Cache miss - call the original GetTime
    int res = orig_LuaGetTime(L);

    __try {
        // Read the top of the Lua stack to retrieve the pushed number
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (top >= 0x10000 && top <= 0xFFE00000) {
            uintptr_t val_tv = top - 16;
            if (*(int*)(val_tv + 8) == 3) { // LUA_TNUMBER is 3
                g_cachedGetTimeValue = *(double*)val_tv;
                g_lastQpc = now;
                g_hasCache = true;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return res;
}

void LuaGetTimeFast_NewFrame() {
    // No-op: cache invalidates automatically after 1ms via high-precision QPC delta
}

bool InstallLuaGetTimeFast() {
    void* target = (void*)ADDR_LUA_GETTIME;
    if (MH_CreateHook(target, (void*)Hooked_LuaGetTime, (void**)&orig_LuaGetTime) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        return false;
    }
    Log("[GetTimeFast] ACTIVE — lua_GetTime cached at 0x%08X", ADDR_LUA_GETTIME);
    CrashDumper::RegisterFeature("GetTimeFast");
    CrashDumper::FeatureSetActive("GetTimeFast", true);
    return true;
}
