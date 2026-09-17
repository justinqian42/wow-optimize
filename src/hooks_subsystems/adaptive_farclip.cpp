// ============================================================================
// Description: Hosts the CVar::Register detour. It no longer scales anything -
//              draw distance is the quality governor's now.
//
// What used to be here: a per-frame controller that started from a hardcoded
// 1250, moved -10 per frame below 55fps and +2 above 58, and clamped to
// [185, 1250]. Three problems, all of them the same ones DynamicShadowScaler was
// removed for. It never knew the player's own value, so a player who chose 500
// was dragged up toward 1250 - heavier, not lighter. The two thresholds sat three
// frames apart, which anyone playing near 60 crosses constantly. And nothing ever
// restored what it changed.
//
// The registration detour stays: nameplate_distance_cvar and particle scaling
// both learn their CVar objects through it.
// ============================================================================

#include "adaptive_farclip.h"
#include "MinHook.h"
#include "version.h"
#include <cstdio>

extern "C" void Log(const char* fmt, ...);
extern "C" void RegisterParticleCVar(const char* name, void* cvar);

namespace AdaptiveFarclip {

typedef void* (__cdecl *CVar_Register_fn)(const char* name, const char* defaultValue, int flags, const char* help, void* callback, int category, bool a7, int a8, bool a9);
static CVar_Register_fn orig_CVar_Register = nullptr;

static void* g_farclipCVar = nullptr;

static void* __cdecl Hooked_CVar_Register(const char* name, const char* defaultValue, int flags, const char* help, void* callback, int category, bool a7, int a8, bool a9) {
    void* cvar = orig_CVar_Register(name, defaultValue, flags, help, callback, category, a7, a8, a9);
    if (cvar && name) {
        if (strcmp(name, "farclip") == 0) {
            g_farclipCVar = cvar;
            Log("[AdaptiveFarclip] Captured farclip CVar pointer: 0x%p", g_farclipCVar);
        }
        RegisterParticleCVar(name, cvar);
    }
    return cvar;
}

bool Init() {
    #if TEST_DISABLE_ADAPTIVE_FARCLIP
    Log("[AdaptiveFarclip] DISABLED via configuration");
    return false;
    #else
    void* target = (void*)0x00767FC0;
    
    unsigned char prologue[3];
    __try {
        memcpy(prologue, target, 3);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[AdaptiveFarclip] Target 0x00767FC0 not readable.");
        return false;
    }

    if (prologue[0] != 0x55 || prologue[1] != 0x8B || prologue[2] != 0xEC) {
        Log("[AdaptiveFarclip] BAD PROLOGUE at 0x00767FC0. Skipping hook.");
        return false;
    }

    if (MH_CreateHook(target, (void*)Hooked_CVar_Register, (void**)&orig_CVar_Register) == MH_OK) {
        if (MH_EnableHook(target) == MH_OK) {
            Log("[AdaptiveFarclip] Hooked CVar::Register successfully");
            return true;
        }
        MH_RemoveHook(target);
    }
    return false;
    #endif
}

void Shutdown() {
    #if !TEST_DISABLE_ADAPTIVE_FARCLIP
    MH_DisableHook((void*)0x00767FC0);
    #endif
}

} // namespace AdaptiveFarclip
