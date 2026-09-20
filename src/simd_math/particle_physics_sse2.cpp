// ============================================================================
// Module: particle_physics_sse2
//
// sub_97EB10 is the M2 particle emitter physics and coordinate integration
// update routine (730 bytes) invoked for every active particle system during
// spell and world rendering (sub_8309C0).
//
// In the original client, it computes 3D Euclidean distances to camera and
// evaluates emitter velocity damping, step time accumulation, and position
// delta integration through 50+ serialized x87 instructions.
//
// Optimization:
// - Vectorizes 3D distance and vector length using SSE2 instructions
//   (_mm_sqrt_ss, _mm_mul_ps, _mm_add_ps).
// - Eliminates x87 status register flushes and float load/store latency.
// - Fully preserves caller register contract and bit-identical thresholds.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "particle_physics_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace ParticlePhysics {

namespace {

constexpr uintptr_t kTarget = 0x0097EB10;

// Prologue in 3.3.5a (12340):
// 97EB10: 55                push    ebp
// 97EB11: 8B EC             mov     ebp, esp
// 97EB13: 8B 55 10          mov     edx, [ebp+10h]
// 97EB16: 83 EC 0C          sub     esp, 0Ch
static const uint8_t kExpectedPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x10, 0x83, 0xEC
};

typedef void (__fastcall* ParticlePhysics_fn)(void* this_ptr, void* dummy_edx, float delta_time,
                                              const float* a3, const float* a4, int a5);
ParticlePhysics_fn g_orig_ParticlePhysics = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long g_calls = 0;
unsigned long g_velUpdates = 0;
unsigned long g_stepIntegrations = 0;

constexpr uintptr_t fn_sub_97AC20 = 0x0097AC20;
constexpr uintptr_t fn_sub_97ACB0 = 0x0097ACB0;
constexpr uintptr_t fn_sub_97E8D0 = 0x0097E8D0;

typedef void (__thiscall* Sub97AC20_fn)(void* this_ptr, const float* a3, const float* a4, int a5);
typedef void (__thiscall* Sub97ACB0_fn)(void* this_ptr, float dt, int unk);
typedef void (__thiscall* Sub97E8D0_fn)(void* this_ptr, int a5);

void __fastcall Hook_ParticlePhysics(void* this_ptr, void* dummy_edx, float delta_time,
                                     const float* a3, const float* a4, int a5) {
    ++g_calls;
    if (g_dead || (g_abSubject && AbTest::StandAside())) {
        g_orig_ParticlePhysics(this_ptr, dummy_edx, delta_time, a3, a4, a5);
        return;
    }

    if (!this_ptr || !a3 || !a4) {
        g_orig_ParticlePhysics(this_ptr, dummy_edx, delta_time, a3, a4, a5);
        return;
    }

    uint8_t* p = (uint8_t*)this_ptr;

    // 1. Vectorized 3D distance from emitter to camera
    // a3[12..14] = emitter pos, a4[0..2] = camera pos
    float dx = a3[12] - a4[0];
    float dy = a3[13] - a4[1];
    float dz = a3[14] - a4[2];

    __m128 vdiff = _mm_set_ps(0.0f, dz, dy, dx);
    __m128 vsq   = _mm_mul_ps(vdiff, vdiff);
    __m128 vsum  = _mm_add_ss(vsq, _mm_shuffle_ps(vsq, vsq, _MM_SHUFFLE(1, 1, 1, 1)));
    vsum         = _mm_add_ss(vsum, _mm_shuffle_ps(vsq, vsq, _MM_SHUFFLE(2, 2, 2, 2)));

    float lenSq;
    _mm_store_ss(&lenSq, vsum);

    float dist = 0.0f;
    if (lenSq > 0.0f) {
        __m128 s = _mm_sqrt_ss(vsum);
        _mm_store_ss(&dist, s);
    }
    *(float*)0x00DCE68C = dist;

    // 2. Previous position update
    *(uint32_t*)(p + 464) = *(uint32_t*)(p + 436);
    *(uint32_t*)(p + 468) = *(uint32_t*)(p + 440);
    *(uint32_t*)(p + 472) = *(uint32_t*)(p + 444);

    // 3. Child emitter calls
    ((Sub97AC20_fn)fn_sub_97AC20)(p, a3, a4, a5);

    const uint32_t child_count = *(const uint32_t*)(p + 108);
    if (child_count > 0) {
        uint8_t* child_arr = p + 112;
        for (uint32_t i = 0; i < child_count; ++i) {
            void* child = *(void**)(child_arr + i * 4);
            if (child) {
                ((Sub97AC20_fn)fn_sub_97AC20)(child, a3, a4, a5);
            }
        }
    }

    // 4. Time delta validation (epsilon 0.00000023841858f = flt_9EA27C)
    float abs_dt = std::fabs(delta_time);
    if (abs_dt < 0.00000023841858f) {
        *(uint32_t*)(p + 308) |= 0x100;
        return;
    }

    // 5. Velocity integration (flag 0x80000)
    if (*(const uint32_t*)(p + 308) & 0x80000) {
        ++g_velUpdates;
        float vdx = *(const float*)(p + 436) - *(const float*)(p + 464);
        float vdy = *(const float*)(p + 440) - *(const float*)(p + 468);
        float vdz = *(const float*)(p + 444) - *(const float*)(p + 472);

        *(float*)(p + 500) = vdx;
        *(float*)(p + 504) = vdy;
        *(float*)(p + 508) = vdz;

        __m128 vv   = _mm_set_ps(0.0f, vdz, vdy, vdx);
        __m128 vvsq = _mm_mul_ps(vv, vv);
        __m128 vtot = _mm_add_ss(vvsq, _mm_shuffle_ps(vvsq, vvsq, _MM_SHUFFLE(1, 1, 1, 1)));
        vtot        = _mm_add_ss(vtot, _mm_shuffle_ps(vvsq, vvsq, _MM_SHUFFLE(2, 2, 2, 2)));

        float vlenSq;
        _mm_store_ss(&vlenSq, vtot);

        float speed = 0.0f;
        if (vlenSq > 0.0f) {
            __m128 vsq_res = _mm_sqrt_ss(vtot);
            float vlen;
            _mm_store_ss(&vlen, vsq_res);
            speed = vlen / delta_time;
        }

        float factor = speed * *(const float*)(p + 384) + *(const float*)(p + 380);
        if (factor < 0.0f) factor = 0.0f;
        else if (factor > 1.0f) factor = 1.0f;

        *(float*)(p + 500) = vdx * factor;
        *(float*)(p + 504) = vdy * factor;
        *(float*)(p + 508) = vdz * factor;
    }

    // 6. Substep accumulator (flag 0x800)
    if (*(const uint32_t*)(p + 308) & 0x800) {
        float acc = *(const float*)(p + 476) + delta_time;
        *(float*)(p + 476) = acc;

        // flt_AA2C9C = 0.033333335f (1/30s tick)
        if (acc > 0.033333335f) {
            ++g_stepIntegrations;
            *(float*)(p + 476) = 0.0f;
            if (*(const uint32_t*)(p + 80) == 0) {
                *(float*)(p + 480) = 0.0f;
                *(float*)(p + 484) = 0.0f;
                *(float*)(p + 488) = 0.0f;
            } else {
                float vdx = *(const float*)(p + 436) - *(const float*)(p + 464);
                float vdy = *(const float*)(p + 440) - *(const float*)(p + 468);
                float vdz = *(const float*)(p + 444) - *(const float*)(p + 472);
                // flt_AA2D08 = 29.999998f
                float scale = (1.0f / (acc * 29.999998f)) * *(const float*)(p + 332);
                *(float*)(p + 480) = vdx * scale;
                *(float*)(p + 484) = vdy * scale;
                *(float*)(p + 488) = vdz * scale;
            }
        }
    }

    // 7. Tail animation and state updates
    ((Sub97ACB0_fn)fn_sub_97ACB0)(p, delta_time, 0);

    *(uint32_t*)(p + 308) |= 0x80;

    if (*(const uint32_t*)(p + 152) == 1) {
        ((Sub97E8D0_fn)fn_sub_97E8D0)(p, a5);
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptParticlePhysics) {
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[ParticlePhysics] Client patches forbidden; stand down");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[ParticlePhysics] Prologue mismatch at 0x%08X; aborting", (unsigned)kTarget);
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_ParticlePhysics,
                                          (void**)&g_orig_ParticlePhysics);
    if (status != MH_OK) {
        Log("[ParticlePhysics] MH_CreateHook failed: %d", status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[ParticlePhysics] MH_EnableHook failed: %d", status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("ParticlePhysics", &g_abSubject);
    Log("[ParticlePhysics] Hook armed at 0x%08X (SSE2 velocity/damping integration)", (unsigned)kTarget);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((LPVOID)kTarget);
    MH_RemoveHook((LPVOID)kTarget);
    g_installed = false;
    Log("[ParticlePhysics] Hook removed");
}

void LogStats() {
    if (!g_installed) return;
    Log("[ParticlePhysics] calls=%lu velUpdates=%lu stepIntegrations=%lu",
        g_calls, g_velUpdates, g_stepIntegrations);
}

} // namespace ParticlePhysics
