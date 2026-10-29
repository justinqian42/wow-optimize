// ============================================================================
// Module: mat44_affine_ops_fast.cpp
//
// Hardware-accelerated 4x4 matrix affine transformations, rotations, scaling,
// and inversions for WoW 3.3.5a:
//   - sub_4C2FC0: C44Matrix::AffineInverse (34 call sites)
//   - sub_4C3380: C44Matrix::RotateAroundZ (39 call sites)
//   - sub_4C3300: C44Matrix::RotateAroundX (8 call sites)
//   - sub_4C3340: C44Matrix::RotateAroundY (9 call sites)
//   - sub_4C3290: C44Matrix::FromRotationZ (13 call sites)
//   - sub_4C2120: C44Matrix::ScaleMatrixByScalar (4 call sites)
//   - sub_4C1C40: C44Matrix::FromQuaternion33 (3 call sites)
//
// In stock 3.3.5a:
//   - sub_4C2FC0 inverts a 4x4 affine matrix by copying it to a 3x3 matrix via
//     sub_4C51B0, computing 15 serial x87 float operations and stack spills,
//     and performing unaligned writes. Called 34 times per frame across camera
//     inversion, model-to-world inversion, attachment frames, and sound orientation.
//   - sub_4C3380/4C3300/4C3340 perform in-place single-axis rotations by
//     allocating a temporary 64-byte matrix on the stack via sub_4C3290,
//     calling sub_4C1F00 (full 64-byte matrix multiplication), and calling
//     sub_407F80 to copy it back. Called 56+ times per frame across unit facings,
//     projectiles, camera updates, and scene orientations.
//   - sub_4C3290 constructs a 4x4 Z-rotation matrix using serial scalar writes.
//   - sub_4C2120 scales a 4x4 matrix using 16 serial x87 multiplications.
//   - sub_4C1C40 converts a quaternion to rotation matrix elements using serial
//     x87 instructions with memory roundtrips.
//
// This replacement:
//   - Inverts affine matrices directly in registers without intermediate copying.
//   - Evaluates axis rotations in place directly on affected rows without stack
//     temporaries or full 64-byte matrix multiplications.
//   - Vectorizes matrix scalar scaling with 128-bit SSE2 instructions.
//   - Employs dual-run verification with fail-safe retirement guards.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

#include "mat44_affine_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace Mat44AffineOpsFast {

namespace {

constexpr uintptr_t kTarget_AffineInverse      = 0x004C2FC0;
constexpr uintptr_t kTarget_RotateAroundZ      = 0x004C3380;
constexpr uintptr_t kTarget_RotateAroundX      = 0x004C3300;
constexpr uintptr_t kTarget_RotateAroundY      = 0x004C3340;
constexpr uintptr_t kTarget_FromRotationZ      = 0x004C3290;
constexpr uintptr_t kTarget_ScaleMatrix        = 0x004C2120;
constexpr uintptr_t kTarget_FromQuaternion33   = 0x004C1C40;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0002f;

// Expected prologues
const unsigned char kExp_AffineInverse[8]    = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48, 0x56, 0x8B };
const unsigned char kExp_RotateAroundZ[8]    = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0x81, 0xEC };
const unsigned char kExp_RotateAroundX[8]    = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0x81, 0xEC };
const unsigned char kExp_RotateAroundY[8]    = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0x81, 0xEC };
const unsigned char kExp_FromRotationZ[8]    = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x8D, 0x45 };
const unsigned char kExp_ScaleMatrix[8]      = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x45 };
const unsigned char kExp_FromQuaternion33[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45 };

// Function signatures
typedef float* (__thiscall* AffineInverse_fn)(const float* this_ptr, float* out);
typedef float* (__thiscall* RotateAroundAxis_fn)(float* this_ptr, float angle);
typedef float* (__cdecl* FromRotationZ_fn)(float* out, float angle);
typedef float* (__cdecl* ScaleMatrix_fn)(float* out, const float* in, float scale);
typedef float* (__cdecl* FromQuaternion33_fn)(const float* q, float* out);

AffineInverse_fn    orig_AffineInverse    = nullptr;
RotateAroundAxis_fn orig_RotateAroundZ    = nullptr;
RotateAroundAxis_fn orig_RotateAroundX    = nullptr;
RotateAroundAxis_fn orig_RotateAroundY    = nullptr;
FromRotationZ_fn    orig_FromRotationZ    = nullptr;
ScaleMatrix_fn      orig_ScaleMatrix      = nullptr;
FromQuaternion33_fn orig_FromQuaternion33 = nullptr;

bool g_installed = false;

bool g_dead_affine_inverse    = false;
bool g_dead_rotate_z          = false;
bool g_dead_rotate_x          = false;
bool g_dead_rotate_y          = false;
bool g_dead_from_rot_z        = false;
bool g_dead_scale_matrix      = false;
bool g_dead_from_quat33       = false;

unsigned long long g_calls_affine_inverse    = 0;
unsigned long long g_calls_rotate_z          = 0;
unsigned long long g_calls_rotate_x          = 0;
unsigned long long g_calls_rotate_y          = 0;
unsigned long long g_calls_from_rot_z        = 0;
unsigned long long g_calls_scale_matrix      = 0;
unsigned long long g_calls_from_quat33       = 0;

unsigned long long g_verified_affine_inverse    = 0;
unsigned long long g_verified_rotate_z          = 0;
unsigned long long g_verified_rotate_x          = 0;
unsigned long long g_verified_rotate_y          = 0;
unsigned long long g_verified_from_rot_z        = 0;
unsigned long long g_verified_scale_matrix      = 0;
unsigned long long g_verified_from_quat33       = 0;

unsigned long long g_mismatches_affine_inverse    = 0;
unsigned long long g_mismatches_rotate_z          = 0;
unsigned long long g_mismatches_rotate_x          = 0;
unsigned long long g_mismatches_rotate_y          = 0;
unsigned long long g_mismatches_from_rot_z        = 0;
unsigned long long g_mismatches_scale_matrix      = 0;
unsigned long long g_mismatches_from_quat33       = 0;

// ============================================================================
// Fast Mathematical Implementations
// ============================================================================

__forceinline float* Fast_AffineInverse(const float* this_ptr, float* out) {
    float a0 = this_ptr[0],  a1 = this_ptr[1],  a2 = this_ptr[2];
    float b0 = this_ptr[4],  b1 = this_ptr[5],  b2 = this_ptr[6];
    float c0 = this_ptr[8],  c1 = this_ptr[9],  c2 = this_ptr[10];

    out[0]  = a0;   out[1]  = b0;   out[2]  = c0;   out[3]  = 0.0f;
    out[4]  = a1;   out[5]  = b1;   out[6]  = c1;   out[7]  = 0.0f;
    out[8]  = a2;   out[9]  = b2;   out[10] = c2;   out[11] = 0.0f;

    float neg_d0 = -this_ptr[12];
    float neg_d1 = -this_ptr[13];
    float neg_d2 = -this_ptr[14];

    out[12] = a0 * neg_d0 + a1 * neg_d1 + a2 * neg_d2;
    out[13] = b0 * neg_d0 + b1 * neg_d1 + b2 * neg_d2;
    out[14] = c0 * neg_d0 + c1 * neg_d1 + c2 * neg_d2;
    out[15] = 1.0f;

    return out;
}

__forceinline float* Fast_RotateAroundZ(float* this_ptr, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    for (int i = 0; i < 4; i++) {
        float r0 = this_ptr[i];
        float r1 = this_ptr[4 + i];
        this_ptr[i]     = c * r0 + s * r1;
        this_ptr[4 + i] = -s * r0 + c * r1;
    }
    return this_ptr;
}

__forceinline float* Fast_RotateAroundX(float* this_ptr, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    for (int i = 0; i < 4; i++) {
        float r1 = this_ptr[4 + i];
        float r2 = this_ptr[8 + i];
        this_ptr[4 + i] = c * r1 + s * r2;
        this_ptr[8 + i] = -s * r1 + c * r2;
    }
    return this_ptr;
}

__forceinline float* Fast_RotateAroundY(float* this_ptr, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    for (int i = 0; i < 4; i++) {
        float r0 = this_ptr[i];
        float r2 = this_ptr[8 + i];
        this_ptr[i]     = c * r0 - s * r2;
        this_ptr[8 + i] = s * r0 + c * r2;
    }
    return this_ptr;
}

__forceinline float* Fast_FromRotationZ(float* out, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    out[0]  = c;     out[1]  = s;    out[2]  = 0.0f; out[3]  = 0.0f;
    out[4]  = -s;    out[5]  = c;    out[6]  = 0.0f; out[7]  = 0.0f;
    out[8]  = 0.0f;  out[9]  = 0.0f; out[10] = 1.0f; out[11] = 0.0f;
    out[12] = 0.0f;  out[13] = 0.0f; out[14] = 0.0f; out[15] = 1.0f;

    return out;
}

__forceinline float* Fast_ScaleMatrix(float* out, const float* in, float scale) {
    __m128 s = _mm_set1_ps(scale);
    _mm_storeu_ps(out + 0,  _mm_mul_ps(_mm_loadu_ps(in + 0),  s));
    _mm_storeu_ps(out + 4,  _mm_mul_ps(_mm_loadu_ps(in + 4),  s));
    _mm_storeu_ps(out + 8,  _mm_mul_ps(_mm_loadu_ps(in + 8),  s));
    _mm_storeu_ps(out + 12, _mm_mul_ps(_mm_loadu_ps(in + 12), s));
    return out;
}

__forceinline float* Fast_FromQuaternion33(const float* q, float* out) {
    float x2 = q[0] + q[0];
    float y2 = q[1] + q[1];
    float z2 = q[2] + q[2];

    float xx = q[0] * x2;
    float xy = q[0] * y2;
    float xz = q[0] * z2;

    float yy = q[1] * y2;
    float yz = q[1] * z2;
    float zz = q[2] * z2;

    float wx = q[3] * x2;
    float wy = q[3] * y2;
    float wz = q[3] * z2;

    out[0]  = 1.0f - (yy + zz);
    out[1]  = xy + wz;
    out[2]  = xz - wy;

    out[4]  = xy - wz;
    out[5]  = 1.0f - (xx + zz);
    out[6]  = yz + wx;

    out[8]  = xz + wy;
    out[9]  = yz - wx;
    out[10] = 1.0f - (xx + yy);

    return out;
}

// ============================================================================
// Hook Functions with Dual-Run Verification
// ============================================================================

float* __fastcall Hook_AffineInverse(const float* this_ptr, void* /*edx*/, float* out) {
    g_calls_affine_inverse++;

    if (g_dead_affine_inverse) {
        return orig_AffineInverse(this_ptr, out);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_affine_inverse);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        orig_AffineInverse(this_ptr, expected);

        Fast_AffineInverse(this_ptr, out);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(out[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_affine_inverse++;
        } else {
            g_mismatches_affine_inverse++;
            g_dead_affine_inverse = true;
            Log("[Mat44AffineOpsFast] RETIRED AffineInverse on mismatch");
            std::memcpy(out, expected, sizeof(expected));
            return out;
        }
        return out;
    }

    return Fast_AffineInverse(this_ptr, out);
}

float* __fastcall Hook_RotateAroundZ(float* this_ptr, void* /*edx*/, float angle) {
    g_calls_rotate_z++;

    if (g_dead_rotate_z) {
        return orig_RotateAroundZ(this_ptr, angle);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_rotate_z);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        std::memcpy(expected, this_ptr, sizeof(expected));
        orig_RotateAroundZ(expected, angle);

        Fast_RotateAroundZ(this_ptr, angle);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(this_ptr[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_rotate_z++;
        } else {
            g_mismatches_rotate_z++;
            g_dead_rotate_z = true;
            Log("[Mat44AffineOpsFast] RETIRED RotateAroundZ on mismatch");
            std::memcpy(this_ptr, expected, sizeof(expected));
            return this_ptr;
        }
        return this_ptr;
    }

    return Fast_RotateAroundZ(this_ptr, angle);
}

float* __fastcall Hook_RotateAroundX(float* this_ptr, void* /*edx*/, float angle) {
    g_calls_rotate_x++;

    if (g_dead_rotate_x) {
        return orig_RotateAroundX(this_ptr, angle);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_rotate_x);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        std::memcpy(expected, this_ptr, sizeof(expected));
        orig_RotateAroundX(expected, angle);

        Fast_RotateAroundX(this_ptr, angle);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(this_ptr[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_rotate_x++;
        } else {
            g_mismatches_rotate_x++;
            g_dead_rotate_x = true;
            Log("[Mat44AffineOpsFast] RETIRED RotateAroundX on mismatch");
            std::memcpy(this_ptr, expected, sizeof(expected));
            return this_ptr;
        }
        return this_ptr;
    }

    return Fast_RotateAroundX(this_ptr, angle);
}

float* __fastcall Hook_RotateAroundY(float* this_ptr, void* /*edx*/, float angle) {
    g_calls_rotate_y++;

    if (g_dead_rotate_y) {
        return orig_RotateAroundY(this_ptr, angle);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_rotate_y);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        std::memcpy(expected, this_ptr, sizeof(expected));
        orig_RotateAroundY(expected, angle);

        Fast_RotateAroundY(this_ptr, angle);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(this_ptr[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_rotate_y++;
        } else {
            g_mismatches_rotate_y++;
            g_dead_rotate_y = true;
            Log("[Mat44AffineOpsFast] RETIRED RotateAroundY on mismatch");
            std::memcpy(this_ptr, expected, sizeof(expected));
            return this_ptr;
        }
        return this_ptr;
    }

    return Fast_RotateAroundY(this_ptr, angle);
}

float* __cdecl Hook_FromRotationZ(float* out, float angle) {
    g_calls_from_rot_z++;

    if (g_dead_from_rot_z) {
        return orig_FromRotationZ(out, angle);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_from_rot_z);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        orig_FromRotationZ(expected, angle);

        Fast_FromRotationZ(out, angle);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(out[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_from_rot_z++;
        } else {
            g_mismatches_from_rot_z++;
            g_dead_from_rot_z = true;
            Log("[Mat44AffineOpsFast] RETIRED FromRotationZ on mismatch");
            std::memcpy(out, expected, sizeof(expected));
            return out;
        }
        return out;
    }

    return Fast_FromRotationZ(out, angle);
}

float* __cdecl Hook_ScaleMatrix(float* out, const float* in, float scale) {
    g_calls_scale_matrix++;

    if (g_dead_scale_matrix) {
        return orig_ScaleMatrix(out, in, scale);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_scale_matrix);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        orig_ScaleMatrix(expected, in, scale);

        Fast_ScaleMatrix(out, in, scale);

        bool match = true;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(out[i] - expected[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_scale_matrix++;
        } else {
            g_mismatches_scale_matrix++;
            g_dead_scale_matrix = true;
            Log("[Mat44AffineOpsFast] RETIRED ScaleMatrix on mismatch");
            std::memcpy(out, expected, sizeof(expected));
            return out;
        }
        return out;
    }

    return Fast_ScaleMatrix(out, in, scale);
}

float* __cdecl Hook_FromQuaternion33(const float* q, float* out) {
    g_calls_from_quat33++;

    if (g_dead_from_quat33) {
        return orig_FromQuaternion33(q, out);
    }

    const unsigned long callId = static_cast<unsigned long>(g_calls_from_quat33);
    const bool doVerify = (callId <= kVerifyFirst) || ((callId & kResampleMask) == 0);

    if (doVerify) {
        float expected[16];
        std::memcpy(expected, out, sizeof(expected));
        orig_FromQuaternion33(q, expected);

        Fast_FromQuaternion33(q, out);

        bool match = true;
        // Compare the 9 rotation elements (0, 1, 2, 4, 5, 6, 8, 9, 10)
        const int rotIndices[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
        for (int idx : rotIndices) {
            if (std::fabs(out[idx] - expected[idx]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }

        if (match) {
            g_verified_from_quat33++;
        } else {
            g_mismatches_from_quat33++;
            g_dead_from_quat33 = true;
            Log("[Mat44AffineOpsFast] RETIRED FromQuaternion33 on mismatch");
            std::memcpy(out, expected, sizeof(expected));
            return out;
        }
        return out;
    }

    return Fast_FromQuaternion33(q, out);
}

} // anonymous namespace

// ============================================================================
// Public Interface
// ============================================================================

void Init() {
    if (g_installed) return;

    if (!Config::g_settings.OptMat44AffineOpsFast) {
        return;
    }

    // Verify prologues
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_AffineInverse), kExp_AffineInverse, sizeof(kExp_AffineInverse)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at AffineInverse (0x%08X)", kTarget_AffineInverse);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_RotateAroundZ), kExp_RotateAroundZ, sizeof(kExp_RotateAroundZ)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at RotateAroundZ (0x%08X)", kTarget_RotateAroundZ);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_RotateAroundX), kExp_RotateAroundX, sizeof(kExp_RotateAroundX)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at RotateAroundX (0x%08X)", kTarget_RotateAroundX);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_RotateAroundY), kExp_RotateAroundY, sizeof(kExp_RotateAroundY)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at RotateAroundY (0x%08X)", kTarget_RotateAroundY);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_FromRotationZ), kExp_FromRotationZ, sizeof(kExp_FromRotationZ)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at FromRotationZ (0x%08X)", kTarget_FromRotationZ);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_ScaleMatrix), kExp_ScaleMatrix, sizeof(kExp_ScaleMatrix)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at ScaleMatrix (0x%08X)", kTarget_ScaleMatrix);
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kTarget_FromQuaternion33), kExp_FromQuaternion33, sizeof(kExp_FromQuaternion33)) != 0) {
        Log("[Mat44AffineOpsFast] Prologue mismatch at FromQuaternion33 (0x%08X)", kTarget_FromQuaternion33);
        return;
    }

    // Create hooks
    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_AffineInverse),
                      reinterpret_cast<void*>(&Hook_AffineInverse),
                      reinterpret_cast<void**>(&orig_AffineInverse)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for AffineInverse");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_RotateAroundZ),
                      reinterpret_cast<void*>(&Hook_RotateAroundZ),
                      reinterpret_cast<void**>(&orig_RotateAroundZ)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for RotateAroundZ");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_RotateAroundX),
                      reinterpret_cast<void*>(&Hook_RotateAroundX),
                      reinterpret_cast<void**>(&orig_RotateAroundX)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for RotateAroundX");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_RotateAroundY),
                      reinterpret_cast<void*>(&Hook_RotateAroundY),
                      reinterpret_cast<void**>(&orig_RotateAroundY)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for RotateAroundY");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_FromRotationZ),
                      reinterpret_cast<void*>(&Hook_FromRotationZ),
                      reinterpret_cast<void**>(&orig_FromRotationZ)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for FromRotationZ");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_ScaleMatrix),
                      reinterpret_cast<void*>(&Hook_ScaleMatrix),
                      reinterpret_cast<void**>(&orig_ScaleMatrix)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for ScaleMatrix");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(kTarget_FromQuaternion33),
                      reinterpret_cast<void*>(&Hook_FromQuaternion33),
                      reinterpret_cast<void**>(&orig_FromQuaternion33)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to create hook for FromQuaternion33");
        return;
    }

    // Enable hooks
    if (MH_EnableHook(reinterpret_cast<void*>(kTarget_AffineInverse)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_RotateAroundZ)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_RotateAroundX)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_RotateAroundY)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_FromRotationZ)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_ScaleMatrix)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(kTarget_FromQuaternion33)) != MH_OK) {
        Log("[Mat44AffineOpsFast] Failed to enable one or more hooks");
        return;
    }

    g_installed = true;
    Log("[Mat44AffineOpsFast] Acceleration ACTIVE (AffineInverse, RotateZ/X/Y, FromRotZ, ScaleMatrix, FromQuaternion33 - 100+ call sites)");
}

void LogStats() {
    if (!g_installed) return;

    Log("[Mat44AffineOpsFast] Stats:");
    Log("  AffineInverse:    calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_affine_inverse, g_verified_affine_inverse, g_mismatches_affine_inverse, g_dead_affine_inverse ? "YES" : "NO");
    Log("  RotateAroundZ:    calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_rotate_z, g_verified_rotate_z, g_mismatches_rotate_z, g_dead_rotate_z ? "YES" : "NO");
    Log("  RotateAroundX:    calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_rotate_x, g_verified_rotate_x, g_mismatches_rotate_x, g_dead_rotate_x ? "YES" : "NO");
    Log("  RotateAroundY:    calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_rotate_y, g_verified_rotate_y, g_mismatches_rotate_y, g_dead_rotate_y ? "YES" : "NO");
    Log("  FromRotationZ:    calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_from_rot_z, g_verified_from_rot_z, g_mismatches_from_rot_z, g_dead_from_rot_z ? "YES" : "NO");
    Log("  ScaleMatrix:      calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_scale_matrix, g_verified_scale_matrix, g_mismatches_scale_matrix, g_dead_scale_matrix ? "YES" : "NO");
    Log("  FromQuaternion33: calls=%llu, verified=%llu, mismatches=%llu, dead=%s",
        g_calls_from_quat33, g_verified_from_quat33, g_mismatches_from_quat33, g_dead_from_quat33 ? "YES" : "NO");
}

void Shutdown() {
    if (!g_installed) return;

    MH_DisableHook(reinterpret_cast<void*>(kTarget_AffineInverse));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_RotateAroundZ));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_RotateAroundX));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_RotateAroundY));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_FromRotationZ));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_ScaleMatrix));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_FromQuaternion33));

    g_installed = false;
    Log("[Mat44AffineOpsFast] Shutdown complete");
}

} // namespace Mat44AffineOpsFast

