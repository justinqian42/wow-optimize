// ============================================================================
// Module: collision_poly_clip_sse2.cpp
//
// The polygon-against-plane clip in the client's Collide.cpp, sub_75B710. In a
// tester's 2026-09-19 profile it is 8.75% of executing main-thread time, second
// only to the sky texture build, and its caller sub_75C5A0 another 2.19%. That
// caller copies each collision face into a 15-vertex polygon and clips it
// against every plane in turn, so this runs once per face per plane.
//
// The function is __usercall: the plane in edx, the polygon in esi, and one
// four-byte value on the stack that the clip writes beside each vertex it
// creates. The caller cleans that argument.
//
// Polygon layout, from the caller's frame and this function's own writes:
//   +0x00  15 vertices, three floats each
//   +0xB4  15 values, one per vertex, the plane index that made it
//   +0xF0  the vertex count, as an int
//
// What it does, in order: it computes a signed distance for every vertex,
// remembers the smallest and the largest, and only then decides.
//
//   d = -(((x*p0) + (z*p2)) + (y*p1) + p3)
//
//   if the smallest is above -1/720   nothing happens at all
//   else if the largest is below 1/720   the count is set to zero
//   else the polygon is clipped
//
// The first case is the common one - a face entirely inside the plane - and the
// client pays for the whole distance pass, four vertices at a time on the x87
// stack with two status-word round trips each, to conclude that there is
// nothing to do. This computes the same distances in packed double, decides,
// and answers those two cases itself. A polygon that really is cut goes to the
// client's own code, which recomputes what it needs.
//
// Width. The client's x87 runs at 53-bit precision here, as this project has
// confirmed against three arithmetic modules, and every operand is a float
// widened exactly. The smallest and largest are held in x87 registers and never
// rounded to float, so they are 53-bit values, and a double lane carries
// exactly what they carry as long as the operation order is the client's. It is
// transcribed above, term by term, and the two thresholds are read from the
// client at 0x00A37F18 and 0x00A37F14 rather than written here as literals.
//
// NaN. The client updates the smallest only on an ordered less-than and the
// largest only on an ordered greater-than - the branches after each fnstsw test
// C0 and C2, so an unordered compare leaves both alone - and both decisions
// above fall through to the clip when a comparison is unordered. The ordered
// comparisons here behave the same way, and a NaN coordinate therefore ends up
// in the client's code rather than being decided here.
//
// Verification, predict-then-compare. While learning, the polygon is copied
// first, the client's function runs, and then the decision is worked out from
// the copy and checked against what the client actually did: nothing at all, or
// a count of zero. The first disagreement retires this for the session.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "collision_poly_clip_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionPolyClip {

namespace {

constexpr uintptr_t kTarget = 0x0075B710;
constexpr uintptr_t kNegEps = 0x00A37F18;   // -1/720, read from the client
constexpr uintptr_t kPosEps = 0x00A37F14;   // +1/720

// push ebp / mov ebp,esp / fld ds:flt_9EA8FC / sub esp,130h / fld ds:flt_A37F1C
const unsigned char kPrologue[20] = {
    0x55, 0x8B, 0xEC, 0xD9, 0x05, 0xFC, 0xA8, 0x9E, 0x00, 0x81,
    0xEC, 0x30, 0x01, 0x00, 0x00, 0xD9, 0x05, 0x1C, 0x7F, 0xA3
};

constexpr unsigned kCountOffset = 0xF0;
constexpr int      kMaxVerts    = 15;
constexpr size_t   kPolyBytes   = 61 * 4;   // vertices, values and the count

constexpr unsigned long kLearnCalls   = 20000;
constexpr unsigned long kResampleMask = 1023;

void* g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Main thread only, so plain counters; lower bounds if that ever stops being true.
unsigned long long g_calls = 0;
unsigned long long g_inside = 0;
unsigned long long g_outside = 0;
unsigned long long g_clipped = 0;
unsigned long long g_oddCount = 0;
unsigned long long g_control = 0;
unsigned long g_verified = 0;
unsigned g_mismatches = 0;

enum Outcome { kNoChange, kEmpty, kClip };

__forceinline Outcome Decide(const float* plane, const float* poly, int n) {
    const double p0 = plane[0], p1 = plane[1], p2 = plane[2], p3 = plane[3];
    double lo = 3.4028234663852886e38;    // the client's FLT_MAX seed
    double hi = -3.4028234663852886e38;
    for (int i = 0; i < n; ++i) {
        const double x = poly[3 * i + 0];
        const double y = poly[3 * i + 1];
        const double z = poly[3 * i + 2];
        const double d = -(((x * p0) + (z * p2)) + (y * p1) + p3);
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    const double negEps = *(const float*)kNegEps;
    const double posEps = *(const float*)kPosEps;
    if (negEps < lo) return kNoChange;
    if (hi < posEps) return kEmpty;
    return kClip;
}

// The original takes its arguments in edx and esi; this is the only way back
// into it from C.
__declspec(naked) void CallOrig(const float* /*plane*/, float* /*poly*/,
                                uint32_t /*value*/) {
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        push edi
        push ebx
        mov  edx, [ebp+8]
        mov  esi, [ebp+12]
        mov  eax, [ebp+16]
        push eax
        call dword ptr [g_orig]
        add  esp, 4
        pop  ebx
        pop  edi
        pop  esi
        pop  ebp
        ret
    }
}

__declspec(noinline) void Retire(const char* what, int n) {
    ++g_mismatches;
    g_dead = true;
    Log("[CollisionPolyClip] RETIRED: %s on a polygon of %d vertices. Every clip "
        "from here on is the client's own.", what, n);
    Verdict::Add(Verdict::Bad, "CollisionPolyClip decided a collision polygon "
                 "differently from the client and retired itself for this session");
}

// Returns true when the caller's thunk should jump into the client's function.
bool __cdecl Handle(const float* plane, float* poly, uint32_t value) {
    ++g_calls;
    if (g_dead || !plane || !poly) return true;

    const int n = *(const int*)((const char*)poly + kCountOffset);
    if (n < 0 || n > kMaxVerts) { ++g_oddCount; return true; }

    if (g_abSubject && AbTest::StandAside()) { ++g_control; return true; }

    const bool learning = (g_verified < kLearnCalls) ||
                          ((unsigned long)g_calls & kResampleMask) == 0;
    if (learning) {
        unsigned char before[kPolyBytes];
        memcpy(before, poly, kPolyBytes);
        const Outcome mine = Decide(plane, (const float*)before, n);

        CallOrig(plane, poly, value);

        const int after = *(const int*)((const char*)poly + kCountOffset);
        if (mine == kNoChange) {
            if (memcmp(before, poly, kPolyBytes) != 0) {
                Retire("the client changed a polygon this would have left alone", n);
                return false;
            }
            ++g_inside;
        } else if (mine == kEmpty) {
            if (after != 0) {
                Retire("the client kept vertices in a polygon this would have "
                       "emptied", n);
                return false;
            }
            ++g_outside;
        } else {
            ++g_clipped;
        }
        ++g_verified;
        return false;
    }

    const Outcome mine = Decide(plane, poly, n);
    if (mine == kNoChange) { ++g_inside; return false; }
    if (mine == kEmpty) {
        *(float*)((char*)poly + kCountOffset) = 0.0f;
        ++g_outside;
        return false;
    }
    ++g_clipped;
    return true;
}

// Entry state: the return address and the caller's four-byte value on the
// stack, the plane in edx and the polygon in esi. Nothing here touches the x87
// stack, which may hold the caller's values.
__declspec(naked) void Detour() {
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        push edx
        push ecx
        mov  eax, [ebp+8]
        push eax
        push esi
        push edx
        call Handle
        add  esp, 12
        test al, al
        pop  ecx
        pop  edx
        pop  esi
        pop  ebp
        jnz  delegate
        ret
    delegate:
        jmp  dword ptr [g_orig]
    }
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionPolyClip) return true;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[CollisionPolyClip] NOT active: the bytes at 0x%08X are not the polygon "
            "clip this was read from, so nothing was hooked.", (unsigned)kTarget);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[CollisionPolyClip] NOT active: No Client Patches is on, and this hooks "
            "a function inside wow.exe.");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Detour, &g_orig) != MH_OK) {
        Log("[CollisionPolyClip] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kTarget);
        return false;
    }
    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[CollisionPolyClip] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kTarget);
        return false;
    }
    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionPolyClip", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("CollisionPolyClip", (const void*)&Detour);

    Log("[CollisionPolyClip] ACTIVE on the collision polygon clip (sub_75B710 @ "
        "0x%08X), 8.75%% of executing time in an uncapped tester session. The client "
        "computes a distance for every vertex on the x87 stack, with two status-word "
        "round trips a vertex, before finding that the face is entirely inside the "
        "plane and doing nothing. That pass is packed double here, in the client's "
        "own operation order and at the width its x87 runs at, and a polygon that "
        "really is cut still goes to the client. The first %lu calls are answered by "
        "the client and the decision checked against what it did, then one in %lu.",
        (unsigned)kTarget, kLearnCalls, kResampleMask + 1);
    if (g_abSubject)
        Log("[CollisionPolyClip]   under A/B test: the control half runs the client's "
            "function through the same hook.");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptCollisionPolyClip) return;
    if (!g_installed) {
        Log("[CollisionPolyClip] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[CollisionPolyClip] hooked, and no clip has been reached yet. That is a "
            "measurement: no collision query has run since it went in.");
        return;
    }
    Log("[CollisionPolyClip] %llu call(s): %llu entirely inside the plane, %llu "
        "entirely outside, %llu really clipped and handed to the client. The first "
        "figure is the one this exists for. Plain counters, lower bounds.",
        g_calls, g_inside, g_outside, g_clipped);
    if (g_mismatches) {
        Log("[CollisionPolyClip]   DISABLED after a decision the client did not "
            "agree with; the line that says which is earlier in this log.");
    } else if (g_verified < kLearnCalls) {
        Log("[CollisionPolyClip]   %lu of %lu calls checked against the client so "
            "far; every call is still the client's own.", g_verified, kLearnCalls);
    } else {
        Log("[CollisionPolyClip]   %lu calls checked against the client, none "
            "disagreed; one in %lu is still checked.", g_verified, kResampleMask + 1);
    }
    if (g_oddCount)
        Log("[CollisionPolyClip]   %llu call(s) arrived with a vertex count outside "
            "0 to %d and were handed straight to the client.", g_oddCount, kMaxVerts);
    if (g_abSubject)
        Log("[CollisionPolyClip]   %llu call(s) ran the client's function as the A/B "
            "control half.", g_control);
}

}  // namespace CollisionPolyClip
