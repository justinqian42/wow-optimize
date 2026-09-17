#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

// lua_createtable at 0x84E6E0 — create table (L, narray, nhash)
// Fast path: narray==0 && nhash==0 (most common addon pattern)
typedef int(__cdecl *createtable_fn)(uintptr_t L, int narray, int nhash);
static createtable_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int narray, int nhash) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, narray, nhash); }

    // If both are 0, pre-size the table to prevent immediate rehashes!
    if (narray == 0 && nhash == 0) {
        g_hits++;
        return orig(L, 4, 4);
    }

    g_misses++;
    return orig(L, narray, nhash);
}

bool InstallLuaCreateTableFast() {
    void* t = (void*)0x0084E6E0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[CreateTable] ACTIVE — lua_createtable inline at 0x84E6E0");
    CrashDumper::RegisterFeature("CreateTable");
    CrashDumper::FeatureSetActive("CreateTable", true);
    return true;
}
