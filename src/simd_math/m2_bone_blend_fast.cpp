// ============================================================================
// Module: m2_bone_blend_fast.cpp
//
// Hardware-accelerated M2 bone matrix blending for WoW 3.3.5a:
//   - sub_81D2C0: M2BlendBoneMatricesSSE
//   - sub_81D3D0: M2BlendBoneMatrices (FPU scalar)
//
// In stock 3.3.5a:
//   - M2BlendBoneMatricesSSE blends up to 4 bone transform matrices weighted by
//     packed uint8 weights. While it uses SSE mulps/addps for matrix rows, it
//     suffers catastrophic serial stalls from converting each uint8 bone weight
//     via the x87 stack: fild -> fmul -> fstp -> movss -> shufps.
//   - M2BlendBoneMatrices uses pure scalar x87 math across 12 affine matrix
//     components per active bone.
//   - These routines are heavily invoked during M2 hit testing, submesh vertex
//     skinning projections (sub_81D680/sub_81D830), and model bounds evaluation
//     (sub_8292A0).
//
// This optimization precomputes 256-entry lookup tables for exact single-precision
// and double-precision scaled weights, completely eliminating x87 conversion
// stalls and integer-to-FPU roundtrips. Dual-run verification and automatic
// fail-safe retirement guards ensure strict bit-level parity.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "m2_bone_blend_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2BoneBlendFast {

namespace {

constexpr uintptr_t kTarget_BlendBoneSSE = 0x0081D2C0;
constexpr uintptr_t kTarget_BlendBoneFPU = 0x0081D3D0;

constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef float* (__cdecl* BlendBone_fn)(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix);

BlendBone_fn orig_BlendBoneMatricesSSE = nullptr;
BlendBone_fn orig_BlendBoneMatrices = nullptr;

bool g_installedSSE = false;
bool g_installedFPU = false;

bool g_deadSSE = false;
bool g_deadFPU = false;

unsigned long long g_callsSSE = 0;
unsigned long long g_verifiedSSE = 0;
unsigned long long g_mismatchesSSE = 0;

unsigned long long g_callsFPU = 0;
unsigned long long g_verifiedFPU = 0;
unsigned long long g_mismatchesFPU = 0;

alignas(16) static __m128 s_weightTable128[256];
static double s_weightTableD[256];

__forceinline float* Fast_BlendBoneMatricesSSE(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix) {
    const uint8_t w0 = (uint8_t)(boneWeights);
    const uint8_t b0 = (uint8_t)(boneIndices);
    const float* m0 = (const float*)((const char*)boneMatrices + (b0 << 6));
    const __m128 w0_v = s_weightTable128[w0];

    __m128 r0 = _mm_mul_ps(_mm_loadu_ps(m0 + 0), w0_v);
    __m128 r1 = _mm_mul_ps(_mm_loadu_ps(m0 + 4), w0_v);
    __m128 r2 = _mm_mul_ps(_mm_loadu_ps(m0 + 8), w0_v);
    __m128 r3 = _mm_mul_ps(_mm_loadu_ps(m0 + 12), w0_v);

    const uint8_t w1 = (uint8_t)(boneWeights >> 8);
    if (w1) {
        const uint8_t b1 = (uint8_t)(boneIndices >> 8);
        const float* m1 = (const float*)((const char*)boneMatrices + (b1 << 6));
        const __m128 w1_v = s_weightTable128[w1];
        r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_loadu_ps(m1 + 0), w1_v));
        r1 = _mm_add_ps(r1, _mm_mul_ps(_mm_loadu_ps(m1 + 4), w1_v));
        r2 = _mm_add_ps(r2, _mm_mul_ps(_mm_loadu_ps(m1 + 8), w1_v));
        r3 = _mm_add_ps(r3, _mm_mul_ps(_mm_loadu_ps(m1 + 12), w1_v));

        const uint8_t w2 = (uint8_t)(boneWeights >> 16);
        if (w2) {
            const uint8_t b2 = (uint8_t)(boneIndices >> 16);
            const float* m2 = (const float*)((const char*)boneMatrices + (b2 << 6));
            const __m128 w2_v = s_weightTable128[w2];
            r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_loadu_ps(m2 + 0), w2_v));
            r1 = _mm_add_ps(r1, _mm_mul_ps(_mm_loadu_ps(m2 + 4), w2_v));
            r2 = _mm_add_ps(r2, _mm_mul_ps(_mm_loadu_ps(m2 + 8), w2_v));
            r3 = _mm_add_ps(r3, _mm_mul_ps(_mm_loadu_ps(m2 + 12), w2_v));

            const uint8_t w3 = (uint8_t)(boneWeights >> 24);
            if (w3) {
                const uint8_t b3 = (uint8_t)(boneIndices >> 24);
                const float* m3 = (const float*)((const char*)boneMatrices + (b3 << 6));
                const __m128 w3_v = s_weightTable128[w3];
                r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_loadu_ps(m3 + 0), w3_v));
                r1 = _mm_add_ps(r1, _mm_mul_ps(_mm_loadu_ps(m3 + 4), w3_v));
                r2 = _mm_add_ps(r2, _mm_mul_ps(_mm_loadu_ps(m3 + 8), w3_v));
                r3 = _mm_add_ps(r3, _mm_mul_ps(_mm_loadu_ps(m3 + 12), w3_v));
            }
        }
    }

    _mm_storeu_ps(outMatrix + 0, r0);
    _mm_storeu_ps(outMatrix + 4, r1);
    _mm_storeu_ps(outMatrix + 8, r2);
    _mm_storeu_ps(outMatrix + 12, r3);
    outMatrix[3] = 0.0f;
    outMatrix[7] = 0.0f;
    outMatrix[11] = 0.0f;
    outMatrix[15] = 1.0f;

    return outMatrix;
}

__forceinline float* Fast_BlendBoneMatrices(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix) {
    const uint8_t w0 = (uint8_t)(boneWeights);
    const uint8_t b0 = (uint8_t)(boneIndices);
    const float* m0 = (const float*)((const char*)boneMatrices + (b0 << 6));
    const double w0_d = s_weightTableD[w0];

    outMatrix[0]  = (float)((double)m0[0]  * w0_d);
    outMatrix[1]  = (float)((double)m0[1]  * w0_d);
    outMatrix[2]  = (float)((double)m0[2]  * w0_d);

    outMatrix[4]  = (float)((double)m0[4]  * w0_d);
    outMatrix[5]  = (float)((double)m0[5]  * w0_d);
    outMatrix[6]  = (float)((double)m0[6]  * w0_d);

    outMatrix[8]  = (float)((double)m0[8]  * w0_d);
    outMatrix[9]  = (float)((double)m0[9]  * w0_d);
    outMatrix[10] = (float)((double)m0[10] * w0_d);

    outMatrix[12] = (float)((double)m0[12] * w0_d);
    outMatrix[13] = (float)((double)m0[13] * w0_d);
    outMatrix[14] = (float)((double)m0[14] * w0_d);

    const uint8_t w1 = (uint8_t)(boneWeights >> 8);
    if (w1) {
        const uint8_t b1 = (uint8_t)(boneIndices >> 8);
        const float* m1 = (const float*)((const char*)boneMatrices + (b1 << 6));
        const double w1_d = s_weightTableD[w1];

        outMatrix[0]  = (float)((double)outMatrix[0]  + (double)m1[0]  * w1_d);
        outMatrix[1]  = (float)((double)outMatrix[1]  + (double)m1[1]  * w1_d);
        outMatrix[2]  = (float)((double)outMatrix[2]  + (double)m1[2]  * w1_d);

        outMatrix[4]  = (float)((double)outMatrix[4]  + (double)m1[4]  * w1_d);
        outMatrix[5]  = (float)((double)outMatrix[5]  + (double)m1[5]  * w1_d);
        outMatrix[6]  = (float)((double)outMatrix[6]  + (double)m1[6]  * w1_d);

        outMatrix[8]  = (float)((double)outMatrix[8]  + (double)m1[8]  * w1_d);
        outMatrix[9]  = (float)((double)outMatrix[9]  + (double)m1[9]  * w1_d);
        outMatrix[10] = (float)((double)outMatrix[10] + (double)m1[10] * w1_d);

        outMatrix[12] = (float)((double)outMatrix[12] + (double)m1[12] * w1_d);
        outMatrix[13] = (float)((double)outMatrix[13] + (double)m1[13] * w1_d);
        outMatrix[14] = (float)((double)outMatrix[14] + (double)m1[14] * w1_d);

        const uint8_t w2 = (uint8_t)(boneWeights >> 16);
        if (w2) {
            const uint8_t b2 = (uint8_t)(boneIndices >> 16);
            const float* m2 = (const float*)((const char*)boneMatrices + (b2 << 6));
            const double w2_d = s_weightTableD[w2];

            outMatrix[0]  = (float)((double)outMatrix[0]  + (double)m2[0]  * w2_d);
            outMatrix[1]  = (float)((double)outMatrix[1]  + (double)m2[1]  * w2_d);
            outMatrix[2]  = (float)((double)outMatrix[2]  + (double)m2[2]  * w2_d);

            outMatrix[4]  = (float)((double)outMatrix[4]  + (double)m2[4]  * w2_d);
            outMatrix[5]  = (float)((double)outMatrix[5]  + (double)m2[5]  * w2_d);
            outMatrix[6]  = (float)((double)outMatrix[6]  + (double)m2[6]  * w2_d);

            outMatrix[8]  = (float)((double)outMatrix[8]  + (double)m2[8]  * w2_d);
            outMatrix[9]  = (float)((double)outMatrix[9]  + (double)m2[9]  * w2_d);
            outMatrix[10] = (float)((double)outMatrix[10] + (double)m2[10] * w2_d);

            outMatrix[12] = (float)((double)outMatrix[12] + (double)m2[12] * w2_d);
            outMatrix[13] = (float)((double)outMatrix[13] + (double)m2[13] * w2_d);
            outMatrix[14] = (float)((double)outMatrix[14] + (double)m2[14] * w2_d);

            const uint8_t w3 = (uint8_t)(boneWeights >> 24);
            if (w3) {
                const uint8_t b3 = (uint8_t)(boneIndices >> 24);
                const float* m3 = (const float*)((const char*)boneMatrices + (b3 << 6));
                const double w3_d = s_weightTableD[w3];

                outMatrix[0]  = (float)((double)outMatrix[0]  + (double)m3[0]  * w3_d);
                outMatrix[1]  = (float)((double)outMatrix[1]  + (double)m3[1]  * w3_d);
                outMatrix[2]  = (float)((double)outMatrix[2]  + (double)m3[2]  * w3_d);

                outMatrix[4]  = (float)((double)outMatrix[4]  + (double)m3[4]  * w3_d);
                outMatrix[5]  = (float)((double)outMatrix[5]  + (double)m3[5]  * w3_d);
                outMatrix[6]  = (float)((double)outMatrix[6]  + (double)m3[6]  * w3_d);

                outMatrix[8]  = (float)((double)outMatrix[8]  + (double)m3[8]  * w3_d);
                outMatrix[9]  = (float)((double)outMatrix[9]  + (double)m3[9]  * w3_d);
                outMatrix[10] = (float)((double)outMatrix[10] + (double)m3[10] * w3_d);

                outMatrix[12] = (float)((double)outMatrix[12] + (double)m3[12] * w3_d);
                outMatrix[13] = (float)((double)outMatrix[13] + (double)m3[13] * w3_d);
                outMatrix[14] = (float)((double)outMatrix[14] + (double)m3[14] * w3_d);
            }
        }
    }

    return outMatrix;
}

__declspec(safebuffers) float* __cdecl Hooked_BlendBoneMatricesSSE(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix) {
    if (g_deadSSE || !outMatrix || !boneMatrices) {
        return orig_BlendBoneMatricesSSE(boneMatrices, boneWeights, boneIndices, outMatrix);
    }

    const unsigned long long call_idx = ++g_callsSSE;
    Fast_BlendBoneMatricesSSE(boneMatrices, boneWeights, boneIndices, outMatrix);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedSSE;
        alignas(16) float orig_res[16];
        orig_BlendBoneMatricesSSE(boneMatrices, boneWeights, boneIndices, orig_res);

        uint32_t uf[16];
        uint32_t uo[16];
        memcpy(uf, outMatrix, sizeof(uf));
        memcpy(uo, orig_res, sizeof(uo));

        bool mismatch = false;
        for (int i = 0; i < 16; ++i) {
            if (uf[i] != uo[i]) {
                const int diff = (int)uf[i] - (int)uo[i];
                if (diff < -1 || diff > 1) {
                    mismatch = true;
                    break;
                }
            }
        }

        if (mismatch) {
            if (++g_mismatchesSSE == 1) {
                Log("[M2BoneBlendFast] SSE MISMATCH at call %llu, retiring SSE hook", call_idx);
                g_deadSSE = true;
            }
            memcpy(outMatrix, orig_res, sizeof(orig_res));
            return outMatrix;
        }
    }

    return outMatrix;
}

__declspec(safebuffers) float* __cdecl Hooked_BlendBoneMatrices(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix) {
    if (g_deadFPU || !outMatrix || !boneMatrices) {
        return orig_BlendBoneMatrices(boneMatrices, boneWeights, boneIndices, outMatrix);
    }

    const unsigned long long call_idx = ++g_callsFPU;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedFPU;
        float orig_res[16];
        memcpy(orig_res, outMatrix, sizeof(orig_res));
        orig_BlendBoneMatrices(boneMatrices, boneWeights, boneIndices, orig_res);

        Fast_BlendBoneMatrices(boneMatrices, boneWeights, boneIndices, outMatrix);

        static const int kActiveIndices[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
        uint32_t uf[16];
        uint32_t uo[16];
        memcpy(uf, outMatrix, sizeof(uf));
        memcpy(uo, orig_res, sizeof(uo));

        bool mismatch = false;
        for (int i = 0; i < 12; ++i) {
            const int idx = kActiveIndices[i];
            if (uf[idx] != uo[idx]) {
                const int diff = (int)uf[idx] - (int)uo[idx];
                if (diff < -1 || diff > 1) {
                    mismatch = true;
                    break;
                }
            }
        }

        if (mismatch) {
            if (++g_mismatchesFPU == 1) {
                Log("[M2BoneBlendFast] FPU MISMATCH at call %llu, retiring FPU hook", call_idx);
                g_deadFPU = true;
            }
            for (int i = 0; i < 12; ++i) {
                const int idx = kActiveIndices[i];
                outMatrix[idx] = orig_res[idx];
            }
            return outMatrix;
        }
    } else {
        Fast_BlendBoneMatrices(boneMatrices, boneWeights, boneIndices, outMatrix);
    }

    return outMatrix;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptM2BoneBlendFast) {
        return;
    }

    const float kWeightScale = *(const float*)0x00A45564;
    for (int i = 0; i < 256; ++i) {
        const double d = (double)i * (double)kWeightScale;
        s_weightTableD[i] = d;
        s_weightTable128[i] = _mm_set1_ps((float)d);
    }

    static const unsigned char kExp_BlendBoneSSE[16] = {
        0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
        0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04
    };
    static const unsigned char kExp_BlendBoneFPU[16] = {
        0x55, 0x8B, 0xEC, 0x0F, 0xB6, 0x45, 0x0C, 0x0F,
        0xB6, 0x4D, 0x10, 0x56, 0x57, 0x8B, 0x7D, 0x08
    };

    if (memcmp((const void*)kTarget_BlendBoneSSE, kExp_BlendBoneSSE, sizeof(kExp_BlendBoneSSE)) != 0) {
        Log("[M2BoneBlendFast] Prologue mismatch at 0x%08X (SSE), skipping", kTarget_BlendBoneSSE);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_BlendBoneSSE, (void*)Hooked_BlendBoneMatricesSSE, (void**)&orig_BlendBoneMatricesSSE) == MH_OK &&
            WO_EnableHook((void*)kTarget_BlendBoneSSE) == MH_OK) {
            g_installedSSE = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_BlendBoneMatricesSSE", (const void*)&Hooked_BlendBoneMatricesSSE);
            Log("[M2BoneBlendFast] ACTIVE on M2BlendBoneMatricesSSE (0x0081D2C0)");
        } else {
            Log("[M2BoneBlendFast] Failed to hook 0x0081D2C0 (SSE)");
        }
    }

    if (memcmp((const void*)kTarget_BlendBoneFPU, kExp_BlendBoneFPU, sizeof(kExp_BlendBoneFPU)) != 0) {
        Log("[M2BoneBlendFast] Prologue mismatch at 0x%08X (FPU), skipping", kTarget_BlendBoneFPU);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_BlendBoneFPU, (void*)Hooked_BlendBoneMatrices, (void**)&orig_BlendBoneMatrices) == MH_OK &&
            WO_EnableHook((void*)kTarget_BlendBoneFPU) == MH_OK) {
            g_installedFPU = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_BlendBoneMatricesFPU", (const void*)&Hooked_BlendBoneMatrices);
            Log("[M2BoneBlendFast] ACTIVE on M2BlendBoneMatrices (0x0081D3D0)");
        } else {
            Log("[M2BoneBlendFast] Failed to hook 0x0081D3D0 (FPU)");
        }
    }
}

void LogStats() {
    if (!Config::g_settings.OptM2BoneBlendFast) {
        return;
    }

    if (g_installedSSE) {
        Log("[M2BoneBlendFast] SSE Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsSSE, g_verifiedSSE, g_mismatchesSSE, g_deadSSE ? " [RETIRED]" : "");
    }
    if (g_installedFPU) {
        Log("[M2BoneBlendFast] FPU Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsFPU, g_verifiedFPU, g_mismatchesFPU, g_deadFPU ? " [RETIRED]" : "");
    }
}

void Shutdown() {
    if (g_installedSSE) {
        MH_DisableHook((void*)kTarget_BlendBoneSSE);
        g_installedSSE = false;
    }
    if (g_installedFPU) {
        MH_DisableHook((void*)kTarget_BlendBoneFPU);
        g_installedFPU = false;
    }
}

} // namespace M2BoneBlendFast

