#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "quality_governor.h"
#include "cvar_null_guard.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

#include <string.h>

typedef char (__fastcall *Sub7668C0_t)(void* ecx, void* edx, char* Str1, char a3, char a4, char a5, char a6);
static Sub7668C0_t g_orig7668C0 = nullptr;

static char __fastcall Hooked_7668C0(void* ecx, void* edx, char* Str1, char a3, char a4, char a5, char a6)
{
    uintptr_t p = (uintptr_t)ecx;
    if (p < 0x10000 || p > 0xFFE00000) {
        return 1;
    }
    uintptr_t ptrAt104 = *(uintptr_t*)((char*)ecx + 104);
    if (ptrAt104 != 0 && (ptrAt104 < 0x10000 || ptrAt104 > 0xFFE00000)) {
        return 1;
    }

    // Pinning timingMethod and timingTestError has nothing to do with guarding
    // null pointers, and it used to ride along here with no setting of its own -
    // OptTimingFix gates the GetTickCount, timeGetTime, QPC and SysInfo hooks and
    // never covered this. A player reading "safety hooks to prevent crashes" had
    // no way to know their timing method was being pinned.
    //
    // It now has its own switch, on by default, so nobody's behaviour changes and
    // anyone who wants the client's own choice back can have it.
#if !TEST_DISABLE_TIMING_FIX
    if (Config::g_settings.OptTimingCvarPin && ecx && Str1) {
        const char* name = *(const char**)((char*)ecx + 20);
        if (name && (uintptr_t)name >= 0x10000 && (uintptr_t)name <= 0xFFE00000) {
            char c0 = name[0];
            if (c0 == 't' || c0 == 'T') {
                static bool s_saidMethod = false, s_saidError = false;
                if (_stricmp(name, "timingMethod") == 0) {
                    if (!s_saidMethod) {
                        s_saidMethod = true;
                        Log("[CvarNullGuard] Forcing timingMethod to 2 (client asked "
                            "for \"%s\") - switch: Timing CVar Pin", Str1 ? Str1 : "(null)");
                    }
                    Str1 = (char*)"2";
                } else if (_stricmp(name, "timingTestError") == 0) {
                    if (!s_saidError) {
                        s_saidError = true;
                        Log("[CvarNullGuard] Forcing timingTestError to 0 (client "
                            "asked for \"%s\")", Str1 ? Str1 : "(null)");
                    }
                    Str1 = (char*)"0";
                }
            }
        }
    }
#endif

    // Every CVar the client sets passes here, which is the only place anything
    // can learn what the player actually chose.
    if (ecx && Str1) {
        const char* nm = *(const char**)((char*)ecx + 20);
        if (nm && (uintptr_t)nm >= 0x10000 && (uintptr_t)nm <= 0xFFE00000) {
            QualityGovernor::NoteCVarObject(ecx, nm);
            QualityGovernor::NoteCVarWrite(nm, Str1);
        }
    }

    return g_orig7668C0(ecx, edx, Str1, a3, a4, a5, a6);
}

bool InstallCvarNullGuard()
{
    void* target = (void*)0x007668C0;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[CvarGuard] BAD PROLOGUE (got %02X %02X %02X)", p[0], p[1], p[2]);
        return false;
    }
    if (MH_CreateHook(target, (void*)Hooked_7668C0, (void**)&g_orig7668C0) != MH_OK) {
        Log("[CvarGuard] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[CvarGuard] MH_EnableHook FAILED");
        return false;
    }
    Log("[CvarGuard] ACTIVE -- NULL this guard on sub_7668C0");
    return true;
}
