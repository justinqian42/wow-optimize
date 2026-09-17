// ============================================================================
// Module: x87_precision_check
// Description: Measures the floating point control state every SIMD
//              replacement in this project assumes.
// Safety & Threading: Reads two registers. Called from init and the frame path.
// ============================================================================
// Eight modules here replace client x87 code with SSE2 and argue bit-exactness
// from one sentence: "the x87 control word is left at 53-bit precision, which
// is exactly what a double lane carries." anim_quat_unpack, anim_vec3_track,
// frustum_aabb, hooks_simd, matrix_copy, segment_aabb, simd_math_fast and
// lua_hget_dispatch all say some version of it. Not one of them reads the
// register.
//
// It is worth reading, because something in this process can change it.
// IDirect3D9::CreateDevice sets the x87 precision control to 24-bit for the
// whole process unless the caller passes D3DCREATE_FPU_PRESERVE. If this client
// does not pass it, then after the device exists the client's own arithmetic
// rounds every intermediate to a float's 24-bit mantissa, and eight modules are
// computing in double where the client computes in single. Their comparisons
// would still usually agree, which is the dangerous part: a learning phase that
// passes and a rare divergence in the field is exactly the shape of defect this
// project keeps finding.
//
// The precision control field is bits 8 and 9 of the x87 control word: 00 is
// 24-bit, 10 is 53-bit, 11 is 64-bit. It is sampled before anything renders and
// again once frames are running, because the interesting case is it changing
// between the two.
//
// MXCSR is read for the same reason and it is not the same question. SSE
// arithmetic has no precision control - each instruction is single or double by
// its own opcode - but flush-to-zero and denormals-are-zero do change results,
// and x87 has no equivalent of either. With those bits set, an SSE replacement
// turns a denormal into zero where the client's x87 keeps it. No module here
// accounts for that, so if the bits are ever set the report says so rather than
// leaving it to be discovered by a mismatch.
//
// This module changes nothing. It reads two registers and reports what they
// say, so that "the control word is at 53-bit" stops being a sentence in eight
// comment blocks and becomes a line in the log.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "x87_precision_check.h"

extern "C" void Log(const char* fmt, ...);

namespace X87Precision {

namespace {

struct Reading {
    const char* when;
    uint16_t    cw;      // x87 control word
    uint32_t    mxcsr;
    bool        taken;
};

constexpr int kMaxSamples = 4;
Reading g_sample[kMaxSamples];
int    g_count = 0;

uint16_t ReadControlWord() {
    uint16_t cw = 0;
    __asm {
        fnstcw cw
    }
    return cw;
}

uint32_t ReadMxcsr() {
    uint32_t m = 0;
    __asm {
        stmxcsr m
    }
    return m;
}

const char* PrecisionName(uint16_t cw) {
    switch ((cw >> 8) & 3u) {
        case 0:  return "24-bit, a float's mantissa";
        case 1:  return "reserved";
        case 2:  return "53-bit, a double's mantissa";
        default: return "64-bit, the full x87 mantissa";
    }
}

const char* RoundingName(uint16_t cw) {
    switch ((cw >> 10) & 3u) {
        case 0:  return "to nearest";
        case 1:  return "down";
        case 2:  return "up";
        default: return "toward zero";
    }
}

bool IsDoublePrecision(uint16_t cw) { return ((cw >> 8) & 3u) == 2u; }

}  // namespace

void Sample(const char* when) {
    if (g_count >= kMaxSamples) return;
    // A moment is only sampled once, so a per-frame caller costs one compare.
    for (int i = 0; i < g_count; ++i)
        if (g_sample[i].when == when) return;
    Reading* s = &g_sample[g_count++];
    s->when  = when;
    s->cw    = ReadControlWord();
    s->mxcsr = ReadMxcsr();
    s->taken = true;
}

bool IsDouble() {
    if (g_count == 0) return false;
    return IsDoublePrecision(g_sample[g_count - 1].cw);
}

void LogStats() {
    if (g_count == 0) {
        Log("[FpuState] not measured: no sample was taken.");
        return;
    }

    for (int i = 0; i < g_count; ++i) {
        const Reading& s = g_sample[i];
        Log("[FpuState] %-14s x87 control word 0x%04X - precision %s, rounding "
            "%s. MXCSR 0x%08X.",
            s.when, (unsigned)s.cw, PrecisionName(s.cw), RoundingName(s.cw),
            (unsigned)s.mxcsr);
    }

    const uint16_t last = g_sample[g_count - 1].cw;

    if (g_count > 1 && ((g_sample[0].cw >> 8) & 3u) != ((last >> 8) & 3u))
        Log("[Wrong] [FpuState] the x87 precision changed during the session, "
            "from %s to %s. Every SSE2 replacement here was written against one "
            "of those two and is wrong under the other.",
            PrecisionName(g_sample[0].cw), PrecisionName(last));

    if (!IsDoublePrecision(last))
        Log("[Wrong] [FpuState] the x87 precision is %s, and eight modules here "
            "argue bit-exactness from it being 53-bit. Their comparisons would "
            "mostly still agree, so this will not show up as a retirement; it "
            "shows up as a rare wrong answer. Read this line before trusting "
            "any of them.", PrecisionName(last));
    else
        Log("[FpuState]   53-bit is what the SSE2 replacements here were written "
            "against, so a double lane carries what the client's x87 carries.");

    const uint32_t mx = g_sample[g_count - 1].mxcsr;
    const bool ftz = (mx & 0x8000u) != 0;
    const bool daz = (mx & 0x0040u) != 0;
    if (ftz || daz)
        Log("[Wrong] [FpuState] MXCSR has %s%s%s set. x87 has no equivalent, so "
            "an SSE replacement turns a denormal into zero where the client "
            "keeps it. No module here accounts for that.",
            ftz ? "flush-to-zero" : "",
            (ftz && daz) ? " and " : "",
            daz ? "denormals-are-zero" : "");
    else
        Log("[FpuState]   MXCSR has neither flush-to-zero nor "
            "denormals-are-zero set, so denormals reach an SSE lane the way "
            "they reach the x87 stack.");
}

}  // namespace X87Precision
