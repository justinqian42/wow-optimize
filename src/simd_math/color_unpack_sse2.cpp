// ============================================================================
// Module: color_unpack_sse2
//
// Hardware SSE2 color unpacking and vector math optimizations:
//   sub_984C90: Color_UnpackBGRA (32-bit BGRA bytes -> 4x float RGBA in [0, 1])
//   sub_982970: Color_UnpackBGR  (24-bit BGR bytes  -> 3x float RGB  in [0, 1])
//   sub_9829B0: Vec3_DominantAxis (C3Vector dominant axis index 0, 1, 2)
//
// Background & Analysis:
//   sub_984C90 is invoked across 12 callers in the rendering and material pipeline
//   (vertex lighting setup, particle emitter initialization, water/sky rendering,
//   M2 model lighting, and WMO material passes). In stock x87, it spills each of
//   the 4 bytes to stack memory, then executes integer load fild, fmul ds:flt_A45564
//   (1.0f / 255.0f), and fstp dword ptr four times in a row.
//
//   sub_982970 is invoked across 6 callers in character model rendering and
//   lighting, performing the same stack spill and fild/fmul/fstp pattern for 3 channels.
//
//   sub_9829B0 is invoked across 8 callers in collision clipping, polygon
//   projection, and bounding calculations. It loads 3 floats to the x87 stack,
//   computes fabs, and evaluates 3 conditional branches with status word transfers
//   (fnstsw ax / test ah, 41h), suffering frequent branch mispredictions.
//
//   This module vectorizes color unpacking using parallel integer unpack/shuffle
//   and SSE2 cvtdq2ps/mulps in a single pipeline without stack spills.
//   Vec3_DominantAxis evaluates coordinate magnitudes using bitwise IEEE fabs.
//
//   Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "color_unpack_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"
#include "session_verdict.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace ColorUnpack {

#if !TEST_DISABLE_COLOR_UNPACK_SSE2

namespace {

constexpr uintptr_t kColorUnpackBGRA = 0x00984C90;
constexpr uintptr_t kColorUnpackBGR  = 0x00982970;
constexpr uintptr_t kVec3DomAxis     = 0x009829B0;

// __fastcall mirrors x86 __thiscall ABI: ECX = this, EDX = dummy padding
typedef float* (__fastcall* ColorUnpackBGRA_fn)(float* this_out, void* edx, const uint8_t* bgra);
typedef float* (__fastcall* ColorUnpackBGR_fn)(float* this_out, void* edx, const uint8_t* bgr);
typedef int    (__fastcall* Vec3DominantAxis_fn)(const float* this_vec, void* edx);

ColorUnpackBGRA_fn  orig_ColorUnpackBGRA  = nullptr;
ColorUnpackBGR_fn   orig_ColorUnpackBGR   = nullptr;
Vec3DominantAxis_fn orig_Vec3DominantAxis = nullptr;

bool g_installed      = false;
bool g_armedBgra      = false;
bool g_armedBgr       = false;
bool g_armedDomAxis   = false;
bool g_abSubject      = false;
bool g_dead           = false;

unsigned long g_callsBgra        = 0;
unsigned long g_callsBgr         = 0;
unsigned long g_callsDomAxis     = 0;
unsigned long g_verifiedBgra     = 0;
unsigned long g_verifiedBgr      = 0;
unsigned long g_verifiedDomAxis  = 0;

constexpr unsigned long kVerifyFirst  = 30000;
constexpr unsigned long kResampleMask = 8191;

// Constant 1.0f / 255.0f matching client ds:flt_A45564 (0x3B808081)
constexpr float kInv255 = 0.003921568859368563f;

// ============================================================================
// Core Vectorized Implementations
// ============================================================================

inline float* Color_UnpackBGRA_SSE2(float* this_out, const uint8_t* bgra) {
    if (!this_out || !bgra) return this_out;

    const __m128 scale = _mm_set1_ps(kInv255);
    const __m128i zero = _mm_setzero_si128();

    uint32_t raw;
    memcpy(&raw, bgra, sizeof(raw));

    __m128i v = _mm_cvtsi32_si128((int)raw);
    v = _mm_unpacklo_epi8(v, zero);   // words: B, G, R, A, 0, 0, 0, 0
    v = _mm_unpacklo_epi16(v, zero);  // dwords: B(0), G(1), R(2), A(3)
    // Swizzle from BGRA to RGBA
    v = _mm_shuffle_epi32(v, _MM_SHUFFLE(3, 0, 1, 2));

    __m128 f = _mm_cvtepi32_ps(v);
    f = _mm_mul_ps(f, scale);

    _mm_storeu_ps(this_out, f);
    return this_out;
}

inline float* Color_UnpackBGR_SSE2(float* this_out, const uint8_t* bgr) {
    if (!this_out || !bgr) return this_out;

    const __m128 scale = _mm_set1_ps(kInv255);
    const __m128i zero = _mm_setzero_si128();

    const uint32_t raw = (uint32_t)bgr[0] | ((uint32_t)bgr[1] << 8) | ((uint32_t)bgr[2] << 16);

    __m128i v = _mm_cvtsi32_si128((int)raw);
    v = _mm_unpacklo_epi8(v, zero);
    v = _mm_unpacklo_epi16(v, zero);
    v = _mm_shuffle_epi32(v, _MM_SHUFFLE(3, 0, 1, 2));

    __m128 f = _mm_cvtepi32_ps(v);
    f = _mm_mul_ps(f, scale);

    alignas(16) float buf[4];
    _mm_store_ps(buf, f);

    this_out[0] = buf[0];
    this_out[1] = buf[1];
    this_out[2] = buf[2];
    return this_out;
}

inline int Vec3_DominantAxis_Fast(const float* this_vec) {
    if (!this_vec) return 0;

    uint32_t ix, iy, iz;
    memcpy(&ix, &this_vec[0], sizeof(float));
    memcpy(&iy, &this_vec[1], sizeof(float));
    memcpy(&iz, &this_vec[2], sizeof(float));

    // Clear IEEE sign bit for instant branchless fabs
    ix &= 0x7FFFFFFFu;
    iy &= 0x7FFFFFFFu;
    iz &= 0x7FFFFFFFu;

    float ax, ay, az;
    memcpy(&ax, &ix, sizeof(float));
    memcpy(&ay, &iy, sizeof(float));
    memcpy(&az, &iz, sizeof(float));

    // Match client sub_9829B0 exact branch order and tie-breaking:
    // Ties favour Z over X/Y and Y over X.
    if (ax <= ay) {
        return (ay > az) ? 1 : 2;
    } else {
        return (ax > az) ? 0 : 2;
    }
}

// ============================================================================
// Dual-Run Hook Bodies
// ============================================================================

float* Hooked_ColorUnpackBGRABody(float* this_out, void* edx, const uint8_t* bgra) {
    g_callsBgra++;
    if (g_dead) {
        return orig_ColorUnpackBGRA(this_out, edx, bgra);
    }

    if (!g_armedBgra || (g_callsBgra & kResampleMask) == 0) {
        float theirs[4];
        float mine[4];
        orig_ColorUnpackBGRA(theirs, edx, bgra);
        Color_UnpackBGRA_SSE2(mine, bgra);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "Color_UnpackBGRA disagreed with client; retired for session");
            Log("[ColorUnpack] DISAGREED on Color_UnpackBGRA at call %lu - retired for session", g_callsBgra);
            memcpy(this_out, theirs, sizeof(theirs));
            return this_out;
        }

        g_verifiedBgra++;
        if (!g_armedBgra && g_verifiedBgra >= kVerifyFirst) {
            g_armedBgra = true;
            Log("[ColorUnpack] Color_UnpackBGRA armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedBgra, kResampleMask + 1);
        }
        memcpy(this_out, theirs, sizeof(theirs));
        return this_out;
    }

    return Color_UnpackBGRA_SSE2(this_out, bgra);
}

float* Hooked_ColorUnpackBGRBody(float* this_out, void* edx, const uint8_t* bgr) {
    g_callsBgr++;
    if (g_dead) {
        return orig_ColorUnpackBGR(this_out, edx, bgr);
    }

    if (!g_armedBgr || (g_callsBgr & kResampleMask) == 0) {
        float theirs[3];
        float mine[3];
        orig_ColorUnpackBGR(theirs, edx, bgr);
        Color_UnpackBGR_SSE2(mine, bgr);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "Color_UnpackBGR disagreed with client; retired for session");
            Log("[ColorUnpack] DISAGREED on Color_UnpackBGR at call %lu - retired for session", g_callsBgr);
            memcpy(this_out, theirs, sizeof(theirs));
            return this_out;
        }

        g_verifiedBgr++;
        if (!g_armedBgr && g_verifiedBgr >= kVerifyFirst) {
            g_armedBgr = true;
            Log("[ColorUnpack] Color_UnpackBGR armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedBgr, kResampleMask + 1);
        }
        memcpy(this_out, theirs, sizeof(theirs));
        return this_out;
    }

    return Color_UnpackBGR_SSE2(this_out, bgr);
}

int Hooked_Vec3DominantAxisBody(const float* this_vec, void* edx) {
    g_callsDomAxis++;
    if (g_dead) {
        return orig_Vec3DominantAxis(this_vec, edx);
    }

    if (!g_armedDomAxis || (g_callsDomAxis & kResampleMask) == 0) {
        const int theirs = orig_Vec3DominantAxis(this_vec, edx);
        const int mine = Vec3_DominantAxis_Fast(this_vec);

        if (theirs != mine) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "Vec3_DominantAxis disagreed with client; retired for session");
            Log("[ColorUnpack] DISAGREED on Vec3_DominantAxis at call %lu - retired for session", g_callsDomAxis);
            return theirs;
        }

        g_verifiedDomAxis++;
        if (!g_armedDomAxis && g_verifiedDomAxis >= kVerifyFirst) {
            g_armedDomAxis = true;
            Log("[ColorUnpack] Vec3_DominantAxis armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedDomAxis, kResampleMask + 1);
        }
        return theirs;
    }

    return Vec3_DominantAxis_Fast(this_vec);
}

// ============================================================================
// Hook Detours with AbTest and SEH Protection
// ============================================================================

float* __fastcall Hooked_ColorUnpackBGRA(float* this_out, void* edx, const uint8_t* bgra) {
    if (!g_abSubject) {
        return Hooked_ColorUnpackBGRABody(this_out, edx, bgra);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_ColorUnpackBGRA(this_out, edx, bgra);
        } else {
            res = Hooked_ColorUnpackBGRABody(this_out, edx, bgra);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_ColorUnpackBGRA(this_out, edx, bgra);
    }
    AbTest::TickOut(t);
    return res;
}

float* __fastcall Hooked_ColorUnpackBGR(float* this_out, void* edx, const uint8_t* bgr) {
    if (!g_abSubject) {
        return Hooked_ColorUnpackBGRBody(this_out, edx, bgr);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_ColorUnpackBGR(this_out, edx, bgr);
        } else {
            res = Hooked_ColorUnpackBGRBody(this_out, edx, bgr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_ColorUnpackBGR(this_out, edx, bgr);
    }
    AbTest::TickOut(t);
    return res;
}

int __fastcall Hooked_Vec3DominantAxis(const float* this_vec, void* edx) {
    if (!g_abSubject) {
        return Hooked_Vec3DominantAxisBody(this_vec, edx);
    }
    const unsigned long long t = AbTest::TickIn();
    int res = 0;
    __try {
        if (AbTest::StandAside()) {
            res = orig_Vec3DominantAxis(this_vec, edx);
        } else {
            res = Hooked_Vec3DominantAxisBody(this_vec, edx);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_Vec3DominantAxis(this_vec, edx);
    }
    AbTest::TickOut(t);
    return res;
}

// ============================================================================
// Startup Self-Tests
// ============================================================================

static bool SelfTestColorUnpack() {
    ColorUnpackBGRA_fn orig_bgra = (ColorUnpackBGRA_fn)kColorUnpackBGRA;
    ColorUnpackBGR_fn  orig_bgr  = (ColorUnpackBGR_fn)kColorUnpackBGR;

    // Test every single one of the 256 possible byte values across all 4 channels
    for (int b = 0; b < 256; ++b) {
        const uint8_t in_bgra[4] = {
            (uint8_t)b,
            (uint8_t)((b * 37 + 13) & 0xFF),
            (uint8_t)((b * 73 + 47) & 0xFF),
            (uint8_t)((b * 101 + 83) & 0xFF)
        };

        float theirs_bgra[4] = { 0 };
        float mine_bgra[4]   = { 0 };

        __try {
            orig_bgra(theirs_bgra, nullptr, in_bgra);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[ColorUnpack] SelfTestColorUnpack: client Color_UnpackBGRA faulted on byte %d - not installing", b);
            return false;
        }

        Color_UnpackBGRA_SSE2(mine_bgra, in_bgra);

        if (memcmp(theirs_bgra, mine_bgra, sizeof(theirs_bgra)) != 0) {
            Log("[ColorUnpack] SelfTestColorUnpack: MISMATCH on BGRA byte %d!\n"
                "  Client: (%.6f, %.6f, %.6f, %.6f)\n"
                "  Ours:   (%.6f, %.6f, %.6f, %.6f)",
                b,
                theirs_bgra[0], theirs_bgra[1], theirs_bgra[2], theirs_bgra[3],
                mine_bgra[0], mine_bgra[1], mine_bgra[2], mine_bgra[3]);
            return false;
        }

        float theirs_bgr[3] = { 0 };
        float mine_bgr[3]   = { 0 };

        __try {
            orig_bgr(theirs_bgr, nullptr, in_bgra);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[ColorUnpack] SelfTestColorUnpack: client Color_UnpackBGR faulted on byte %d - not installing", b);
            return false;
        }

        Color_UnpackBGR_SSE2(mine_bgr, in_bgra);

        if (memcmp(theirs_bgr, mine_bgr, sizeof(theirs_bgr)) != 0) {
            Log("[ColorUnpack] SelfTestColorUnpack: MISMATCH on BGR byte %d!\n"
                "  Client: (%.6f, %.6f, %.6f)\n"
                "  Ours:   (%.6f, %.6f, %.6f)",
                b,
                theirs_bgr[0], theirs_bgr[1], theirs_bgr[2],
                mine_bgr[0], mine_bgr[1], mine_bgr[2]);
            return false;
        }
    }

    Log("[ColorUnpack] SelfTestColorUnpack: 256/256 byte permutations passed with 100%% bit-exact match against client.");
    return true;
}

static bool SelfTestDominantAxis() {
    Vec3DominantAxis_fn orig_dom = (Vec3DominantAxis_fn)kVec3DomAxis;
    const int CASES = 10000;
    unsigned seed = 0x54321A7Du;

    for (int c = 0; c < CASES; ++c) {
        float v[3];
        for (int i = 0; i < 3; ++i) {
            seed = seed * 1103515245u + 12345u;
            v[i] = (float)((int)(seed >> 16) - 16384) * 0.25f;
        }

        // Include edge cases with exact ties
        if (c % 10 == 0) {
            v[1] = v[0]; // X == Y
        }
        if (c % 15 == 0) {
            v[2] = v[0]; // X == Z
        }
        if (c % 25 == 0) {
            v[2] = v[1]; // Y == Z
        }

        int theirs = -1;
        __try {
            theirs = orig_dom(v, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[ColorUnpack] SelfTestDominantAxis: client routine faulted on case %d - not installing", c);
            return false;
        }

        const int mine = Vec3_DominantAxis_Fast(v);

        if (theirs != mine) {
            Log("[ColorUnpack] SelfTestDominantAxis: MISMATCH on case %d!\n"
                "  Vec=(%.6f, %.6f, %.6f) Client=%d Ours=%d",
                c, v[0], v[1], v[2], theirs, mine);
            return false;
        }
    }

    Log("[ColorUnpack] SelfTestDominantAxis: passed %d test cases with 100%% identity match against client.", CASES);
    return true;
}

}  // namespace

#endif  // !TEST_DISABLE_COLOR_UNPACK_SSE2

bool Init() {
#if TEST_DISABLE_COLOR_UNPACK_SSE2
    Log("[ColorUnpack] DISABLED via feature flag");
    return false;
#else
    if (!Config::g_settings.OptColorUnpack) {
        Log("[ColorUnpack] not installed: switched off.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kColorUnpackBGRA) ||
        !WowOpt_ClientPatchAllowed((const void*)kColorUnpackBGR) ||
        !WowOpt_ClientPatchAllowed((const void*)kVec3DomAxis)) {
        Log("[ColorUnpack] not installed: No Client Patches is active.");
        return false;
    }

    if (IsBadReadPtr((void*)kColorUnpackBGRA, 16) ||
        IsBadReadPtr((void*)kColorUnpackBGR, 16) ||
        IsBadReadPtr((void*)kVec3DomAxis, 16)) {
        Log("[ColorUnpack] unreadable targets - not installing.");
        return false;
    }

    const unsigned char* pBgra = (const unsigned char*)kColorUnpackBGRA;
    const unsigned char* pBgr  = (const unsigned char*)kColorUnpackBGR;
    const unsigned char* pDom  = (const unsigned char*)kVec3DomAxis;

    if (pBgra[0] != 0x55 || pBgra[1] != 0x8B || pBgra[2] != 0xEC ||
        pBgr[0]  != 0x55 || pBgr[1]  != 0x8B || pBgr[2]  != 0xEC ||
        pDom[0]  != 0xD9 || pDom[1]  != 0x01 || pDom[2]  != 0xD9 || pDom[3] != 0xE1) {
        Log("[ColorUnpack] unexpected prologue bytes - not installing.");
        return false;
    }

    if (!SelfTestColorUnpack() || !SelfTestDominantAxis()) {
        Log("[ColorUnpack] startup self-tests failed - not installing.");
        return false;
    }

    int ok = 0;
    if (WineSafe_CreateHook((void*)kColorUnpackBGRA, (void*)Hooked_ColorUnpackBGRA,
                            (void**)&orig_ColorUnpackBGRA) == MH_OK &&
        WO_EnableHook((void*)kColorUnpackBGRA) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("ColorUnpackBGRA_SSE2", (const void*)&Hooked_ColorUnpackBGRA);
    }

    if (WineSafe_CreateHook((void*)kColorUnpackBGR, (void*)Hooked_ColorUnpackBGR,
                            (void**)&orig_ColorUnpackBGR) == MH_OK &&
        WO_EnableHook((void*)kColorUnpackBGR) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("ColorUnpackBGR_SSE2", (const void*)&Hooked_ColorUnpackBGR);
    }

    if (WineSafe_CreateHook((void*)kVec3DomAxis, (void*)Hooked_Vec3DominantAxis,
                            (void**)&orig_Vec3DominantAxis) == MH_OK &&
        WO_EnableHook((void*)kVec3DomAxis) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("Vec3DominantAxis_SSE2", (const void*)&Hooked_Vec3DominantAxis);
    }

    if (ok == 0) {
        Log("[ColorUnpack] not installed: MinHook failed on all targets.");
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("ColorUnpack", &g_abSubject);

    Log("[ColorUnpack] ACTIVE on %d of 3 color unpacking and vector routines "
        "(Color_UnpackBGRA, Color_UnpackBGR, Vec3_DominantAxis). "
        "Verifying first %lu calls bit for bit with ongoing resampling.%s",
        ok, kVerifyFirst,
        Config::g_settings.OptAbTest
            ? " The A/B harness alternates the switch between stints."
            : "");
    return true;
#endif
}

void Shutdown() {
#if !TEST_DISABLE_COLOR_UNPACK_SSE2
    if (!g_installed) return;
    MH_DisableHook((void*)kColorUnpackBGRA);
    MH_DisableHook((void*)kColorUnpackBGR);
    MH_DisableHook((void*)kVec3DomAxis);
    g_installed = false;
#endif
}

void LogStats() {
#if TEST_DISABLE_COLOR_UNPACK_SSE2
    Log("[ColorUnpack] not measured: disabled via feature flag.");
#else
    if (!Config::g_settings.OptColorUnpack) {
        Log("[ColorUnpack] not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("[ColorUnpack] not measured: hooks are not installed.");
        return;
    }
    if (g_dead) {
        Log("[ColorUnpack] retired early: evaluation disagreed with the client.");
        return;
    }
    const unsigned long total = g_callsBgra + g_callsBgr + g_callsDomAxis;
    if (total == 0) {
        Log("[ColorUnpack] measured and zero: the hooks are in and none was reached.");
        return;
    }
    Log("[ColorUnpack] calls: BGRA=%lu (verified=%lu, armed=%s) | "
        "BGR=%lu (verified=%lu, armed=%s) | "
        "dom_axis=%lu (verified=%lu, armed=%s)",
        g_callsBgra, g_verifiedBgra, g_armedBgra ? "yes" : "no",
        g_callsBgr, g_verifiedBgr, g_armedBgr ? "yes" : "no",
        g_callsDomAxis, g_verifiedDomAxis, g_armedDomAxis ? "yes" : "no");
#endif
}

}  // namespace ColorUnpack
