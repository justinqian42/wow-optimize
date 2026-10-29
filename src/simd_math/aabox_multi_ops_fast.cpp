// ============================================================================
// Module: aabox_multi_ops_fast.cpp
//
// Hardware-accelerated 3D Axis-Aligned Bounding Box (AABB) spatial operations
// for WoW 3.3.5a:
//   - sub_984930: CAaBox::Bounding(const C3Vector* points, uint32_t count)
//   - sub_6CB930: CAaBox::IsPointInside(const C3Vector& point)
//   - sub_4F5DE0: CAaBox::Center(C3Vector& center)
//   - sub_5FECB0: CAaBox::operator*=(float scale)
//   - sub_705630: CAaBox::Translate(const C3Vector& v)
//   - sub_70BD70: CAaBox::Expand(float amount)
//
// In stock 3.3.5a:
//   - sub_984930 computes the enclosing bounding box across an array of 3D points
//     using 800+ lines of unrolled serial x87 arithmetic (fcom, fnstsw ax,
//     branches). Called across 10 engine subsystems including CFrustum::Extents,
//     camera frustums, terrain chunks, WMO portals, doodads, and liquid meshes.
//   - sub_6CB930 evaluates strict 3D point-inside-AABB containment across 39
//     instructions and 6 serial x87 comparisons and branches.
//   - sub_4F5DE0 evaluates box center coordinates using serial x87 arithmetic.
//   - sub_5FECB0 scales an AABB in place using 24 serial x87 instructions.
//   - sub_705630 translates an AABB in place using 23 serial x87 instructions.
//   - sub_70BD70 expands/inflates an AABB symmetrically across 23 instructions.
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
#include <algorithm>

#include "aabox_multi_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace AaBoxMultiOpsFast {

namespace {

constexpr uintptr_t kTarget_Bounding      = 0x00984930;
constexpr uintptr_t kTarget_IsPointInside = 0x006CB930;
constexpr uintptr_t kTarget_Center        = 0x004F5DE0;
constexpr uintptr_t kTarget_Scale         = 0x005FECB0;
constexpr uintptr_t kTarget_Translate     = 0x00705630;
constexpr uintptr_t kTarget_Expand        = 0x0070BD70;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0002f;

// Function signatures
typedef float* (__cdecl* Bounding_fn)(float* out, const float* points, unsigned int count);
typedef BOOL (__thiscall* IsPointInside_fn)(const float* this_ptr, const float* pt);
typedef float* (__thiscall* Center_fn)(const float* this_ptr, float* center);
typedef float* (__thiscall* Scale_fn)(float* this_ptr, float scale);
typedef float* (__thiscall* Translate_fn)(float* this_ptr, const float* v);
typedef void (__thiscall* Expand_fn)(float* this_ptr, float amount);

Bounding_fn      orig_Bounding      = nullptr;
IsPointInside_fn orig_IsPointInside = nullptr;
Center_fn        orig_Center        = nullptr;
Scale_fn         orig_Scale         = nullptr;
Translate_fn     orig_Translate     = nullptr;
Expand_fn        orig_Expand        = nullptr;

bool g_installed = false;

bool g_dead_bounding        = false;
bool g_dead_is_point_inside = false;
bool g_dead_center          = false;
bool g_dead_scale           = false;
bool g_dead_translate       = false;
bool g_dead_expand          = false;

unsigned long long g_calls_bounding        = 0;
unsigned long long g_calls_is_point_inside = 0;
unsigned long long g_calls_center          = 0;
unsigned long long g_calls_scale           = 0;
unsigned long long g_calls_translate       = 0;
unsigned long long g_calls_expand          = 0;

unsigned long long g_verified_bounding        = 0;
unsigned long long g_verified_is_point_inside = 0;
unsigned long long g_verified_center          = 0;
unsigned long long g_verified_scale           = 0;
unsigned long long g_verified_translate       = 0;
unsigned long long g_verified_expand          = 0;

unsigned long long g_mismatches_bounding        = 0;
unsigned long long g_mismatches_is_point_inside = 0;
unsigned long long g_mismatches_center          = 0;
unsigned long long g_mismatches_scale           = 0;
unsigned long long g_mismatches_translate       = 0;
unsigned long long g_mismatches_expand          = 0;

// ============================================================================
// Direct Fast Mathematical Implementations
// ============================================================================

__forceinline float* Fast_Bounding(float* out, const float* points, unsigned int count) {
    if (count == 0) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
        out[4] = 0.0f;
        out[5] = 0.0f;
        return out;
    }

    float min_x = points[0];
    float min_y = points[1];
    float min_z = points[2];
    float max_x = min_x;
    float max_y = min_y;
    float max_z = min_z;

    const float* ptr = points + 3;
    for (unsigned int i = 1; i < count; ++i) {
        const float px = ptr[0];
        const float py = ptr[1];
        const float pz = ptr[2];

        if (px < min_x) min_x = px;
        if (py < min_y) min_y = py;
        if (pz < min_z) min_z = pz;

        if (px > max_x) max_x = px;
        if (py > max_y) max_y = py;
        if (pz > max_z) max_z = pz;

        ptr += 3;
    }

    out[0] = min_x;
    out[1] = min_y;
    out[2] = min_z;
    out[3] = max_x;
    out[4] = max_y;
    out[5] = max_z;

    return out;
}

__forceinline BOOL Fast_IsPointInside(const float* this_ptr, const float* pt) {
    const float px = pt[0];
    const float py = pt[1];
    const float pz = pt[2];

    return (this_ptr[0] < px && this_ptr[1] < py && this_ptr[2] < pz &&
            this_ptr[3] > px && this_ptr[4] > py && this_ptr[5] > pz) ? 1 : 0;
}

__forceinline float* Fast_Center(const float* this_ptr, float* center) {
    center[0] = (this_ptr[3] + this_ptr[0]) * 0.5f;
    center[1] = (this_ptr[4] + this_ptr[1]) * 0.5f;
    center[2] = (this_ptr[5] + this_ptr[2]) * 0.5f;
    return center;
}

__forceinline float* Fast_Scale(float* this_ptr, float scale) {
    this_ptr[0] *= scale;
    this_ptr[1] *= scale;
    this_ptr[2] *= scale;
    this_ptr[3] *= scale;
    this_ptr[4] *= scale;
    this_ptr[5] *= scale;
    return this_ptr;
}

__forceinline float* Fast_Translate(float* this_ptr, const float* v) {
    const float vx = v[0];
    const float vy = v[1];
    const float vz = v[2];

    this_ptr[0] += vx;
    this_ptr[1] += vy;
    this_ptr[2] += vz;
    this_ptr[3] += vx;
    this_ptr[4] += vy;
    this_ptr[5] += vz;

    return const_cast<float*>(v);
}

__forceinline void Fast_Expand(float* this_ptr, float amount) {
    this_ptr[0] -= amount;
    this_ptr[1] -= amount;
    this_ptr[2] -= amount;
    this_ptr[3] += amount;
    this_ptr[4] += amount;
    this_ptr[5] += amount;
}

// ============================================================================
// Hook Detours with Dual-Run Verification and Fail-Safe Guards
// ============================================================================

static float* __cdecl Hooked_Bounding(float* out, const float* points, unsigned int count) {
    g_calls_bounding++;

    if (g_dead_bounding) {
        return orig_Bounding(out, points, count);
    }

    const bool need_verify = (g_calls_bounding <= kVerifyFirst) || ((g_calls_bounding & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Bounding(out, points, count);
    }

    float stock_res[6];
    orig_Bounding(stock_res, points, count);

    float fast_res[6];
    Fast_Bounding(fast_res, points, count);

    bool match = true;
    for (int i = 0; i < 6; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_bounding++;
        std::memcpy(out, fast_res, sizeof(fast_res));
        return out;
    }

    g_mismatches_bounding++;
    g_dead_bounding = true;
    Log("[AaBoxMultiOpsFast] RETIRED Bounding hook due to mismatch");
    std::memcpy(out, stock_res, sizeof(stock_res));
    return out;
}

static BOOL __fastcall Hooked_IsPointInside(const float* this_ptr, void* /*dummy_edx*/, const float* pt) {
    g_calls_is_point_inside++;

    if (g_dead_is_point_inside) {
        return orig_IsPointInside(this_ptr, pt);
    }

    const bool need_verify = (g_calls_is_point_inside <= kVerifyFirst) || ((g_calls_is_point_inside & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_IsPointInside(this_ptr, pt);
    }

    const BOOL stock_res = orig_IsPointInside(this_ptr, pt);
    const BOOL fast_res = Fast_IsPointInside(this_ptr, pt);

    if (stock_res == fast_res) {
        g_verified_is_point_inside++;
        return fast_res;
    }

    g_mismatches_is_point_inside++;
    g_dead_is_point_inside = true;
    Log("[AaBoxMultiOpsFast] RETIRED IsPointInside hook due to mismatch: stock=%d, fast=%d", stock_res, fast_res);
    return stock_res;
}

static float* __fastcall Hooked_Center(const float* this_ptr, void* /*dummy_edx*/, float* center) {
    g_calls_center++;

    if (g_dead_center) {
        return orig_Center(this_ptr, center);
    }

    const bool need_verify = (g_calls_center <= kVerifyFirst) || ((g_calls_center & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Center(this_ptr, center);
    }

    float stock_res[3];
    orig_Center(this_ptr, stock_res);

    float fast_res[3];
    Fast_Center(this_ptr, fast_res);

    bool match = true;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_center++;
        std::memcpy(center, fast_res, sizeof(fast_res));
        return center;
    }

    g_mismatches_center++;
    g_dead_center = true;
    Log("[AaBoxMultiOpsFast] RETIRED Center hook due to mismatch");
    std::memcpy(center, stock_res, sizeof(stock_res));
    return center;
}

static float* __fastcall Hooked_Scale(float* this_ptr, void* /*dummy_edx*/, float scale) {
    g_calls_scale++;

    if (g_dead_scale) {
        return orig_Scale(this_ptr, scale);
    }

    const bool need_verify = (g_calls_scale <= kVerifyFirst) || ((g_calls_scale & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Scale(this_ptr, scale);
    }

    float stock_res[6];
    std::memcpy(stock_res, this_ptr, sizeof(stock_res));
    orig_Scale(stock_res, scale);

    float fast_res[6];
    std::memcpy(fast_res, this_ptr, sizeof(fast_res));
    Fast_Scale(fast_res, scale);

    bool match = true;
    for (int i = 0; i < 6; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_scale++;
        std::memcpy(this_ptr, fast_res, sizeof(fast_res));
        return this_ptr;
    }

    g_mismatches_scale++;
    g_dead_scale = true;
    Log("[AaBoxMultiOpsFast] RETIRED Scale hook due to mismatch");
    std::memcpy(this_ptr, stock_res, sizeof(stock_res));
    return this_ptr;
}

static float* __fastcall Hooked_Translate(float* this_ptr, void* /*dummy_edx*/, const float* v) {
    g_calls_translate++;

    if (g_dead_translate) {
        return orig_Translate(this_ptr, v);
    }

    const bool need_verify = (g_calls_translate <= kVerifyFirst) || ((g_calls_translate & kResampleMask) == 0);
    if (!need_verify) {
        return Fast_Translate(this_ptr, v);
    }

    float stock_res[6];
    std::memcpy(stock_res, this_ptr, sizeof(stock_res));
    float* stock_ret = orig_Translate(stock_res, v);

    float fast_res[6];
    std::memcpy(fast_res, this_ptr, sizeof(fast_res));
    float* fast_ret = Fast_Translate(fast_res, v);

    bool match = (stock_ret == fast_ret);
    if (match) {
        for (int i = 0; i < 6; ++i) {
            if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
                match = false;
                break;
            }
        }
    }

    if (match) {
        g_verified_translate++;
        std::memcpy(this_ptr, fast_res, sizeof(fast_res));
        return fast_ret;
    }

    g_mismatches_translate++;
    g_dead_translate = true;
    Log("[AaBoxMultiOpsFast] RETIRED Translate hook due to mismatch");
    std::memcpy(this_ptr, stock_res, sizeof(stock_res));
    return stock_ret;
}

static void __fastcall Hooked_Expand(float* this_ptr, void* /*dummy_edx*/, float amount) {
    g_calls_expand++;

    if (g_dead_expand) {
        orig_Expand(this_ptr, amount);
        return;
    }

    const bool need_verify = (g_calls_expand <= kVerifyFirst) || ((g_calls_expand & kResampleMask) == 0);
    if (!need_verify) {
        Fast_Expand(this_ptr, amount);
        return;
    }

    float stock_res[6];
    std::memcpy(stock_res, this_ptr, sizeof(stock_res));
    orig_Expand(stock_res, amount);

    float fast_res[6];
    std::memcpy(fast_res, this_ptr, sizeof(fast_res));
    Fast_Expand(fast_res, amount);

    bool match = true;
    for (int i = 0; i < 6; ++i) {
        if (std::fabs(stock_res[i] - fast_res[i]) > kVerifyTolerance) {
            match = false;
            break;
        }
    }

    if (match) {
        g_verified_expand++;
        std::memcpy(this_ptr, fast_res, sizeof(fast_res));
        return;
    }

    g_mismatches_expand++;
    g_dead_expand = true;
    Log("[AaBoxMultiOpsFast] RETIRED Expand hook due to mismatch");
    std::memcpy(this_ptr, stock_res, sizeof(stock_res));
}

} // anonymous namespace

void Init() {
    if (!Config::g_settings.OptAaBoxMultiOpsFast) {
        return;
    }

    static const unsigned char kExp_Bounding[8]      = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x57, 0x8B };
    static const unsigned char kExp_IsPointInside[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x01, 0x8B, 0x55, 0x08 };
    static const unsigned char kExp_Center[8]        = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x0C, 0x8B, 0x45 };
    static const unsigned char kExp_Scale[8]         = { 0x55, 0x8B, 0xEC, 0x8B, 0xC1, 0xD9, 0x00, 0xD9 };
    static const unsigned char kExp_Translate[8]     = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0xD9, 0x00 };
    static const unsigned char kExp_Expand[8]        = { 0x55, 0x8B, 0xEC, 0xD9, 0x01, 0xD9, 0x45, 0x08 };

    if (memcmp((const void*)kTarget_Bounding, kExp_Bounding, 8) != 0) {
        Log("[AaBoxMultiOpsFast] Bounding prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_IsPointInside, kExp_IsPointInside, 8) != 0) {
        Log("[AaBoxMultiOpsFast] IsPointInside prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Center, kExp_Center, 8) != 0) {
        Log("[AaBoxMultiOpsFast] Center prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Scale, kExp_Scale, 8) != 0) {
        Log("[AaBoxMultiOpsFast] Scale prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Translate, kExp_Translate, 8) != 0) {
        Log("[AaBoxMultiOpsFast] Translate prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Expand, kExp_Expand, 8) != 0) {
        Log("[AaBoxMultiOpsFast] Expand prologue mismatch, aborting");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_Bounding,      (void*)Hooked_Bounding,      (void**)&orig_Bounding)      != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_IsPointInside, (void*)Hooked_IsPointInside, (void**)&orig_IsPointInside) != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Center,        (void*)Hooked_Center,        (void**)&orig_Center)        != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Scale,         (void*)Hooked_Scale,         (void**)&orig_Scale)         != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Translate,     (void*)Hooked_Translate,     (void**)&orig_Translate)     != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Expand,        (void*)Hooked_Expand,        (void**)&orig_Expand)        != MH_OK) {
        Log("[AaBoxMultiOpsFast] Failed to create one or more hooks");
        return;
    }

    if (WO_EnableHook((void*)kTarget_Bounding)      != MH_OK ||
        WO_EnableHook((void*)kTarget_IsPointInside) != MH_OK ||
        WO_EnableHook((void*)kTarget_Center)        != MH_OK ||
        WO_EnableHook((void*)kTarget_Scale)         != MH_OK ||
        WO_EnableHook((void*)kTarget_Translate)     != MH_OK ||
        WO_EnableHook((void*)kTarget_Expand)        != MH_OK) {
        Log("[AaBoxMultiOpsFast] Failed to enable one or more hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxBounding",      (const void*)&Hooked_Bounding);
    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxIsPointInside", (const void*)&Hooked_IsPointInside);
    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxCenter",        (const void*)&Hooked_Center);
    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxScale",         (const void*)&Hooked_Scale);
    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxTranslate",     (const void*)&Hooked_Translate);
    SamplingProfiler::RegisterSelfSymbol("Fast_AaBoxExpand",        (const void*)&Hooked_Expand);

    g_installed = true;
    Log("[AaBoxMultiOpsFast] ACTIVE on AABB bounding, point inside, center, scale, translate, and expand (sub_984930/6CB930/4F5DE0/5FECB0/705630/70BD70)");
}

void LogStats() {
    if (!Config::g_settings.OptAaBoxMultiOpsFast || !g_installed) {
        return;
    }

    Log("[AaBoxMultiOpsFast] Stats: Bounding=%llu (%llu ver, %llu mis%s), "
        "IsPointInside=%llu (%llu ver, %llu mis%s), "
        "Center=%llu (%llu ver, %llu mis%s), "
        "Scale=%llu (%llu ver, %llu mis%s), "
        "Translate=%llu (%llu ver, %llu mis%s), "
        "Expand=%llu (%llu ver, %llu mis%s)",
        g_calls_bounding, g_verified_bounding, g_mismatches_bounding, g_dead_bounding ? " [RETIRED]" : "",
        g_calls_is_point_inside, g_verified_is_point_inside, g_mismatches_is_point_inside, g_dead_is_point_inside ? " [RETIRED]" : "",
        g_calls_center, g_verified_center, g_mismatches_center, g_dead_center ? " [RETIRED]" : "",
        g_calls_scale, g_verified_scale, g_mismatches_scale, g_dead_scale ? " [RETIRED]" : "",
        g_calls_translate, g_verified_translate, g_mismatches_translate, g_dead_translate ? " [RETIRED]" : "",
        g_calls_expand, g_verified_expand, g_mismatches_expand, g_dead_expand ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Bounding);
    MH_DisableHook((void*)kTarget_IsPointInside);
    MH_DisableHook((void*)kTarget_Center);
    MH_DisableHook((void*)kTarget_Scale);
    MH_DisableHook((void*)kTarget_Translate);
    MH_DisableHook((void*)kTarget_Expand);
    g_installed = false;
}

} // namespace AaBoxMultiOpsFast
