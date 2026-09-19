// ============================================================================
// Module: anim_spline_track_sse2
//
// Hardware double-precision SSE2 rewrite of the M2 cubic spline animation track
// evaluators:
//   sub_82B460: 3D vector spline tracks (cubic Hermite / Bezier)
//   sub_82B8A0: scalar spline tracks (cubic Hermite / Bezier)
//
// Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "anim_spline_track_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"
#include "session_verdict.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace AnimSplineTrack {

namespace {

constexpr uintptr_t kTrackSplineVec3   = 0x0082B460;
constexpr uintptr_t kTrackSplineScalar = 0x0082B8A0;
constexpr uintptr_t kFindKey           = 0x008284D0;

// Animation state layout
constexpr unsigned kS_timing1  = 0x40;
constexpr unsigned kS_trackIdx = 0x44;   // uint16_t
constexpr unsigned kS_timing2  = 0x64;
constexpr unsigned kS_blendIdx = 0x68;   // uint16_t
constexpr unsigned kS_blend    = 0xA8;   // float

// Track descriptor layout
constexpr unsigned kT_interp    = 0x00;  // uint16_t; 0: none, 1: linear, 2: bezier, 3: hermite
constexpr unsigned kT_globalSeq = 0x02;  // uint16_t; 0xFFFF means no global sequence
constexpr unsigned kT_count     = 0x0C;  // uint32_t
constexpr unsigned kT_entries   = 0x10;  // uint32_t pointer to { uint32_t count; void* keys }

// sub_8284D0 is __thiscall with five stack arguments and cleans them itself.
typedef void* (__fastcall* FindKey_fn)(
    const void* this_ptr,
    void* dummy_edx,
    const void* timing,
    const void* track,
    uint32_t* hint,
    uint32_t* second,
    float* frac);

typedef void (__cdecl* SplineVec3_fn)(void* obj, void* state, void* track,
                                      uint32_t* out, const float* defVal);
typedef void (__cdecl* SplineScalar_fn)(void* obj, void* state, void* track,
                                        uint32_t* out, const float* defVal);

SplineVec3_fn   orig_SplineVec3   = nullptr;
SplineScalar_fn orig_SplineScalar = nullptr;

bool g_installed = false;
bool g_armed     = false;
bool g_abSubject = false;
bool g_dead      = false;

unsigned long g_callsVec3       = 0;
unsigned long g_callsScalar     = 0;
unsigned long g_verifiedVec3    = 0;
unsigned long g_verifiedScalar  = 0;
unsigned long g_blendsVec3      = 0;
unsigned long g_blendsScalar    = 0;

constexpr unsigned long kVerifyFirst  = 30000;
constexpr unsigned long kResampleMask = 8191;

// Evaluate 3D vector cubic spline track (sub_82B460)
void EvaluateSplineVec3(void* obj, uint8_t* state, uint8_t* track,
                        uint32_t* out, const float* defVal) {
    const uint32_t count   = *(const uint32_t*)(track + kT_count);
    const uint32_t entries = *(const uint32_t*)(track + kT_entries);
    const uint16_t interp  = *(const uint16_t*)(track + kT_interp);

    const uint16_t want = *(const uint16_t*)(state + kS_trackIdx);
    const uint32_t sel  = (want < count) ? want : 0u;
    const uint32_t* entry = (const uint32_t*)(entries + 8u * sel);

    float* o = (float*)(out + 2);

    if (entry[0] != 0) {
        uint32_t second = 0;
        float frac = 0.0f;
        ((FindKey_fn)kFindKey)(obj, nullptr, state + kS_timing1, track,
                               out, &second, &frac);
        const float* k0 = (const float*)(entry[1] + 36u * out[0]);
        const float* k1 = (const float*)(entry[1] + 36u * second);

        if (interp == 0) {
            o[0] = k0[0];
            o[1] = k0[1];
            o[2] = k0[2];
            return;
        }

        const double t = (double)frac;
        if (interp == 1) {
            // Linear interpolation
            o[0] = (float)((double)(k1[0] - k0[0]) * t + (double)k0[0]);
            o[1] = (float)((double)(k1[1] - k0[1]) * t + (double)k0[1]);
            o[2] = (float)((double)(k1[2] - k0[2]) * t + (double)k0[2]);
        } else if (interp == 2) {
            // Bezier spline
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double w0 = 3.0 * t2 - t3 - 3.0 * t + 1.0;
            const double w1 = 3.0 * t + 3.0 * t3 - 6.0 * t2;
            const double w2 = 3.0 * t2 - 3.0 * t3;
            const double w3 = t3;

            // Load components and evaluate in double precision matching client order
            const __m128d w0_2 = _mm_set1_pd(w0);
            const __m128d w1_2 = _mm_set1_pd(w1);
            const __m128d w2_2 = _mm_set1_pd(w2);
            const __m128d w3_2 = _mm_set1_pd(w3);

            const __m128d k0_outTan_xy = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k0[6])));
            const __m128d k1_val_xy    = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k1[0])));
            const __m128d k1_inTan_xy  = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k1[3])));
            const __m128d k0_val_xy    = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k0[0])));

            // Sequential client order: (k0_outTan * w1 + k1_val * w3) + k1_inTan * w2 + k0_val * w0
            __m128d xy = _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(k0_outTan_xy, w1_2),
                                                          _mm_mul_pd(k1_val_xy, w3_2)),
                                               _mm_mul_pd(k1_inTan_xy, w2_2)),
                                    _mm_mul_pd(k0_val_xy, w0_2));

            _mm_storel_pi((__m64*)&o[0], _mm_cvtpd_ps(xy));
            o[2] = (float)(((double)k0[8] * w1 + (double)k1[5] * w2) + (double)k0[2] * w0 + (double)k1[2] * w3);
        } else {
            // Hermite spline (interp == 3)
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double w0 = 2.0 * t3 - 3.0 * t2 + 1.0;
            const double w1 = t3 - 2.0 * t2 + t;
            const double w2 = t3 - t2;
            const double w3 = 3.0 * t2 - 2.0 * t3;

            const __m128d w0_2 = _mm_set1_pd(w0);
            const __m128d w1_2 = _mm_set1_pd(w1);
            const __m128d w2_2 = _mm_set1_pd(w2);
            const __m128d w3_2 = _mm_set1_pd(w3);

            const __m128d k0_outTan_xy = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k0[6])));
            const __m128d k1_inTan_xy  = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k1[3])));
            const __m128d k1_val_xy    = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k1[0])));
            const __m128d k0_val_xy    = _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)&k0[0])));

            // Sequential client order: (k0_outTan * w1 + k1_inTan * w2) + k1_val * w3 + k0_val * w0
            __m128d xy = _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(k0_outTan_xy, w1_2),
                                                          _mm_mul_pd(k1_inTan_xy, w2_2)),
                                               _mm_mul_pd(k1_val_xy, w3_2)),
                                    _mm_mul_pd(k0_val_xy, w0_2));

            _mm_storel_pi((__m64*)&o[0], _mm_cvtpd_ps(xy));
            o[2] = (float)(((double)k0[8] * w1 + (double)k1[5] * w2) + (double)k1[2] * w3 + (double)k0[2] * w0);
        }
    } else {
        o[0] = defVal[0];
        o[1] = defVal[1];
        o[2] = defVal[2];
        if (interp == 0) {
            return;
        }
    }

    // Stage 2: Blending
    const float blend = *(const float*)(state + kS_blend);
    const uint16_t globalSeq = *(const uint16_t*)(track + kT_globalSeq);
    if (blend != 0.0f && globalSeq == 0xFFFF) {
        g_blendsVec3++;
        const uint16_t want2 = *(const uint16_t*)(state + kS_blendIdx);
        const uint32_t sel2  = (want2 < count) ? want2 : 0u;
        const uint32_t* entry2 = (const uint32_t*)(entries + 8u * sel2);

        double b0, b1, b2;
        if (entry2[0] != 0) {
            uint32_t second2 = 0;
            float frac2 = 0.0f;
            ((FindKey_fn)kFindKey)(obj, nullptr, state + kS_timing2, track,
                                   out + 1, &second2, &frac2);
            const float* k0 = (const float*)(entry2[1] + 36u * out[1]);
            const float* k1 = (const float*)(entry2[1] + 36u * second2);

            const double t = (double)frac2;
            if (interp == 2) {
                const double t2 = t * t;
                const double t3 = t2 * t;
                const double w0 = 3.0 * t2 - t3 - 3.0 * t + 1.0;
                const double w1 = 3.0 * t + 3.0 * t3 - 6.0 * t2;
                const double w2 = 3.0 * t2 - 3.0 * t3;
                const double w3 = t3;
                b0 = (double)k0[6] * w1 + (double)k1[0] * w3 + (double)k1[3] * w2 + (double)k0[0] * w0;
                b1 = (double)k1[4] * w2 + (double)k0[7] * w1 + (double)k0[1] * w0 + (double)k1[1] * w3;
                b2 = (double)k0[8] * w1 + (double)k1[5] * w2 + (double)k0[2] * w0 + (double)k1[2] * w3;
            } else if (interp == 3) {
                const double t2 = t * t;
                const double t3 = t2 * t;
                const double w0 = 2.0 * t3 - 3.0 * t2 + 1.0;
                const double w1 = t3 - 2.0 * t2 + t;
                const double w2 = t3 - t2;
                const double w3 = 3.0 * t2 - 2.0 * t3;
                b0 = (double)k0[6] * w1 + (double)k1[3] * w2 + (double)k1[0] * w3 + (double)k0[0] * w0;
                b1 = (double)k1[4] * w2 + (double)k0[7] * w1 + (double)k1[1] * w3 + (double)k0[1] * w0;
                b2 = (double)k0[8] * w1 + (double)k1[5] * w2 + (double)k1[2] * w3 + (double)k0[2] * w0;
            } else {
                b0 = (double)(k1[0] - k0[0]) * t + (double)k0[0];
                b1 = (double)(k1[1] - k0[1]) * t + (double)k0[1];
                b2 = (double)(k1[2] - k0[2]) * t + (double)k0[2];
            }
        } else {
            b0 = (double)defVal[0];
            b1 = (double)defVal[1];
            b2 = (double)defVal[2];
        }

        const double bd = (double)blend;
        o[0] = (float)((b0 - (double)o[0]) * bd + (double)o[0]);
        o[1] = (float)((b1 - (double)o[1]) * bd + (double)o[1]);
        o[2] = (float)((b2 - (double)o[2]) * bd + (double)o[2]);
    }
}

// Evaluate scalar cubic spline track (sub_82B8A0)
void EvaluateSplineScalar(void* obj, uint8_t* state, uint8_t* track,
                          uint32_t* out, const float* defVal) {
    const uint32_t count   = *(const uint32_t*)(track + kT_count);
    const uint32_t entries = *(const uint32_t*)(track + kT_entries);
    const uint16_t interp  = *(const uint16_t*)(track + kT_interp);

    const uint16_t want = *(const uint16_t*)(state + kS_trackIdx);
    const uint32_t sel  = (want < count) ? want : 0u;
    const uint32_t* entry = (const uint32_t*)(entries + 8u * sel);

    float* o = (float*)(out + 2);

    if (entry[0] != 0) {
        uint32_t second = 0;
        float frac = 0.0f;
        ((FindKey_fn)kFindKey)(obj, nullptr, state + kS_timing1, track,
                               out, &second, &frac);
        const float* k0 = (const float*)(entry[1] + 12u * out[0]);
        const float* k1 = (const float*)(entry[1] + 12u * second);

        if (interp == 0) {
            *o = k0[0];
            return;
        }

        const double t = (double)frac;
        if (interp == 1) {
            *o = (float)((double)k0[0] + (double)(k1[0] - k0[0]) * t);
        } else if (interp == 2) {
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double w0 = 3.0 * t2 - t3 - 3.0 * t + 1.0;
            const double w1 = 3.0 * t3 - 6.0 * t2 + 3.0 * t;
            const double w2 = 3.0 * t2 - 3.0 * t3;
            const double r = t3 * (double)k1[0]
                           + w1 * (double)k0[2]
                           + w2 * (double)k1[1]
                           + w0 * (double)k0[0];
            *o = (float)r;
        } else {
            const double t2 = t * t;
            const double t3 = t2 * t;
            const double w0 = 2.0 * t3 - 3.0 * t2 + 1.0;
            const double w1 = t3 - 2.0 * t2 + t;
            const double w2 = t3 - t2;
            const double w3 = 3.0 * t2 - 2.0 * t3;
            const double r = w1 * (double)k0[2]
                           + w2 * (double)k1[1]
                           + w0 * (double)k0[0]
                           + w3 * (double)k1[0];
            *o = (float)r;
        }
    } else {
        *o = *defVal;
        if (interp == 0) {
            return;
        }
    }

    // Stage 2: Blending
    const float blend = *(const float*)(state + kS_blend);
    const uint16_t globalSeq = *(const uint16_t*)(track + kT_globalSeq);
    if (blend != 0.0f && globalSeq == 0xFFFF) {
        g_blendsScalar++;
        const uint16_t want2 = *(const uint16_t*)(state + kS_blendIdx);
        const uint32_t sel2  = (want2 < count) ? want2 : 0u;
        const uint32_t* entry2 = (const uint32_t*)(entries + 8u * sel2);

        double b;
        if (entry2[0] != 0) {
            uint32_t second2 = 0;
            float frac2 = 0.0f;
            ((FindKey_fn)kFindKey)(obj, nullptr, state + kS_timing2, track,
                                   out + 1, &second2, &frac2);
            const float* k0 = (const float*)(entry2[1] + 12u * out[1]);
            const float* k1 = (const float*)(entry2[1] + 12u * second2);

            const double t = (double)frac2;
            if (interp == 2) {
                const double t2 = t * t;
                const double t3 = t2 * t;
                const double w0 = 3.0 * t2 - t3 - 3.0 * t + 1.0;
                const double w1 = 3.0 * t3 - 6.0 * t2 + 3.0 * t;
                const double w2 = 3.0 * t2 - 3.0 * t3;
                b = t3 * (double)k1[0] + w1 * (double)k0[2] + w2 * (double)k1[1] + w0 * (double)k0[0];
            } else if (interp == 3) {
                const double t2 = t * t;
                const double t3 = t2 * t;
                const double w0 = 2.0 * t3 - 3.0 * t2 + 1.0;
                const double w1 = t3 - 2.0 * t2 + t;
                const double w2 = t3 - t2;
                const double w3 = 3.0 * t2 - 2.0 * t3;
                b = w1 * (double)k0[2] + w2 * (double)k1[1] + w0 * (double)k0[0] + w3 * (double)k1[0];
            } else {
                b = (double)k0[0] + (double)(k1[0] - k0[0]) * t;
            }
        } else {
            b = (double)*defVal;
        }

        const double bd = (double)blend;
        *o = (float)((double)*o + (b - (double)*o) * bd);
    }
}

void __cdecl Hooked_SplineVec3Body(void* obj, void* state, void* track,
                                   uint32_t* out, const float* defVal) {
    g_callsVec3++;

    if (g_dead || !out || !state || !track || !defVal) {
        orig_SplineVec3(obj, state, track, out, defVal);
        return;
    }

    if (!g_armed || (g_callsVec3 & kResampleMask) == 0) {
        uint32_t saved[5], theirs[5];
        memcpy(saved, out, sizeof(saved));
        orig_SplineVec3(obj, state, track, out, defVal);
        memcpy(theirs, out, sizeof(theirs));
        memcpy(out, saved, sizeof(saved));

        EvaluateSplineVec3(obj, (uint8_t*)state, (uint8_t*)track, out, defVal);
        g_verifiedVec3++;

        if (memcmp(out, theirs, sizeof(theirs)) != 0) {
            memcpy(out, theirs, sizeof(theirs));
            g_dead = true;
            Verdict::Add(Verdict::Bad,
                         "AnimSplineTrack (vec3 spline) disagreed with client output");
            Log("[AnimSplineTrack] Vec3 track DISAGREED with client after %lu checks - "
                "retired for this session. Client gave (%f, %f, %f) (hints %u/%u), "
                "vectorized gave (%f, %f, %f) (hints %u/%u).",
                g_verifiedVec3,
                *(float*)&theirs[2], *(float*)&theirs[3], *(float*)&theirs[4], theirs[0], theirs[1],
                *(float*)&out[2], *(float*)&out[3], *(float*)&out[4], out[0], out[1]);
            return;
        }
        if (!g_armed && g_verifiedVec3 >= kVerifyFirst) {
            g_armed = true;
            Log("[AnimSplineTrack] armed: %lu calls matched client bit for bit. "
                "Now evaluating directly and rechecking one call in %lu.",
                g_verifiedVec3, kResampleMask + 1);
        }
        return;
    }

    EvaluateSplineVec3(obj, (uint8_t*)state, (uint8_t*)track, out, defVal);
}

void __cdecl Hooked_SplineScalarBody(void* obj, void* state, void* track,
                                     uint32_t* out, const float* defVal) {
    g_callsScalar++;

    if (g_dead || !out || !state || !track || !defVal) {
        orig_SplineScalar(obj, state, track, out, defVal);
        return;
    }

    if (!g_armed || (g_callsScalar & kResampleMask) == 0) {
        uint32_t saved[3], theirs[3];
        memcpy(saved, out, sizeof(saved));
        orig_SplineScalar(obj, state, track, out, defVal);
        memcpy(theirs, out, sizeof(theirs));
        memcpy(out, saved, sizeof(saved));

        EvaluateSplineScalar(obj, (uint8_t*)state, (uint8_t*)track, out, defVal);
        g_verifiedScalar++;

        if (memcmp(out, theirs, sizeof(theirs)) != 0) {
            memcpy(out, theirs, sizeof(theirs));
            g_dead = true;
            Verdict::Add(Verdict::Bad,
                         "AnimSplineTrack (scalar spline) disagreed with client output");
            Log("[AnimSplineTrack] Scalar track DISAGREED with client after %lu checks - "
                "retired for this session. Client gave %08X (hints %u/%u), "
                "vectorized gave %08X (hints %u/%u).",
                g_verifiedScalar, theirs[2], theirs[0], theirs[1],
                out[2], out[0], out[1]);
            return;
        }
        return;
    }

    EvaluateSplineScalar(obj, (uint8_t*)state, (uint8_t*)track, out, defVal);
}

void __cdecl Hooked_SplineVec3(void* obj, void* state, void* track,
                               uint32_t* out, const float* defVal) {
    if (g_abSubject && AbTest::StandAside()) {
        orig_SplineVec3(obj, state, track, out, defVal);
        return;
    }
    const unsigned long long t = AbTest::TickIn();
    __try {
        Hooked_SplineVec3Body(obj, state, track, out, defVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        orig_SplineVec3(obj, state, track, out, defVal);
    }
    AbTest::TickOut(t);
}

void __cdecl Hooked_SplineScalar(void* obj, void* state, void* track,
                                 uint32_t* out, const float* defVal) {
    if (g_abSubject && AbTest::StandAside()) {
        orig_SplineScalar(obj, state, track, out, defVal);
        return;
    }
    const unsigned long long t = AbTest::TickIn();
    __try {
        Hooked_SplineScalarBody(obj, state, track, out, defVal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        orig_SplineScalar(obj, state, track, out, defVal);
    }
    AbTest::TickOut(t);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptAnimSplineTrack) {
        Log("[AnimSplineTrack] not installed: switched off.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTrackSplineVec3) ||
        !WowOpt_ClientPatchAllowed((const void*)kTrackSplineScalar)) {
        Log("[AnimSplineTrack] not installed: No Client Patches is active.");
        return false;
    }

    if (IsBadReadPtr((void*)kTrackSplineVec3, 16) ||
        IsBadReadPtr((void*)kTrackSplineScalar, 16) ||
        IsBadReadPtr((void*)kFindKey, 16)) {
        Log("[AnimSplineTrack] unreadable targets - not installing.");
        return false;
    }

    const unsigned char* pV = (const unsigned char*)kTrackSplineVec3;
    const unsigned char* pS = (const unsigned char*)kTrackSplineScalar;
    if (pV[0] != 0x55 || pV[1] != 0x8B || pV[2] != 0xEC ||
        pS[0] != 0x55 || pS[1] != 0x8B || pS[2] != 0xEC) {
        Log("[AnimSplineTrack] unexpected prologue bytes - not installing.");
        return false;
    }

    int ok = 0;
    if (WineSafe_CreateHook((void*)kTrackSplineVec3, (void*)Hooked_SplineVec3,
                            (void**)&orig_SplineVec3) == MH_OK &&
        WO_EnableHook((void*)kTrackSplineVec3) == MH_OK) {
        ok++;
    }

    if (WineSafe_CreateHook((void*)kTrackSplineScalar, (void*)Hooked_SplineScalar,
                            (void**)&orig_SplineScalar) == MH_OK &&
        WO_EnableHook((void*)kTrackSplineScalar) == MH_OK) {
        ok++;
    }

    if (ok == 0) {
        Log("[AnimSplineTrack] not installed: MinHook failed on both spline track evaluators.");
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("AnimSplineTrack_SSE2", (const void*)&Hooked_SplineVec3);
    SamplingProfiler::RegisterSelfSymbol("AnimSplineScalarTrack_SSE2", (const void*)&Hooked_SplineScalar);

    g_abSubject = AbTest::IsSubject("AnimSplineTrack", &g_abSubject);

    Log("[AnimSplineTrack] ACTIVE on %d of 2 spline track evaluators (sub_82B460 and sub_82B8A0). "
        "Evaluates 3D vector and scalar cubic spline tracks in hardware SSE2. "
        "Verifying the first %lu calls bit for bit with ongoing resampling.%s",
        ok, kVerifyFirst,
        Config::g_settings.OptAbTest
            ? " The A/B harness alternates the switch between stints."
            : "");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTrackSplineVec3);
    MH_DisableHook((void*)kTrackSplineScalar);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptAnimSplineTrack) {
        Log("[AnimSplineTrack] not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("[AnimSplineTrack] not measured: hooks are not installed.");
        return;
    }
    if (g_dead) {
        Log("[AnimSplineTrack] retired early: evaluation disagreed with the client.");
        return;
    }
    const unsigned long total = g_callsVec3 + g_callsScalar;
    if (total == 0) {
        Log("[AnimSplineTrack] measured and zero: the hooks are in and neither was reached.");
        return;
    }
    Log("[AnimSplineTrack] calls: vec3=%lu (verified=%lu, blends=%lu) | "
        "scalar=%lu (verified=%lu, blends=%lu)",
        g_callsVec3, g_verifiedVec3, g_blendsVec3,
        g_callsScalar, g_verifiedScalar, g_blendsScalar);
}

}  // namespace AnimSplineTrack
