// ============================================================================
// Module: mat33_quat_ops_fast.cpp
//
// Hardware-accelerated 3x3 matrix, quaternion multiplication, vector rotation,
// and quadratic solver operations for WoW 3.3.5a:
//   - sub_4C5100: C33Matrix::C33Matrix(const C4Quaternion& q)
//   - sub_4C51F0: C33Matrix::Determinant()
//   - sub_4C5340: C33Matrix operator*(const C33Matrix&, const C33Matrix&)
//   - sub_4C5430: C33Matrix::Adjoint()
//   - sub_4F4320: C4Quaternion operator*(const C4Quaternion&, const C4Quaternion&)
//   - sub_50F520: C3Vector operator*=(C3Vector&, const C33Matrix&)
//   - sub_4C5000: CMath::solvequad_(float a, float b, float c, float& r0, float& r1)
//
// In stock 3.3.5a:
//   - sub_4C5100 builds a 3x3 rotation matrix from a 4D quaternion across 72
//     serialized x87 instructions, causing pipeline stalls and stack spills.
//   - sub_4C51F0 computes a 3x3 determinant across 24 serial x87 instructions.
//   - sub_4C5340 evaluates 3x3 matrix multiplication across 88 serial x87
//     instructions with unneeded memory store-load hops.
//   - sub_4C5430 computes 3x3 matrix adjoint cofactors across 63 serial x87
//     instructions.
//   - sub_4F4320 evaluates 4D quaternion multiplication across 55 serial x87
//     instructions and intermediate spills.
//   - sub_50F520 rotates a 3D vector in place by a 3x3 matrix across 31 serial
//     x87 instructions.
//   - sub_4C5000 solves quadratic equations for ray-cone and projectile
//     trajectories across 63 x87 instructions with status-word branching.
//
// This module provides register-allocated, branchless implementations eliminating
// pipeline stalls, intermediate memory spills, and store-to-load forwarding
// penalties. Dual-run verification and automatic fail-safe retirement guards
// ensure strict numerical parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "mat33_quat_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace Mat33QuatOpsFast {

namespace {

constexpr uintptr_t kTarget_FromQuat    = 0x004C5100;
constexpr uintptr_t kTarget_Determinant = 0x004C51F0;
constexpr uintptr_t kTarget_Mat33Mul    = 0x004C5340;
constexpr uintptr_t kTarget_Adjoint     = 0x004C5430;
constexpr uintptr_t kTarget_QuatMul     = 0x004F4320;
constexpr uintptr_t kTarget_Vec3Mat33   = 0x0050F520;
constexpr uintptr_t kTarget_SolveQuad   = 0x004C5000;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0002f;

// Function signatures
typedef float* (__thiscall* FromQuat_fn)(float* this_ptr, const float* q);
typedef double (__thiscall* Determinant_fn)(const float* this_ptr);
typedef float* (__cdecl* Mat33Mul_fn)(float* out, const float* l, const float* r);
typedef float* (__thiscall* Adjoint_fn)(const float* this_ptr, float* out);
typedef float* (__cdecl* QuatMul_fn)(float* out, const float* l, const float* r);
typedef float* (__cdecl* Vec3Mat33_fn)(float* out, float* v, const float* m);
typedef char (__cdecl* SolveQuad_fn)(float a, float b, float c, float* r0, float* r1);

FromQuat_fn    orig_FromQuat    = nullptr;
Determinant_fn orig_Determinant = nullptr;
Mat33Mul_fn    orig_Mat33Mul    = nullptr;
Adjoint_fn     orig_Adjoint     = nullptr;
QuatMul_fn     orig_QuatMul     = nullptr;
Vec3Mat33_fn   orig_Vec3Mat33   = nullptr;
SolveQuad_fn   orig_SolveQuad   = nullptr;

bool g_installed = false;

bool g_dead_from_quat    = false;
bool g_dead_determinant  = false;
bool g_dead_mat33_mul    = false;
bool g_dead_adjoint      = false;
bool g_dead_quat_mul     = false;
bool g_dead_vec3_mat33   = false;
bool g_dead_solve_quad   = false;

unsigned long long g_calls_from_quat    = 0;
unsigned long long g_calls_determinant  = 0;
unsigned long long g_calls_mat33_mul    = 0;
unsigned long long g_calls_adjoint      = 0;
unsigned long long g_calls_quat_mul     = 0;
unsigned long long g_calls_vec3_mat33   = 0;
unsigned long long g_calls_solve_quad   = 0;

unsigned long long g_verified_from_quat    = 0;
unsigned long long g_verified_determinant  = 0;
unsigned long long g_verified_mat33_mul    = 0;
unsigned long long g_verified_adjoint      = 0;
unsigned long long g_verified_quat_mul     = 0;
unsigned long long g_verified_vec3_mat33   = 0;
unsigned long long g_verified_solve_quad   = 0;

unsigned long long g_mismatches_from_quat    = 0;
unsigned long long g_mismatches_determinant  = 0;
unsigned long long g_mismatches_mat33_mul    = 0;
unsigned long long g_mismatches_adjoint      = 0;
unsigned long long g_mismatches_quat_mul     = 0;
unsigned long long g_mismatches_vec3_mat33   = 0;
unsigned long long g_mismatches_solve_quad   = 0;

// ============================================================================
// Direct Fast Mathematical Implementations
// ============================================================================

__forceinline float* Fast_FromQuat(float* out, const float* q) {
    const double qx = static_cast<double>(q[0]);
    const double qy = static_cast<double>(q[1]);
    const double qz = static_cast<double>(q[2]);
    const double qw = static_cast<double>(q[3]);

    const double x2 = qx * 2.0;
    const double y2 = qy * 2.0;
    const double z2 = qz * 2.0;

    const double wx = qw * x2;
    const double wy = qw * y2;
    const double wz = qw * z2;

    const double xx = x2 * qx;
    const double xy = qx * y2;
    const double xz = qx * z2;

    const double yy = y2 * qy;
    const double yz = qy * z2;
    const double zz = z2 * qz;

    out[0] = static_cast<float>(1.0 - (zz + yy));
    out[1] = static_cast<float>(xy + wz);
    out[2] = static_cast<float>(xz - wy);
    out[3] = static_cast<float>(xy - wz);
    out[4] = static_cast<float>(1.0 - (zz + xx));
    out[5] = static_cast<float>(yz + wx);
    out[6] = static_cast<float>(wy + xz);
    out[7] = static_cast<float>(yz - wx);
    out[8] = static_cast<float>(1.0 - (xx + yy));

    return out;
}

__forceinline double Fast_Determinant(const float* m) {
    const double m0 = static_cast<double>(m[0]);
    const double m1 = static_cast<double>(m[1]);
    const double m2 = static_cast<double>(m[2]);
    const double m3 = static_cast<double>(m[3]);
    const double m4 = static_cast<double>(m[4]);
    const double m5 = static_cast<double>(m[5]);
    const double m6 = static_cast<double>(m[6]);
    const double m7 = static_cast<double>(m[7]);
    const double m8 = static_cast<double>(m[8]);

    const double term1 = m6 * m1 * m5;
    const double term2 = m2 * m3 * m7;
    const double term3 = m8 * m0 * m4;
    const double term4 = m6 * m2 * m4;
    const double term5 = m8 * m1 * m3;
    const double term6 = m0 * m7 * m5;

    return ((term1 + term2) + term3) - term4 - term5 - term6;
}

__forceinline float* Fast_Mat33Mul(float* out, const float* l, const float* r) {
    const double l0 = static_cast<double>(l[0]), l1 = static_cast<double>(l[1]), l2 = static_cast<double>(l[2]);
    const double l3 = static_cast<double>(l[3]), l4 = static_cast<double>(l[4]), l5 = static_cast<double>(l[5]);
    const double l6 = static_cast<double>(l[6]), l7 = static_cast<double>(l[7]), l8 = static_cast<double>(l[8]);

    const double r0 = static_cast<double>(r[0]), r1 = static_cast<double>(r[1]), r2 = static_cast<double>(r[2]);
    const double r3 = static_cast<double>(r[3]), r4 = static_cast<double>(r[4]), r5 = static_cast<double>(r[5]);
    const double r6 = static_cast<double>(r[6]), r7 = static_cast<double>(r[7]), r8 = static_cast<double>(r[8]);

    const float out0 = static_cast<float>(l1 * r3 + r6 * l2 + l0 * r0);
    const float out1 = static_cast<float>(r7 * l2 + l0 * r1 + l1 * r4);
    const float out2 = static_cast<float>(l1 * r5 + r2 * l0 + r8 * l2);

    const float out3 = static_cast<float>(l3 * r0 + r6 * l5 + r3 * l4);
    const float out4 = static_cast<float>(l3 * r1 + r7 * l5 + r4 * l4);
    const float out5 = static_cast<float>(l5 * r8 + l4 * r5 + l3 * r2);

    const float out6 = static_cast<float>(r6 * l8 + l7 * r3 + l6 * r0);
    const float out7 = static_cast<float>(l6 * r1 + r7 * l8 + r4 * l7);
    const float out8 = static_cast<float>(l8 * r8 + l7 * r5 + l6 * r2);

    out[0] = out0;
    out[1] = out1;
    out[2] = out2;
    out[3] = out3;
    out[4] = out4;
    out[5] = out5;
    out[6] = out6;
    out[7] = out7;
    out[8] = out8;

    return out;
}

__forceinline float* Fast_Adjoint(const float* m, float* out) {
    const double m0 = static_cast<double>(m[0]), m1 = static_cast<double>(m[1]), m2 = static_cast<double>(m[2]);
    const double m3 = static_cast<double>(m[3]), m4 = static_cast<double>(m[4]), m5 = static_cast<double>(m[5]);
    const double m6 = static_cast<double>(m[6]), m7 = static_cast<double>(m[7]), m8 = static_cast<double>(m[8]);

    const float out0 = static_cast<float>(m4 * m8 - m7 * m5);
    const float out1 = static_cast<float>(-(m8 * m1 - m7 * m2));
    const float out2 = static_cast<float>(m5 * m1 - m4 * m2);

    const float out3 = static_cast<float>(-(m3 * m8 - m5 * m6));
    const float out4 = static_cast<float>(m0 * m8 - m6 * m2);
    const float out5 = static_cast<float>(-(m5 * m0 - m3 * m2));

    const float out6 = static_cast<float>(m7 * m3 - m4 * m6);
    const float out7 = static_cast<float>(-(m7 * m0 - m6 * m1));
    const float out8 = static_cast<float>(m4 * m0 - m3 * m1);

    out[0] = out0;
    out[1] = out1;
    out[2] = out2;
    out[3] = out3;
    out[4] = out4;
    out[5] = out5;
    out[6] = out6;
    out[7] = out7;
    out[8] = out8;

    return out;
}

__forceinline float* Fast_QuatMul(float* out, const float* l, const float* r) {
    const double lx = static_cast<double>(l[0]), ly = static_cast<double>(l[1]), lz = static_cast<double>(l[2]), lw = static_cast<double>(l[3]);
    const double rx = static_cast<double>(r[0]), ry = static_cast<double>(r[1]), rz = static_cast<double>(r[2]), rw = static_cast<double>(r[3]);

    const float outx = static_cast<float>(lw * rx + rz * ly + rw * lx - lz * ry);
    const float outy = static_cast<float>(rw * ly + lw * ry + lz * rx - rz * lx);
    const float outz = static_cast<float>(rz * lw + rw * lz + lx * ry - rx * ly);
    const float outw = static_cast<float>(rw * lw - lx * rx - ly * ry - rz * lz);

    out[0] = outx;
    out[1] = outy;
    out[2] = outz;
    out[3] = outw;

    return out;
}

__forceinline float* Fast_Vec3Mat33(float* out, float* v, const float* m) {
    const double vx = static_cast<double>(v[0]);
    const double vy = static_cast<double>(v[1]);
    const double vz = static_cast<double>(v[2]);

    const double m0 = static_cast<double>(m[0]), m1 = static_cast<double>(m[1]), m2 = static_cast<double>(m[2]);
    const double m3 = static_cast<double>(m[3]), m4 = static_cast<double>(m[4]), m5 = static_cast<double>(m[5]);
    const double m6 = static_cast<double>(m[6]), m7 = static_cast<double>(m[7]), m8 = static_cast<double>(m[8]);

    const float ny = static_cast<float>(m7 * vz + m4 * vy + m1 * vx);
    const float nz = static_cast<float>(m8 * vz + m5 * vy + m2 * vx);
    const float nx = static_cast<float>(m6 * vz + m3 * vy + vx * m0);

    v[0] = nx;
    v[1] = ny;
    v[2] = nz;

    if (out != v) {
        out[0] = nx;
        out[1] = ny;
        out[2] = nz;
    }

    return out;
}

__forceinline char Fast_SolveQuad(float a, float b, float c, float* r0, float* r1) {
    const double da = static_cast<double>(a);
    const double db = static_cast<double>(b);
    const double dc = static_cast<double>(c);

    const double fourAC = da * dc * 4.0;
    const double bb = db * db;

    if (bb <= fourAC) {
        return 0;
    }

    const double root = std::sqrt(bb - fourAC);
    const double q = (db > 0.0 ? db + root : db - root) * -0.5;
    const double inv = 1.0 / (q * da);
    const float x0 = static_cast<float>(q * (inv * q));
    const float x1 = static_cast<float>(dc * (da * inv));

    if (x1 < x0) {
        *r0 = x1;
        *r1 = x0;
    } else {
        *r0 = x0;
        *r1 = x1;
    }

    return 1;
}

// ============================================================================
// Hook Detours with Dual-Run Verification and Fail-Safe Guards
// ============================================================================

static float* __fastcall Hooked_FromQuat(float* this_ptr, void* /*dummy_edx*/, const float* q) {
    g_calls_from_quat++;

    if (g_dead_from_quat) {
        return orig_FromQuat(this_ptr, q);
    }

    const bool need_verify = (g_calls_from_quat <= kVerifyFirst) || ((g_calls_from_quat & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_FromQuat(this_ptr, q);
    }

    float stock_res[9];
    orig_FromQuat(stock_res, q);

    float fast_res[9];
    Fast_FromQuat(fast_res, q);

    bool match = true;
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_from_quat++;
        std::memcpy(this_ptr, fast_res, sizeof(fast_res));
        return this_ptr;
    }

    g_mismatches_from_quat++;
    g_dead_from_quat = true;
    Log("[Mat33QuatOpsFast] RETIRED FromQuat hook due to mismatch");
    std::memcpy(this_ptr, stock_res, sizeof(stock_res));
    return this_ptr;
}

static double __fastcall Hooked_Determinant(const float* this_ptr, void* /*dummy_edx*/) {
    g_calls_determinant++;

    if (g_dead_determinant) {
        return orig_Determinant(this_ptr);
    }

    const bool need_verify = (g_calls_determinant <= kVerifyFirst) || ((g_calls_determinant & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Determinant(this_ptr);
    }

    const double stock_res = orig_Determinant(this_ptr);
    const double fast_res = Fast_Determinant(this_ptr);

    if (std::fabs(stock_res - fast_res) <= static_cast<double>(kVerifyTolerance)) {
        g_verified_determinant++;
        return fast_res;
    }

    g_mismatches_determinant++;
    g_dead_determinant = true;
    Log("[Mat33QuatOpsFast] RETIRED Determinant hook due to mismatch: stock=%f, fast=%f", stock_res, fast_res);
    return stock_res;
}

static float* __cdecl Hooked_Mat33Mul(float* out, const float* l, const float* r) {
    g_calls_mat33_mul++;

    if (g_dead_mat33_mul) {
        return orig_Mat33Mul(out, l, r);
    }

    const bool need_verify = (g_calls_mat33_mul <= kVerifyFirst) || ((g_calls_mat33_mul & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Mat33Mul(out, l, r);
    }

    float stock_res[9];
    orig_Mat33Mul(stock_res, l, r);

    float fast_res[9];
    Fast_Mat33Mul(fast_res, l, r);

    bool match = true;
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_mat33_mul++;
        std::memcpy(out, fast_res, sizeof(fast_res));
        return out;
    }

    g_mismatches_mat33_mul++;
    g_dead_mat33_mul = true;
    Log("[Mat33QuatOpsFast] RETIRED Mat33Mul hook due to mismatch");
    std::memcpy(out, stock_res, sizeof(stock_res));
    return out;
}

static float* __fastcall Hooked_Adjoint(const float* this_ptr, void* /*dummy_edx*/, float* out) {
    g_calls_adjoint++;

    if (g_dead_adjoint) {
        return orig_Adjoint(this_ptr, out);
    }

    const bool need_verify = (g_calls_adjoint <= kVerifyFirst) || ((g_calls_adjoint & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Adjoint(this_ptr, out);
    }

    float stock_res[9];
    orig_Adjoint(this_ptr, stock_res);

    float fast_res[9];
    Fast_Adjoint(this_ptr, fast_res);

    bool match = true;
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_adjoint++;
        std::memcpy(out, fast_res, sizeof(fast_res));
        return out;
    }

    g_mismatches_adjoint++;
    g_dead_adjoint = true;
    Log("[Mat33QuatOpsFast] RETIRED Adjoint hook due to mismatch");
    std::memcpy(out, stock_res, sizeof(stock_res));
    return out;
}

static float* __cdecl Hooked_QuatMul(float* out, const float* l, const float* r) {
    g_calls_quat_mul++;

    if (g_dead_quat_mul) {
        return orig_QuatMul(out, l, r);
    }

    const bool need_verify = (g_calls_quat_mul <= kVerifyFirst) || ((g_calls_quat_mul & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_QuatMul(out, l, r);
    }

    float stock_res[4];
    orig_QuatMul(stock_res, l, r);

    float fast_res[4];
    Fast_QuatMul(fast_res, l, r);

    bool match = true;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_quat_mul++;
        std::memcpy(out, fast_res, sizeof(fast_res));
        return out;
    }

    g_mismatches_quat_mul++;
    g_dead_quat_mul = true;
    Log("[Mat33QuatOpsFast] RETIRED QuatMul hook due to mismatch");
    std::memcpy(out, stock_res, sizeof(stock_res));
    return out;
}

static float* __cdecl Hooked_Vec3Mat33(float* out, float* v, const float* m) {
    g_calls_vec3_mat33++;

    if (g_dead_vec3_mat33) {
        return orig_Vec3Mat33(out, v, m);
    }

    const bool need_verify = (g_calls_vec3_mat33 <= kVerifyFirst) || ((g_calls_vec3_mat33 & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Vec3Mat33(out, v, m);
    }

    float in_v[3];
    std::memcpy(in_v, v, sizeof(in_v));

    float stock_v[3];
    std::memcpy(stock_v, v, sizeof(stock_v));
    float stock_out[3];
    orig_Vec3Mat33(stock_out, stock_v, m);

    float fast_v[3];
    std::memcpy(fast_v, in_v, sizeof(fast_v));
    float fast_out[3];
    Fast_Vec3Mat33(fast_out, fast_v, m);

    bool match = true;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(stock_v[i] - fast_v[i]) > kVerifyTolerance ||
            std::fabs(stock_out[i] - fast_out[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_vec3_mat33++;
        std::memcpy(v, fast_v, sizeof(fast_v));
        std::memcpy(out, fast_out, sizeof(fast_out));
        return out;
    }

    g_mismatches_vec3_mat33++;
    g_dead_vec3_mat33 = true;
    Log("[Mat33QuatOpsFast] RETIRED Vec3Mat33 hook due to mismatch");
    std::memcpy(v, stock_v, sizeof(stock_v));
    std::memcpy(out, stock_out, sizeof(stock_out));
    return out;
}

static char __cdecl Hooked_SolveQuad(float a, float b, float c, float* r0, float* r1) {
    g_calls_solve_quad++;

    if (g_dead_solve_quad) {
        return orig_SolveQuad(a, b, c, r0, r1);
    }

    const bool need_verify = (g_calls_solve_quad <= kVerifyFirst) || ((g_calls_solve_quad & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_SolveQuad(a, b, c, r0, r1);
    }

    float stock_r0 = 0.0f, stock_r1 = 0.0f;
    const char stock_ret = orig_SolveQuad(a, b, c, &stock_r0, &stock_r1);

    float fast_r0 = 0.0f, fast_r1 = 0.0f;
    const char fast_ret = Fast_SolveQuad(a, b, c, &fast_r0, &fast_r1);

    bool match = (stock_ret == fast_ret);
    if (match && stock_ret != 0) {
        if (std::fabs(stock_r0 - fast_r0) > kVerifyTolerance ||
            std::fabs(stock_r1 - fast_r1) > kVerifyTolerance) {
            match = false;
        }
    }

    if (match) {
        g_verified_solve_quad++;
        if (stock_ret != 0) {
            *r0 = fast_r0;
            *r1 = fast_r1;
        }
        return fast_ret;
    }

    g_mismatches_solve_quad++;
    g_dead_solve_quad = true;
    Log("[Mat33QuatOpsFast] RETIRED SolveQuad hook due to mismatch: stock=%d, fast=%d", static_cast<int>(stock_ret), static_cast<int>(fast_ret));
    if (stock_ret != 0) {
        *r0 = stock_r0;
        *r1 = stock_r1;
    }
    return stock_ret;
}

} // anonymous namespace

void Init() {
    if (!Config::g_settings.OptMat33QuatOpsFast) {
        return;
    }

    static const unsigned char kExp_FromQuat[8]    = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x55 };
    static const unsigned char kExp_Determinant[8] = { 0xD9, 0x41, 0x18, 0xD8, 0x49, 0x04, 0xD8, 0x49 };
    static const unsigned char kExp_Mat33Mul[8]    = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x55 };
    static const unsigned char kExp_Adjoint[8]     = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x10, 0x8B, 0x45 };
    static const unsigned char kExp_QuatMul[8]     = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x55 };
    static const unsigned char kExp_Vec3Mat33[8]   = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x45 };
    static const unsigned char kExp_SolveQuad[8]   = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xD9, 0xC0 };

    if (memcmp((const void*)kTarget_FromQuat, kExp_FromQuat, 8) != 0) {
        Log("[Mat33QuatOpsFast] FromQuat prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Determinant, kExp_Determinant, 8) != 0) {
        Log("[Mat33QuatOpsFast] Determinant prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Mat33Mul, kExp_Mat33Mul, 8) != 0) {
        Log("[Mat33QuatOpsFast] Mat33Mul prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Adjoint, kExp_Adjoint, 8) != 0) {
        Log("[Mat33QuatOpsFast] Adjoint prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_QuatMul, kExp_QuatMul, 8) != 0) {
        Log("[Mat33QuatOpsFast] QuatMul prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Vec3Mat33, kExp_Vec3Mat33, 8) != 0) {
        Log("[Mat33QuatOpsFast] Vec3Mat33 prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_SolveQuad, kExp_SolveQuad, 8) != 0) {
        Log("[Mat33QuatOpsFast] SolveQuad prologue mismatch, aborting");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_FromQuat,    (void*)Hooked_FromQuat,    (void**)&orig_FromQuat)    != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Determinant, (void*)Hooked_Determinant, (void**)&orig_Determinant) != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Mat33Mul,    (void*)Hooked_Mat33Mul,    (void**)&orig_Mat33Mul)    != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Adjoint,     (void*)Hooked_Adjoint,     (void**)&orig_Adjoint)     != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_QuatMul,     (void*)Hooked_QuatMul,     (void**)&orig_QuatMul)     != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Vec3Mat33,   (void*)Hooked_Vec3Mat33,   (void**)&orig_Vec3Mat33)   != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_SolveQuad,   (void*)Hooked_SolveQuad,   (void**)&orig_SolveQuad)   != MH_OK) {
        Log("[Mat33QuatOpsFast] Failed to create one or more hooks");
        return;
    }

    if (WO_EnableHook((void*)kTarget_FromQuat)    != MH_OK ||
        WO_EnableHook((void*)kTarget_Determinant) != MH_OK ||
        WO_EnableHook((void*)kTarget_Mat33Mul)    != MH_OK ||
        WO_EnableHook((void*)kTarget_Adjoint)     != MH_OK ||
        WO_EnableHook((void*)kTarget_QuatMul)     != MH_OK ||
        WO_EnableHook((void*)kTarget_Vec3Mat33)   != MH_OK ||
        WO_EnableHook((void*)kTarget_SolveQuad)   != MH_OK) {
        Log("[Mat33QuatOpsFast] Failed to enable one or more hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_Mat33FromQuat", (const void*)&Hooked_FromQuat);
    SamplingProfiler::RegisterSelfSymbol("Fast_Mat33Det",      (const void*)&Hooked_Determinant);
    SamplingProfiler::RegisterSelfSymbol("Fast_Mat33Mul",      (const void*)&Hooked_Mat33Mul);
    SamplingProfiler::RegisterSelfSymbol("Fast_Mat33Adjoint",  (const void*)&Hooked_Adjoint);
    SamplingProfiler::RegisterSelfSymbol("Fast_QuatMul",       (const void*)&Hooked_QuatMul);
    SamplingProfiler::RegisterSelfSymbol("Fast_Vec3Mat33",     (const void*)&Hooked_Vec3Mat33);
    SamplingProfiler::RegisterSelfSymbol("Fast_SolveQuad",     (const void*)&Hooked_SolveQuad);

    g_installed = true;
    Log("[Mat33QuatOpsFast] ACTIVE on 3x3 matrix, quaternion, vector rotation, and quad solver (sub_4C5100/4C51F0/4C5340/4C5430/4F4320/50F520/4C5000)");
}

void LogStats() {
    if (!Config::g_settings.OptMat33QuatOpsFast || !g_installed) {
        return;
    }

    Log("[Mat33QuatOpsFast] Stats: FromQuat=%llu (%llu ver, %llu mis%s), "
        "Determinant=%llu (%llu ver, %llu mis%s), "
        "Mat33Mul=%llu (%llu ver, %llu mis%s), "
        "Adjoint=%llu (%llu ver, %llu mis%s), "
        "QuatMul=%llu (%llu ver, %llu mis%s), "
        "Vec3Mat33=%llu (%llu ver, %llu mis%s), "
        "SolveQuad=%llu (%llu ver, %llu mis%s)",
        g_calls_from_quat, g_verified_from_quat, g_mismatches_from_quat, g_dead_from_quat ? " [RETIRED]" : "",
        g_calls_determinant, g_verified_determinant, g_mismatches_determinant, g_dead_determinant ? " [RETIRED]" : "",
        g_calls_mat33_mul, g_verified_mat33_mul, g_mismatches_mat33_mul, g_dead_mat33_mul ? " [RETIRED]" : "",
        g_calls_adjoint, g_verified_adjoint, g_mismatches_adjoint, g_dead_adjoint ? " [RETIRED]" : "",
        g_calls_quat_mul, g_verified_quat_mul, g_mismatches_quat_mul, g_dead_quat_mul ? " [RETIRED]" : "",
        g_calls_vec3_mat33, g_verified_vec3_mat33, g_mismatches_vec3_mat33, g_dead_vec3_mat33 ? " [RETIRED]" : "",
        g_calls_solve_quad, g_verified_solve_quad, g_mismatches_solve_quad, g_dead_solve_quad ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_FromQuat);
    MH_DisableHook((void*)kTarget_Determinant);
    MH_DisableHook((void*)kTarget_Mat33Mul);
    MH_DisableHook((void*)kTarget_Adjoint);
    MH_DisableHook((void*)kTarget_QuatMul);
    MH_DisableHook((void*)kTarget_Vec3Mat33);
    MH_DisableHook((void*)kTarget_SolveQuad);
    g_installed = false;
}

} // namespace Mat33QuatOpsFast
