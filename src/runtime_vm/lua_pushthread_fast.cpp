#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

// lua_pushthread at 0x84E530 — push current thread onto stack
typedef int(__cdecl *pushthread_fn)(uintptr_t L);
static pushthread_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L); }

    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (top < 0x10000 || top > 0xFFE00000) { g_misses++; return orig(L); }

        uint32_t taint = *(uint32_t*)0x00D4139C;
        *(uintptr_t*)(top + 0) = L;
        *(uint32_t*)(top + 4) = 0;
        *(uint32_t*)(top + 8) = 8;   // LUA_TTHREAD
        *(uint32_t*)(top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = top + 16;

        uintptr_t g = *(uintptr_t*)(L + 0x14);
        int isMain = (*(uintptr_t*)(g + 0x78) == L) ? 1 : 0;

        g_hits++;
        return isMain;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L);
}

bool InstallLuaPushThreadFast() {
    void* t = (void*)0x0084E530;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[PushThread] ACTIVE — lua_pushthread inline at 0x84E530");
    CrashDumper::RegisterFeature("PushThread");
    CrashDumper::FeatureSetActive("PushThread", true);
    return true;
}
