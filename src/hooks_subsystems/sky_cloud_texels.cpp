// ============================================================================
// Module: sky_cloud_texels.cpp
//
// The texel loops of the cloud texture build, sub_7EFD00 at 0x007EFEEA through
// 0x007F03DA, transcribed out of the client and checked against it on live
// data. Nothing here changes what the client does. It is the foundation for
// something that would: see the end of this comment.
//
// Why this region and not the whole function. Reading the disassembly rather
// than the decompiler's argument list, the loops touch no client global at all.
// Their inputs are the object's own fields, four read-only tables, and the
// twelve floats sub_7EFAE0 leaves on the caller's stack; their outputs are the
// RGBA buffer at +38, the coverage buffer at +44 and the one row of noise at
// +50. Everything in sub_7EFD00 that writes a global is outside them:
// sub_7EFAE0 writes dword_D38CD0 and dword_D38CD4 before the loops start, and
// sub_4B6CB0 and sub_681F20 lock and upload the texture after they finish. So
// this region, and only this region, can be moved.
//
// What the loops compute. For every texel, octaves of three-dimensional value
// noise. Each octave holds sixteen-bit x, y and z accumulators; x starts at the
// phase word and steps by the octave's frequency across a row, y steps by the
// same frequency down the rows, and z is the phase word and does not move
// within a pass. The high bytes index the permutation table at 0x00AF4A70 in
// the usual nested way, perm[x + perm[y + perm[z]]], and the eight corner
// values come from the table at 0x00D38688; the low bytes index the fade table
// at 0x00D38188. The eight corners are interpolated in x, then y, then z, and
// the octave's contribution is scaled by one over two to its index. The x
// interpolants are recomputed only when the high byte of x changes, which is
// the client's own cache and is kept here.
//
// The third octave is special: after it, the running sum is differenced against
// the same point one column back and one row back to give the two slopes the
// lighting uses, scaled by one shifted left by the row shift minus seven. The
// row-back value is the float row at +50, which this reads and writes exactly
// where the client does.
//
// The sum times sixty-four plus a hundred and twenty-eight, rounded to an
// integer, minus the density byte at +08 indexes the ramp at 0x00D38588 and
// gives the coverage byte. Zero coverage copies the previous texel's four bytes
// and clears the alpha, except in the first column where the client writes
// nothing at all and the texel keeps whatever was there. Non-zero coverage runs
// the lighting: a height from the coverage byte, three colour multiply-adds, a
// dot product against the light vector, two reciprocal square roots by the
// 0x5F3997BB approximation with no Newton step, three clamps at one, and three
// conversions to bytes.
//
// Precision. x87 on this client runs at 53-bit precision, so the arithmetic
// here is plain double in the client's own order and association, which is
// bit-exact. Where the client stores an intermediate into a four-byte local -
// the two slopes, the two squared lengths, the green and blue channels, and
// every value on its way to a byte - the rounding to float is reproduced
// explicitly, because those roundings are part of the answer. The three
// conversions to integer are bare fistp with no control-word load, so they
// round to nearest and not toward zero, and _mm_cvtss_si32 is used rather than
// a C cast, which would truncate.
//
// Verification. Predict-then-compare, on the real thing. The pass's inputs and
// the three output buffers are copied before the client runs, the client runs,
// then this computes what the loops should have produced from the copies and
// compares every byte of RGBA, every coverage byte and every float of the noise
// row against what the client actually left. A single differing byte retires
// this for the session and the log says which texel and what the two answers
// were. While it is running the work is done twice, so this is off by default
// and belongs in a proving session, not in play.
//
// What it is for. Once the transcription is proven in the field, the loops are
// code we own, and two things become possible that are not possible now: doing
// them two texels at a time with packed doubles, and doing them on a worker
// thread while the main thread goes on with the frame. Neither is built here,
// and neither should be built before the millisecond figure SkyTextureReuse now
// reports says the build is worth the trouble on a machine that is busy.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "sky_cloud_texels.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace SkyCloudTexels {

namespace {

constexpr uintptr_t kLightParams = 0x007EFAE0;   // fills the twelve floats

constexpr uintptr_t kPerm     = 0x00AF4A70;      // permutation, indices masked to a byte
constexpr uintptr_t kValues   = 0x00D38688;      // lattice values
constexpr uintptr_t kFade     = 0x00D38188;      // fade weights
constexpr uintptr_t kRamp     = 0x00D38588;      // coverage steps
constexpr uintptr_t kFreqBase = 0x00AF4DC4;      // five frequency words per table index
constexpr uintptr_t kK255     = 0x009E30C0;
constexpr uintptr_t kK128     = 0x009E8C6C;
constexpr uintptr_t kK64      = 0x009E8DD8;
constexpr uintptr_t kKByte    = 0x00A45564;      // one over two hundred and fifty-five

constexpr unsigned kF_densByte   = 0x08;
constexpr unsigned kF_tableIdx   = 0x09;
constexpr unsigned kF_rebuildAll = 0x0A;
constexpr unsigned kF_rowsPass   = 0x10;
constexpr unsigned kF_nextRow    = 0x14;
constexpr unsigned kF_size       = 0x1C;
constexpr unsigned kF_shift      = 0x20;
constexpr unsigned kF_octaves    = 0x28;
constexpr unsigned kF_rgba       = 0x38;
constexpr unsigned kF_coverage   = 0x44;
constexpr unsigned kF_noiseRow   = 0x50;
constexpr unsigned kF_phase      = 0x88;

// The frequency table holds five words, so five octaves is the client's own
// ceiling. The rest are what a sane sky object looks like; anything outside
// them is handed straight back without a comparison and counted.
constexpr uint32_t kMaxOct   = 5;
constexpr uint32_t kMaxSize  = 512;
constexpr uint32_t kMaxShift = 9;

constexpr unsigned long kProve = 48;   // passes compared before this stops working

const uint8_t* g_perm   = nullptr;
const float*   g_values = nullptr;
const float*   g_fade   = nullptr;
const uint8_t* g_ramp   = nullptr;
double g_k255 = 0.0, g_k128 = 0.0, g_k64 = 0.0, g_kByte = 0.0;

// ---------------------------------------------------------------------------
// The twelve floats sub_7EFAE0 leaves behind, captured from the call the build
// makes just before the loops.
// ---------------------------------------------------------------------------
struct Light {
    float height[3];      // a2, scaled by the texel height
    float direct[3];      // a3, added where the dot product is positive
    float ambient[3];     // a4, the base colour
    float toLight[3];     // a5, the light position and the constant z
    float strength;       // a6
};
Light g_light;
unsigned long g_lightSeq = 0;

typedef float* (__fastcall* LightFn)(void*, void*, float*, float*, float*, float*, float*);
LightFn g_origLight = nullptr;

float* __fastcall LightDetour(void* self, void* edx, float* a2, float* a3,
                              float* a4, float* a5, float* a6) {
    float* r = g_origLight(self, edx, a2, a3, a4, a5, a6);
    if (a2 && a3 && a4 && a5 && a6) {
        memcpy(g_light.height,  a2, 12);
        memcpy(g_light.direct,  a3, 12);
        memcpy(g_light.ambient, a4, 12);
        memcpy(g_light.toLight, a5, 12);
        g_light.strength = *a6;
        ++g_lightSeq;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Everything the loops read, copied before the client runs, because the tail of
// sub_7EFD00 moves the next row and the phase on.
// ---------------------------------------------------------------------------
struct Snap {
    uint32_t rowsPass, nextRow, size, shift, octaves, stride;
    uint16_t phase;
    uint8_t  densByte;
    uint16_t freq[kMaxOct];
    uint8_t* rgba;
    uint8_t* cov;
    float*   noise;
};
Snap g_snap;
bool g_snapValid = false;
unsigned long g_snapSeq = 0;

uint8_t* g_sRgba  = nullptr;   // our own copies, which the transcription fills
uint8_t* g_sCov   = nullptr;
float*   g_sNoise = nullptr;
size_t   g_sRgbaN = 0, g_sCovN = 0, g_sNoiseN = 0;

bool g_installed = false;
bool g_dead = false;
unsigned long long g_calls = 0;
unsigned long g_proven = 0;
unsigned long g_declined = 0;
unsigned g_mismatches = 0;

bool Reserve(void** p, size_t* have, size_t want) {
    if (*have >= want) return true;
    if (*p) { VirtualFree(*p, 0, MEM_RELEASE); *p = nullptr; *have = 0; }
    // Above two gigabytes where there is room; the low half is the scarce one.
    void* m = VirtualAlloc(nullptr, want, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                           PAGE_READWRITE);
    if (!m) return false;
    *p = m;
    *have = want;
    return true;
}

inline uint32_t RD32(const void* p, unsigned off) {
    return *(const uint32_t*)((const char*)p + off);
}
inline uint16_t RD16(const void* p, unsigned off) {
    return *(const uint16_t*)((const char*)p + off);
}
inline uint8_t RD8(const void* p, unsigned off) {
    return *(const uint8_t*)((const char*)p + off);
}

inline float Bits2F(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
inline uint32_t F2Bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

// The client's reciprocal square root: one magic subtraction, no Newton step.
inline float RSqrtApprox(float x) {
    return Bits2F(0x5F3997BBu - ((F2Bits(x) >> 1) & 0x3FFFFFFFu));
}

// A bare fistp with no control-word load rounds to nearest, which a C cast does
// not. SSE2 conversion rounds by MXCSR, which is nearest, and gives the same
// indefinite value on overflow.
inline int ToInt(float f) { return _mm_cvtss_si32(_mm_set_ss(f)); }

// ---------------------------------------------------------------------------
// One octave's state, in our own layout rather than the client's eighty-four
// byte record, since nothing outside these loops reads it.
// ---------------------------------------------------------------------------
struct Oct {
    uint16_t x, y, step;
    float    amp;
    uint32_t p00, p01, p10, p11;
    float    g[8];
    int32_t  xhiCached;
};

// ---------------------------------------------------------------------------
// The loops themselves.
// ---------------------------------------------------------------------------
void Generate(const Snap& s, const Light& L,
              uint8_t* rgba, uint8_t* cov, float* noise) {
    Oct oct[kMaxOct];
    const uint32_t n = s.octaves;

    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t f = s.freq[i];
        oct[i].y    = (uint16_t)((uint16_t)s.nextRow * f);
        oct[i].step = f;
        oct[i].amp  = (float)(1.0 / (double)(1 << i));
    }

    const uint8_t phaseHi = (uint8_t)(s.phase >> 8);
    const uint32_t permZ0 = g_perm[phaseHi];
    const uint32_t permZ1 = g_perm[(uint8_t)(phaseHi + 1)];
    const double   fadeZ  = g_fade[(uint8_t)s.phase];
    const int      slopeShift = 1 << (s.shift - 7);

    for (uint32_t r = 0; r < s.rowsPass; ++r) {
        const uint32_t rowIndex = s.nextRow + r;
        uint8_t* texRow = rgba + 4 * ((size_t)rowIndex << s.shift);
        uint8_t* covRow = cov + ((size_t)rowIndex << s.shift);
        const float rowY = (float)(double)rowIndex;

        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t yhi  = (uint8_t)(oct[i].y >> 8);
            const uint8_t yhi1 = (uint8_t)(yhi + 1);
            oct[i].p00 = g_perm[(uint8_t)(yhi  + permZ0)];
            oct[i].p01 = g_perm[(uint8_t)(yhi1 + permZ0)];
            oct[i].p10 = g_perm[(uint8_t)(permZ1 + yhi)];
            oct[i].p11 = g_perm[(uint8_t)(permZ1 + yhi1)];
            oct[i].xhiCached = -1;
            oct[i].x = s.phase;
        }

        double prev = 0.0;   // the running sum one column back, rounded to float

        for (uint32_t x = 0; x < s.size; ++x) {
            double acc = 0.0;
            double atThird = 0.0;
            float  slopeX = 0.0f, slopeY = 0.0f;
            bool   haveSlopes = false;

            for (uint32_t i = 0; i < n; ++i) {
                Oct& o = oct[i];
                const uint32_t xw = o.x;
                const int xhi = (int)(xw >> 8);
                const uint32_t xlo = xw & 0xFFu;

                if (xhi != o.xhiCached) {
                    o.xhiCached = xhi;
                    const uint32_t base[4] = { o.p00, o.p01, o.p10, o.p11 };
                    for (int k = 0; k < 4; ++k) {
                        const uint8_t bi = (uint8_t)((uint32_t)xhi + base[k]);
                        const double lo = g_values[g_perm[bi]];
                        const double hi = g_values[g_perm[(uint8_t)(bi + 1)]];
                        o.g[2 * k]     = (float)lo;
                        o.g[2 * k + 1] = (float)(hi - lo);
                    }
                }

                const double fx = g_fade[xlo];
                const double fy = g_fade[(uint8_t)o.y];

                const double a = (double)o.g[1] * fx + (double)o.g[0];
                const double c = (double)o.g[5] * fx + (double)o.g[4];
                const double b = (double)o.g[3] * fx + (double)o.g[2];
                const double d = a + (b - a) * fy;
                const double e = fx * (double)o.g[7] + (double)o.g[6];
                const double f = c + (e - c) * fy;

                o.x = (uint16_t)(o.x + o.step);
                acc = acc + (d + (f - d) * fadeZ) * (double)o.amp;

                if (i == 2) {
                    const double scale = (double)slopeShift;
                    slopeX = (float)((prev - acc) * scale);
                    slopeY = (float)(((double)noise[x] - acc) * scale);
                    noise[x] = (float)acc;
                    atThird = acc;
                    haveSlopes = true;
                }
            }
            if (haveSlopes) prev = (double)(float)atThird;

            const int q = ToInt((float)(acc * g_k64 + g_k128));
            const int idx = (int)(uint8_t)q - (int)s.densByte;
            const uint8_t c8 = (idx >= 0) ? g_ramp[idx] : (uint8_t)0;
            covRow[x] = c8;

            uint8_t* tex = texRow + 4 * x;
            if (c8 == 0) {
                if (x >= 1) {
                    memcpy(tex, tex - 4, 4);
                    tex[3] = 0;
                }
                continue;
            }

            const double dx = (double)L.toLight[0] - (double)x;
            const double dy = (double)L.toLight[1] - (double)rowY;
            const double nz = (double)L.toLight[2];

            const uint8_t h = (uint8_t)((uint8_t)((uint8_t)(0xFF - c8) >> 1) + 0x40);
            const double hf = (double)(int)h * g_kByte;

            double cr = (double)L.height[0] * hf + (double)L.ambient[0];
            float  cg = (float)((double)L.height[1] * hf + (double)L.ambient[1]);
            float  cb = (float)((double)L.height[2] * hf + (double)L.ambient[2]);

            const float len  = (float)((nz * nz + dy * dy) + dx * dx);
            const float slen = (float)(((double)slopeY * (double)slopeY +
                                        (double)slopeX * (double)slopeX) + 1.0);
            const double dot = nz + ((double)slopeX * dx + (double)slopeY * dy);
            const double lit = dot * ((double)RSqrtApprox(slen) * (double)RSqrtApprox(len));

            if (lit > 0.0) {
                const double s2 = lit * (double)L.strength;
                cr = cr + (double)L.direct[0] * s2;
                cg = (float)((double)L.direct[1] * s2 + (double)cg);
                cb = (float)(s2 * (double)L.direct[2] + (double)cb);
            }
            if (cr > 1.0) cr = 1.0;
            if (1.0 < (double)cg) cg = 1.0f;
            if (1.0 < (double)cb) cb = 1.0f;

            tex[2] = (uint8_t)ToInt((float)(cr * g_k255));
            tex[1] = (uint8_t)ToInt((float)((double)cg * g_k255));
            tex[0] = (uint8_t)ToInt((float)((double)cb * g_k255));
            tex[3] = c8;
        }

        for (uint32_t i = 0; i < n; ++i)
            oct[i].y = (uint16_t)(oct[i].y + oct[i].step);
    }
}

__declspec(noinline) void Retire(const char* what, uint32_t row, uint32_t col,
                                 unsigned mine, unsigned theirs) {
    ++g_mismatches;
    g_dead = true;
    Log("[SkyCloudTexels] RETIRED: %s at row %u column %u - the client left %u and "
        "this worked out %u. The transcription is not the client's arithmetic, and "
        "nothing built on it would be safe.", what, row, col, theirs, mine);
    Verdict::Add(Verdict::Bad, "SkyCloudTexels reproduced a cloud texel differently "
                 "from the client and retired itself for this session");
}

}  // namespace

// ---------------------------------------------------------------------------

bool Wanted() {
    return Config::g_settings.OptSkyCloudTexels && g_installed && !g_dead &&
           g_proven < kProve;
}

void Before(void* self) {
    g_snapValid = false;
    if (!Wanted() || !self) return;

    Snap s;
    s.rowsPass = RD32(self, kF_rowsPass);
    s.nextRow  = RD32(self, kF_nextRow);
    s.size     = RD32(self, kF_size);
    s.shift    = RD32(self, kF_shift);
    s.octaves  = RD32(self, kF_octaves);
    s.phase    = RD16(self, kF_phase);
    if (RD8(self, kF_rebuildAll)) { s.nextRow = 0; s.rowsPass = s.size; }

    const uint32_t tableIdx = RD8(self, kF_tableIdx);
    if (s.octaves == 0 || s.octaves > kMaxOct || s.size == 0 || s.size > kMaxSize ||
        s.shift < 7 || s.shift > kMaxShift || s.size > (1u << s.shift) ||
        s.rowsPass == 0 || s.nextRow + s.rowsPass > s.size || tableIdx > 31) {
        ++g_declined;
        return;
    }
    s.stride = 1u << s.shift;
    s.densByte = 0;   // set by the client before the loops; read in After
    for (uint32_t i = 0; i < s.octaves; ++i)
        s.freq[i] = *(const uint16_t*)(kFreqBase + 10 * tableIdx + 2 * i);

    s.rgba  = (uint8_t*)(uintptr_t)RD32(self, kF_rgba);
    s.cov   = (uint8_t*)(uintptr_t)RD32(self, kF_coverage);
    s.noise = (float*)(uintptr_t)RD32(self, kF_noiseRow);
    if (!s.rgba || !s.cov || !s.noise) { ++g_declined; return; }

    const size_t rgbaN  = (size_t)s.size * s.stride * 4;
    const size_t covN   = (size_t)s.size * s.stride;
    const size_t noiseN = (size_t)s.size * sizeof(float);
    if (!Reserve((void**)&g_sRgba, &g_sRgbaN, rgbaN) ||
        !Reserve((void**)&g_sCov, &g_sCovN, covN) ||
        !Reserve((void**)&g_sNoise, &g_sNoiseN, noiseN)) {
        ++g_declined;
        return;
    }

    // Only the rows this pass writes matter, but the untouched first column of a
    // zero-coverage row keeps what was already there, so the copy has to carry
    // it across.
    memcpy(g_sRgba + 4 * ((size_t)s.nextRow << s.shift),
           s.rgba + 4 * ((size_t)s.nextRow << s.shift),
           (size_t)s.rowsPass * s.stride * 4);
    memcpy(g_sCov + ((size_t)s.nextRow << s.shift),
           s.cov + ((size_t)s.nextRow << s.shift),
           (size_t)s.rowsPass * s.stride);
    memcpy(g_sNoise, s.noise, noiseN);

    g_snap = s;
    g_snapValid = true;
    g_snapSeq = g_lightSeq;
}

void After(void* self) {
    if (!g_snapValid || !self) return;
    g_snapValid = false;
    // The build calls sub_7EFAE0 exactly once, right before the loops. Anything
    // else means this pass did not reach them.
    if (g_lightSeq != g_snapSeq + 1) return;

    ++g_calls;
    Snap& s = g_snap;
    s.densByte = RD8(self, kF_densByte);

    Generate(s, g_light, g_sRgba, g_sCov, g_sNoise);

    for (uint32_t r = 0; r < s.rowsPass; ++r) {
        const size_t off = ((size_t)(s.nextRow + r) << s.shift);
        for (uint32_t x = 0; x < s.size; ++x) {
            if (g_sCov[off + x] != s.cov[off + x]) {
                Retire("the coverage byte", s.nextRow + r, x,
                       g_sCov[off + x], s.cov[off + x]);
                return;
            }
            for (int k = 0; k < 4; ++k) {
                const size_t b = 4 * (off + x) + k;
                if (g_sRgba[b] != s.rgba[b]) {
                    Retire(k == 3 ? "the alpha byte" : "a colour byte",
                           s.nextRow + r, x, g_sRgba[b], s.rgba[b]);
                    return;
                }
            }
        }
    }
    for (uint32_t x = 0; x < s.size; ++x) {
        if (memcmp(&g_sNoise[x], &s.noise[x], 4) != 0) {
            Retire("the stored noise row", s.nextRow, x,
                   F2Bits(g_sNoise[x]), F2Bits(s.noise[x]));
            return;
        }
    }
    ++g_proven;
    if (g_proven == kProve)
        Log("[SkyCloudTexels] %lu pass(es) reproduced byte for byte, including the "
            "noise row. The transcription is the client's arithmetic on this "
            "machine. Nothing further runs this session.", g_proven);
}

// ---------------------------------------------------------------------------

bool Init() {
    if (!Config::g_settings.OptSkyCloudTexels) return true;

    if (!WowOpt_ClientPatchAllowed((const void*)kLightParams)) {
        Log("[SkyCloudTexels] NOT active: No Client Patches is on, and the light "
            "parameters can only be read by hooking a function inside wow.exe.");
        return false;
    }
    if (WineSafe_CreateHook((void*)kLightParams, (void*)&LightDetour,
                            (void**)&g_origLight) != MH_OK) {
        Log("[SkyCloudTexels] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kLightParams);
        return false;
    }
    if (WO_EnableHook((void*)kLightParams) != MH_OK) {
        MH_RemoveHook((void*)kLightParams);
        Log("[SkyCloudTexels] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kLightParams);
        return false;
    }

    g_perm   = (const uint8_t*)kPerm;
    g_values = (const float*)kValues;
    g_fade   = (const float*)kFade;
    g_ramp   = (const uint8_t*)kRamp;
    g_k255  = (double)(*(const float*)kK255);
    g_k128  = (double)(*(const float*)kK128);
    g_k64   = (double)(*(const float*)kK64);
    g_kByte = (double)(*(const float*)kKByte);

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("SkyCloudTexels", (const void*)&Generate);

    Log("[SkyCloudTexels] ACTIVE, and it changes nothing. The texel loops of the "
        "cloud texture build are transcribed here and run a second time on their "
        "own copy of the buffers, and every byte is compared with what the client "
        "left. The first %lu pass(es) are checked and then it stops, because while "
        "it runs the work is done twice.", kProve);
    Log("[SkyCloudTexels]   constants read from the client: %.9g, %.9g, %.9g and "
        "%.9g for the byte scale.", g_k255, g_k128, g_k64, g_kByte);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kLightParams);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptSkyCloudTexels) return;
    if (!g_installed) {
        Log("[SkyCloudTexels] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[SkyCloudTexels] hooked, and no cloud texture pass has been compared. "
            "%lu pass(es) were handed back for a shape outside what this reads. If "
            "both numbers are zero, the sky build has not run.", g_declined);
        return;
    }
    if (g_mismatches) {
        Log("[SkyCloudTexels] %llu pass(es) compared, and one differed; the line "
            "naming the texel is earlier in this log. %lu pass(es) agreed before "
            "it.", g_calls, g_proven);
        return;
    }
    Log("[SkyCloudTexels] %llu pass(es) compared, %lu agreed byte for byte, %lu "
        "handed back for a shape outside what this reads. Plain counters, lower "
        "bounds.", g_calls, g_proven, g_declined);
    if (g_proven < kProve)
        Log("[SkyCloudTexels]   %lu of %lu still to go before this stops working.",
            g_proven, kProve);
}

}  // namespace SkyCloudTexels
