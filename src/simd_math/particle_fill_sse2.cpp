// ============================================================================
// The per-particle vertex fill in the client's particle emitter, sub_6C4440
// (Particle_FillVertices, 2.5% of self time in the corrected combat profile and
// claimed by nothing in this project until now).
//
// The function is __thiscall with nine stack arguments and ends in `retn 24h`:
//
//   ecx      the emitter: +0Ch particle count, +10h particles (20 bytes each),
//            +1Ch colour count, +20h colours (4 bytes each)
//   arg_0    destination, 24 bytes a vertex
//   arg_4    the single colour used when the emitter has no colour array
//   arg_8    UV scroll pair, read only by the wrapping path
//   arg_C    a colour, read only by the wrapping path
//   arg_10   the world offset added to every particle position
//   arg_14   low byte set: the wrapping path with floor() and a colour convert
//   arg_18   low byte, read only by the wrapping path
//   arg_1C   first particle
//   arg_20   particles wanted
//
// It has two loops. The one taken when arg_14's low byte is clear and the
// global byte at 0x00C7D2E4 is zero writes, for each particle:
//
//   +0   x, y, z       fld [arg_10+k] / fadd [particle+k] / fstp
//   +12  colour        the colour array entry or arg_4, red and blue swapped
//                      when the device asks for it
//   +16  two dwords    copied from particle +12 and +16
//
// and for the colour swap it calls sub_532AF0 once per particle, which is
// `lea eax, [ecx+214h]; retn` on the global device, to read one flag at +14h
// that nothing in the loop can change. The same getter the UI batch fill paid
// per vertex.
//
// Exactness. The only arithmetic is one addition of two single-precision
// values per coordinate. The exact sum of two singles is representable in a
// double, so the x87 at 53 bits holds it exactly and rounds once on the store,
// and an SSE single addition is correctly rounded from the same exact sum:
// the same bits whatever the x87 precision control is set to. The two can
// differ only when both operands are NaN, where x87 and SSE pick different
// payloads; that is not argued away either, the verification below compares.
//
// What this replaces and what it leaves. The hook takes the whole function.
// When the call is the common case above it does the clamping the client does
// (start below the count, count cut to what is left, a colour array used only
// when its count equals the particle count) and fills the vertices itself;
// every other call, and every call while this is learning, runs the client's
// own code.
//
// Verification, predict-then-compare. For the first 4096 fills and one in 1024
// after that, the fill is computed into a private buffer, the client's function
// runs and writes the real one, and the two are compared byte for byte. The
// first difference retires this for the session and logs where it was.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <emmintrin.h>

#include "particle_fill_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace ParticleFill {

namespace {

constexpr uintptr_t kTarget    = 0x006C4440;
constexpr uintptr_t kGateByte  = 0x00C7D2E4;
constexpr uintptr_t kDevicePtr = 0x00C5DF88;

// push ebp / mov ebp,esp / sub esp,0ECh / push edi / mov edi,[ebp+28h] /
// test edi,edi / mov [ebp-10h],ecx - the first bytes this was read from.
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xEC, 0x00, 0x00,
    0x00, 0x57, 0x8B, 0x7D, 0x28, 0x85, 0xFF, 0x89
};

// Emitter fields.
constexpr unsigned kE_count       = 0x0C;
constexpr unsigned kE_particles   = 0x10;
constexpr unsigned kE_colourCount = 0x1C;
constexpr unsigned kE_colours     = 0x20;

// No emitter in a measured session is known to be this large; a larger call is
// left to the client while learning rather than overrun the buffer.
constexpr uint32_t kMaxParticles = 0x8000;
constexpr uint32_t kLearnFills   = 4096;
constexpr uint32_t kResampleMask = 1023;

typedef void (__fastcall *FillFn)(void* self, void* edx, uint8_t* dst,
                                  const uint8_t* colour, const float* scroll,
                                  const uint8_t* colour2, const float* offset,
                                  uint32_t wrap, uint32_t arg18, int32_t start,
                                  int32_t count);
FillFn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;
uint8_t* g_scratch = nullptr;

// Main thread only, so plain. Lower bounds if that ever stops being true.
unsigned long long g_calls = 0;
unsigned long long g_filled = 0;
unsigned long long g_particles = 0;
unsigned long long g_wrapPath = 0;
unsigned long long g_gatePath = 0;
unsigned long long g_control = 0;
unsigned long long g_tooLarge = 0;
unsigned g_verified = 0;
unsigned g_mismatches = 0;
uint32_t g_largest = 0;

struct Plan {
    const uint8_t* src;
    const uint8_t* colour;
    size_t         colourStride;
    int32_t        count;
};

// The clamping at 0x006C461F, in the client's signed comparisons.
bool MakePlan(void* self, const uint8_t* colour, int32_t start, int32_t count,
              Plan* out) {
    if (count == 0) return false;
    const uintptr_t e = (uintptr_t)self;
    const int32_t n = *(const int32_t*)(e + kE_count);
    if (n == 0) return false;
    if (start >= n) return false;
    const int32_t avail = n - start;
    if (count >= avail) count = avail;
    if (count <= 0) return false;

    const int32_t colourCount = *(const int32_t*)(e + kE_colourCount);
    out->colour = colour;
    out->colourStride = 0;
    if (colourCount != 0 && colourCount == n) {
        out->colour = *(const uint8_t* const*)(e + kE_colours) + (intptr_t)start * 4;
        out->colourStride = 4;
    }
    out->src = *(const uint8_t* const*)(e + kE_particles) + (intptr_t)start * 20;
    out->count = count;
    return true;
}

void Fill(const Plan& p, const float* offset, uint8_t* dst) {
    const uintptr_t device = *(const uintptr_t*)kDevicePtr;
    const bool swap = *(const int32_t*)(device + 0x214 + 0x14) == 1;
    const __m128 off = _mm_setr_ps(offset[0], offset[1], offset[2], 0.0f);
    const uint8_t* src = p.src;
    const uint8_t* col = p.colour;
    for (int32_t i = 0; i < p.count; ++i, src += 20, dst += 24, col += p.colourStride) {
        // The fourth lane is the first UV dword; it is overwritten by the
        // colour just below, so what the addition does to it is discarded.
        _mm_storeu_ps((float*)dst, _mm_add_ps(_mm_loadu_ps((const float*)src), off));
        uint32_t c;
        memcpy(&c, col, 4);
        if (swap) c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
        memcpy(dst + 12, &c, 4);
        memcpy(dst + 16, src + 12, 8);
    }
}

// Out of line on purpose: its message buffer would otherwise put a stack
// cookie check into the prologue of the hook, which runs for every fill.
__declspec(noinline) void Compare(const Plan& p, const uint8_t* client) {
    for (int32_t i = 0; i < p.count; ++i) {
        const uint8_t* a = client + 24 * (size_t)i;
        const uint8_t* b = g_scratch + 24 * (size_t)i;
        if (memcmp(a, b, 24) == 0) continue;
        char what[160] = "";
        for (int f = 0; f < 24; f += 4) {
            uint32_t ca, cb;
            memcpy(&ca, a + f, 4);
            memcpy(&cb, b + f, 4);
            if (ca != cb) {
                _snprintf(what, sizeof(what) - 1, "particle %d of %d, bytes +%d: "
                          "client %08X, prediction %08X", i, p.count, f, ca, cb);
                break;
            }
        }
        what[sizeof(what) - 1] = '\0';
        ++g_mismatches;
        g_dead = true;
        Log("[ParticleFill] DISABLED for this session: a fill came out differently "
            "from the client's own loop - %s. The client's bytes are the ones in the "
            "buffer; every fill from here on is left to the client.", what);
        Verdict::Add(Verdict::Bad, "ParticleFill predicted particle vertices that "
                     "differed from what the client wrote and retired itself for this "
                     "session");
        return;
    }
    ++g_verified;
}

void __fastcall Hooked_Fill(void* self, void* edx, uint8_t* dst, const uint8_t* colour,
                            const float* scroll, const uint8_t* colour2,
                            const float* offset, uint32_t wrap, uint32_t arg18,
                            int32_t start, int32_t count) {
    ++g_calls;
    if (g_dead || (wrap & 0xFF) != 0 || *(const uint8_t*)kGateByte != 0) {
        if (!g_dead) {
            if ((wrap & 0xFF) != 0) ++g_wrapPath;
            else ++g_gatePath;
        }
        g_orig(self, edx, dst, colour, scroll, colour2, offset, wrap, arg18, start, count);
        return;
    }
    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        g_orig(self, edx, dst, colour, scroll, colour2, offset, wrap, arg18, start, count);
        return;
    }

    Plan p;
    if (!MakePlan(self, colour, start, count, &p)) {
        // The client's own function would write nothing either.
        return;
    }
    if ((uint32_t)p.count > g_largest) g_largest = (uint32_t)p.count;

    const bool learning = g_verified < kLearnFills ||
                          ((uint32_t)g_calls & kResampleMask) == 0;
    if (learning) {
        if ((uint32_t)p.count > kMaxParticles) {
            ++g_tooLarge;
            g_orig(self, edx, dst, colour, scroll, colour2, offset, wrap, arg18, start, count);
            return;
        }
        Fill(p, offset, g_scratch);
        g_orig(self, edx, dst, colour, scroll, colour2, offset, wrap, arg18, start, count);
        Compare(p, dst);
        return;
    }

    Fill(p, offset, dst);
    ++g_filled;
    g_particles += (unsigned long long)p.count;
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int n) {
    __try {
        return memcmp((const void*)addr, want, (size_t)n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptParticleFill) return true;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[ParticleFill] NOT active: the bytes at 0x%08X are not the particle fill "
            "this was read from, so nothing was hooked.", (unsigned)kTarget);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[ParticleFill] NOT active: No Client Patches is on, and this hooks a "
            "function inside wow.exe.");
        return false;
    }

    // Without the scratch the verification cannot run, and without the
    // verification this does not hook.
    g_scratch = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)kMaxParticles * 24,
                                       MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                       PAGE_READWRITE);
    if (!g_scratch) {
        Log("[ParticleFill] NOT active: the %u KB verification buffer could not be "
            "committed, error %lu", (unsigned)((SIZE_T)kMaxParticles * 24 / 1024),
            GetLastError());
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hooked_Fill, (void**)&g_orig) != MH_OK) {
        Log("[ParticleFill] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kTarget);
        return false;
    }
    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[ParticleFill] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kTarget);
        return false;
    }
    g_installed = true;
    g_abSubject = AbTest::IsSubject("ParticleFill", &g_abSubject);

    Log("[ParticleFill] ACTIVE on the particle vertex fill in sub_6C4440 (2.5%% of "
        "self time in the corrected combat profile). A getter call and a branch per "
        "particle become one read per fill, and the position add is one SSE addition. "
        "The wrapping path and the transformed path stay with the client. The first "
        "%u fills, and one in %u after, are predicted and compared byte for byte "
        "with what the client writes; the first difference switches this off.",
        kLearnFills, kResampleMask + 1);
    if (g_abSubject)
        Log("[ParticleFill]   under A/B test: the control half calls the client's "
            "function through the same hook, so both halves pay for the hook and only "
            "the fill differs.");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptParticleFill) return;
    if (!g_installed) {
        Log("[ParticleFill] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[ParticleFill] hooked, and no particle fill has been reached yet. That is "
            "a measurement: no emitter has drawn since it went in.");
        return;
    }
    Log("[ParticleFill] %llu fill(s) reached the hook; %llu filled here (%llu particles); "
        "largest %u particles. %llu took the wrapping path and %llu the transformed "
        "path, both left to the client. Plain counters, lower bounds.",
        g_calls, g_filled, g_particles, g_largest, g_wrapPath, g_gatePath);
    if (g_mismatches) {
        Log("[ParticleFill]   DISABLED after a difference from the client's own output; "
            "the line that says where is earlier in this log.");
    } else if (g_verified < kLearnFills) {
        Log("[ParticleFill]   %u of %u fills verified against the client so far; still "
            "leaving every fill to the client and comparing.", g_verified, kLearnFills);
    } else {
        Log("[ParticleFill]   %u fills verified byte for byte against the client, none "
            "differed; one in %u is still compared.", g_verified, kResampleMask + 1);
    }
    if (g_tooLarge)
        Log("[ParticleFill]   %llu fill(s) were larger than the verification buffer and "
            "were left to the client.", g_tooLarge);
    if (g_abSubject)
        Log("[ParticleFill]   %llu fill(s) went through the client's function as the A/B "
            "control half.", g_control);
}

}  // namespace ParticleFill
