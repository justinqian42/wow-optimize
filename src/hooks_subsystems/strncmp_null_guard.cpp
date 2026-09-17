#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "strncmp_null_guard.h"

extern "C" void Log(const char* fmt, ...);

typedef int (__stdcall *StrncmpWrapper_t)(char* Str1, char* Str2, size_t MaxCount);
static StrncmpWrapper_t g_origStrncmpWrap = nullptr;

static int __stdcall Hooked_StrncmpWrapper(char* Str1, char* Str2, size_t MaxCount)
{
    if (!Str1 && !Str2) return 0;
    if (!Str1) return -1;
    if (!Str2) return 1;

    uintptr_t s1 = (uintptr_t)Str1;
    uintptr_t s2 = (uintptr_t)Str2;
    if (s1 < 0x10000) return Str2 ? -1 : 0;
    if (s2 < 0x10000) return Str1 ? 1 : 0;

    return g_origStrncmpWrap(Str1, Str2, MaxCount);
}

bool InstallStrncmpNullGuard()
{
    void* target = (void*)0x0076E760;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[StrncmpGuard] BAD PROLOGUE (got %02X %02X %02X)", p[0], p[1], p[2]);
        return false;
    }
    if (MH_CreateHook(target, (void*)Hooked_StrncmpWrapper, (void**)&g_origStrncmpWrap) != MH_OK) {
        Log("[StrncmpGuard] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[StrncmpGuard] MH_EnableHook FAILED");
        return false;
    }
    Log("[StrncmpGuard] ACTIVE -- NULL guard on sub_76E760 strncmp wrapper");
    return true;
}
