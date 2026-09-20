// ============================================================================
// Module: collision_ray_verts_sse2
//
// sub_7C6D50 (Collision_ClipVertsToBox) is the pick-ray and line-of-sight sibling
// of sub_7C7230. Called once per pick-ray or line-of-sight test from sub_7C9A00,
// it classifies every vertex of a collision model against the ray's bounding box
// into a 6-bit outcode, then loops through triangles rejecting any whose three
// vertices share an outside bit.
//
// The vertex loop has 158 serialized x87 floating-point instructions that
// compare 4 vertices per unrolled pass against 6 bounding planes, incurring
// repeated fnstsw ax CPU pipeline stalls.
//
// This replaces the classification with packed SSE2 comparisons evaluating 4
// vertices per pass simultaneously, eliminating status-word stalls while
// matching exact IEEE coordinate thresholds bit for bit.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "collision_ray_verts_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace CollisionRayVerts {

namespace {

constexpr uintptr_t kClipVerts  = 0x007C6D50;
constexpr uintptr_t kFindModel  = 0x0079B1F0;
constexpr uintptr_t kRayTri16   = 0x00983490;
constexpr uintptr_t kEnabledFlg = 0x00CDD7A0;

constexpr uintptr_t kHitCount   = 0x00D2DBF8;
constexpr uintptr_t kHitArray   = 0x00D25BF8;
constexpr uintptr_t kNearCount  = 0x00D2DBFC;
constexpr uintptr_t kNearArray  = 0x00D29BF8;
constexpr uint32_t  kRingCap    = 0x2000;

constexpr unsigned kM_vertCount = 6;
constexpr unsigned kM_verts     = 8;
constexpr unsigned kM_triCount  = 6308;
constexpr unsigned kM_triVerts  = 6310;
constexpr unsigned kM_triMask   = 8110;
constexpr unsigned kM_triIndex  = 8710;

constexpr int kMaxVerts = 452;

typedef int (__fastcall* findModel_fn)(void* cache, void* edx,
                                       int a0, int a4, void* p1, void* p2, void* p3);

typedef int (__cdecl* RayTriIntersect16_fn)(const void* ray, const void* verts,
                                            const void* tri, void* outT, void* outUV,
                                            float eps);

typedef char (__thiscall* ClipVerts_fn)(void* this_ptr, int a2, int a3);
ClipVerts_fn g_orig_ClipVerts = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long g_calls = 0;
unsigned long long g_verts = 0, g_tris = 0;

inline uint32_t RD32(uintptr_t a) { return *(const uint32_t*)a; }
inline uint16_t RD16(uintptr_t a) { return *(const uint16_t*)a; }

void ClassifySse2(uint8_t* codes, const float* v, int n, const float* b) {
    const __m128 bx0 = _mm_set1_ps(b[0]), bx1 = _mm_set1_ps(b[3]);
    const __m128 by0 = _mm_set1_ps(b[1]), by1 = _mm_set1_ps(b[4]);
    const __m128 bz0 = _mm_set1_ps(b[2]), bz1 = _mm_set1_ps(b[5]);

    const __m128i m20 = _mm_set1_epi32(0x20), m10 = _mm_set1_epi32(0x10);
    const __m128i m08 = _mm_set1_epi32(0x08), m04 = _mm_set1_epi32(0x04);
    const __m128i m02 = _mm_set1_epi32(0x02), m01 = _mm_set1_epi32(0x01);

    int i = 0;
    for (; i + 4 <= n; i += 4, v += 12) {
        __m128 X = _mm_set_ps(v[9],  v[6], v[3], v[0]);
        __m128 Y = _mm_set_ps(v[10], v[7], v[4], v[1]);
        __m128 Z = _mm_set_ps(v[11], v[8], v[5], v[2]);

        __m128i r =
            _mm_or_si128(
                _mm_or_si128(
                    _mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(bx0, X)), m20),
                                 _mm_and_si128(_mm_castps_si128(_mm_cmple_ps(bx1, X)), m10)),
                    _mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(by0, Y)), m08),
                                 _mm_and_si128(_mm_castps_si128(_mm_cmple_ps(by1, Y)), m04))),
                _mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(bz0, Z)), m02),
                             _mm_and_si128(_mm_castps_si128(_mm_cmple_ps(bz1, Z)), m01)));

        r = _mm_packs_epi32(r, r);
        r = _mm_packus_epi16(r, r);
        *(uint32_t*)(codes + i) = (uint32_t)_mm_cvtsi128_si32(r);
    }

    for (; i < n; ++i, v += 3) {
        uint8_t c = 0;
        if (b[0] >= v[0]) c |= 0x20;
        if (b[3] <= v[0]) c |= 0x10;
        if (b[1] >= v[1]) c |= 0x08;
        if (b[4] <= v[1]) c |= 0x04;
        if (b[2] >= v[2]) c |= 0x02;
        if (b[5] <= v[2]) c |= 0x01;
        codes[i] = c;
    }
}

char __fastcall Hook_Collision_ClipVertsToBox(void* thisPtr, void* /*edx*/, int a0, int a4) {
    ++g_calls;
    if (g_dead || (g_abSubject && AbTest::StandAside())) {
        return g_orig_ClipVerts(thisPtr, a0, a4);
    }

    void* cache = *(void**)kEnabledFlg;
    if (!cache) return 0;

    float* thisF = (float*)thisPtr;
    findModel_fn findModel = (findModel_fn)kFindModel;
    int model = findModel(cache, nullptr, a0, a4,
                          *(void**)(thisF + 1),
                          *(void**)(thisF + 2),
                          *(void**)(thisF + 3));
    if (!model) return 0;

    int n = (int)RD16((uintptr_t)model + kM_vertCount);
    if (n <= 0 || n > kMaxVerts) {
        return g_orig_ClipVerts(thisPtr, a0, a4);
    }

    double v6 = thisF[9];
    double v7 = thisF[10];
    double v8 = thisF[6];
    double v9 = thisF[11];
    if (v8 > v6) { double t = v8; v8 = v6; v6 = t; }
    double v11 = thisF[7];
    if (v11 > v7) { double t = v11; v11 = v7; v7 = t; }
    double v14 = thisF[8];
    if (v14 > v9) { double t = v14; v14 = v9; v9 = t; }

    float bounds[6];
    bounds[0] = (float)(v8 - 0.0099999998);
    bounds[1] = (float)(v11 - 0.0099999998);
    bounds[2] = (float)(v14 - 0.0099999998);
    bounds[3] = (float)(v6 + 0.0099999998);
    bounds[4] = (float)(v7 + 0.0099999998);
    bounds[5] = (float)(v9 + 0.0099999998);

    // Static buffer to eliminate /GS stack cookie
    static uint8_t codes[kMaxVerts];
    ClassifySse2(codes, (const float*)((uintptr_t)model + kM_verts), n, bounds);

    g_verts += (unsigned long long)n;

    uint32_t triCount = RD16((uintptr_t)model + kM_triCount);
    uint16_t mask     = *(const uint16_t*)((uintptr_t)thisPtr + 0x58);
    uint8_t* flags    = *(uint8_t**)((uintptr_t)thisPtr + 0x04);
    if (!flags) return g_orig_ClipVerts(thisPtr, a0, a4);

    uintptr_t T_50 = *(uintptr_t*)((uintptr_t)thisPtr + 0x50);
    RayTriIntersect16_fn rayTri = (RayTriIntersect16_fn)kRayTri16;

    for (uint32_t i = 0; i < triCount; ++i) {
        uint16_t triMask = RD16((uintptr_t)model + kM_triMask + 2u * i);
        if ((mask & triMask) != 0) continue;

        uint16_t idx = RD16((uintptr_t)model + kM_triIndex + 2u * i);
        if (((uint8_t)mask & flags[2u * idx]) != 0) continue;

        uint8_t f1 = flags[2u * idx + 1];
        bool flagCheck;
        if (f1 == 0xFF || (T_50 != 0 && *(const uint32_t*)(T_50 + ((uint32_t)f1 << 6) + 8) == 0)) {
            flagCheck = (mask & 0x200) == 0;
        } else {
            flagCheck = (mask & 0x100) == 0;
        }
        if (!flagCheck) continue;

        uint32_t hitCount = RD32(kHitCount);
        if (hitCount >= kRingCap) {
            void* ov = *(void**)thisPtr;
            if (ov) *(uint32_t*)ov |= 1u;
            break;
        }

        ((uint16_t*)kHitArray)[hitCount] = idx;
        *(uint32_t*)kHitCount = hitCount + 1;
        flags[2u * idx] |= 0x80u;

        uintptr_t tri = (uintptr_t)model + 6u * i;
        uint8_t ca = codes[RD16(tri + kM_triVerts)];
        uint8_t cb = codes[RD16(tri + kM_triVerts + 2)];
        uint8_t cc = codes[RD16(tri + kM_triVerts + 4)];
        if (((ca & cb & cc) & 0x3F) == 0) {
            float t = 0.0f;
            if (rayTri((const void*)(thisF + 12),
                       (const void*)((uintptr_t)model + 8),
                       (const void*)(tri + kM_triVerts),
                       &t, nullptr, 0.0020000001f)) {
                if (t >= 0.0f && (double)t <= (double)thisF[19]) {
                    thisF[19] = t;
                    *(uint16_t*)kNearArray = idx;
                    *(uint32_t*)kNearCount = 1;
                    float** ppRes = (float**)((uintptr_t)thisPtr + 0x10);
                    if (ppRes && *ppRes) {
                        float r = t * thisF[18];
                        if (r > thisF[5]) r = thisF[5];
                        **ppRes = r;
                    }
                }
            }
        }
    }

    g_tris += triCount;
    return 1;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionRayVerts) return true;

    if (!WowOpt_ClientPatchAllowed((const void*)kClipVerts)) {
        Log("CollisionRayVerts: client patches disallowed by policy - not hooking");
        return false;
    }

    static const unsigned char kExp_ClipVerts[8] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xC8, 0x01, 0x00 };
    if (IsBadReadPtr((void*)kClipVerts, 8) || memcmp((const void*)kClipVerts, kExp_ClipVerts, 8) != 0) {
        Log("CollisionRayVerts: 0x%08X bad prologue or unreadable - not installing", (unsigned)kClipVerts);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kClipVerts,
                                       (void*)&Hook_Collision_ClipVertsToBox,
                                       (void**)&g_orig_ClipVerts);
    if (st != MH_OK) {
        Log("CollisionRayVerts: failed to create hook on sub_7C6D50: %d", st);
        return false;
    }

    if (WO_EnableHook((void*)kClipVerts) != MH_OK) {
        Log("CollisionRayVerts: failed to enable hook on sub_7C6D50");
        MH_DisableHook((void*)kClipVerts);
        return false;
    }

    g_abSubject = AbTest::IsSubject("CollisionRayVerts", &g_abSubject);
    g_installed = true;

    Log("CollisionRayVerts: ACTIVE on sub_7C6D50 (Ray/Pick Box Outcode Reject).");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kClipVerts);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptCollisionRayVerts) {
        Log("CollisionRayVerts: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("CollisionRayVerts: not installed.");
        return;
    }
    Log("CollisionRayVerts: %lu calls, %llu vertices classified, %llu triangles evaluated.",
        g_calls, g_verts, g_tris);
}

} // namespace CollisionRayVerts
