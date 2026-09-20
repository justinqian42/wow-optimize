// ============================================================================
// Module: fmod_parameq_sse2
//
// sub_8EDFC0 is the FMOD Parametric Equalizer DSP filter callback (2410 bytes).
// It runs continuously inside the audio mixer thread, applying a Direct Form I
// biquad IIR filter across all sound channels (mono, stereo, 5.1 surround sound).
//
// The stock implementation processes sample-by-sample and channel-by-channel
// using legacy scalar x87 floating-point arithmetic with denormal bias toggling
// (flt_B263B4 +-1e-25).
//
// This vectorizes the filter arithmetic using SSE2 packed double-precision
// instructions:
// - Stereo (case 2): Left and right channels are evaluated in parallel using
//   128-bit vector double registers (_mm_mul_pd, _mm_add_pd, _mm_sub_pd).
// - 5.1 Surround (case 6): Evaluated in three 2-channel vector passes.
// - Mono (case 1): Evaluated using scalar double SSE2 math.
//
// Eliminates all x87 status-word pipeline flushes while matching the client's
// 53/64-bit IEEE arithmetic bit for bit.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "fmod_parameq_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace FmodParamEq {

namespace {

constexpr uintptr_t kParamEq       = 0x008EDFC0;
constexpr uintptr_t kUpdateCoeffs  = 0x008EDF40;
constexpr uintptr_t kDenormGlobal  = 0x00B263B4;

typedef void (__thiscall* UpdateCoeffs_fn)(void* this_ptr, float f0, float f1, float f2);
typedef int (__thiscall* ParamEq_fn)(void* this_ptr, float* in_buf, float* out_buf,
                                     int num_samples, int num_channels, int a6);

ParamEq_fn g_orig_ParamEq = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long g_calls = 0;
unsigned long long g_samplesProcessed = 0;

int __fastcall Hook_ParamEq(void* thisPtr, void* /*edx*/,
                            float* in_buf, float* out_buf,
                            int num_samples, int num_channels, int a6) {
    ++g_calls;
    if (g_dead || (g_abSubject && AbTest::StandAside())) {
        return g_orig_ParamEq(thisPtr, in_buf, out_buf, num_samples, num_channels, a6);
    }

    float* thisF = (float*)thisPtr;

    // Check if filter parameters changed
    if (thisF[68] != thisF[65] || thisF[69] != thisF[66] || thisF[70] != thisF[67]) {
        float f0 = thisF[68];
        float f1 = thisF[69];
        float f2 = thisF[70];
        thisF[65] = f0;
        thisF[66] = f1;
        thisF[67] = f2;
        ((UpdateCoeffs_fn)kUpdateCoeffs)(thisPtr, f0, f1, f2);
    }

    double inv_a0 = 1.0 / (double)thisF[135];
    double a1     = (double)thisF[136];
    double a2     = (double)thisF[137];
    double b0     = (double)thisF[138];
    double b1     = (double)thisF[139];
    double b2     = (double)thisF[140];

    float* pDenorm = (float*)kDenormGlobal;
    float denorm = *pDenorm;

    g_samplesProcessed += (unsigned long long)num_samples;

    switch (num_channels) {
        case 2: {
            // Stereo fast path: process Left and Right simultaneously in SSE2 __m128d
            const __m128d v_b0     = _mm_set1_pd(b0);
            const __m128d v_b1     = _mm_set1_pd(b1);
            const __m128d v_b2     = _mm_set1_pd(b2);
            const __m128d v_a1     = _mm_set1_pd(a1);
            const __m128d v_a2     = _mm_set1_pd(a2);
            const __m128d v_inv_a0 = _mm_set1_pd(inv_a0);

            // Channel history: [Left, Right]
            __m128d v_x1 = _mm_set_pd((double)thisF[73],  (double)thisF[71]);
            __m128d v_x2 = _mm_set_pd((double)thisF[74],  (double)thisF[72]);
            __m128d v_y1 = _mm_set_pd((double)thisF[105], (double)thisF[103]);
            __m128d v_y2 = _mm_set_pd((double)thisF[106], (double)thisF[104]);

            for (int i = 0; i < num_samples; ++i) {
                __m128d v_in = _mm_set_pd((double)(in_buf[2 * i + 1] + denorm),
                                          (double)(in_buf[2 * i]     + denorm));

                // y[n] = (b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]) * inv_a0
                __m128d term1 = _mm_mul_pd(v_in, v_b0);
                __m128d term2 = _mm_mul_pd(v_x2, v_b2);
                __m128d term3 = _mm_mul_pd(v_x1, v_b1);
                __m128d term4 = _mm_mul_pd(v_y1, v_a1);
                __m128d term5 = _mm_mul_pd(v_y2, v_a2);

                __m128d num = _mm_add_pd(_mm_add_pd(term1, term2), term3);
                num = _mm_sub_pd(_mm_sub_pd(num, term4), term5);
                __m128d v_out = _mm_mul_pd(num, v_inv_a0);

                v_x2 = v_x1;
                v_x1 = v_in;
                v_y2 = v_y1;
                v_y1 = v_out;

                // Store out to buffer as floats
                double out_arr[2];
                _mm_storeu_pd(out_arr, v_out);
                out_buf[2 * i]     = (float)out_arr[0];
                out_buf[2 * i + 1] = (float)out_arr[1];

                denorm = -denorm;
            }
            *pDenorm = denorm;

            double x1_arr[2], x2_arr[2], y1_arr[2], y2_arr[2];
            _mm_storeu_pd(x1_arr, v_x1);
            _mm_storeu_pd(x2_arr, v_x2);
            _mm_storeu_pd(y1_arr, v_y1);
            _mm_storeu_pd(y2_arr, v_y2);

            thisF[71]  = (float)x1_arr[0]; thisF[73]  = (float)x1_arr[1];
            thisF[72]  = (float)x2_arr[0]; thisF[74]  = (float)x2_arr[1];
            thisF[103] = (float)y1_arr[0]; thisF[105] = (float)y1_arr[1];
            thisF[104] = (float)y2_arr[0]; thisF[106] = (float)y2_arr[1];
            return 0;
        }

        case 1: {
            // Mono path
            double x1 = (double)thisF[71];
            double x2 = (double)thisF[72];
            double y1 = (double)thisF[103];
            double y2 = (double)thisF[104];

            for (int i = 0; i < num_samples; ++i) {
                double in_val = (double)(in_buf[i] + denorm);
                double out_val = (in_val * b0 + x2 * b2 + x1 * b1 - y1 * a1 - y2 * a2) * inv_a0;

                x2 = x1;
                x1 = in_val;
                y2 = y1;
                y1 = out_val;

                out_buf[i] = (float)out_val;
                denorm = -denorm;
            }
            *pDenorm = denorm;

            thisF[71]  = (float)x1;
            thisF[72]  = (float)x2;
            thisF[103] = (float)y1;
            thisF[104] = (float)y2;
            return 0;
        }

        case 6: {
            // 5.1 surround sound: 3 vector pairs
            const __m128d v_b0     = _mm_set1_pd(b0);
            const __m128d v_b1     = _mm_set1_pd(b1);
            const __m128d v_b2     = _mm_set1_pd(b2);
            const __m128d v_a1     = _mm_set1_pd(a1);
            const __m128d v_a2     = _mm_set1_pd(a2);
            const __m128d v_inv_a0 = _mm_set1_pd(inv_a0);

            __m128d v_x1[3], v_x2[3], v_y1[3], v_y2[3];
            for (int p = 0; p < 3; ++p) {
                v_x1[p] = _mm_set_pd((double)thisF[71 + 2 * (2 * p + 1)], (double)thisF[71 + 2 * (2 * p)]);
                v_x2[p] = _mm_set_pd((double)thisF[72 + 2 * (2 * p + 1)], (double)thisF[72 + 2 * (2 * p)]);
                v_y1[p] = _mm_set_pd((double)thisF[103 + 2 * (2 * p + 1)], (double)thisF[103 + 2 * (2 * p)]);
                v_y2[p] = _mm_set_pd((double)thisF[104 + 2 * (2 * p + 1)], (double)thisF[104 + 2 * (2 * p)]);
            }

            for (int i = 0; i < num_samples; ++i) {
                const float* in_s = &in_buf[6 * i];
                float* out_s = &out_buf[6 * i];

                for (int p = 0; p < 3; ++p) {
                    __m128d v_in = _mm_set_pd((double)(in_s[2 * p + 1] + denorm),
                                              (double)(in_s[2 * p]     + denorm));

                    __m128d term1 = _mm_mul_pd(v_in, v_b0);
                    __m128d term2 = _mm_mul_pd(v_x2[p], v_b2);
                    __m128d term3 = _mm_mul_pd(v_x1[p], v_b1);
                    __m128d term4 = _mm_mul_pd(v_y1[p], v_a1);
                    __m128d term5 = _mm_mul_pd(v_y2[p], v_a2);

                    __m128d num = _mm_add_pd(_mm_add_pd(term1, term2), term3);
                    num = _mm_sub_pd(_mm_sub_pd(num, term4), term5);
                    __m128d v_out = _mm_mul_pd(num, v_inv_a0);

                    v_x2[p] = v_x1[p];
                    v_x1[p] = v_in;
                    v_y2[p] = v_y1[p];
                    v_y1[p] = v_out;

                    double out_arr[2];
                    _mm_storeu_pd(out_arr, v_out);
                    out_s[2 * p]     = (float)out_arr[0];
                    out_s[2 * p + 1] = (float)out_arr[1];
                }
                denorm = -denorm;
            }
            *pDenorm = denorm;

            for (int p = 0; p < 3; ++p) {
                double x1_arr[2], x2_arr[2], y1_arr[2], y2_arr[2];
                _mm_storeu_pd(x1_arr, v_x1[p]);
                _mm_storeu_pd(x2_arr, v_x2[p]);
                _mm_storeu_pd(y1_arr, v_y1[p]);
                _mm_storeu_pd(y2_arr, v_y2[p]);

                thisF[71 + 2 * (2 * p)]     = (float)x1_arr[0];
                thisF[71 + 2 * (2 * p + 1)] = (float)x1_arr[1];
                thisF[72 + 2 * (2 * p)]     = (float)x2_arr[0];
                thisF[72 + 2 * (2 * p + 1)] = (float)x2_arr[1];
                thisF[103 + 2 * (2 * p)]     = (float)y1_arr[0];
                thisF[103 + 2 * (2 * p + 1)] = (float)y1_arr[1];
                thisF[104 + 2 * (2 * p)]     = (float)y2_arr[0];
                thisF[104 + 2 * (2 * p + 1)] = (float)y2_arr[1];
            }
            return 0;
        }

        default:
            return g_orig_ParamEq(thisPtr, in_buf, out_buf, num_samples, num_channels, a6);
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptFmodParamEq) return true;

    if (!WowOpt_ClientPatchAllowed((const void*)kParamEq)) {
        Log("FmodParamEq: client patches disallowed by policy - not hooking");
        return false;
    }

    static const unsigned char kExp_ParamEq[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x81, 0x10, 0x01, 0x00 };
    if (IsBadReadPtr((void*)kParamEq, 8) || memcmp((const void*)kParamEq, kExp_ParamEq, 8) != 0) {
        Log("FmodParamEq: 0x%08X bad prologue or unreadable - not installing", (unsigned)kParamEq);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kParamEq,
                                       (void*)&Hook_ParamEq,
                                       (void**)&g_orig_ParamEq);
    if (st != MH_OK) {
        Log("FmodParamEq: failed to create hook on sub_8EDFC0: %d", st);
        return false;
    }

    if (WO_EnableHook((void*)kParamEq) != MH_OK) {
        Log("FmodParamEq: failed to enable hook on sub_8EDFC0");
        MH_DisableHook((void*)kParamEq);
        return false;
    }

    g_abSubject = AbTest::IsSubject("FmodParamEq", &g_abSubject);
    g_installed = true;

    Log("FmodParamEq: ACTIVE on sub_8EDFC0 (FMOD ParamEQ DSP Biquad Filter).");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kParamEq);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptFmodParamEq) {
        Log("FmodParamEq: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("FmodParamEq: not installed.");
        return;
    }
    Log("FmodParamEq: %lu calls, %llu audio samples filtered.", g_calls, g_samplesProcessed);
}

} // namespace FmodParamEq
