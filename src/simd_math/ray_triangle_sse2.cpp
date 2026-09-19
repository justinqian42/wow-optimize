// ============================================================================
// Module: ray_triangle_sse2
// Description: SSE2 double transcription of the client's ray-triangle test.
// ============================================================================
// sub_983490 is Moller-Trumbore, and it is the shared ray-triangle test for the
// whole collision family: sub_7C6600, sub_7C6790, sub_7C6C30 and sub_7C6D50 all
// call it, plus sub_81E110. That family is about 6.4% of executing main-thread
// time in the corrected profile, and this is the part of it that does the real
// arithmetic - the two outcode passes around it are already replaced.
//
// It is 231 instructions of x87 with fifteen fxch and five fnstsw. Every fnstsw
// is a status word round trip feeding a conditional jump, and the jumps are on
// whether a ray misses a triangle, which is not predictable. The transcription
// below has no fnstsw and no stack juggling: comisd sets the flags directly.
// ---------------------------------------------------------------------------
// The width, which is measured rather than assumed
//
// The client keeps its intermediates in x87 registers, so their width is the
// precision control field, not the type of anything in memory. That field is
// 53-bit here, which makes a double lane exact. It is worth saying how that is
// known, because IDirect3D9::CreateDevice sets it to 24-bit unless the caller
// passes D3DCREATE_FPU_PRESERVE and this client renders through D3D9.
//
// Three modules already compare the bytes of a float result against the client
// rather than a predicate, and all three armed in both field sessions to hand:
// anim_vec3_track at 30453 and 31049 agreements, anim_quat_unpack at 30560 and
// 31317, quat_lerp at 24292 and 29936 bit-identical. anim_vec3_track computes
// in double. At 24-bit the client would round every intermediate to a float's
// mantissa while that module rounds to a double's, and a lerp would diverge in
// the first handful of calls rather than after ninety thousand byte-exact
// agreements. x87_precision_check.cpp now reads the register and prints it, so
// a machine where this is not true says so.
// ---------------------------------------------------------------------------
// Which intermediates the client rounds to float, and which it does not
//
// This is the part that cannot be reasoned about and had to be traced. The
// client stores some intermediates with fstp - rounding to float and dropping
// the register - and others with fst, which rounds a copy and keeps the wide
// value. A few it never stores at all. Writing the algebra out cleanly, in one
// width, gives a different answer:
//
//   p.x   fstp to var_20   -> enters the determinant AS A FLOAT
//   p.y   never stored     -> stays wide
//   p.z   never stored     -> stays wide
//   e1.z  fst  to var_24   -> the wide copy is used for the determinant,
//                             the float copy for the second cross product
//   e2.*  fst  to var_14/10/C -> wide for the first cross product,
//                             float for the distance
//   1/det fstp to arg_8    -> every barycentric is scaled by the FLOAT
//   u     fst  to arg_8    -> wide for its two range tests, float for u+v
//
// So the same quantity appears at two widths within one call, on purpose or by
// accident, and both have to be reproduced. F() below is that rounding.
// ---------------------------------------------------------------------------
// The associations, also traced
//
// A reversed subtract is spelled almost the same as a plain one. fsubp st(1),
// st computes st(1) - st(0); fsubrp st(1), st computes st(0) - st(1). Three of
// the six cross product terms here use the reversed form, and reading them as
// the plain form negates p.y, p.z and q.z. A reference implementation written
// from the same misreading agrees with the transcription perfectly, so that is
// not what caught it: aiming a hundred and twenty thousand rays at the centroid
// of their own triangle and getting a hit on 0.8% of them caught it. An
// arithmetic identity that has to hold is worth more than a second opinion
// derived from the same source.
//
//   det   = (e1.z*p.z + e1.y*p.y) + e1.x*p.x_f
//   u_raw = (T.y*p.y  + T.z*p.z ) + T.x *p.x_f
//   v_raw = (d.y*q.y  + d.z*q.z ) + d.x *q.x
//   t_raw = (q.z*e2.z_f + q.y*e2.y_f) + q.x*e2.x_f
//
// Left operand first in every sum. Multiplication is exactly commutative in
// IEEE so the order inside a product does not matter; the order of the two
// additions does, and it is not the order the algebra is usually written in.
// ---------------------------------------------------------------------------
// The predicates, read from the raw disassembly
//
// The determinant test rejects when the value lies BETWEEN the two constants,
// which are -1e-6 and +1e-6: the near-parallel case. It is written here in the
// client's own direction so that a NaN determinant continues, the way an
// unordered fcom leaves both masked bits set and takes the jump.
//
// The barycentric tests are the same shape: reject on an ordered comparison
// only, continue on unordered.
// ---------------------------------------------------------------------------
// The return value is one byte
//
// Every exit is mov al, 1 or xor al, al and none of them clears the rest of
// EAX. Reading all thirty-two bits of a hooked original like this one is what
// retired the sort key cache on its first comparison in every session; the
// verification here compares AL.
// ---------------------------------------------------------------------------
// Measured, before any of it reaches a client
//
// The same algorithm was compiled a second time with /arch:IA32 /fp:precise,
// which makes MSVC emit x87 - fld, fmul, fsub, fcomp, fnstsw against the stack
// at the control word's precision, which is what sub_983490 is. The compiler's
// output is a close stand-in for the client's hand-written original: 228
// instructions against 231, six fnstsw against five, eleven fxch against
// fifteen. Slightly fewer register exchanges than the client, so it is if
// anything the faster of the two, which makes the figure below a floor.
//
// Four million calls each, half of them rays aimed at their own triangle so the
// deep path is exercised and half random so the early rejects are:
//
//     SSE2 double   21.90 ns/call
//     x87           41.40 ns/call
//     speedup            1.89x
//
// And the two agree. Over 20000 cases with 10062 hits, no verdict differed and
// no returned distance differed by a bit. That is worth more than the timing:
// the x87 build came from the same trace but through a different instruction
// set and a different compiler path, so agreeing with it is a check on the
// reading of the disassembly that the Python reference could not give - that one
// was written by hand from the same reading and carried the same three sign
// errors until an arithmetic identity caught them.
//
// This is the function measured, not a frame. What share of a frame it is
// depends on how often the collision family runs, which is what the A/B harness
// answers.
// ---------------------------------------------------------------------------
// Verification
//
// The function is pure apart from two optional out pointers, so both versions
// simply run and their answers are compared - the byte of the return, and the
// floats each wrote. Nothing is saved, restored or predicted. The client's own
// answer is handed back for the whole checking phase, so a session that never
// arms behaves exactly like an unhooked one.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "ray_triangle_sse2.h"
#include "x87_precision_check.h"
#include "self_bench.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace RayTriangle {

namespace {

constexpr uintptr_t kTarget16 = 0x00983490;
constexpr uintptr_t kTarget32 = 0x009836B0;

// flt_AA2E70 and flt_A32B48, read out of the image: -1e-6f and +1e-6f.
const float kDetLo = -9.9999997e-07f;
const float kDetHi =  9.9999997e-07f;

constexpr long kVerifyFirst  = 20000;
constexpr unsigned kResample = 4095;   // one call in this many, as a mask

typedef int (__cdecl* Compare_t)(const void* ray, const void* verts,
                                 const void* tri, void* outT, void* outUV,
                                 float tol);

struct Channel {
    const char*   name;
    const char*   symbolName;
    uintptr_t     target;
    Compare_t     origTest;
    bool          installed;
    bool          armed;
    bool          dead;
    bool          abSubject;
    int           benchSlot;

    unsigned long calls;
    unsigned long verified;
    unsigned long hits;
    unsigned long stood;
    unsigned long caught;
};

static Channel g_ch16 = { "16-bit", "RayTriIntersect16_SSE2", kTarget16, nullptr, false, false, false, false, -1, 0, 0, 0, 0, 0 };
static Channel g_ch32 = { "32-bit", "RayTriIntersect32_SSE2", kTarget32, nullptr, false, false, false, false, -1, 0, 0, 0, 0, 0 };

// Rounds to float and back, which is what an fstp to a dword slot does.
inline double F(double x) { return (double)(float)x; }

struct Out {
    float t;
    float u;
    float v;
    bool  wroteT;
    bool  wroteUV;
};

// A verbatim transcription. Every rounding and every association above is
// reproduced; nothing here is simplified.
template <typename IndexT>
int Test(const float* ray, const float* verts, const IndexT* tri,
         bool wantT, bool wantUV, float tolIn, Out* out) {
    out->wroteT = false;
    out->wroteUV = false;

    const double tol     = (double)tolIn;
    const double negTol  = -tol;                 // fchs then fstp: exact
    const double onePlus = F(tol + 1.0);         // fadd 1.0 then fstp to float

    const float* v0 = verts + 3 * (size_t)tri[0];
    const float* v1 = verts + 3 * (size_t)tri[1];
    const float* v2 = verts + 3 * (size_t)tri[2];

    const double e1x = (double)v1[0] - (double)v0[0];
    const double e1y = (double)v1[1] - (double)v0[1];
    const double e1z = (double)v1[2] - (double)v0[2];
    const double e2x = (double)v2[0] - (double)v0[0];
    const double e2y = (double)v2[1] - (double)v0[1];
    const double e2z = (double)v2[2] - (double)v0[2];

    // fst keeps the wide value and rounds a copy; both copies are used later.
    const double e1z_f = F(e1z);
    const double e2x_f = F(e2x);
    const double e2y_f = F(e2y);
    const double e2z_f = F(e2z);

    const double dx = (double)ray[3];
    const double dy = (double)ray[4];
    const double dz = (double)ray[5];

    // p = dir x e2. Only p.x is stored with fstp, so only p.x loses the wide
    // value. The operand order matters and is not what the mnemonic suggests:
    // fsubp st(1), st is st(1) - st(0), while fsubrp st(1), st is the reverse,
    // st(0) - st(1). Reading the second as the first flips the sign of two of
    // these three components. See the note on how that was caught.
    const double px_f = F(dy * e2z - dz * e2y);   // fsubp  at 0x00983507
    const double py   = dz * e2x - dx * e2z;      // fsubrp at 0x00983516
    const double pz   = dx * e2y - dy * e2x;      // fsubrp at 0x00983524

    const double det = (e1z * pz + e1y * py) + e1x * px_f;

    // Reject only on an ordered comparison, so a NaN determinant carries on
    // exactly as the client's two fcom sequences let it.
    if ((double)kDetLo < det && (double)kDetHi > det) return 0;

    const double invDet = F(1.0 / det);

    const double tx = (double)ray[0] - (double)v0[0];
    const double ty = (double)ray[1] - (double)v0[1];
    const double tz = (double)ray[2] - (double)v0[2];

    const double u = ((ty * py + tz * pz) + tx * px_f) * invDet;
    const double u_f = F(u);                 // fst: the wide u is still live

    if (negTol > u) return 0;
    if (u > onePlus) return 0;

    // q = T x e1, and note e1.z arrives here as the float copy while e1.x and
    // e1.y are still the wide ones.
    const double qx = e1z_f * ty - tz * e1y;   // fsubp  at 0x009835C2
    const double qy = tz * e1x - e1z_f * tx;   // fsubp  at 0x009835CD
    const double qz = tx * e1y - ty * e1x;     // fsubrp at 0x009835D7

    const double v = ((dy * qy + dz * qz) + dx * qx) * invDet;

    if (negTol > v) return 0;
    if ((v + u_f) > onePlus) return 0;

    if (wantT) {
        const double t = ((qz * e2z_f + qy * e2y_f) + qx * e2x_f) * invDet;
        out->t = (float)t;
        out->wroteT = true;
    }
    if (wantUV) {
        out->u = (float)u_f;
        out->v = (float)v;
        out->wroteUV = true;
    }
    return 1;
}

bool ReadableRange(const void* p, size_t bytes) {
    const uintptr_t a = (uintptr_t)p;
    return a >= 0x10000 && a < 0xFFE00000 && (a + bytes) > a;
}

// Faults the guard caught, while checking or after. The fallback is the
// client's own test, which reads the same ray, vertices and triangle indices, so
// a fault here is one the fallback would take too; the armed path calls Test
// without an exception frame once kVerifyFirst comparisons have run guarded and
// this is still zero. If it ever is not, every armed call stays guarded.
//
// The guard lives in its own function so that Hooked_Test has no __try at all:
// one anywhere in it puts the frame into its prologue for every call.
template <typename IndexT>
__declspec(noinline) bool TestGuardedCall(const void* ray, const void* verts,
                                          const void* tri, bool wantT, bool wantUV,
                                          float tol, Out* o, int* r,
                                          unsigned long& caughtCounter) {
    __try {
        *r = Test<IndexT>((const float*)ray, (const float*)verts, (const IndexT*)tri,
                          wantT, wantUV, tol, o);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++caughtCounter;
        return false;
    }
}

template <typename IndexT>
inline int Hooked_TestImpl(const void* ray, const void* verts, const void* tri,
                           void* outT, void* outUV, float tol, Channel& ch) {
    ++ch.calls;

    if (ch.dead || !ray || !verts || !tri)
        return ch.origTest(ray, verts, tri, outT, outUV, tol);
    if (!ReadableRange(ray, 24) || !ReadableRange(tri, sizeof(IndexT) * 3))
        return ch.origTest(ray, verts, tri, outT, outUV, tol);

    const bool checking = !ch.armed || (ch.calls & kResample) == 0;

    if (!checking) {
        // Both halves of the A/B run are bracketed by the same pair, so the
        // harness files an ON sample against an OFF one and the report is this
        // function measured against the client's rather than a frame time that
        // cannot see it.
        const unsigned long long t = AbTest::TickIn();
        int r;
        if (ch.abSubject && AbTest::StandAside()) {
            ++ch.stood;
            r = ch.origTest(ray, verts, tri, outT, outUV, tol);
            if (r & 0xFF) ++ch.hits;
        } else {
            Out o;
            if (ch.caught == 0) {
                r = Test<IndexT>((const float*)ray, (const float*)verts,
                                 (const IndexT*)tri, outT != nullptr,
                                 outUV != nullptr, tol, &o);
            } else if (!TestGuardedCall<IndexT>(ray, verts, tri, outT != nullptr,
                                                outUV != nullptr, tol, &o, &r, ch.caught)) {
                AbTest::TickOut(t);
                return ch.origTest(ray, verts, tri, outT, outUV, tol);
            }
            if (r) {
                ++ch.hits;
                if (o.wroteT)  *(float*)outT = o.t;
                if (o.wroteUV) { ((float*)outUV)[0] = o.u; ((float*)outUV)[1] = o.v; }
            }
        }
        AbTest::TickOut(t);
        return r;
    }

    if (ch.abSubject && AbTest::StandAside()) {
        ++ch.stood;
        return ch.origTest(ray, verts, tri, outT, outUV, tol);
    }

    // Checking: work out the answer without touching the caller's buffers, let
    // the client run and write them, then compare.
    // The verification runs both halves on the same input, which is the only
    // paired comparison this project ever gets. Timing it costs two rdtsc on a
    // path that was already doing twice the work.
    Out mine;
    int ours;
    const uint64_t tOursA = SelfBench::Now();
    if (!TestGuardedCall<IndexT>(ray, verts, tri, outT != nullptr, outUV != nullptr,
                                 tol, &mine, &ours, ch.caught)) {
        return ch.origTest(ray, verts, tri, outT, outUV, tol);
    }

    const uint64_t tOursB = SelfBench::Now();
    const int theirs = ch.origTest(ray, verts, tri, outT, outUV, tol);
    const uint64_t tTheirsB = SelfBench::Now();
    SelfBench::Pair(ch.benchSlot, tOursB - tOursA, tTheirsB - tOursB);
    ++ch.verified;
    if (theirs & 0xFF) ++ch.hits;   // counted on both paths, or the rate lies

    // Only AL is the answer. See the note above about the sort key cache.
    const bool sameAnswer = ((ours & 0xFF) != 0) == ((theirs & 0xFF) != 0);
    bool sameData = true;
    if (sameAnswer && (theirs & 0xFF)) {
        if (mine.wroteT && memcmp(&mine.t, outT, sizeof(float)) != 0)
            sameData = false;
        if (mine.wroteUV && memcmp(&mine.u, outUV, 2 * sizeof(float)) != 0)
            sameData = false;
    }

    if (!sameAnswer || !sameData) {
        ch.dead = true;
        Log("[RayTriangle] [%s] DISAGREED with the client after %lu comparisons and "
            "retired for this session. It answered %d and this answered %d%s. "
            "The client's own result stands - nothing of ours was written - so "
            "the session is unaffected.",
            ch.name, ch.verified, theirs & 0xFF, ours & 0xFF,
            sameAnswer ? ", and the answers matched but a written value did not"
                       : "");
        return theirs;
    }

    if (!ch.armed && ch.verified >= kVerifyFirst) {
        ch.armed = true;
        Log("[RayTriangle] [%s] armed: %lu comparisons agreed with the client, the "
            "returned byte and every float written. Now answering directly and "
            "rechecking one call in %u.", ch.name, ch.verified, kResample + 1);
    }
    return theirs;
}

int __cdecl Hooked_Test16(const void* ray, const void* verts, const void* tri,
                          void* outT, void* outUV, float tol) {
    return Hooked_TestImpl<uint16_t>(ray, verts, tri, outT, outUV, tol, g_ch16);
}

int __cdecl Hooked_Test32(const void* ray, const void* verts, const void* tri,
                          void* outT, void* outUV, float tol) {
    return Hooked_TestImpl<uint32_t>(ray, verts, tri, outT, outUV, tol, g_ch32);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptRayTriangleSse2) return true;

    if (!X87Precision::IsDouble())
        Log("[Wrong] [RayTriangle] the x87 precision control is not 53-bit, so "
            "the width this was written for is not the width the client is "
            "using. Read the FpuState lines above.");

    bool abSubject = false;
    abSubject = AbTest::IsSubject("RayTriangleSse2", &abSubject);

    // 16-bit indices target (0x00983490)
    if (!IsBadReadPtr((void*)kTarget16, 16)) {
        const unsigned char* p16 = (const unsigned char*)kTarget16;
        if (p16[0] == 0x55 && p16[1] == 0x8B && p16[2] == 0xEC) {
            if (WineSafe_CreateHook((void*)kTarget16, (void*)&Hooked_Test16,
                                    (void**)&g_ch16.origTest) == MH_OK) {
                if (WO_EnableHook((void*)kTarget16) == MH_OK) {
                    g_ch16.installed = true;
                    g_ch16.abSubject = abSubject;
                    g_ch16.benchSlot = SelfBench::Register("RayTri16");
                    SamplingProfiler::RegisterSelfSymbol(g_ch16.symbolName, (const void*)&Hooked_Test16);
                    Log("[RayTriangle] ACTIVE on 16-bit indices (0x%08X), the shared "
                        "ray-triangle test for the collision family (sub_7C6600, sub_7C6790, "
                        "sub_7C6C30, sub_7C6D50, sub_81E110). Verified first %ld calls.",
                        (unsigned)kTarget16, kVerifyFirst);
                } else {
                    Log("[RayTriangle] NOT active: could not enable 16-bit hook at 0x%08X.", (unsigned)kTarget16);
                }
            } else {
                Log("[RayTriangle] NOT active: could not hook 0x%08X.", (unsigned)kTarget16);
            }
        }
    }

    // 32-bit indices target (0x009836B0)
    if (!IsBadReadPtr((void*)kTarget32, 16)) {
        const unsigned char* p32 = (const unsigned char*)kTarget32;
        if (p32[0] == 0x55 && p32[1] == 0x8B && p32[2] == 0xEC) {
            if (WineSafe_CreateHook((void*)kTarget32, (void*)&Hooked_Test32,
                                    (void**)&g_ch32.origTest) == MH_OK) {
                if (WO_EnableHook((void*)kTarget32) == MH_OK) {
                    g_ch32.installed = true;
                    g_ch32.abSubject = abSubject;
                    g_ch32.benchSlot = SelfBench::Register("RayTri32");
                    SamplingProfiler::RegisterSelfSymbol(g_ch32.symbolName, (const void*)&Hooked_Test32);
                    Log("[RayTriangle] ACTIVE on 32-bit indices (0x%08X), terrain/mesh "
                        "collision paths (sub_7A3570, sub_7C8DD0, sub_7D8730). "
                        "Verified first %ld calls.",
                        (unsigned)kTarget32, kVerifyFirst);
                } else {
                    Log("[RayTriangle] NOT active: could not enable 32-bit hook at 0x%08X.", (unsigned)kTarget32);
                }
            } else {
                Log("[RayTriangle] NOT active: could not hook 0x%08X.", (unsigned)kTarget32);
            }
        }
    }

    if (abSubject) {
        Log("[RayTriangle]   under A/B test, timed directly with rdtsc.");
    }
    return g_ch16.installed || g_ch32.installed;
}

void Shutdown() {
    if (g_ch16.installed) MH_DisableHook((void*)kTarget16);
    if (g_ch32.installed) MH_DisableHook((void*)kTarget32);
    g_ch16.installed = false;
    g_ch32.installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptRayTriangleSse2) return;
    if (!g_ch16.installed && !g_ch32.installed) {
        Log("[RayTriangle] switched on but not installed, so nothing here was measured.");
        return;
    }
    auto reportChannel = [](const Channel& ch) {
        if (!ch.installed) return;
        if (ch.calls == 0) {
            Log("[RayTriangle] [%s] installed, 0 calls measured.", ch.name);
            return;
        }
        Log("[RayTriangle] [%s] exception guard: %lu fault(s) caught; the armed path runs %s.",
            ch.name, ch.caught, !ch.armed ? "guarded, still checking"
                                          : (ch.caught ? "guarded, because the guard has caught something"
                                                       : "without an exception frame"));
        Log("[RayTriangle] [%s] %lu tests, %lu hit (%.1f%%), %lu compared with client%s.",
            ch.name, ch.calls, ch.hits, 100.0 * (double)ch.hits / (double)ch.calls, ch.verified,
            ch.dead ? " - RETIRED on a disagreement"
                    : (ch.armed ? " - armed" : " - still verifying, the client still answers"));
        if (ch.stood > 0)
            Log("[RayTriangle] [%s] %lu calls stood aside for the A/B off stint.",
                ch.name, ch.stood);
    };

    reportChannel(g_ch16);
    reportChannel(g_ch32);
}

}  // namespace RayTriangle
