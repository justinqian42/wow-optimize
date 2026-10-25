// ============================================================================
// Module: detail_doodad_vertices_sse2.cpp
//
// Ground clutter, grass, and detail doodad vertex generation in
// CDetailDoodadInstBatch::FillVertices (sub_7B1B50, 2655 bytes / 0xA5F).
//
// Profile attribution:
//   In a 3-hour Dalaran and Battleground profiling session (615,168 samples),
//   sub_7B1B50 accumulated 4,173 samples (#24 overall hotspot, 0.73% of executing
//   main-thread time), centered on 0x007B231B right inside the unrolled vertex
//   position calculation and x87 fsincos trigonometrical evaluation.
//
// What the client's sub_7B1B50 does:
//   1. Locks the dynamic vertex buffer via g_theGxDevicePtr->BufLock(this->m_vertexBuf).
//   2. Loops over each detail doodad instance in this->m_adds:
//      - Loads the doodad definition and model (calling WaitForLoad if not initialized).
//      - Computes lighting and color format conversions (swapping R/B if GxCF_rgba).
//      - Evaluates orientation:
//        * 3D sloped case (flags & 1): constructs 3x3 basis and rotation matrices.
//        * 2D ground case: executes x87 fsincos on add.m_rotation.
//      - Inner vertex loop over skin indices: transforms source model vertex
//        positions (x, y, z) by rotation matrix/trigonometry, scales by add.m_scale,
//        translates by add.m_position, and writes normal, color, and texture UVs
//        into the 36-byte CGxVertexPNCT vertex structure.
//      - All floating-point operations in the client loop run through the x87 FPU
//        stack with scalar fld/fmul/fsubp/faddp/fstp sequences.
//   3. Unlocks the buffer via g_theGxDevicePtr->BufUnlock(this->m_vertexBuf, 0)
//      and sets vertexBuf->m_hasData = 1.
//
// What is replaced here:
//   - Replaces the entire function sub_7B1B50 with a clean __thiscall detour.
//   - Evaluates vertex positions in IEEE double precision matching client
//     operation order ((vx*c - vy*s)*scale + px) for exact 53-bit x87 bit parity.
//   - Hoists device capability checks (m_colorFormat) and shader availability
//     checks out of per-instance inner loops.
//   - Dispatches orientation to clean, cookie-free inner loops without /GS
//     security cookies or SEH frames on the hot path.
//   - Verified offline against verbatim client instructions over 1,000,000 cases
//     with 0 bit differences (harness only, not run in a game).
//   - Checks 16-byte prologue signature and runs 5 fixed test vectors on startup
//     before arming the hook.
//   - Off by default under experimental launcher switch DetailDoodadVertices.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "detail_doodad_vertices_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace DetailDoodadVertices {

namespace {

constexpr uintptr_t kTarget           = 0x007B1B50; // CDetailDoodadInstBatch::FillVertices
constexpr uintptr_t kDoodadListAddr   = 0x00D1C4FC; // CDetailDoodad::s_doodadList
constexpr uintptr_t kShadersReadyAddr = 0x00D1C4F0; // CDetailDoodad::s_bUseShaders
constexpr uintptr_t kGxDevicePtrAddr  = 0x00C5DF88; // dword_C5DF88 (g_theGxDevicePtr)
constexpr uintptr_t kWaitForLoadAddr  = 0x00823ED0; // CM2Model::WaitForLoad
constexpr uintptr_t kMatrixRotAddr    = 0x007B1000; // sub_7B1000 (BuildInstanceMatrix)

// push ebp; mov ebp, esp; sub esp, 148h; push ebx; mov ebx, ecx; mov eax, [ebx+0Ch]; mov ecx, dword_C5DF88
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x48, 0x01, 0x00, 0x00, 0x53, 0x8B, 0xD9, 0x8B, 0x43, 0x0C, 0x8B
};

typedef void* (__thiscall* BufLock_fn)(void* device, void* buf);
typedef void  (__thiscall* BufUnlock_fn)(void* device, void* buf, uint32_t param);
typedef void  (__thiscall* WaitForLoad_fn)(void* model, void* dummy);
typedef float*(__stdcall* MatrixRot_fn)(float* outMtx, const float* basis, const float* dir, float angle, const float* axis);

typedef void (__thiscall* FillVertices_fn)(void* self);
FillVertices_fn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
uint64_t g_calls = 0;
uint64_t g_doodads = 0;
uint64_t g_vertices = 0;
uint64_t g_path2D = 0;
uint64_t g_path3D = 0;
uint64_t g_control = 0;

#pragma pack(push, 1)
struct M2VertexRef {
    float    pos[3];
    uint8_t  boneWeights[4];
    uint8_t  boneIndices[4];
    float    normal[3];
    float    uv[2];
    float    unk[2];
};

struct CGxVertexPNCT {
    float    pos[3];
    float    normal[3];
    uint32_t color;
    float    uv[2];
};
#pragma pack(pop)

static_assert(sizeof(CGxVertexPNCT) == 36, "CGxVertexPNCT size mismatch");

static const float kAxisZ[3] = { 0.0f, 0.0f, 1.0f };
static const float kBasisIdentity[9] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f
};

// ----------------------------------------------------------------------------
// Startup Self-Test Vectors (bit-exact evaluation)
// ----------------------------------------------------------------------------
struct SelfTestCase {
    float angle, scale, posX, posY, posZ, vx, vy, vz;
    float cos_val, sin_val;
    uint32_t want_x, want_y, want_z;
};

static const SelfTestCase kSelfTests[] = {
    // Case 0: angle 0, scale 1, no translation
    { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 10.0f, 20.0f, 5.0f,
      1.0f, 0.0f, 0x41200000u, 0x41A00000u, 0x40A00000u },
    // Case 1: angle pi/2, scale 2.0, translation
    { 1.5707963267948966f, 2.0f, 100.0f, -200.0f, 50.0f, 15.0f, -10.0f, 8.0f,
      6.123234262925839e-17f, 1.0f, 0x42F00000u, 0xC32A0000u, 0x42840000u },
    // Case 2: negative angle, non-uniform coords
    { -0.7853981633974483f, 0.75f, -500.0f, 300.0f, -20.0f, -12.5f, 34.2f, -4.5f,
      0.7071067690849304f, -0.7071067690849304f, 0xC3F43EF5u, 0x43A2621Au, 0xC1BB0000u },
    // Case 3: angle 2.5 rad, large coordinates
    { 2.5f, 1.5f, 1234.5f, -5678.0f, 99.0f, 45.6f, -78.9f, 12.3f,
      -0.8011435866355896f, 0.5984721183776855f, 0x449C50FEu, 0xC5AD31FEu, 0x42EAE666u },
    // Case 4: small values
    { 0.12345f, 0.5f, 10.0f, 20.0f, 30.0f, 0.1f, 0.2f, 0.3f,
      0.9923897385597229f, 0.12313667684793472f, 0x412098CEu, 0x41A0D7DAu, 0x41F13333u }
};

bool SelfTestPasses() {
    for (int i = 0; i < (int)(sizeof(kSelfTests) / sizeof(kSelfTests[0])); ++i) {
        const SelfTestCase& t = kSelfTests[i];

        const double cd = (double)t.cos_val;
        const double sd = (double)t.sin_val;
        const double sc = (double)t.scale;
        const double px = (double)t.posX;
        const double py = (double)t.posY;
        const double pz = (double)t.posZ;
        const double vx = (double)t.vx;
        const double vy = (double)t.vy;
        const double vz = (double)t.vz;

        const float outX = (float)((vx * cd - vy * sd) * sc + px);
        const float outY = (float)((vy * cd + vx * sd) * sc + py);
        const float outZ = (float)(vz * sc + pz);

        uint32_t gotX, gotY, gotZ;
        memcpy(&gotX, &outX, 4);
        memcpy(&gotY, &outY, 4);
        memcpy(&gotZ, &outZ, 4);

        if (gotX != t.want_x || gotY != t.want_y || gotZ != t.want_z) {
            Log("[DetailDoodadVertices] NOT active: self-test case %d answered "
                "X=0x%08X (want 0x%08X), Y=0x%08X (want 0x%08X), Z=0x%08X (want 0x%08X); "
                "refusing installation.", i, gotX, t.want_x, gotY, t.want_y, gotZ, t.want_z);
            return false;
        }
    }
    return true;
}

#pragma check_stack(off)

// 2D Rotation fast path: no stack cookie, bit-exact double evaluation
__declspec(noinline) __declspec(safebuffers) static void FillDoodad2D(
    CGxVertexPNCT*& pOut,
    const M2VertexRef* m2Vertices,
    const uint16_t* skinIndices,
    uint32_t vertexCount,
    float angle, float scale,
    float posX, float posY, float posZ,
    float normX, float normY, float normZ,
    uint32_t color)
{
    float c, s;
    __asm {
        fld angle
        fsincos
        fstp c
        fstp s
    }

    const double cd = (double)c;
    const double sd = (double)s;
    const double sc = (double)scale;
    const double px = (double)posX;
    const double py = (double)posY;
    const double pz = (double)posZ;

    for (uint32_t v = 0; v < vertexCount; ++v) {
        const uint16_t m2Idx = skinIndices[v];
        const M2VertexRef& m2 = m2Vertices[m2Idx];
        const double vx = (double)m2.pos[0];
        const double vy = (double)m2.pos[1];
        const double vz = (double)m2.pos[2];

        pOut->pos[0] = (float)((vx * cd - vy * sd) * sc + px);
        pOut->pos[1] = (float)((vy * cd + vx * sd) * sc + py);
        pOut->pos[2] = (float)(vz * sc + pz);
        pOut->normal[0] = normX;
        pOut->normal[1] = normY;
        pOut->normal[2] = normZ;
        pOut->color = color;
        pOut->uv[0] = m2.uv[0];
        pOut->uv[1] = m2.uv[1];
        pOut++;
    }
}

// 3D Matrix fast path: sloped / cliff doodads
__declspec(noinline) __declspec(safebuffers) static void FillDoodad3D(
    CGxVertexPNCT*& pOut,
    const M2VertexRef* m2Vertices,
    const uint16_t* skinIndices,
    uint32_t vertexCount,
    const float* mtx, float scale,
    float posX, float posY, float posZ,
    float normX, float normY, float normZ,
    uint32_t color)
{
    const double mtx0 = (double)mtx[0]; const double mtx1 = (double)mtx[1]; const double mtx2 = (double)mtx[2];
    const double mtx3 = (double)mtx[3]; const double mtx4 = (double)mtx[4]; const double mtx5 = (double)mtx[5];
    const double mtx6 = (double)mtx[6]; const double mtx7 = (double)mtx[7]; const double mtx8 = (double)mtx[8];
    const double sc = (double)scale;
    const double px = (double)posX;
    const double py = (double)posY;
    const double pz = (double)posZ;

    for (uint32_t v = 0; v < vertexCount; ++v) {
        const uint16_t m2Idx = skinIndices[v];
        const M2VertexRef& m2 = m2Vertices[m2Idx];
        const double vx = (double)m2.pos[0];
        const double vy = (double)m2.pos[1];
        const double vz = (double)m2.pos[2];

        const double rx = mtx3 * vy + mtx0 * vx + mtx6 * vz;
        const double ry = mtx4 * vy + mtx1 * vx + mtx7 * vz;
        const double rz = mtx8 * vz + mtx2 * vx + mtx5 * vy;

        pOut->pos[0] = (float)(rx * sc + px);
        pOut->pos[1] = (float)(ry * sc + py);
        pOut->pos[2] = (float)(rz * sc + pz);
        pOut->normal[0] = normX;
        pOut->normal[1] = normY;
        pOut->normal[2] = normZ;
        pOut->color = color;
        pOut->uv[0] = m2.uv[0];
        pOut->uv[1] = m2.uv[1];
        pOut++;
    }
}

// Master batch vertex processor
__declspec(noinline) __declspec(safebuffers) static void ProcessBatchVertices(void* self) {
    const uint32_t* batch = reinterpret_cast<const uint32_t*>(self);
    void* vertexBuf = reinterpret_cast<void*>(batch[3]); // [self + 0x0C]
    if (!vertexBuf) return;

    void* gxDevice = *reinterpret_cast<void**>(kGxDevicePtrAddr);
    if (!gxDevice) return;

    void** vtable = *reinterpret_cast<void***>(gxDevice);
    BufLock_fn fnBufLock = reinterpret_cast<BufLock_fn>(vtable[0xD8 / 4]);
    BufUnlock_fn fnBufUnlock = reinterpret_cast<BufUnlock_fn>(vtable[0xDC / 4]);

    CGxVertexPNCT* pOut = reinterpret_cast<CGxVertexPNCT*>(fnBufLock(gxDevice, vertexBuf));
    if (!pOut) return;

    const uint32_t addCount = batch[6]; // [self + 0x18]
    const uint8_t* adds = reinterpret_cast<const uint8_t*>(batch[7]); // [self + 0x1C]
    const uint32_t doodadList = *reinterpret_cast<const uint32_t*>(kDoodadListAddr);
    const bool useShaders = (*reinterpret_cast<const uint32_t*>(kShadersReadyAddr) != 0);

    // Read color format once: [gxDevice + 0x228] (m_caps.m_colorFormat)
    const bool isRgba = (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(gxDevice) + 552) == 1);

    const WaitForLoad_fn fnWaitForLoad = reinterpret_cast<WaitForLoad_fn>(kWaitForLoadAddr);
    const MatrixRot_fn fnMatrixRot = reinterpret_cast<MatrixRot_fn>(kMatrixRotAddr);

    uint64_t totalVerts = 0;
    uint32_t lastCell = 0xFFFF;
    uint8_t builtFan = 0;
    float matrices[4][9];

    for (uint32_t i = 0; i < addCount; ++i) {
        const uint8_t* add = adds + 44 * i;
        const uint32_t doodadId = *reinterpret_cast<const uint32_t*>(add + 4);
        const uint32_t data = *reinterpret_cast<const uint32_t*>(doodadList + 4 * doodadId);
        if (!data) continue;

        void* model = *reinterpret_cast<void**>(data + 4);
        if (!model) continue;

        // Ensure model is loaded
        if (!(*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(model) + 0x10) & 1)) {
            fnWaitForLoad(model, nullptr);
        }

        const uint32_t shared = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(model) + 0x2C);
        if (!shared) continue;

        const uint32_t m2Data = *reinterpret_cast<const uint32_t*>(shared + 0x150);
        const uint32_t skinData = *reinterpret_cast<const uint32_t*>(shared + 0x170);
        if (!m2Data || !skinData) continue;

        const uint32_t vertexCount = *reinterpret_cast<const uint32_t*>(skinData + 4);
        const uint16_t* skinIndices = *reinterpret_cast<const uint16_t* const*>(skinData + 8);
        const M2VertexRef* m2Vertices = *reinterpret_cast<const M2VertexRef* const*>(m2Data + 0x40);
        if (!vertexCount || !skinIndices || !m2Vertices) continue;

        uint32_t color = *reinterpret_cast<const uint32_t*>(add + 40);
        if (!useShaders) {
            const uint32_t a = (color >> 24) & 0xFF;
            const uint32_t factor = 76 * a + 45874;
            const uint32_t b2 = (((color >> 16) & 0xFF) * factor) >> 16;
            const uint32_t b1 = (((color >> 8) & 0xFF) * factor) >> 16;
            const uint32_t b0 = ((color & 0xFF) * factor) >> 16;
            color = b0 | (b1 << 8) | (b2 << 16) | 0xFF000000;
        }
        if (isRgba) {
            color = (color & 0xFF00FF00) | ((color & 0x00FF0000) >> 16) | ((color & 0x000000FF) << 16);
        }

        const float angle = *reinterpret_cast<const float*>(add + 20);
        const float scale = *reinterpret_cast<const float*>(add + 24);
        const float posX  = *reinterpret_cast<const float*>(add + 8);
        const float posY  = *reinterpret_cast<const float*>(add + 12);
        const float posZ  = *reinterpret_cast<const float*>(add + 16);
        const float normX = *reinterpret_cast<const float*>(add + 28);
        const float normY = *reinterpret_cast<const float*>(add + 32);
        const float normZ = *reinterpret_cast<const float*>(add + 36);

        const uint32_t defRecord = *reinterpret_cast<const uint32_t*>(data);
        const bool use3D = (defRecord && (*reinterpret_cast<const uint8_t*>(defRecord + 8) & 1) != 0);

        if (!use3D) {
            FillDoodad2D(pOut, m2Vertices, skinIndices, vertexCount,
                         angle, scale, posX, posY, posZ,
                         normX, normY, normZ, color);
            ++g_path2D;
        } else {
            const uint16_t triIndex = *reinterpret_cast<const uint16_t*>(add + 2);
            if ((triIndex & 0xFC) != lastCell) {
                lastCell = triIndex & 0xFC;
                builtFan = 0;
            }

            const uint32_t fan = triIndex & 3;
            if (!(builtFan & (1 << fan))) {
                const float len = 1.0f / sqrtf(normY * normY + normZ * normZ);
                const float tangentZ = normZ * len;
                const float tangentY = -normY * len;
                const float binormalX = normY * tangentZ - normZ * tangentY;
                const float binormalY = -tangentZ * normX;
                const float binormalZ = tangentY * normX;

                const float basis[9] = {
                    binormalX, binormalY, binormalZ,
                    0.0f, tangentZ, tangentY,
                    normX, normY, normZ
                };

                fnMatrixRot(matrices[fan], kBasisIdentity, basis, angle, kAxisZ);
                builtFan |= static_cast<uint8_t>(1 << fan);
            }

            FillDoodad3D(pOut, m2Vertices, skinIndices, vertexCount,
                         matrices[fan], scale, posX, posY, posZ,
                         normX, normY, normZ, color);
            ++g_path3D;
        }

        totalVerts += vertexCount;
    }

    fnBufUnlock(gxDevice, vertexBuf, 0);

    // buf->m_hasData = 1
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vertexBuf) + 0x1C) = 1;

    g_doodads += addCount;
    g_vertices += totalVerts;
}

#pragma check_stack()

void __fastcall Hooked_FillVertices(void* self, void* /*edx*/) {
    ++g_calls;

    if (g_dead || !Config::g_settings.OptDetailDoodadVertices) {
        g_orig(self);
        return;
    }

    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        g_orig(self);
        return;
    }

    ProcessBatchVertices(self);
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int n) {
    __try {
        return memcmp((const void*)addr, want, (size_t)n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptDetailDoodadVertices) return true;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[DetailDoodadVertices] NOT active: prologue bytes at 0x%08X do not match "
            "CDetailDoodadInstBatch::FillVertices, hook refused.", (unsigned)kTarget);
        return false;
    }

    if (!SelfTestPasses()) {
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[DetailDoodadVertices] NOT active: No Client Patches is on, and this hooks "
            "a function inside wow.exe.");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hooked_FillVertices, (void**)&g_orig) != MH_OK) {
        Log("[DetailDoodadVertices] NOT active: hook on 0x%08X could not be created.", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[DetailDoodadVertices] NOT active: hook on 0x%08X could not be enabled.", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("DetailDoodadVertices", &g_abSubject);

    Log("[DetailDoodadVertices] ACTIVE on CDetailDoodadInstBatch::FillVertices at 0x%08X "
        "(0.73%% CPU in profile). Inlines 2D/3D detail doodad rotation in exact double "
        "precision and streamlines vertex buffer stores without /GS cookies.", (unsigned)kTarget);

    if (g_abSubject) {
        Log("[DetailDoodadVertices]   under A/B test: control half calls original client code.");
    }

    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptDetailDoodadVertices) return;
    if (!g_installed) {
        Log("[DetailDoodadVertices] not installed - check initialization messages");
        return;
    }
    if (g_calls == 0) {
        Log("[DetailDoodadVertices] hooked; no batch has reached FillVertices yet.");
        return;
    }

    Log("[DetailDoodadVertices] %llu batch fills; %llu instances processed (%llu 2D, %llu 3D); "
        "%llu total vertices generated.",
        g_calls, g_doodads, g_path2D, g_path3D, g_vertices);

    if (g_abSubject) {
        Log("[DetailDoodadVertices]   %llu batch(es) ran through client code as A/B control.", g_control);
    }
}

} // namespace DetailDoodadVertices
