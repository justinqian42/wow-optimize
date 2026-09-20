#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "sound_driver_guard.h"
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

extern "C" void Log(const char* fmt, ...);

typedef void (__cdecl* sub_508260_fn)(int a1, char a2);
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static sub_508260_fn g_orig_sub_508260 = nullptr;

static volatile LONG64 g_total_calls  = 0;
static volatile LONG64 g_recovered    = 0;
static volatile long   g_logged       = 0;

#include <cstring>

__declspec(noinline) static void SafeInvokeSub508260(int a1, char a2, void* retAddr)
{
    __try {
        g_orig_sub_508260(a1, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_recovered;
        if (InterlockedCompareExchange(&g_logged, 1, 0) == 0) {
            Log("[SndDriver] ONE-SHOT DIAGNOSTIC: Caught crash in sub_508260! a1=%d a2=%d RetAddr=%p",
                a1, (int)a2, retAddr);
        }
    }
}

static void __cdecl Safe_sub_508260(int a1, char a2)
{
    ++g_total_calls;
    SafeInvokeSub508260(a1, a2, _ReturnAddress());
}

bool InstallSoundDriverGuard()
{
    void* target = (void*)0x00508260;
    if (!WowOpt_ClientPatchAllowed(target)) {
        Log("[SndDriver] NOT active: client patches disallowed at 0x%08X", (uintptr_t)target);
        return false;
    }

    static const unsigned char kExpectedPrologue[8] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x39, 0x05
    };
    if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        const unsigned char* p = (const unsigned char*)target;
        Log("[SndDriver] BAD PROLOGUE at 0x%08X (got %02X %02X %02X %02X %02X %02X %02X %02X)",
            (uintptr_t)target, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        return false;
    }

    if (WineSafe_CreateHook(target, (void*)Safe_sub_508260, (void**)&g_orig_sub_508260) != MH_OK) {
        Log("[SndDriver] MH_CreateHook FAILED");
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        Log("[SndDriver] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    CrashDumper::RegisterFeature("SndDriver");
    CrashDumper::FeatureSetActive("SndDriver", true);

    Log("[SndDriver] ACTIVE: SEH guard on sub_508260 (sound mode toggle)");
    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void SoundDriverGuard_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[SndDriver] not measured: the guard is not installed.");
        return;
    }
    Log("[SndDriver] %lld call(s), %lld recovered from a crash.",
        (long long)g_total_calls, (long long)g_recovered);
}

void UninstallSoundDriverGuard()
{
    MH_DisableHook((void*)0x00508260);
    MH_RemoveHook((void*)0x00508260);

    LONG64 total     = g_total_calls;
    LONG64 recovered = g_recovered;
    if (recovered > 0) {
        Log("[SndDriver] Stats: %lld calls | %lld recovered from crashes",
            total, recovered);
    }

    CrashDumper::FeatureSetActive("SndDriver", false);
}
