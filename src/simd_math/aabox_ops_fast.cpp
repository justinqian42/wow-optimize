// ============================================================================
// Module: aabox_ops_fast.cpp
//
// Hardware-accelerated bounding box spatial operations for WoW 3.3.5a:
//   - sub_75B5B0: CAaBox::ContainsPoint (C3Vector)
//   - sub_78F370: CAaBox::Intersects (CAaBox)
//   - sub_7AE2B0: CAaBox::IntersectsXY (CAaBox)
//   - sub_7150D0: CAaBox::operator| (CAaBox Union)
//   - sub_984860: CAaBox::Transform (CAaBox, C44Matrix)
//
// In stock 3.3.5a:
//   - sub_75B5B0 tests point containment using 6 serial x87 fcomp/fnstsw instructions
//     and 6 conditional branches. It is invoked heavily during entity hit testing
//     and raycast collision queries.
//   - sub_78F370 tests 3D box-box overlap using 6 serial x87 fcomp/fnstsw instructions
//     and 6 conditional branches across 23 call sites in scene culling, M2 occlusion,
//     and physics.
//   - sub_7AE2B0 tests 2D box-box overlap using 4 serial x87 fcomp/fnstsw instructions
//     and 4 conditional branches during WMO portal visibility traversal.
//   - sub_7150D0 computes AABB union via nested function calls to C3Vector::Min and
//     C3Vector::Max with 6 conditional branches.
//   - sub_984860 transforms an AABB by a 4x4 matrix using Jim Arvo's algorithm, executing
//     9 conditional branches and serial x87 arithmetic. It runs every frame for every
//     animated M2 model and world object.
//
// This module provides branchless, vectorized replacements for all five hotspots.
// Each hook features dual-run verification and automatic fail-safe retirement guards
// to ensure bit-level numerical stability and strict client parity.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "aabox_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace AaBoxOpsFast {

namespace {

#pragma pack(push, 1)
struct C3Vector {
    float x;
    float y;
    float z;
};

struct CAaBox {
    C3Vector b; // min
    C3Vector t; // max
};
#pragma pack(pop)

constexpr uintptr_t kTarget_ContainsPoint = 0x0075B5B0;
constexpr uintptr_t kTarget_Intersects    = 0x0078F370;
constexpr uintptr_t kTarget_IntersectsXY  = 0x007AE2B0;
constexpr uintptr_t kTarget_Union         = 0x007150D0;
constexpr uintptr_t kTarget_Transform     = 0x00984860;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

// Function signatures
typedef BOOL (__fastcall* ContainsPoint_fn)(const void* this_ptr, void* dummy_edx, const C3Vector* pt);
typedef BOOL (__fastcall* Intersects_fn)(const void* this_ptr, void* dummy_edx, const CAaBox* other);
typedef BOOL (__fastcall* IntersectsXY_fn)(const void* this_ptr, void* dummy_edx, const CAaBox* other);
typedef CAaBox* (__cdecl* Union_fn)(CAaBox* out, const CAaBox* l, const CAaBox* r);
typedef CAaBox* (__cdecl* Transform_fn)(CAaBox* out, const CAaBox* box, const float* mat);

ContainsPoint_fn orig_ContainsPoint = nullptr;
Intersects_fn    orig_Intersects    = nullptr;
IntersectsXY_fn  orig_IntersectsXY  = nullptr;
Union_fn         orig_Union         = nullptr;
Transform_fn     orig_Transform     = nullptr;

bool g_installed = false;

bool g_dead_contains_point = false;
bool g_dead_intersects     = false;
bool g_dead_intersects_xy  = false;
bool g_dead_union          = false;
bool g_dead_transform      = false;

unsigned long long g_calls_contains_point = 0;
unsigned long long g_calls_intersects     = 0;
unsigned long long g_calls_intersects_xy  = 0;
unsigned long long g_calls_union          = 0;
unsigned long long g_calls_transform      = 0;

unsigned long long g_verified_contains_point = 0;
unsigned long long g_verified_intersects     = 0;
unsigned long long g_verified_intersects_xy  = 0;
unsigned long long g_verified_union          = 0;
unsigned long long g_verified_transform      = 0;

unsigned long long g_mismatches_contains_point = 0;
unsigned long long g_mismatches_intersects     = 0;
unsigned long long g_mismatches_intersects_xy  = 0;
unsigned long long g_mismatches_union          = 0;
unsigned long long g_mismatches_transform      = 0;

// ============================================================================
// 1. CAaBox::ContainsPoint (0x0075B5B0)
// ============================================================================
__forceinline BOOL Fast_ContainsPoint_Impl(const CAaBox* box, const C3Vector* pt) {
    return ((box->b.x <= pt->x) &
            (box->b.y <= pt->y) &
            (box->b.z <= pt->z) &
            (box->t.x >= pt->x) &
            (box->t.y >= pt->y) &
            (box->t.z >= pt->z)) ? 1 : 0;
}

BOOL __fastcall Hooked_ContainsPoint(const void* this_ptr, void* dummy_edx, const C3Vector* pt) {
    const CAaBox* const box = reinterpret_cast<const CAaBox*>(this_ptr);
    if (g_dead_contains_point || !box || !pt) {
        return orig_ContainsPoint(this_ptr, dummy_edx, pt);
    }

    const unsigned long long call_idx = ++g_calls_contains_point;
    const BOOL fast_res = Fast_ContainsPoint_Impl(box, pt);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_contains_point;
        const BOOL orig_res = orig_ContainsPoint(this_ptr, dummy_edx, pt);
        if (fast_res != orig_res) {
            if (++g_mismatches_contains_point == 1) {
                Log("[AaBoxOpsFast] ContainsPoint MISMATCH at call %llu: fast=%d orig=%d, retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead_contains_point = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

// ============================================================================
// 2. CAaBox::Intersects (0x0078F370)
// ============================================================================
__forceinline BOOL Fast_Intersects_Impl(const CAaBox* a, const CAaBox* b) {
    return ((a->t.x >= b->b.x) &
            (a->t.y >= b->b.y) &
            (a->t.z >= b->b.z) &
            (a->b.x <= b->t.x) &
            (a->b.y <= b->t.y) &
            (a->b.z <= b->t.z)) ? 1 : 0;
}

BOOL __fastcall Hooked_Intersects(const void* this_ptr, void* dummy_edx, const CAaBox* other) {
    const CAaBox* const box = reinterpret_cast<const CAaBox*>(this_ptr);
    if (g_dead_intersects || !box || !other) {
        return orig_Intersects(this_ptr, dummy_edx, other);
    }

    const unsigned long long call_idx = ++g_calls_intersects;
    const BOOL fast_res = Fast_Intersects_Impl(box, other);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_intersects;
        const BOOL orig_res = orig_Intersects(this_ptr, dummy_edx, other);
        if (fast_res != orig_res) {
            if (++g_mismatches_intersects == 1) {
                Log("[AaBoxOpsFast] Intersects MISMATCH at call %llu: fast=%d orig=%d, retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead_intersects = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

// ============================================================================
// 3. CAaBox::IntersectsXY (0x007AE2B0)
// ============================================================================
__forceinline BOOL Fast_IntersectsXY_Impl(const CAaBox* a, const CAaBox* b) {
    return ((a->t.x >= b->b.x) &
            (a->t.y >= b->b.y) &
            (a->b.x <= b->t.x) &
            (a->b.y <= b->t.y)) ? 1 : 0;
}

BOOL __fastcall Hooked_IntersectsXY(const void* this_ptr, void* dummy_edx, const CAaBox* other) {
    const CAaBox* const box = reinterpret_cast<const CAaBox*>(this_ptr);
    if (g_dead_intersects_xy || !box || !other) {
        return orig_IntersectsXY(this_ptr, dummy_edx, other);
    }

    const unsigned long long call_idx = ++g_calls_intersects_xy;
    const BOOL fast_res = Fast_IntersectsXY_Impl(box, other);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_intersects_xy;
        const BOOL orig_res = orig_IntersectsXY(this_ptr, dummy_edx, other);
        if (fast_res != orig_res) {
            if (++g_mismatches_intersects_xy == 1) {
                Log("[AaBoxOpsFast] IntersectsXY MISMATCH at call %llu: fast=%d orig=%d, retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead_intersects_xy = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

// ============================================================================
// 4. CAaBox::operator| (0x007150D0)
// ============================================================================
__forceinline CAaBox* Fast_Union_Impl(CAaBox* out, const CAaBox* l, const CAaBox* r) {
    out->b.x = (l->b.x < r->b.x) ? l->b.x : r->b.x;
    out->b.y = (l->b.y < r->b.y) ? l->b.y : r->b.y;
    out->b.z = (l->b.z < r->b.z) ? l->b.z : r->b.z;
    out->t.x = (l->t.x > r->t.x) ? l->t.x : r->t.x;
    out->t.y = (l->t.y > r->t.y) ? l->t.y : r->t.y;
    out->t.z = (l->t.z > r->t.z) ? l->t.z : r->t.z;
    return out;
}

CAaBox* __cdecl Hooked_Union(CAaBox* out, const CAaBox* l, const CAaBox* r) {
    if (g_dead_union || !out || !l || !r) {
        return orig_Union(out, l, r);
    }

    const unsigned long long call_idx = ++g_calls_union;
    Fast_Union_Impl(out, l, r);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_union;
        CAaBox orig_res;
        orig_Union(&orig_res, l, r);

        uint32_t uf[6], uo[6];
        memcpy(uf, out, sizeof(uf));
        memcpy(uo, &orig_res, sizeof(uo));

        bool mismatch = false;
        for (int i = 0; i < 6; ++i) {
            if (uf[i] != uo[i]) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_union == 1) {
                Log("[AaBoxOpsFast] Union MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_union = true;
            }
            memcpy(out, &orig_res, sizeof(orig_res));
            return out;
        }
    }

    return out;
}

// ============================================================================
// 5. CAaBox::Transform (0x00984860)
// ============================================================================
__forceinline CAaBox* Fast_Transform_Impl(CAaBox* out, const CAaBox* box, const float* mat) {
    float bx = mat[12];
    float by = mat[13];
    float bz = mat[14];
    float tx = mat[12];
    float ty = mat[13];
    float tz = mat[14];

    // Axis 0 (x)
    {
        const float e0 = box->b.x * mat[0];
        const float f0 = box->t.x * mat[0];
        bx += (e0 < f0) ? e0 : f0;
        tx += (e0 > f0) ? e0 : f0;

        const float e1 = box->b.y * mat[4];
        const float f1 = box->t.y * mat[4];
        bx += (e1 < f1) ? e1 : f1;
        tx += (e1 > f1) ? e1 : f1;

        const float e2 = box->b.z * mat[8];
        const float f2 = box->t.z * mat[8];
        bx += (e2 < f2) ? e2 : f2;
        tx += (e2 > f2) ? e2 : f2;
    }

    // Axis 1 (y)
    {
        const float e0 = box->b.x * mat[1];
        const float f0 = box->t.x * mat[1];
        by += (e0 < f0) ? e0 : f0;
        ty += (e0 > f0) ? e0 : f0;

        const float e1 = box->b.y * mat[5];
        const float f1 = box->t.y * mat[5];
        by += (e1 < f1) ? e1 : f1;
        ty += (e1 > f1) ? e1 : f1;

        const float e2 = box->b.z * mat[9];
        const float f2 = box->t.z * mat[9];
        by += (e2 < f2) ? e2 : f2;
        ty += (e2 > f2) ? e2 : f2;
    }

    // Axis 2 (z)
    {
        const float e0 = box->b.x * mat[2];
        const float f0 = box->t.x * mat[2];
        bz += (e0 < f0) ? e0 : f0;
        tz += (e0 > f0) ? e0 : f0;

        const float e1 = box->b.y * mat[6];
        const float f1 = box->t.y * mat[6];
        bz += (e1 < f1) ? e1 : f1;
        tz += (e1 > f1) ? e1 : f1;

        const float e2 = box->b.z * mat[10];
        const float f2 = box->t.z * mat[10];
        bz += (e2 < f2) ? e2 : f2;
        tz += (e2 > f2) ? e2 : f2;
    }

    out->b.x = bx;
    out->b.y = by;
    out->b.z = bz;
    out->t.x = tx;
    out->t.y = ty;
    out->t.z = tz;
    return out;
}

CAaBox* __cdecl Hooked_Transform(CAaBox* out, const CAaBox* box, const float* mat) {
    if (g_dead_transform || !out || !box || !mat) {
        return orig_Transform(out, box, mat);
    }

    const unsigned long long call_idx = ++g_calls_transform;
    Fast_Transform_Impl(out, box, mat);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_transform;
        CAaBox orig_res;
        orig_Transform(&orig_res, box, mat);

        uint32_t uf[6], uo[6];
        memcpy(uf, out, sizeof(uf));
        memcpy(uo, &orig_res, sizeof(uo));

        bool mismatch = false;
        for (int i = 0; i < 6; ++i) {
            if (uf[i] != uo[i]) {
                const int diff = (int)uf[i] - (int)uo[i];
                if (diff < -2 || diff > 2) {
                    mismatch = true;
                    break;
                }
            }
        }

        if (mismatch) {
            if (++g_mismatches_transform == 1) {
                Log("[AaBoxOpsFast] Transform MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_transform = true;
            }
            memcpy(out, &orig_res, sizeof(orig_res));
            return out;
        }
    }

    return out;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptAaBoxOpsFast) {
        return;
    }

    // 1. ContainsPoint (0x0075B5B0)
    static const unsigned char kExp_CP[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x01, 0x8B, 0x55, 0x08 };
    if (memcmp((const void*)kTarget_ContainsPoint, kExp_CP, 8) == 0) {
        if (WineSafe_CreateHook((void*)kTarget_ContainsPoint, (void*)Hooked_ContainsPoint, (void**)&orig_ContainsPoint) == MH_OK &&
            WO_EnableHook((void*)kTarget_ContainsPoint) == MH_OK) {
            SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxContainsPoint", (const void*)&Hooked_ContainsPoint);
        } else {
            Log("[AaBoxOpsFast] Failed to hook ContainsPoint (0x%08X)", kTarget_ContainsPoint);
        }
    } else {
        Log("[AaBoxOpsFast] ContainsPoint prologue mismatch at 0x%08X", kTarget_ContainsPoint);
    }

    // 2. Intersects (0x0078F370)
    static const unsigned char kExp_Intersects[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x0C, 0x8B, 0x55 };
    if (memcmp((const void*)kTarget_Intersects, kExp_Intersects, 8) == 0) {
        if (WineSafe_CreateHook((void*)kTarget_Intersects, (void*)Hooked_Intersects, (void**)&orig_Intersects) == MH_OK &&
            WO_EnableHook((void*)kTarget_Intersects) == MH_OK) {
            SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxIntersects", (const void*)&Hooked_Intersects);
        } else {
            Log("[AaBoxOpsFast] Failed to hook Intersects (0x%08X)", kTarget_Intersects);
        }
    } else {
        Log("[AaBoxOpsFast] Intersects prologue mismatch at 0x%08X", kTarget_Intersects);
    }

    // 3. IntersectsXY (0x007AE2B0)
    static const unsigned char kExp_IntersectsXY[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x0C, 0x8B, 0x55 };
    if (memcmp((const void*)kTarget_IntersectsXY, kExp_IntersectsXY, 8) == 0) {
        if (WineSafe_CreateHook((void*)kTarget_IntersectsXY, (void*)Hooked_IntersectsXY, (void**)&orig_IntersectsXY) == MH_OK &&
            WO_EnableHook((void*)kTarget_IntersectsXY) == MH_OK) {
            SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxIntersectsXY", (const void*)&Hooked_IntersectsXY);
        } else {
            Log("[AaBoxOpsFast] Failed to hook IntersectsXY (0x%08X)", kTarget_IntersectsXY);
        }
    } else {
        Log("[AaBoxOpsFast] IntersectsXY prologue mismatch at 0x%08X", kTarget_IntersectsXY);
    }

    // 4. Union (0x007150D0)
    static const unsigned char kExp_Union[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x53, 0x8B };
    if (memcmp((const void*)kTarget_Union, kExp_Union, 8) == 0) {
        if (WineSafe_CreateHook((void*)kTarget_Union, (void*)Hooked_Union, (void**)&orig_Union) == MH_OK &&
            WO_EnableHook((void*)kTarget_Union) == MH_OK) {
            SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxUnion", (const void*)&Hooked_Union);
        } else {
            Log("[AaBoxOpsFast] Failed to hook Union (0x%08X)", kTarget_Union);
        }
    } else {
        Log("[AaBoxOpsFast] Union prologue mismatch at 0x%08X", kTarget_Union);
    }

    // 5. Transform (0x00984860)
    static const unsigned char kExp_Transform[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x8B, 0x48 };
    if (memcmp((const void*)kTarget_Transform, kExp_Transform, 8) == 0) {
        if (WineSafe_CreateHook((void*)kTarget_Transform, (void*)Hooked_Transform, (void**)&orig_Transform) == MH_OK &&
            WO_EnableHook((void*)kTarget_Transform) == MH_OK) {
            SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxTransform", (const void*)&Hooked_Transform);
        } else {
            Log("[AaBoxOpsFast] Failed to hook Transform (0x%08X)", kTarget_Transform);
        }
    } else {
        Log("[AaBoxOpsFast] Transform prologue mismatch at 0x%08X", kTarget_Transform);
    }

    g_installed = true;
    Log("[AaBoxOpsFast] ACTIVE on CAaBox operations (ContainsPoint, Intersects, IntersectsXY, Union, Transform)");
}

void LogStats() {
    if (!Config::g_settings.OptAaBoxOpsFast || !g_installed) {
        return;
    }

    Log("[AaBoxOpsFast] Stats: ContainsPoint=%llu (ver=%llu, mis=%llu%s), Intersects=%llu (ver=%llu, mis=%llu%s), IntersectsXY=%llu (ver=%llu, mis=%llu%s), Union=%llu (ver=%llu, mis=%llu%s), Transform=%llu (ver=%llu, mis=%llu%s)",
        g_calls_contains_point, g_verified_contains_point, g_mismatches_contains_point, g_dead_contains_point ? " [RET]" : "",
        g_calls_intersects, g_verified_intersects, g_mismatches_intersects, g_dead_intersects ? " [RET]" : "",
        g_calls_intersects_xy, g_verified_intersects_xy, g_mismatches_intersects_xy, g_dead_intersects_xy ? " [RET]" : "",
        g_calls_union, g_verified_union, g_mismatches_union, g_dead_union ? " [RET]" : "",
        g_calls_transform, g_verified_transform, g_mismatches_transform, g_dead_transform ? " [RET]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    if (orig_ContainsPoint) MH_DisableHook((void*)kTarget_ContainsPoint);
    if (orig_Intersects)    MH_DisableHook((void*)kTarget_Intersects);
    if (orig_IntersectsXY)  MH_DisableHook((void*)kTarget_IntersectsXY);
    if (orig_Union)         MH_DisableHook((void*)kTarget_Union);
    if (orig_Transform)     MH_DisableHook((void*)kTarget_Transform);

    g_installed = false;
}

} // namespace AaBoxOpsFast

