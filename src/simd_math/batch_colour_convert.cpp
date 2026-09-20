// ============================================================================
// Module: batch_colour_convert.cpp
//
// The colour block inside the M2 batch state setup, sub_81FB10. That function
// is 2.49% of executing main-thread time in a tester's 2026-09-19 profile, and
// the profile's hottest instruction in it is 0x0081FC88 - the third `fnstcw` of
// three float-to-byte conversions, each of which switches the x87 control word
// to truncating and back:
//
//     fnstcw [cw] / movzx / or eax,0C00h / fldcw [tmp] / fistp [tmp] / fldcw [cw]
//
// Six control-word loads a batch, and a control-word load drains the x87
// pipeline. The conversion itself is one SSE2 instruction that needs no mode
// change, so what is replaced here is almost entirely stalls.
//
// What the block does, over the three floats at object+0B8h, 0BCh and 0C0h:
//
//     nothing at or below zero, 255 at or above one, otherwise x*255 + 0.5
//     truncated - and the result is a colour with those three bytes and 0xFF
//
// It is reached only as case 1 of a jump table at 0x0081FBA3, runs from
// 0x0081FBAA to 0x0081FCAC, consumes the one value the caller left on the x87
// stack, and leaves the colour in [ebp-4] before joining the rest of the
// function at 0x0081FCEE. So the replacement is five bytes at the top of the
// block jumping to a thunk that does exactly that and jumps back.
//
// How the reading above was checked. The client's 258 bytes were copied
// instruction for instruction into an offline harness and run against this
// code over four million cases - whole-bit-pattern garbage, denormals, values
// either side of both thresholds:
//
//     4000000 cases, 0 differing
//
// That also proves the stack discipline: a replacement that consumed the wrong
// number of x87 values would have wrapped the stack long before four million
// iterations and answered nonsense.
//
// What this cannot do is verify itself against the client at runtime, because
// the block it replaces is gone once the jump is written. In its place:
//
//   - the nine bytes at the top of the block and the eleven at the bottom are
//     compared against what was read from this client before anything is
//     patched, so a different build is refused rather than half-replaced;
//   - six fixed cases are run through the replacement at startup and compared
//     against what the client's own block answers for them, which catches a
//     compiler that has decided to be clever about the arithmetic.
//
// The thunk keeps ebx, esi, edi and ebp, which the rest of the function needs,
// and touches the x87 stack exactly once, to drop the value the block would
// have consumed.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "batch_colour_convert.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace BatchColourConvert {

namespace {

constexpr uintptr_t kBlockHead = 0x0081FBAA;
constexpr uintptr_t kBlockTail = 0x0081FCA3;
constexpr uintptr_t kRejoin    = 0x0081FCEE;
constexpr unsigned  kHeadLen   = 9;

// mov ecx,[esi+70h] / fcom dword ptr [ecx+0C0h]
const unsigned char kHeadWant[kHeadLen] = {
    0x8B, 0x4E, 0x70, 0xD8, 0x91, 0xC0, 0x00, 0x00, 0x00
};
// mov edx,[ebp-8] / fldcw [ebp-2] / mov [ebp-4],edx / jmp short
const unsigned char kTailWant[11] = {
    0x8B, 0x55, 0xF8, 0xD9, 0x6D, 0xFE, 0x89, 0x55, 0xFC, 0xEB, 0x40
};

constexpr unsigned kColoursOffset = 0xB8;

bool g_patched = false;
unsigned char g_saved[kHeadLen];
uintptr_t g_rejoin = kRejoin;

// Plain counter on a path that runs once per batch. Lower bound.
unsigned long long g_calls = 0;

__forceinline uint32_t Channel(float x) {
    if (!(0.0f < x)) return 0;
    if (!(1.0f > x)) return 255;
    const double v = (double)x * 255.0 + 0.5;
    return (uint32_t)(int32_t)v & 0xFFu;
}

uint32_t __cdecl ComputeColour(const float* v) {
    ++g_calls;
    return 0xFF000000u | (Channel(v[0]) << 16) | (Channel(v[1]) << 8) | Channel(v[2]);
}

// Entered by a jump from inside sub_81FB10: esi is the object, ebp is its
// frame, and the x87 stack holds the one value the block would have consumed.
__declspec(naked) void Thunk() {
    __asm {
        fstp st(0)
        mov  eax, [esi+70h]
        add  eax, 0B8h          // kColoursOffset, as an immediate
        push eax
        call ComputeColour
        add  esp, 4
        mov  [ebp-4], eax
        jmp  dword ptr [g_rejoin]
    }
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What the client's own block answers for six fixed inputs, taken from the
// harness that ran its instruction sequence.
struct SelfTest { float v0, v1, v2; uint32_t want; };
const SelfTest kSelfTest[] = {
    { 0.0f, 0.0f, 0.0f, 0xFF000000u },
    { 1.0f, 1.0f, 1.0f, 0xFFFFFFFFu },
    { 0.5f, 0.25f, 0.125f, 0xFF804020u },
    { -1.0f, 2.0f, 0.99999994f, 0xFF00FFFFu },
    { 0.00196078001f, 0.501960814f, 0.998039186f, 0xFF0080FEu },
    { 9.99999972e-10f, 0.333333343f, 0.666666687f, 0xFF0055AAu },
};

bool SelfTestPasses() {
    for (int i = 0; i < (int)(sizeof(kSelfTest) / sizeof(kSelfTest[0])); ++i) {
        const float v[3] = { kSelfTest[i].v0, kSelfTest[i].v1, kSelfTest[i].v2 };
        const uint32_t got = 0xFF000000u | (Channel(v[0]) << 16) |
                             (Channel(v[1]) << 8) | Channel(v[2]);
        if (got != kSelfTest[i].want) {
            Log("[BatchColour] NOT patched: case %d (%.9g, %.9g, %.9g) answered "
                "%08X here and %08X in the client's own block.",
                i, v[0], v[1], v[2], got, kSelfTest[i].want);
            return false;
        }
    }
    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptBatchColourConvert) return true;

    if (!BytesMatch(kBlockHead, kHeadWant, kHeadLen)) {
        Log("[BatchColour] NOT patched: the bytes at 0x%08X are not the colour block "
            "this was read from.", (unsigned)kBlockHead);
        return false;
    }
    if (!BytesMatch(kBlockTail, kTailWant, sizeof(kTailWant))) {
        Log("[BatchColour] NOT patched: the head at 0x%08X matches but the tail at "
            "0x%08X does not, so the block between them is not the one being "
            "replaced.", (unsigned)kBlockHead, (unsigned)kBlockTail);
        return false;
    }
    if (!SelfTestPasses()) return false;
    if (!WowOpt_ClientPatchAllowed((const void*)kBlockHead)) {
        Log("[BatchColour] NOT patched: No Client Patches is on, and this writes "
            "five bytes into wow.exe.");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)kBlockHead, kHeadLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[BatchColour] NOT patched: could not make 0x%08X writable",
            (unsigned)kBlockHead);
        return false;
    }
    memcpy(g_saved, (const void*)kBlockHead, kHeadLen);

    unsigned char patch[kHeadLen];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&Thunk - (kBlockHead + 5));
    memset(patch + 5, 0x90, kHeadLen - 5);
    memcpy((void*)kBlockHead, patch, kHeadLen);

    DWORD ignored = 0;
    VirtualProtect((void*)kBlockHead, kHeadLen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kHeadLen);

    g_patched = true;
    SamplingProfiler::RegisterSelfSymbol("BatchColourConvert", (const void*)&Thunk);
    Log("[BatchColour] ACTIVE on the colour block in sub_81FB10 (0x%08X), whose "
        "function is 2.49%% of executing time in an uncapped tester session and "
        "whose hottest instruction is the third of six x87 control-word loads. "
        "Three clamps and three converts, with no mode change. Checked against the "
        "client's own instruction sequence over four million cases offline and on "
        "six fixed ones at startup; the twenty bytes that bracket the block were "
        "compared before anything was written.", (unsigned)kBlockHead);
    return true;
}

void Shutdown() {
    if (!g_patched) return;
    DWORD old = 0;
    if (VirtualProtect((void*)kBlockHead, kHeadLen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void*)kBlockHead, g_saved, kHeadLen);
        DWORD ignored = 0;
        VirtualProtect((void*)kBlockHead, kHeadLen, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kHeadLen);
    }
    g_patched = false;
}

void LogStats() {
    if (!Config::g_settings.OptBatchColourConvert) return;
    if (!g_patched) {
        Log("[BatchColour] not patched - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[BatchColour] patched, and the block has not been reached yet. That is "
            "a measurement: no batch has taken this branch since it went in.");
        return;
    }
    Log("[BatchColour] %llu batch colour(s) converted here, each saving six x87 "
        "control-word loads. Plain counter, a lower bound.", g_calls);
}

}  // namespace BatchColourConvert
