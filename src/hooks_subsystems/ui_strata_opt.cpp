#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <MinHook.h>

#include "ui_strata_opt.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace UIStrataOpt {
namespace {

// Target function addresses in wow.exe 3.3.5a build 12340
static const uintptr_t kStrataOnUpdateAddr      = 0x00495320;
static const uintptr_t kSimpleFrameOnUpdateAddr  = 0x00490770;
static const uintptr_t kFrameLevelOnUpdateAddr   = 0x00494A10;

// Callees
static const uintptr_t kLayoutResolveAddr        = 0x004898B0;
static const uintptr_t kPendingCleanupAddr      = 0x0048ECF0;
static const uintptr_t kMouseMoveHitTestAddr    = 0x004945A0;
static const uintptr_t kGetLuaStateAddr         = 0x00817DB0;
static const uintptr_t kPushNumberAddr          = 0x0084E2A0;
static const uintptr_t kSignalEventAddr         = 0x0081A2C0;

// Function pointer typedefs
typedef void* (__thiscall *FrameStrataOnUpdateFn)(void* this_ptr, float elapsed);
typedef int   (__thiscall *SimpleFrameOnUpdateFn)(void* this_ptr, float elapsed);
typedef void* (__thiscall *FrameLevelOnUpdateFn)(void* this_ptr, float elapsed);

typedef int   (__cdecl *LayoutResolveFn)();
typedef void  (__cdecl *PendingCleanupFn)(int);
typedef BOOL  (__cdecl *MouseMoveHitTestFn)(int a1, int a2);
typedef void* (__cdecl *GetLuaStateFn)();
typedef void  (__cdecl *PushNumberFn)(void* L, double num);
typedef int   (__thiscall *SignalEventFn)(void* frame, void* handler, int a3, int a4);
typedef void  (__thiscall *Vtable10Fn)(void* frame);
typedef int   (__thiscall *Vtable11Fn)(void* frame, float elapsed);

// Original function trampolines
static FrameStrataOnUpdateFn orig_StrataOnUpdate     = nullptr;
static SimpleFrameOnUpdateFn orig_SimpleFrameOnUpdate = nullptr;

// State flags and counters
static bool g_installed         = false;
static bool g_abSubject         = false;
static bool g_retired           = false;

static uint32_t g_strataManagerCalls   = 0;
static uint32_t g_levelsEvaluated       = 0;
static uint32_t g_levelsSkippedEmpty   = 0;
static uint32_t g_framesEvaluated       = 0;
static uint32_t g_framesFastBypassed    = 0;
static uint32_t g_framesWithLuaScript   = 0;
static uint32_t g_framesWithDirtyState  = 0;
static uint32_t g_framesWithAnim        = 0;

static MH_STATUS WowOpt_CreateHookGuarded(void* target, void* detour, void** original) {
    if (!WowOpt_ClientPatchAllowed(target)) {
        return MH_ERROR_NOT_EXECUTABLE;
    }
    return MH_CreateHook(target, detour, original);
}

// Detour for CSimpleFrame::OnUpdate (0x00490770 / UIFrame_OnUpdateTree)
// Hot per-frame callback. NO __try here (preserves /GS omitted and zero SEH overhead).
int __fastcall Hook_SimpleFrame_OnUpdate(void* this_ptr, void* /*edx*/, float elapsed) {
    if (g_retired || g_abSubject) {
        return orig_SimpleFrameOnUpdate(this_ptr, elapsed);
    }

    ++g_framesEvaluated;
    const uintptr_t frame = (uintptr_t)this_ptr;

    // 1. Lua script handler at [frame + 0x144]
    const uintptr_t scriptHandler = *(const uintptr_t*)(frame + 0x144);
    if (scriptHandler) {
        void* L = ((GetLuaStateFn)kGetLuaStateAddr)();
        ((PushNumberFn)kPushNumberAddr)(L, (double)elapsed);
        ((SignalEventFn)kSignalEventAddr)((void*)frame, (void*)(frame + 0x144), 1, 0);
        ++g_framesWithLuaScript;
    }

    // 2. Dirty alpha / visibility state update: vtable[10] (offset 0x28)
    // Inlined dirty check from sub_48EBA0: tests bit 2 of byte [frame + 0xBF]
    const uint8_t flags = *(const uint8_t*)(frame + 0xBF);
    if (flags & 2) {
        const uintptr_t vtable = *(const uintptr_t*)frame;
        ((Vtable10Fn)*(const uintptr_t*)(vtable + 0x28))((void*)frame);
        ++g_framesWithDirtyState;
    }

    // 3. Children dirty check (first pass)
    const uintptr_t children = *(const uintptr_t*)(frame + 0x214);
    if (children && !(children & 1)) {
        const uint32_t linkOffset = *(const uint32_t*)(frame + 0x20C);
        uintptr_t child = children;
        do {
            const uint8_t childFlags = *(const uint8_t*)(child + 0xBF);
            if (childFlags & 2) {
                const uintptr_t childVtable = *(const uintptr_t*)child;
                ((Vtable10Fn)*(const uintptr_t*)(childVtable + 0x28))((void*)child);
            }
            child = *(const uintptr_t*)(child + linkOffset + 4);
        } while (child && !(child & 1));
    }

    // 4. Animation groups update: vtable[11] (offset 0x2C)
    // Inlined animation check from sub_488890: checks pointer [frame + 0x98]
    const uintptr_t animGroup = *(const uintptr_t*)(frame + 0x98);
    int result = 0;
    if (animGroup) {
        const uintptr_t vtable = *(const uintptr_t*)frame;
        result = ((Vtable11Fn)*(const uintptr_t*)(vtable + 0x2C))((void*)frame, elapsed);
        ++g_framesWithAnim;
    }

    // 5. Children animation update (second pass)
    if (children && !(children & 1)) {
        const uint32_t linkOffset = *(const uint32_t*)(frame + 0x20C);
        uintptr_t child = children;
        do {
            const uintptr_t childAnim = *(const uintptr_t*)(child + 0x98);
            if (childAnim) {
                const uintptr_t childVtable = *(const uintptr_t*)child;
                result = ((Vtable11Fn)*(const uintptr_t*)(childVtable + 0x2C))((void*)child, elapsed);
            }
            child = *(const uintptr_t*)(child + linkOffset + 4);
        } while (child && !(child & 1));
    }

    if (!scriptHandler && !(flags & 2) && !animGroup && (!children || (children & 1))) {
        ++g_framesFastBypassed;
    }

    return result;
}

// Detour for CFrameStrataManager::OnUpdate (0x00495320)
// Hot per-frame callback. NO __try here.
void* __fastcall Hook_FrameStrataManager_OnUpdate(void* this_ptr, void* /*edx*/, float elapsed) {
    if (g_retired || g_abSubject) {
        return orig_StrataOnUpdate(this_ptr, elapsed);
    }

    ++g_strataManagerCalls;
    const uintptr_t mgr = (uintptr_t)this_ptr;

    // 1. Process pending cleanup/unlinking queue at [mgr + 0xCE0]
    while (true) {
        const uint32_t v3 = *(const uint32_t*)(mgr + 0xCE0);
        if ((v3 & 1) != 0 || !v3) {
            break;
        }
        const uint32_t v4 = *(const uint32_t*)(mgr + 0xCD8);
        const uint32_t v5 = *(const uint32_t*)(v4 + v3);
        uint32_t* v6 = (uint32_t*)(v3 + v4);
        if (v5) {
            const uint32_t v7 = v6[1];
            uint32_t* v8;
            if ((v7 & 1) == 0 && v7) {
                v8 = (uint32_t*)((char*)v6 + v7 - *(const uint32_t*)(v5 + 4));
            } else {
                v8 = (uint32_t*)(v7 & 0xFFFFFFFE);
            }
            *v8 = v5;
            *(uint32_t*)(*v6 + 4) = v6[1];
            *v6 = 0;
            v6[1] = 0;
        }
        ((PendingCleanupFn)kPendingCleanupAddr)(0);
    }

    // 2. Pre-update layout resolve (anchors/geometry flush)
    ((LayoutResolveFn)kLayoutResolveAddr)();

    // 3. Iterate 9 frame strata (WORLD to TOOLTIP)
    const uintptr_t* strataArray = (const uintptr_t*)(mgr + 0xCE4);
    for (int s = 0; s < 9; ++s) {
        const uintptr_t stratum = strataArray[s];
        if (!stratum) continue;

        const uint32_t levelCount = *(const uint32_t*)(stratum + 8);
        const uintptr_t* levelArray = *(const uintptr_t**)(stratum + 0x14);
        if (!levelArray || !levelCount) continue;

        for (uint32_t i = 0; i < levelCount; ++i) {
            const uintptr_t level = levelArray[i];
            if (!level) continue;
            ++g_levelsEvaluated;

            // Empty level check:
            // [level + 0x14] is head of active frames (0 or odd bit means empty)
            // [level + 4] is head of pending additions (points to self &level[1] when empty)
            const uintptr_t firstFrame = *(const uintptr_t*)(level + 0x14);
            const uintptr_t pendingHead = *(const uintptr_t*)(level + 4);

            if ((!firstFrame || (firstFrame & 1)) && (pendingHead == (level + 4))) {
                // Level has zero active frames and zero pending frames: skip call
                ++g_levelsSkippedEmpty;
                continue;
            }

            // Dispatch active level through client CFrameLevel::OnUpdate (0x00494A10)
            ((FrameLevelOnUpdateFn)kFrameLevelOnUpdateAddr)((void*)level, elapsed);
        }
    }

    // 4. Post-update layout resolve
    ((LayoutResolveFn)kLayoutResolveAddr)();

    // 5. Mouse move / hit test check
    void* result = this_ptr;
    if (*(const uint32_t*)(mgr + 0x120C) != 0) {
        *(uint32_t*)(mgr + 0x120C) = 0;
        result = (void*)((MouseMoveHitTestFn)kMouseMoveHitTestAddr)((int)(mgr + 0x1210), (int)mgr);
    }

    return result;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptUIStrataOpt) {
        Log("UIStrataOpt: not active: switched off by config.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed(reinterpret_cast<const void*>(kStrataOnUpdateAddr)) ||
        !WowOpt_ClientPatchAllowed(reinterpret_cast<const void*>(kSimpleFrameOnUpdateAddr))) {
        Log("UIStrataOpt: client patch not allowed at hook targets");
        return false;
    }

    // Verify 8-byte prologues against IDA Pro disassembly before hooking
    // sub_495320: 55 8B EC 83 EC 08 53 56
    static const uint8_t kExp_Strata[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56 };
    if (memcmp((const void*)kStrataOnUpdateAddr, kExp_Strata, sizeof(kExp_Strata)) != 0) {
        Log("UIStrataOpt: BAD PROLOGUE at 0x%08X! Hook aborted.", kStrataOnUpdateAddr);
        return false;
    }

    // sub_490770: 55 8B EC 53 56 8B F1 83
    static const uint8_t kExp_SimpleFrame[8] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x83 };
    if (memcmp((const void*)kSimpleFrameOnUpdateAddr, kExp_SimpleFrame, sizeof(kExp_SimpleFrame)) != 0) {
        Log("UIStrataOpt: BAD PROLOGUE at 0x%08X! Hook aborted.", kSimpleFrameOnUpdateAddr);
        return false;
    }

    // Create MinHook detours
    MH_STATUS st1 = WineSafe_CreateHook((void*)kStrataOnUpdateAddr,
                                        (void*)&Hook_FrameStrataManager_OnUpdate,
                                        (void**)&orig_StrataOnUpdate);
    if (st1 != MH_OK) {
        Log("UIStrataOpt: hook on strata manager failed: %d", st1);
        return false;
    }

    MH_STATUS st2 = WineSafe_CreateHook((void*)kSimpleFrameOnUpdateAddr,
                                        (void*)&Hook_SimpleFrame_OnUpdate,
                                        (void**)&orig_SimpleFrameOnUpdate);
    if (st2 != MH_OK) {
        Log("UIStrataOpt: hook on simple frame failed: %d", st2);
        MH_DisableHook((void*)kStrataOnUpdateAddr);
        return false;
    }

    if (WO_EnableHook((void*)kStrataOnUpdateAddr) != MH_OK ||
        WO_EnableHook((void*)kSimpleFrameOnUpdateAddr) != MH_OK) {
        Log("UIStrataOpt: failed to enable hooks.");
        MH_DisableHook((void*)kStrataOnUpdateAddr);
        MH_DisableHook((void*)kSimpleFrameOnUpdateAddr);
        return false;
    }

    g_abSubject = AbTest::IsSubject("UIStrataOpt", &g_abSubject);
    g_installed = true;

    Log("UIStrataOpt: ACTIVE on CFrameStrataManager::OnUpdate (0x%08X) and CSimpleFrame::OnUpdate (0x%08X).",
        kStrataOnUpdateAddr, kSimpleFrameOnUpdateAddr);
    if (g_abSubject) {
        Log("UIStrataOpt: under A/B test: forwarding to original for baseline comparison.");
    }
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kStrataOnUpdateAddr);
    MH_DisableHook((void*)kSimpleFrameOnUpdateAddr);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptUIStrataOpt) {
        Log("UIStrataOpt: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("UIStrataOpt: not installed.");
        return;
    }

    Log("[UIStrataOpt] strata calls: %lu total strata manager ticks.", g_strataManagerCalls);
    Log("[UIStrataOpt] levels: %lu evaluated, %lu skipped empty (%.1f%%).",
        g_levelsEvaluated, g_levelsSkippedEmpty,
        g_levelsEvaluated > 0 ? (100.0 * g_levelsSkippedEmpty / g_levelsEvaluated) : 0.0);
    Log("[UIStrataOpt] frames: %lu evaluated, %lu fast bypassed (%.1f%%), %lu lua scripts, %lu dirty state, %lu anims.",
        g_framesEvaluated, g_framesFastBypassed,
        g_framesEvaluated > 0 ? (100.0 * g_framesFastBypassed / g_framesEvaluated) : 0.0,
        g_framesWithLuaScript, g_framesWithDirtyState, g_framesWithAnim);
}

}  // namespace UIStrataOpt
