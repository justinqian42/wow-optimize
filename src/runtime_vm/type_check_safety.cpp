// ============================================================================
// Description: SEH guard for the GUID->object type check sub_4D4DB0.
// Safety & Threading: read-only guard; safe under concurrent object-manager use.
// ============================================================================
// sub_4D4DB0 is __cdecl(__int64 guid, int typeMask):
//   result = sub_4D4BB0(guid);              // resolve GUID -> object row
//   if (result && (typeMask & *(*(result+8)+8)) == 0) return 0;   // 0x4D4DF7
//   return result;
//
// It reads the resolved object's type-descriptor flags: *(*(result+8)+8). During
// object teardown - BG load/exit, phasing, unit death (the caller sub_79C110 is
// a C3Vector destructor that runs the WoW free-wrapper) - that descriptor can
// already be freed, so the read at 0x4D4DF7 faults. A tester crashed exactly
// there on a BG loading screen: wow.exe+0xD4DF7, test [ecx+8],edx.
//
// A freed object is of no type, so on a fault we return 0 ("not that type"),
// which is what the function already returns for a non-matching object. This
// turns a hard crash into the correct benign result.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

typedef int (__cdecl* fn_4D4DB0)(int64_t guid, int typeMask);
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static fn_4D4DB0 g_orig_4D4DB0 = nullptr;
static volatile LONG g_tc_averted = 0;
static volatile long g_tc_logged  = 0;

static int __cdecl Safe_sub_4D4DB0(int64_t guid, int typeMask)
{
    __try {
        return g_orig_4D4DB0(guid, typeMask);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_tc_averted);
        if (InterlockedCompareExchange(&g_tc_logged, 1, 0) == 0)
            Log("[TypeCheckSafety] averted a crash in sub_4D4DB0 "
                "(GUID type check on a freed object during teardown)");
        return 0;
    }
}

bool InstallTypeCheckSafety()
{
    void* target = (void*)0x004D4DB0;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {   // push ebp; mov ebp, esp
        Log("[TypeCheckSafety] BAD PROLOGUE at 0x004D4DB0 (expected 55 8B, got %02X %02X)",
            p[0], p[1]);
        return false;
    }
    if (MH_CreateHook(target, (void*)Safe_sub_4D4DB0, (void**)&g_orig_4D4DB0) != MH_OK) {
        Log("[TypeCheckSafety] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[TypeCheckSafety] MH_EnableHook FAILED");
        return false;
    }
    CrashDumper::RegisterFeature("TypeCheckSafety");
    CrashDumper::FeatureSetActive("TypeCheckSafety", true);
    Log("[TypeCheckSafety] ACTIVE: SEH-guarding GUID type check sub_4D4DB0 (BG-load crash fix)");
    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void TypeCheckSafety_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[TypeCheckSafety] not measured: the guard is not installed.");
        return;
    }
    Log("[TypeCheckSafety] %ld crash(es) averted.", (long)g_tc_averted);
}

void UninstallTypeCheckSafety()
{
    MH_DisableHook((void*)0x004D4DB0);
    MH_RemoveHook((void*)0x004D4DB0);
    if (g_tc_averted > 0)
        Log("[TypeCheckSafety] Stats: %ld crashes averted", (long)g_tc_averted);
}
