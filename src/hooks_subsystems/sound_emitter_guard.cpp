#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "sound_emitter_guard.h"
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

extern "C" void Log(const char* fmt, ...);

typedef void (__cdecl* sub_5093F0_fn)(void* emitter, int a2, int a3);
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static sub_5093F0_fn g_orig_sub_5093F0 = nullptr;

static volatile LONG64 g_total_calls  = 0;
static volatile LONG64 g_recovered    = 0;
static volatile long   g_logged       = 0;

#include <cstring>

__declspec(noinline) static void SafeInvokeSub5093F0(void* emitter, int a2, int a3, void* retAddr)
{
    __try {
        g_orig_sub_5093F0(emitter, a2, a3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_recovered;
        if (InterlockedCompareExchange(&g_logged, 1, 0) == 0) {
            Log("[SndEmitter] ONE-SHOT DIAGNOSTIC: Caught crash in sub_5093F0! Emitter=0x%08X a2=%d a3=%d RetAddr=%p",
                (uint32_t)(uintptr_t)emitter, a2, a3, retAddr);
        }
    }
}

static void __cdecl Safe_sub_5093F0(void* emitter, int a2, int a3)
{
    ++g_total_calls;
    SafeInvokeSub5093F0(emitter, a2, a3, _ReturnAddress());
}

bool InstallSoundEmitterGuard()
{
    void* target = (void*)0x005093F0;
    if (!WowOpt_ClientPatchAllowed(target)) {
        Log("[SndEmitter] NOT active: client patches disallowed at 0x%08X", (uintptr_t)target);
        return false;
    }

    static const unsigned char kExpectedPrologue[8] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x81, 0xEC
    };
    if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        const unsigned char* p = (const unsigned char*)target;
        Log("[SndEmitter] BAD PROLOGUE at 0x%08X (got %02X %02X %02X %02X %02X %02X %02X %02X)",
            (uintptr_t)target, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        return false;
    }

    if (WineSafe_CreateHook(target, (void*)Safe_sub_5093F0, (void**)&g_orig_sub_5093F0) != MH_OK) {
        Log("[SndEmitter] MH_CreateHook FAILED");
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        Log("[SndEmitter] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    CrashDumper::RegisterFeature("SndEmitter");
    CrashDumper::FeatureSetActive("SndEmitter", true);

    Log("[SndEmitter] ACTIVE: SEH guard on sub_5093F0 (emitter registration)");
    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void SoundEmitterGuard_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[SndEmitter] not measured: the guard is not installed.");
        return;
    }
    Log("[SndEmitter] %lld call(s), %lld recovered from a crash.",
        (long long)g_total_calls, (long long)g_recovered);
}

void UninstallSoundEmitterGuard()
{
    MH_DisableHook((void*)0x005093F0);
    MH_RemoveHook((void*)0x005093F0);

    LONG64 total     = g_total_calls;
    LONG64 recovered = g_recovered;
    if (recovered > 0) {
        Log("[SndEmitter] Stats: %lld calls | %lld recovered from crashes",
            total, recovered);
    }

    CrashDumper::FeatureSetActive("SndEmitter", false);
}
