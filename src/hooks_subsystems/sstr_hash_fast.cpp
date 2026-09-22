// ============================================================================
// Module: sstr_hash_fast.cpp
//
// Replaces asset path normalization and Jenkins hash lookup in sub_76F640
// (SStrHashHT, 121 bytes).
//
// In profile wow_optimize_2026-09-22_23-08-49.log, sub_76F640 and its callee
// sub_76E7A0 accounted for 4.62% of executing time (292 samples) and were
// repeatedly flagged by FreezeCatcher during loading/transition spikes.
// Across 69 callers in texture, model, sound, and DBC loaders, sub_76F640
// calls sub_76E7A0 (95 instructions with jump tables) for every character
// in every asset path to convert case and replace '/' with '\\'.
//
// This replacement uses SSE2 16-byte vector blocks to perform ASCII case
// conversion and path separator replacement branchlessly without function
// call overhead. If any non-ASCII byte is encountered, it delegates directly
// to the original client function to preserve verbatim UTF-8 semantics.
//
// Verification: Compares results bit for bit against the client function for
// the first 10,000 comparisons and 1 out of every 128 calls thereafter.
// Retires immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sstr_hash_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace SStrHashFast {

namespace {

constexpr uintptr_t kTarget = 0x0076F640;

// push ebp / mov ebp, esp / mov eax, [ebp+arg_0] / sub esp, 408h
const unsigned char kPrologue[12] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x81, 0xEC, 0x08, 0x04, 0x00, 0x00
};

typedef uint32_t (__stdcall *SStrHashHT_fn)(const char* str);
static SStrHashHT_fn g_orig = nullptr;

typedef uint32_t (__cdecl *JenkinsHash_fn)(const void* buf, uint32_t len, uint32_t seed);
static const JenkinsHash_fn pClientJenkins = (JenkinsHash_fn)0x0076F420;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
unsigned long long g_calls = 0;
unsigned long long g_armedCalls = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_controlCalls = 0;
unsigned g_mismatches = 0;

__declspec(noinline) __declspec(safebuffers) static uint32_t Fast_SStrHashHT(const char* str) {
    if (!str) return g_orig(str);

    // Empty string: client sub_76F640 calls sub_76F420(buf, 0, 0)
    if (!*str) {
        return pClientJenkins("", 0, 0);
    }

    char buf[1024];
    uint32_t len = 0;
    const char* s = str;

    const __m128i lower_a   = _mm_set1_epi8('a' - 1);
    const __m128i lower_z   = _mm_set1_epi8('z' + 1);
    const __m128i to_upper  = _mm_set1_epi8(32);
    const __m128i slash     = _mm_set1_epi8('/');
    const __m128i backslash = _mm_set1_epi8('\\');

    while (len + 16 <= 0x3FC) {
        __m128i v = _mm_loadu_si128((const __m128i*)s);

        // Check for non-ASCII (any byte with MSB set >= 0x80)
        int non_ascii = _mm_movemask_epi8(v);
        if (non_ascii != 0) {
            // Non-ASCII UTF-8 detected: fallback to client sub_76E7A0 logic
            return g_orig(str);
        }

        // Check for null terminator
        __m128i is_null = _mm_cmpeq_epi8(v, _mm_setzero_si128());
        int null_mask = _mm_movemask_epi8(is_null);
        if (null_mask != 0) {
            // Reached null terminator within this 16-byte block
            break;
        }

        // ASCII case conversion: if (c >= 'a' && c <= 'z') c -= 32
        __m128i is_lower = _mm_and_si128(_mm_cmpgt_epi8(v, lower_a), _mm_cmplt_epi8(v, lower_z));
        v = _mm_sub_epi8(v, _mm_and_si128(is_lower, to_upper));

        // Replace '/' with '\'
        __m128i is_slash = _mm_cmpeq_epi8(v, slash);
        v = _mm_or_si128(_mm_andnot_si128(is_slash, v), _mm_and_si128(is_slash, backslash));

        _mm_storeu_si128((__m128i*)(buf + len), v);
        len += 16;
        s += 16;
    }

    // Process remainder / tail (or short strings)
    const unsigned char* us = (const unsigned char*)s;
    while (*us && len < 0x3FC) {
        unsigned char c = *us++;
        if (c >= 0x80) {
            return g_orig(str);
        }
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        } else if (c == '/') {
            c = '\\';
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';

    return pClientJenkins((const unsigned char*)buf, len, 0);
}

__declspec(safebuffers) static uint32_t __stdcall Hook_SStrHashHT(const char* str) {
    if (g_dead) return g_orig(str);

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(str);
    }

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        const uint32_t expected = g_orig(str);
        const uint32_t actual   = Fast_SStrHashHT(str);
        if (actual != expected) {
            ++g_mismatches;
            g_dead = true;
            Log("[SStrHashFast] Disagreement at str='%s': fast=0x%08X client=0x%08X. Hook retired.",
                str ? str : "(null)", actual, expected);
            return expected;
        }
        return expected;
    }

    ++g_armedCalls;
    return Fast_SStrHashHT(str);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptSStrHashFast) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[SStrHashFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[SStrHashFast] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_SStrHashHT, (void**)&g_orig) != MH_OK) {
        Log("[SStrHashFast] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[SStrHashFast] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("SStrHashFast", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("SStrHashFast_Hook", (const void*)&Hook_SStrHashHT);
    Log("[SStrHashFast] Hook installed on sub_76F640 (0x79 bytes)");
    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        MH_RemoveHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (!g_installed) return;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[SStrHashFast] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace SStrHashFast
