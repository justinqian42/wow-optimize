// ============================================================================
// Module: m2_particle_emitter_matrix_fast.cpp
//
// Accelerates M2 particle emitter 4x4 bone and billboard matrix composition
// inside sub_8309C0 (0x00830C3C to 0x00830D11, 213 bytes / 0xD5).
//
// In tester profiling sessions, sub_8309C0 accounts for 13,421 samples (1.6% to
// 2.1% of executing CPU time in raid/combat visual effects), with 8,285 samples
// concentrated right at 0x00830C53 on the matrix calculation pipeline.
//
// What the stock client's 213-byte block does per active particle emitter:
//   1. Executes CRT `rep movsd` to copy a 64-byte 4x4 bone matrix onto the stack.
//   2. Calls sub_4C1B30 to translate the bone matrix by emitter offset (x, y, z).
//   3. Calls sub_4C2370, which allocates a 64-byte temp matrix on stack, calls
//      sub_4C1F00 (199 x87 instructions) to multiply with model world matrix,
//      and copies the result back via sub_407F80.
//   4. Calls sub_4C1F00 again to multiply by the static billboard rotation matrix
//      (0x00D411E0), then copies the result back via sub_407F80 into [ebp-44h].
//
// What this replacement provides:
//   - Inlines bone translation, model world matrix multiplication, and billboard
//     transformation into a single pass, eliminating CRT rep movsd, 4 nested
//     function calls, and multiple temporary matrix copies per emitter.
//   - Collapses the second matrix multiply into direct sign negation and row
//     permutation, since the static billboard matrix is a 90-degree Z-rotation
//     consisting solely of 0, 1, and -1.
//   - Evaluates arithmetic in IEEE double precision using the client's verbatim
//     addition order (from matrix_copy_sse2), matching 53-bit x87 bit for bit.
//   - Disassembly audit guarantees zero /GS security cookies and zero SEH frames.
//
// Verification:
//   - Startup self-test validates 4,096 randomized test vectors against the client's
//     sub_4C1B30 and sub_4C1F00 before patching.
//   - Dual-runs against client functions at runtime for the first 10,000 calls
//     and 1 in every 128 calls thereafter; retires immediately on first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "m2_particle_emitter_matrix_fast.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2ParticleEmitterMatrixFast {

namespace {

constexpr uintptr_t kSiteAddr = 0x00830C3C;
constexpr uintptr_t kRejoinAddr = 0x00830D11;
constexpr size_t kSiteLen = 213; // 0x830D11 - 0x830C3C

const unsigned char kHeadBytes[16] = {
    0x0F, 0xB7, 0x76, 0x14, 0xC1, 0xE6, 0x06, 0x03,
    0xB3, 0x98, 0x00, 0x00, 0x00, 0xB9, 0x10, 0x00
};

const unsigned char kTailBytes[12] = {
    0x83, 0xC4, 0x0C, 0x50, 0x8D, 0x4D, 0xBC, 0xE8,
    0x6F, 0x72, 0xBD, 0xFF
};

static unsigned char g_savedBytes[kSiteLen];
static uintptr_t g_rejoin = kRejoinAddr;

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

static uint64_t g_calls = 0;
static uint64_t g_controlCalls = 0;
static uint64_t g_verifyMatches = 0;
static uint64_t g_verifyCount = 0;

typedef float* (__thiscall *FnTranslate)(float* mat, const float* vec);
typedef float* (__cdecl *FnMatMul)(float* out, const float* a, const float* b);
typedef float* (__thiscall *FnMatCopy)(float* dst, const float* src);

static const FnTranslate s_fnTranslate = (FnTranslate)0x004C1B30;
static const FnMatMul    s_fnMatMul    = (FnMatMul)0x004C1F00;
static const FnMatCopy   s_fnMatCopy   = (FnMatCopy)0x00407F80;

#define MAT_PM(r, k, c) ((double)T[(r) * 4 + (k)] * (double)W[(k) * 4 + (c)])
#define MAT_SCALAR_SUM(r, c, k0, k1, k2, k3) \
    (((MAT_PM(r, k0, c) + MAT_PM(r, k1, c)) + MAT_PM(r, k2, c)) + MAT_PM(r, k3, c))

#define MAT_LANE_SUM(A, B, k0, k1, k2, k3) \
    _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(A[k0], B[k0]), _mm_mul_pd(A[k1], B[k1])), \
                          _mm_mul_pd(A[k2], B[k2])), _mm_mul_pd(A[k3], B[k3]))

static inline __m128d MatWidenTwo(const float* p) {
    return _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)p)));
}

// Bit-exact matrix evaluation matching client's x87 operation order
__declspec(safebuffers) static void FastCompute(
    const void* emitterDef,
    const void* sceneNode,
    float* outMatrix)
{
    const uint8_t* def = (const uint8_t*)emitterDef;
    const uint8_t* node = (const uint8_t*)sceneNode;

    const uint16_t boneIdx = *(const uint16_t*)(def + 0x14);
    const float* boneArray = *(const float**)(node + 0x98);
    const float* B = boneArray + (boneIdx << 4);

    const float* W = (const float*)(*(const uint8_t**)(node + 0x28) + 0xC4);

    const double ex = (double)*(const float*)(def + 8);
    const double ey = (double)*(const float*)(def + 12);
    const double ez = (double)*(const float*)(def + 16);

    // Step 1: sub_4C1B30 translation row
    // Rows 0, 1, 2 unchanged. Row 3 translated:
    // (((ez * B[8+j]) + (ey * B[4+j])) + (ex * B[0+j])) + B[12+j]
    float T[16];
    memcpy(T, B, 12 * sizeof(float));
    for (int j = 0; j < 3; ++j) {
        const double tz = (double)B[8 + j] * ez;
        const double ty = (double)B[4 + j] * ey;
        const double zy = tz + ty;
        const double tx = (double)B[0 + j] * ex;
        const double zyx = zy + tx;
        T[12 + j] = (float)(zyx + (double)B[12 + j]);
    }
    T[15] = B[15];

    // Step 2: sub_4C1F00 matrix multiply T x W in verbatim client addition order
    float M_temp[16];

    // Rows 2 and 3 computed via SIMD packed double
    __m128d A23[4], B23[4][4];
    for (int k = 0; k < 4; ++k) A23[k] = _mm_set_pd((double)T[12 + k], (double)T[8 + k]);
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) B23[c][k] = _mm_set1_pd((double)W[k * 4 + c]);
    __m128d bc0[4], bc1[4], bc2[4], bc3[4];
    for (int k = 0; k < 4; ++k) { bc0[k] = B23[0][k]; bc1[k] = B23[1][k]; bc2[k] = B23[2][k]; bc3[k] = B23[3][k]; }
    const __m128d s20 = MAT_LANE_SUM(A23, bc0, 1, 2, 3, 0);
    const __m128d s21 = MAT_LANE_SUM(A23, bc1, 1, 3, 2, 0);
    const __m128d s22 = MAT_LANE_SUM(A23, bc2, 3, 2, 1, 0);
    const __m128d s23 = MAT_LANE_SUM(A23, bc3, 1, 3, 2, 0);
    M_temp[8]  = (float)_mm_cvtsd_f64(s20); M_temp[12] = (float)_mm_cvtsd_f64(_mm_unpackhi_pd(s20, s20));
    M_temp[9]  = (float)_mm_cvtsd_f64(s21); M_temp[13] = (float)_mm_cvtsd_f64(_mm_unpackhi_pd(s21, s21));
    M_temp[10] = (float)_mm_cvtsd_f64(s22); M_temp[14] = (float)_mm_cvtsd_f64(_mm_unpackhi_pd(s22, s22));
    M_temp[11] = (float)_mm_cvtsd_f64(s23); M_temp[15] = (float)_mm_cvtsd_f64(_mm_unpackhi_pd(s23, s23));

    // Row 1, columns 0 and 1 together
    __m128d A1[4], B01[4];
    for (int k = 0; k < 4; ++k) { A1[k] = _mm_set1_pd((double)T[4 + k]); B01[k] = MatWidenTwo(W + k * 4); }
    const __m128d s1 = MAT_LANE_SUM(A1, B01, 1, 2, 3, 0);
    M_temp[4] = (float)_mm_cvtsd_f64(s1);
    M_temp[5] = (float)_mm_cvtsd_f64(_mm_unpackhi_pd(s1, s1));

    // Row 0 and remaining row 1 elements in client addition order
    M_temp[6] = (float)MAT_SCALAR_SUM(1, 2, 3, 2, 1, 0);
    M_temp[7] = (float)MAT_SCALAR_SUM(1, 3, 1, 3, 2, 0);
    M_temp[0] = (float)MAT_SCALAR_SUM(0, 0, 2, 1, 3, 0);
    M_temp[1] = (float)MAT_SCALAR_SUM(0, 1, 2, 1, 0, 3);
    M_temp[2] = (float)MAT_SCALAR_SUM(0, 2, 1, 3, 0, 2);
    M_temp[3] = (float)MAT_SCALAR_SUM(0, 3, 2, 0, 1, 3);

    // Step 3: Billboard matrix multiplication (flt_D411E0 x M_temp)
    // Row 0 = Row 1 of M_temp
    // Row 1 = -Row 0 of M_temp
    // Row 2 = Row 2 of M_temp
    // Row 3 = Row 3 of M_temp
    outMatrix[0] = M_temp[4];
    outMatrix[1] = M_temp[5];
    outMatrix[2] = M_temp[6];
    outMatrix[3] = M_temp[7];

    outMatrix[4] = -M_temp[0];
    outMatrix[5] = -M_temp[1];
    outMatrix[6] = -M_temp[2];
    outMatrix[7] = -M_temp[3];

    outMatrix[8]  = M_temp[8];
    outMatrix[9]  = M_temp[9];
    outMatrix[10] = M_temp[10];
    outMatrix[11] = M_temp[11];

    outMatrix[12] = M_temp[12];
    outMatrix[13] = M_temp[13];
    outMatrix[14] = M_temp[14];
    outMatrix[15] = M_temp[15];
}

// Client verbatim reference
__declspec(safebuffers) static void ClientReferenceCompute(
    const void* emitterDef,
    const void* sceneNode,
    float* outMatrix)
{
    const uint8_t* def = (const uint8_t*)emitterDef;
    const uint8_t* node = (const uint8_t*)sceneNode;

    const uint16_t boneIdx = *(const uint16_t*)(def + 0x14);
    const float* boneArray = *(const float**)(node + 0x98);
    const float* boneMat = boneArray + (boneIdx << 4);

    const float* worldMat = (const float*)(*(const uint8_t**)(node + 0x28) + 0xC4);
    const float* transVec = (const float*)(def + 8);

    alignas(16) float v28[16];
    memcpy(v28, boneMat, 64);
    s_fnTranslate(v28, transVec);

    alignas(16) float temp1[16];
    s_fnMatMul(temp1, v28, worldMat);
    s_fnMatCopy(v28, temp1);

    static const float billboardMat[16] = {
         0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 1.0f
    };

    alignas(16) float temp2[16];
    s_fnMatMul(temp2, (const float*)billboardMat, v28);
    s_fnMatCopy(outMatrix, temp2);
}

static void UnpatchSite() {
    DWORD oldProtect = 0;
    if (VirtualProtect((void*)kSiteAddr, kSiteLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)kSiteAddr, g_savedBytes, kSiteLen);
        VirtualProtect((void*)kSiteAddr, kSiteLen, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSiteAddr, kSiteLen);
    }
}

extern "C" __declspec(noinline) __declspec(safebuffers) void __cdecl ProcessEmitterMatrix(
    const void* emitterDef,
    const void* sceneNode,
    float* outMatrix)
{
    if (g_dead) {
        ClientReferenceCompute(emitterDef, sceneNode, outMatrix);
        return;
    }

    ++g_calls;

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        ++g_controlCalls;
        ClientReferenceCompute(emitterDef, sceneNode, outMatrix);
        return;
    }

    FastCompute(emitterDef, sceneNode, outMatrix);

    if (g_verifyCount < 10000 || (g_verifyCount & 127) == 0) {
        alignas(16) float clientOut[16];
        ClientReferenceCompute(emitterDef, sceneNode, clientOut);

        if (memcmp(outMatrix, clientOut, sizeof(clientOut)) != 0) {
            UnpatchSite();
            g_dead = true;
            Log("[M2ParticleEmitterMatrixFast] Mismatch detected! Retired hook.");
            memcpy(outMatrix, clientOut, sizeof(clientOut));
            return;
        }
        ++g_verifyMatches;
    }
    ++g_verifyCount;
}

__declspec(naked) void Thunk() {
    __asm {
        pushad
        lea  eax, [ebp-44h]
        push eax
        push ebx
        push esi
        call ProcessEmitterMatrix
        add  esp, 12
        popad
        jmp  dword ptr [g_rejoin]
    }
}

static bool SelfTest() {
    alignas(16) float boneArray[64];
    alignas(16) float worldMat[16];
    uint8_t defBuf[64];
    uint8_t nodeBuf[256];

    for (int i = 0; i < 64; ++i) boneArray[i] = (float)((i * 37 + 11) % 100) * 0.1f - 5.0f;
    for (int i = 0; i < 16; ++i) worldMat[i] = (float)((i * 53 + 7) % 100) * 0.1f - 5.0f;

    *(const float**)(nodeBuf + 0x98) = boneArray;
    uint8_t* worldBlock = nodeBuf + 0x80;
    memcpy(worldBlock + 0xC4, worldMat, sizeof(worldMat));
    *(uint8_t**)(nodeBuf + 0x28) = worldBlock;

    *(uint16_t*)(defBuf + 0x14) = 1;
    *(float*)(defBuf + 8)  = 1.5f;
    *(float*)(defBuf + 12) = -2.5f;
    *(float*)(defBuf + 16) = 0.75f;

    alignas(16) float fastOut[16];
    alignas(16) float clientOut[16];

    FastCompute(defBuf, nodeBuf, fastOut);
    ClientReferenceCompute(defBuf, nodeBuf, clientOut);

    if (memcmp(fastOut, clientOut, sizeof(fastOut)) != 0) {
        return false;
    }

    // Run 4096 randomized test vectors
    uint32_t seed = 0x12345678;
    auto lcg = [&seed]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return ((float)(int32_t)(seed & 0xFFFF) - 32768.0f) / 100.0f;
    };

    for (int trial = 0; trial < 4096; ++trial) {
        for (int i = 0; i < 64; ++i) boneArray[i] = lcg();
        for (int i = 0; i < 16; ++i) worldMat[i] = lcg();
        *(uint16_t*)(defBuf + 0x14) = (uint16_t)(trial & 3);
        *(float*)(defBuf + 8)  = lcg();
        *(float*)(defBuf + 12) = lcg();
        *(float*)(defBuf + 16) = lcg();

        FastCompute(defBuf, nodeBuf, fastOut);
        ClientReferenceCompute(defBuf, nodeBuf, clientOut);

        if (memcmp(fastOut, clientOut, sizeof(fastOut)) != 0) {
            return false;
        }
    }

    return true;
}

} // anonymous namespace

bool Install() {
    if (g_installed) return true;

    if (!WowOpt_ClientPatchAllowed((const void*)kSiteAddr)) {
        Log("[M2ParticleEmitterMatrixFast] NOT patched: No Client Patches active.");
        return false;
    }

    if (memcmp((const void*)kSiteAddr, kHeadBytes, sizeof(kHeadBytes)) != 0) {
        Log("[M2ParticleEmitterMatrixFast] NOT patched: Head signature mismatch at 0x%08X.", kSiteAddr);
        return false;
    }

    if (memcmp((const void*)(kRejoinAddr - sizeof(kTailBytes)), kTailBytes, sizeof(kTailBytes)) != 0) {
        Log("[M2ParticleEmitterMatrixFast] NOT patched: Tail signature mismatch.");
        return false;
    }

    if (!SelfTest()) {
        Log("[M2ParticleEmitterMatrixFast] NOT patched: Self-test bit parity failed.");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)kSiteAddr, kSiteLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("[M2ParticleEmitterMatrixFast] NOT patched: VirtualProtect failed.");
        return false;
    }

    memcpy(g_savedBytes, (const void*)kSiteAddr, kSiteLen);

    unsigned char patch[kSiteLen];
    patch[0] = 0xE9; // JMP rel32
    int32_t rel = (int32_t)((uintptr_t)Thunk - (kSiteAddr + 5));
    memcpy(patch + 1, &rel, sizeof(rel));
    memset(patch + 5, 0x90, kSiteLen - 5); // NOP remainder

    memcpy((void*)kSiteAddr, patch, kSiteLen);

    VirtualProtect((void*)kSiteAddr, kSiteLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)kSiteAddr, kSiteLen);

    // Ensure billboard matrix initialization flag is set
    *(uint32_t*)0x00D41220 |= 1;

    g_installed = true;
    AbTest::IsSubject("M2ParticleEmitterMatrixFast", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("ProcessEmitterMatrix", (const void*)ProcessEmitterMatrix);

    Log("[M2ParticleEmitterMatrixFast] Patch installed at 0x%08X (213 bytes mid-function detour)", kSiteAddr);
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    UnpatchSite();
    g_installed = false;
    Log("[M2ParticleEmitterMatrixFast] Patch uninstalled.");
}

bool Init() {
    if (!Config::g_settings.OptM2ParticleEmitterMatrixFast) {
        return false;
    }
    return Install();
}

void Shutdown() {
    Uninstall();
}

void LogStats() {
    if (!g_installed) return;
    char buf[256];
    _snprintf(buf, sizeof(buf),
              "[M2ParticleEmitterMatrixFast] calls=%llu control=%llu verified=%llu matches=%llu%s",
              g_calls, g_controlCalls, g_verifyCount, g_verifyMatches,
              g_dead ? " [RETIRED]" : "");
    buf[sizeof(buf) - 1] = '\0';
    Log("%s", buf);
}

} // namespace M2ParticleEmitterMatrixFast
