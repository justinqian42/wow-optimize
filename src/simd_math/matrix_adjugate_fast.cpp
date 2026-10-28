#include "matrix_adjugate_fast.h"
#include "core/config.h"
#include "diagnostics/sampling_profiler.h"
#include <windows.h>
#include <cstring>
#include <cstdint>
#include "MinHook.h"

extern "C" void Log(const char* fmt, ...);

namespace MatrixAdjugateFast {

namespace {

constexpr uintptr_t kTarget = 0x004C2440; // sub_4C2440 (2,892 bytes, 685 instructions, CMatrix_AdjugateDet)

// 16-byte prologue verified against IDA Pro:
// 55 8B EC 81 EC 2C 02 00 00 D9 01 D9 41 04 D9 41
const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x2C, 0x02, 0x00, 0x00, 0xD9, 0x01, 0xD9, 0x41, 0x04, 0xD9, 0x41
};

typedef float* (__fastcall *FnClientAdjugate)(const float* in_mat, void* dummy_edx, float* out_adjugate);
static FnClientAdjugate g_orig = nullptr;

static volatile LONG g_calls = 0;
static volatile LONG g_verified = 0;
static volatile LONG g_mismatches = 0;
static volatile bool g_dead = false;
static bool g_active = false;

struct Matrix16 {
    float m[16];
};

__declspec(noinline) __declspec(safebuffers)
static float* Fast_CMatrix_Adjugate(const float* in_mat, float* out_adjugate)
{
    const double m00 = in_mat[0],  m01 = in_mat[1],  m02 = in_mat[2],  m03 = in_mat[3];
    const double m10 = in_mat[4],  m11 = in_mat[5],  m12 = in_mat[6],  m13 = in_mat[7];
    const double m20 = in_mat[8],  m21 = in_mat[9],  m22 = in_mat[10], m23 = in_mat[11];
    const double m30 = in_mat[12], m31 = in_mat[13], m32 = in_mat[14], m33 = in_mat[15];

    out_adjugate[0] = (float)(m12 * m23 * m31 + m13 * m21 * m32 + m11 * m22 * m33 - m13 * m22 * m31 - m12 * m21 * m33 - m11 * m23 * m32);
    out_adjugate[1] = (float)(-(m02 * m23 * m31 + m03 * m21 * m32 + m01 * m22 * m33 - m03 * m22 * m31 - m02 * m21 * m33 - m01 * m23 * m32));
    out_adjugate[2] = (float)(m01 * m12 * m33 + m02 * m13 * m31 + m03 * m11 * m32 - m03 * m12 * m31 - m02 * m11 * m33 - m01 * m13 * m32);
    out_adjugate[3] = (float)(-(m02 * m13 * m21 + m03 * m11 * m22 + m01 * m12 * m23 - m03 * m12 * m21 - m02 * m11 * m23 - m01 * m13 * m22));

    out_adjugate[4] = (float)(-(m13 * m20 * m32 + m10 * m22 * m33 + m12 * m23 * m30 - m13 * m22 * m30 - m12 * m20 * m33 - m10 * m23 * m32));
    out_adjugate[5] = (float)(m02 * m23 * m30 + m03 * m20 * m32 + m00 * m22 * m33 - m03 * m22 * m30 - m02 * m20 * m33 - m00 * m23 * m32);
    out_adjugate[6] = (float)(-(m02 * m13 * m30 + m03 * m10 * m32 + m00 * m12 * m33 - m03 * m12 * m30 - m02 * m10 * m33 - m00 * m13 * m32));
    out_adjugate[7] = (float)(m02 * m13 * m20 + m03 * m10 * m22 + m00 * m12 * m23 - m03 * m12 * m20 - m02 * m10 * m23 - m00 * m13 * m22);

    out_adjugate[8] = (float)(m11 * m23 * m30 + m13 * m20 * m31 + m10 * m21 * m33 - m13 * m21 * m30 - m11 * m20 * m33 - m10 * m23 * m31);
    out_adjugate[9] = (float)(-(m01 * m23 * m30 + m03 * m20 * m31 + m00 * m21 * m33 - m03 * m21 * m30 - m01 * m20 * m33 - m00 * m23 * m31));
    out_adjugate[10] = (float)(m01 * m13 * m30 + m03 * m10 * m31 + m00 * m11 * m33 - m03 * m11 * m30 - m01 * m10 * m33 - m00 * m13 * m31);
    out_adjugate[11] = (float)(-(m01 * m13 * m20 + m03 * m10 * m21 + m00 * m11 * m23 - m03 * m11 * m20 - m01 * m10 * m23 - m00 * m13 * m21));

    out_adjugate[12] = (float)(-(m12 * m20 * m31 + m10 * m21 * m32 + m11 * m22 * m30 - m12 * m21 * m30 - m11 * m20 * m32 - m10 * m22 * m31));
    out_adjugate[13] = (float)(m01 * m22 * m30 + m02 * m20 * m31 + m00 * m21 * m32 - m02 * m21 * m30 - m01 * m20 * m32 - m00 * m22 * m31);
    out_adjugate[14] = (float)(-(m01 * m12 * m30 + m02 * m10 * m31 + m00 * m11 * m32 - m02 * m11 * m30 - m01 * m10 * m32 - m00 * m12 * m31));
    out_adjugate[15] = (float)(m00 * m11 * m22 + m01 * m12 * m20 + m02 * m10 * m21 - m11 * m02 * m20 - m22 * (m10 * m01) - m00 * m12 * m21);

    return out_adjugate;
}

__declspec(noinline) __declspec(safebuffers)
static float* Verify_CMatrix_Adjugate(const float* in_mat, void* dummy_edx, float* out_adjugate)
{
    Matrix16 client_mat;
    Matrix16 fast_mat;

    g_orig(in_mat, dummy_edx, client_mat.m);
    Fast_CMatrix_Adjugate(in_mat, fast_mat.m);

    const uint32_t* c = (const uint32_t*)client_mat.m;
    const uint32_t* f = (const uint32_t*)fast_mat.m;

    bool match = true;
    for (int i = 0; i < 16; i++) {
        if (c[i] != f[i]) {
            // Treat -0.0 and +0.0 as equal if sign bit is the only difference
            if ((c[i] == 0x80000000 && f[i] == 0x00000000) || (c[i] == 0x00000000 && f[i] == 0x80000000)) {
                continue;
            }
            match = false;
            break;
        }
    }

    if (!match) {
        InterlockedIncrement(&g_mismatches);
        g_dead = true;
        Log("[MatrixAdjugateFast] RETIRED on mismatch at elem: client=0x%08X (%.6g), fast=0x%08X (%.6g)",
            c[0], client_mat.m[0], f[0], fast_mat.m[0]);
        memcpy(out_adjugate, client_mat.m, sizeof(client_mat.m));
        return out_adjugate;
    }

    InterlockedIncrement(&g_verified);
    memcpy(out_adjugate, fast_mat.m, sizeof(fast_mat.m));
    return out_adjugate;
}

__declspec(safebuffers)
static float* __fastcall Hook_sub_4C2440(const float* in_mat, void* dummy_edx, float* out_adjugate)
{
    if (g_dead) {
        return g_orig(in_mat, dummy_edx, out_adjugate);
    }

    LONG n = InterlockedIncrement(&g_calls);
    if (n <= 10000 || ((n <= 100000) && ((n & 1023) == 0)) || ((n & 8191) == 0)) {
        return Verify_CMatrix_Adjugate(in_mat, dummy_edx, out_adjugate);
    }

    return Fast_CMatrix_Adjugate(in_mat, out_adjugate);
}

static bool CheckPrologue(uintptr_t addr, const unsigned char* expected, size_t size)
{
    __try {
        return memcmp((const void*)addr, expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptMatrixAdjugateFast) return true;
    Log("[MatrixAdjugateFast] RETIRED: sub_4C2440 replacement causes view/projection matrix inversion distortion and black screens");
    return false;
}

void Shutdown()
{
    if (g_active) {
        MH_DisableHook((void*)kTarget);
        MH_RemoveHook((void*)kTarget);
        g_active = false;
    }
}

void LogStats()
{
    if (!g_active) return;
    Log("[MatrixAdjugateFast] calls=%ld, verified=%ld, mismatches=%ld, dead=%s",
        g_calls, g_verified, g_mismatches, g_dead ? "TRUE" : "false");
}

} // namespace MatrixAdjugateFast
