// lua_func_bounds_fast.cpp: Fastpath and zero-allocation check for Lua C function bounds (sub_86B5A0).
//
// Target: sub_86B5A0 (0x0086B5A0, 85 bytes, 30 instructions).
// Callers:
//   1. sub_856370 (luaD_precall at 0x00856514, invoked on every single lua_CFunction call across all addons and UI)
//   2. sub_856550 (luaD_call at 0x008566F8)
//   3. sub_855AF0 (lua_pushcclosure at 0x00855B11)
//   4. sub_855D00 (hook/line dispatch at 0x00855DA6)
//   5. sub_7658A0 through sub_7681F0 (C script callback registration)
//
// Root Cause:
// In sub_86B5A0, the client validates that incoming function pointer argument `fn` resides within
// the executable code segment [dword_D415B8, dword_D415BC) of Wow.exe before dispatching it.
// Every Lua C API call made by addons (e.g. UnitHealth, GetTime, GetSpellInfo, CreateFrame, DEFAULT_CHAT_FRAME:AddMessage)
// passes through this function.
// In the client implementation:
// 1. Sets up an ebp frame and unconditionally allocates 64 bytes of stack space (`sub esp, 40h`).
// 2. Loads dword_D415B8 and dword_D415BC on every single call.
// 3. Tests both global pointers for 0 with conditional branches.
// 4. Executes separate `jb` checks for lower and upper boundaries.
// 5. In >99.999% of calls, `fn` is a valid C function inside Wow.exe.
// 6. Any pointer outside the Wow.exe .text section triggers an assertion formatting a 64-byte string
//    and calling sub_772AA0, which halts the process with ERROR #134.
//
// Speedup and Safety Mechanism:
// 1. Caches code section base and length `[textStart, textStart + textLen)` once initialized.
// 2. Evaluates double-bounded range check with a single unsigned subtraction and comparison:
//    `(fn - textStart) < textLen` in 2 machine instructions.
// 3. Completely eliminates the 64-byte stack allocation (`sub esp, 40h`), running with zero frame allocation.
// 4. Recognizes function pointers located within the wow_optimize.dll module space, allowing safe Lua C function
//    registration from the optimizer without triggering ERROR #134.
// 5. Periodically verifies globals dword_D415B8 and dword_D415BC every 128 calls to guard against memory relocation.
// 6. Retains exact return value (textStart in EAX) and delegates invalid pointers to client's original error reporter.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "lua_func_bounds_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace LuaFuncBoundsFast {

namespace {

constexpr uintptr_t kTarget = 0x0086B5A0;

// Prologue bytes of sub_86B5A0 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// A1 B8 15 D4 00    mov     eax, dword_D415B8
// 8B 0D BC 15 D4 00 mov     ecx, dword_D415BC
// 83 EC 40          sub     esp, 40h
// 85 C0             test    eax, eax
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0xA1, 0xB8, 0x15, 0xD4, 0x00,
    0x8B, 0x0D, 0xBC, 0x15, 0xD4, 0x00, 0x83, 0xEC
};

typedef uint32_t (__cdecl *OrigFn)(uint32_t fn);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint32_t  s_textStart = 0;
static uint32_t  s_textEnd   = 0;
static uint32_t  s_textLen   = 0;

static uintptr_t s_dllBase   = 0;
static uintptr_t s_dllSize   = 0;

static uint64_t g_calls      = 0;
static uint64_t g_fastHits   = 0;
static uint64_t g_dllHits    = 0;
static uint64_t g_slowPass   = 0;

__declspec(safebuffers) static uint32_t __cdecl Hook_sub_86B5A0(uint32_t fn) {
    if (g_retired) {
        return g_orig(fn);
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        return g_orig(fn);
    }

    const uint64_t callIndex = ++g_calls;

    const uint32_t textStart = s_textStart;
    const uint32_t textLen   = s_textLen;

    if (textLen != 0) {
        // Fastpath: single unsigned subtract-and-compare evaluates [textStart, textStart + textLen)
        if ((fn - textStart) < textLen) {
            // Periodic consistency check every 128 calls to ensure globals were not moved
            if ((callIndex & 0x7F) == 0) {
                const uint32_t curStart = *(const uint32_t*)0x00D415B8;
                const uint32_t curEnd   = *(const uint32_t*)0x00D415BC;
                if (curStart != textStart || (curEnd - curStart) != textLen) {
                    s_textStart = curStart;
                    s_textEnd   = curEnd;
                    s_textLen   = (curEnd > curStart) ? (curEnd - curStart) : 0;
                }
            }
            ++g_fastHits;
            return textStart;
        }

        // Check if pointer resides in wow_optimize.dll module space
        if (s_dllSize != 0 && (fn - (uint32_t)s_dllBase) < (uint32_t)s_dllSize) {
            ++g_dllHits;
            return textStart;
        }
    }

    // Pointer is outside valid ranges or globals uninitialized: let client handle it
    const uint32_t ret = g_orig(fn);
    if (s_textLen == 0) {
        s_textStart = *(const uint32_t*)0x00D415B8;
        s_textEnd   = *(const uint32_t*)0x00D415BC;
        if (s_textEnd > s_textStart) {
            s_textLen = s_textEnd - s_textStart;
        }
    }
    ++g_slowPass;
    return ret;
}

} // anonymous namespace

bool Init() {
    if (g_installed) return true;

    if (!Config::g_settings.OptLuaFuncBoundsFast) {
        return false;
    }

    if (memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[LuaFuncBoundsFast] Prologue mismatch at 0x%08X; hook skipped.", (unsigned)kTarget);
        return false;
    }

    // Initialize module bounds for wow_optimize.dll
    HMODULE hSelf = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Init, &hSelf) && hSelf) {
        const PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hSelf;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uintptr_t)hSelf + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                s_dllBase = (uintptr_t)hSelf;
                s_dllSize = nt->OptionalHeader.SizeOfImage;
            }
        }
    }

    // Read current text section boundaries if already populated
    s_textStart = *(const uint32_t*)0x00D415B8;
    s_textEnd   = *(const uint32_t*)0x00D415BC;
    if (s_textEnd > s_textStart) {
        s_textLen = s_textEnd - s_textStart;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_86B5A0, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[LuaFuncBoundsFast] WineSafe_CreateHook failed: %d", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[LuaFuncBoundsFast] WO_EnableHook failed: %d", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("LuaFuncBoundsFast_Hook", (const void*)&Hook_sub_86B5A0);
    g_abSubject = AbTest::IsSubject("LuaFuncBoundsFast", &g_abSubject);
    g_installed = true;

    Log("[LuaFuncBoundsFast] ACTIVE on sub_86B5A0 (0x%08X, 85 bytes). text: [0x%08X, 0x%08X). Off by default.",
        (unsigned)kTarget, (unsigned)s_textStart, (unsigned)s_textEnd);
    if (g_abSubject) {
        Log("[LuaFuncBoundsFast] under A/B test: toggled by the test harness.");
    }

    return true;
}

void Install() {
    Init();
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptLuaFuncBoundsFast) {
        Log("[LuaFuncBoundsFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[LuaFuncBoundsFast] not installed.");
        return;
    }

    Log("[LuaFuncBoundsFast] calls: %llu (fast hits: %llu, dll hits: %llu, engine pass: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_fastHits,
        (unsigned long long)g_dllHits,
        (unsigned long long)g_slowPass);
}

} // namespace LuaFuncBoundsFast
