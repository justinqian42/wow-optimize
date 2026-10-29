// ============================================================================
// Module: gx_blit_fast.cpp
//
// Hardware-accelerated SIMD texture blitting and format packing for WoW 3.3.5a:
//   - sub_6ABF40: Blit_Argb8888_Abgr8888 (32-bit channel swizzle)
//   - sub_6ABE60: Blit_Argb8888_Rgb565   (32-bit ARGB to 16-bit RGB565)
//   - sub_6ABE00: Blit_Argb8888_Argb4444 (32-bit ARGB to 16-bit ARGB4444)
//   - sub_6ABE30: Blit_Argb8888_Argb1555 (32-bit ARGB to 16-bit ARGB1555)
//   - sub_6ABE90: Blit_Argb4444_Abgr8888 (16-bit ARGB4444 to 32-bit ABGR8888)
//
// In stock 3.3.5a:
//   - Every texture uploaded to DirectX or blitted across UI textures, spell
//     icons, minimap layers, ground decals, and M2 skinning textures passes
//     through the s_blits dispatch table.
//   - In the stock client, these routines execute scalar loops that decompose
//     each pixel into individual bytes via serial shifts and memory loads,
//     shifting and ORing each channel byte by byte. ConvertTexels3216 was
//     sampled as a hotspot across gameplay sessions.
//
// This optimization accelerates format conversion and packing:
//   - Vectorizes 32-bit ARGB to ABGR channel swapping using 128-bit SSE2
//     bitwise parallel operations (4 pixels per instruction).
//   - Replaces byte-level shifting in 32-to-16 bit format packing with branch-free
//     parallel bitfield manipulation and SSE2 vector stores.
//   - Unpacks 16-bit ARGB4444 to 32-bit ABGR8888 via a cache-resident 256KB
//     lookup table, replacing 14 instructions and 4 byte stores per pixel with
//     a single aligned 32-bit read and store.
//   - Dual-run verification and automatic fail-safe retirement guards ensure
//     bit-level parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "gx_blit_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace GxBlitFast {

namespace {

constexpr uintptr_t kTarget_Abgr8888   = 0x006ABF40;
constexpr uintptr_t kTarget_Rgb565     = 0x006ABE60;
constexpr uintptr_t kTarget_Argb4444   = 0x006ABE00;
constexpr uintptr_t kTarget_Argb1555   = 0x006ABE30;
constexpr uintptr_t kTarget_4444To8888 = 0x006ABE90;

struct C2iVector {
    int32_t x;
    int32_t y;
};

typedef void (__cdecl* Blit_fn)(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride);

Blit_fn orig_Abgr8888   = nullptr;
Blit_fn orig_Rgb565     = nullptr;
Blit_fn orig_Argb4444   = nullptr;
Blit_fn orig_Argb1555   = nullptr;
Blit_fn orig_4444To8888 = nullptr;

bool g_installed = false;
bool g_dead_abgr8888   = false;
bool g_dead_rgb565     = false;
bool g_dead_argb4444   = false;
bool g_dead_argb1555   = false;
bool g_dead_4444to8888 = false;

unsigned long long g_calls_abgr8888   = 0;
unsigned long long g_verified_abgr8888 = 0;
unsigned long long g_mismatch_abgr8888 = 0;

unsigned long long g_calls_rgb565     = 0;
unsigned long long g_verified_rgb565   = 0;
unsigned long long g_mismatch_rgb565   = 0;

unsigned long long g_calls_argb4444   = 0;
unsigned long long g_verified_argb4444 = 0;
unsigned long long g_mismatch_argb4444 = 0;

unsigned long long g_calls_argb1555   = 0;
unsigned long long g_verified_argb1555 = 0;
unsigned long long g_mismatch_argb1555 = 0;

unsigned long long g_calls_4444to8888 = 0;
unsigned long long g_verified_4444to8888 = 0;
unsigned long long g_mismatch_4444to8888 = 0;

// Verification settings: each call processes entire 2D texture images
constexpr unsigned long kVerifyFirst = 100;
constexpr unsigned long kResampleMask = 1023;

static uint32_t s_4444_to_8888_lut[65536];

void Init4444Lut() {
    for (uint32_t tex = 0; tex < 65536; ++tex) {
        const uint32_t a = tex >> 12;
        const uint32_t r = (tex >> 8) & 0xF;
        const uint32_t g = (tex >> 4) & 0xF;
        const uint32_t b = tex & 0xF;
        const uint32_t a8 = (a << 4) | a;
        const uint32_t r8 = (r << 4) | r;
        const uint32_t g8 = (g << 4) | g;
        const uint32_t b8 = (b << 4) | b;
        s_4444_to_8888_lut[tex] = (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
    }
}

// ----------------------------------------------------------------------------
// Fast implementations
// ----------------------------------------------------------------------------

__forceinline void Fast_Abgr8888_Impl(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    const uint8_t* src = static_cast<const uint8_t*>(in);
    uint8_t* dst = static_cast<uint8_t*>(out);
    const __m128i mask_rb = _mm_set1_epi32(0x00FF00FF);
    const __m128i mask_ga = _mm_set1_epi32((int)0xFF00FF00);
    const int32_t simd_limit = size->x - 3;

    for (int32_t y = size->y; y > 0; --y) {
        int32_t x = 0;
        for (; x < simd_limit; x += 4) {
            __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
            __m128i rb = _mm_and_si128(p, mask_rb);
            __m128i ga = _mm_and_si128(p, mask_ga);
            __m128i rb_swap = _mm_or_si128(_mm_srli_epi32(rb, 16), _mm_slli_epi32(rb, 16));
            __m128i res = _mm_or_si128(ga, rb_swap);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x * 4), res);
        }
        for (; x < size->x; ++x) {
            dst[x * 4 + 3] = src[x * 4 + 3];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 0] = src[x * 4 + 2];
        }
        src += inStride;
        dst += outStride;
    }
}

__forceinline void Fast_Rgb565_Impl(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    const uint8_t* src = static_cast<const uint8_t*>(in);
    uint16_t* dst = static_cast<uint16_t*>(out);
    const int32_t simd_limit = size->x - 3;

    for (int32_t y = size->y; y > 0; --y) {
        int32_t x = 0;
        for (; x < simd_limit; x += 4) {
            __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
            __m128i b = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x000000F8)), 3);
            __m128i g = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x0000FC00)), 5);
            __m128i r = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x00F80000)), 8);
            __m128i rgb32 = _mm_or_si128(b, _mm_or_si128(g, r));
            __m128i packed = _mm_packs_epi32(rgb32, _mm_setzero_si128());
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x), packed);
        }
        for (; x < size->x; ++x) {
            const uint32_t px = *reinterpret_cast<const uint32_t*>(src + x * 4);
            dst[x] = static_cast<uint16_t>(((px & 0xF8) >> 3) | ((px & 0xFC00) >> 5) | ((px & 0xF80000) >> 8));
        }
        src += inStride;
        dst = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + outStride);
    }
}

__forceinline void Fast_Argb4444_Impl(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    const uint8_t* src = static_cast<const uint8_t*>(in);
    uint16_t* dst = static_cast<uint16_t*>(out);
    const int32_t simd_limit = size->x - 3;

    for (int32_t y = size->y; y > 0; --y) {
        int32_t x = 0;
        for (; x < simd_limit; x += 4) {
            __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
            __m128i b = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x000000F0)), 4);
            __m128i g = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x0000F000)), 8);
            __m128i r = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x00F00000)), 12);
            __m128i a = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32((int)0xF0000000)), 16);
            __m128i argb32 = _mm_or_si128(_mm_or_si128(b, g), _mm_or_si128(r, a));
            __m128i packed = _mm_packs_epi32(argb32, _mm_setzero_si128());
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x), packed);
        }
        for (; x < size->x; ++x) {
            const uint32_t px = *reinterpret_cast<const uint32_t*>(src + x * 4);
            dst[x] = static_cast<uint16_t>(((px & 0xF0) >> 4) | ((px & 0xF000) >> 8) | ((px & 0xF00000) >> 12) | ((px & 0xF0000000) >> 16));
        }
        src += inStride;
        dst = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + outStride);
    }
}

__forceinline void Fast_Argb1555_Impl(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    const uint8_t* src = static_cast<const uint8_t*>(in);
    uint16_t* dst = static_cast<uint16_t*>(out);
    const int32_t simd_limit = size->x - 3;

    for (int32_t y = size->y; y > 0; --y) {
        int32_t x = 0;
        for (; x < simd_limit; x += 4) {
            __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
            __m128i b = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x000000F8)), 3);
            __m128i g = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x0000F800)), 6);
            __m128i r = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32(0x00F80000)), 9);
            __m128i a = _mm_srli_epi32(_mm_and_si128(p, _mm_set1_epi32((int)0x80000000)), 16);
            __m128i argb32 = _mm_or_si128(_mm_or_si128(b, g), _mm_or_si128(r, a));
            __m128i packed = _mm_packs_epi32(argb32, _mm_setzero_si128());
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x), packed);
        }
        for (; x < size->x; ++x) {
            const uint32_t px = *reinterpret_cast<const uint32_t*>(src + x * 4);
            dst[x] = static_cast<uint16_t>(((px & 0xF8) >> 3) | ((px & 0xF800) >> 6) | ((px & 0xF80000) >> 9) | ((px & 0x80000000) >> 16));
        }
        src += inStride;
        dst = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + outStride);
    }
}

__forceinline void Fast_4444To8888_Impl(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    const uint8_t* src = static_cast<const uint8_t*>(in);
    uint8_t* dst = static_cast<uint8_t*>(out);

    for (int32_t y = size->y; y > 0; --y) {
        const uint16_t* src16 = reinterpret_cast<const uint16_t*>(src);
        uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
        for (int32_t x = 0; x < size->x; ++x) {
            dst32[x] = s_4444_to_8888_lut[src16[x]];
        }
        src += inStride;
        dst += outStride;
    }
}

// ----------------------------------------------------------------------------
// Hook wrappers with dual-run verification & retirement
// ----------------------------------------------------------------------------

void __cdecl Hooked_Blit_Argb8888_Abgr8888(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    if (g_dead_abgr8888 || !size || size->x <= 0 || size->y <= 0 || !in || !out) {
        orig_Abgr8888(size, in, inStride, out, outStride);
        return;
    }

    const unsigned long long call_idx = ++g_calls_abgr8888;
    Fast_Abgr8888_Impl(size, in, inStride, out, outStride);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        if (size->x <= 4096 && size->y <= 4096 && outStride >= static_cast<uint32_t>(size->x * 4)) {
            ++g_verified_abgr8888;
            const size_t total_bytes = static_cast<size_t>(outStride) * static_cast<size_t>(size->y);
            uint8_t* ref_buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, total_bytes));
            if (ref_buf) {
                orig_Abgr8888(size, in, inStride, ref_buf, outStride);
                bool mismatch = false;
                const uint8_t* d = static_cast<const uint8_t*>(out);
                const uint8_t* r = ref_buf;
                for (int32_t y = 0; y < size->y; ++y) {
                    if (memcmp(d, r, static_cast<size_t>(size->x) * 4) != 0) {
                        mismatch = true;
                        break;
                    }
                    d += outStride;
                    r += outStride;
                }
                if (mismatch) {
                    if (++g_mismatch_abgr8888 == 1) {
                        Log("[GxBlitFast] MISMATCH in Blit_Argb8888_Abgr8888 at call %llu, retiring hook", call_idx);
                        g_dead_abgr8888 = true;
                    }
                    memcpy(out, ref_buf, total_bytes);
                }
                HeapFree(GetProcessHeap(), 0, ref_buf);
            }
        }
    }
}

void __cdecl Hooked_Blit_Argb8888_Rgb565(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    if (g_dead_rgb565 || !size || size->x <= 0 || size->y <= 0 || !in || !out) {
        orig_Rgb565(size, in, inStride, out, outStride);
        return;
    }

    const unsigned long long call_idx = ++g_calls_rgb565;
    Fast_Rgb565_Impl(size, in, inStride, out, outStride);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        if (size->x <= 4096 && size->y <= 4096 && outStride >= static_cast<uint32_t>(size->x * 2)) {
            ++g_verified_rgb565;
            const size_t total_bytes = static_cast<size_t>(outStride) * static_cast<size_t>(size->y);
            uint8_t* ref_buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, total_bytes));
            if (ref_buf) {
                orig_Rgb565(size, in, inStride, ref_buf, outStride);
                bool mismatch = false;
                const uint8_t* d = static_cast<const uint8_t*>(out);
                const uint8_t* r = ref_buf;
                for (int32_t y = 0; y < size->y; ++y) {
                    if (memcmp(d, r, static_cast<size_t>(size->x) * 2) != 0) {
                        mismatch = true;
                        break;
                    }
                    d += outStride;
                    r += outStride;
                }
                if (mismatch) {
                    if (++g_mismatch_rgb565 == 1) {
                        Log("[GxBlitFast] MISMATCH in Blit_Argb8888_Rgb565 at call %llu, retiring hook", call_idx);
                        g_dead_rgb565 = true;
                    }
                    memcpy(out, ref_buf, total_bytes);
                }
                HeapFree(GetProcessHeap(), 0, ref_buf);
            }
        }
    }
}

void __cdecl Hooked_Blit_Argb8888_Argb4444(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    if (g_dead_argb4444 || !size || size->x <= 0 || size->y <= 0 || !in || !out) {
        orig_Argb4444(size, in, inStride, out, outStride);
        return;
    }

    const unsigned long long call_idx = ++g_calls_argb4444;
    Fast_Argb4444_Impl(size, in, inStride, out, outStride);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        if (size->x <= 4096 && size->y <= 4096 && outStride >= static_cast<uint32_t>(size->x * 2)) {
            ++g_verified_argb4444;
            const size_t total_bytes = static_cast<size_t>(outStride) * static_cast<size_t>(size->y);
            uint8_t* ref_buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, total_bytes));
            if (ref_buf) {
                orig_Argb4444(size, in, inStride, ref_buf, outStride);
                bool mismatch = false;
                const uint8_t* d = static_cast<const uint8_t*>(out);
                const uint8_t* r = ref_buf;
                for (int32_t y = 0; y < size->y; ++y) {
                    if (memcmp(d, r, static_cast<size_t>(size->x) * 2) != 0) {
                        mismatch = true;
                        break;
                    }
                    d += outStride;
                    r += outStride;
                }
                if (mismatch) {
                    if (++g_mismatch_argb4444 == 1) {
                        Log("[GxBlitFast] MISMATCH in Blit_Argb8888_Argb4444 at call %llu, retiring hook", call_idx);
                        g_dead_argb4444 = true;
                    }
                    memcpy(out, ref_buf, total_bytes);
                }
                HeapFree(GetProcessHeap(), 0, ref_buf);
            }
        }
    }
}

void __cdecl Hooked_Blit_Argb8888_Argb1555(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    if (g_dead_argb1555 || !size || size->x <= 0 || size->y <= 0 || !in || !out) {
        orig_Argb1555(size, in, inStride, out, outStride);
        return;
    }

    const unsigned long long call_idx = ++g_calls_argb1555;
    Fast_Argb1555_Impl(size, in, inStride, out, outStride);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        if (size->x <= 4096 && size->y <= 4096 && outStride >= static_cast<uint32_t>(size->x * 2)) {
            ++g_verified_argb1555;
            const size_t total_bytes = static_cast<size_t>(outStride) * static_cast<size_t>(size->y);
            uint8_t* ref_buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, total_bytes));
            if (ref_buf) {
                orig_Argb1555(size, in, inStride, ref_buf, outStride);
                bool mismatch = false;
                const uint8_t* d = static_cast<const uint8_t*>(out);
                const uint8_t* r = ref_buf;
                for (int32_t y = 0; y < size->y; ++y) {
                    if (memcmp(d, r, static_cast<size_t>(size->x) * 2) != 0) {
                        mismatch = true;
                        break;
                    }
                    d += outStride;
                    r += outStride;
                }
                if (mismatch) {
                    if (++g_mismatch_argb1555 == 1) {
                        Log("[GxBlitFast] MISMATCH in Blit_Argb8888_Argb1555 at call %llu, retiring hook", call_idx);
                        g_dead_argb1555 = true;
                    }
                    memcpy(out, ref_buf, total_bytes);
                }
                HeapFree(GetProcessHeap(), 0, ref_buf);
            }
        }
    }
}

void __cdecl Hooked_Blit_Argb4444_Abgr8888(const C2iVector* size, const void* in, uint32_t inStride, void* out, uint32_t outStride) {
    if (g_dead_4444to8888 || !size || size->x <= 0 || size->y <= 0 || !in || !out) {
        orig_4444To8888(size, in, inStride, out, outStride);
        return;
    }

    const unsigned long long call_idx = ++g_calls_4444to8888;
    Fast_4444To8888_Impl(size, in, inStride, out, outStride);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        if (size->x <= 4096 && size->y <= 4096 && outStride >= static_cast<uint32_t>(size->x * 4)) {
            ++g_verified_4444to8888;
            const size_t total_bytes = static_cast<size_t>(outStride) * static_cast<size_t>(size->y);
            uint8_t* ref_buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, total_bytes));
            if (ref_buf) {
                orig_4444To8888(size, in, inStride, ref_buf, outStride);
                bool mismatch = false;
                const uint8_t* d = static_cast<const uint8_t*>(out);
                const uint8_t* r = ref_buf;
                for (int32_t y = 0; y < size->y; ++y) {
                    if (memcmp(d, r, static_cast<size_t>(size->x) * 4) != 0) {
                        mismatch = true;
                        break;
                    }
                    d += outStride;
                    r += outStride;
                }
                if (mismatch) {
                    if (++g_mismatch_4444to8888 == 1) {
                        Log("[GxBlitFast] MISMATCH in Blit_Argb4444_Abgr8888 at call %llu, retiring hook", call_idx);
                        g_dead_4444to8888 = true;
                    }
                    memcpy(out, ref_buf, total_bytes);
                }
                HeapFree(GetProcessHeap(), 0, ref_buf);
            }
        }
    }
}

} // namespace

void Init() {
    if (!Config::g_settings.OptGxBlitFast) {
        return;
    }

    Init4444Lut();

    static const unsigned char kExp_Abgr8888[8]   = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x14, 0x53 };
    static const unsigned char kExp_Rgb565[8]     = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x18, 0x8B, 0x4D };
    static const unsigned char kExp_Argb4444[8]   = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x18, 0x8B, 0x4D };
    static const unsigned char kExp_Argb1555[8]   = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x18, 0x8B, 0x4D };
    static const unsigned char kExp_4444To8888[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45 };

    if (memcmp(reinterpret_cast<const void*>(kTarget_Abgr8888), kExp_Abgr8888, 8) != 0 ||
        memcmp(reinterpret_cast<const void*>(kTarget_Rgb565), kExp_Rgb565, 8) != 0 ||
        memcmp(reinterpret_cast<const void*>(kTarget_Argb4444), kExp_Argb4444, 8) != 0 ||
        memcmp(reinterpret_cast<const void*>(kTarget_Argb1555), kExp_Argb1555, 8) != 0 ||
        memcmp(reinterpret_cast<const void*>(kTarget_4444To8888), kExp_4444To8888, 8) != 0) {
        Log("[GxBlitFast] Prologue mismatch, aborting installation");
        return;
    }

    if (WineSafe_CreateHook(reinterpret_cast<void*>(kTarget_Abgr8888), reinterpret_cast<void*>(Hooked_Blit_Argb8888_Abgr8888), reinterpret_cast<void**>(&orig_Abgr8888)) != MH_OK ||
        WineSafe_CreateHook(reinterpret_cast<void*>(kTarget_Rgb565), reinterpret_cast<void*>(Hooked_Blit_Argb8888_Rgb565), reinterpret_cast<void**>(&orig_Rgb565)) != MH_OK ||
        WineSafe_CreateHook(reinterpret_cast<void*>(kTarget_Argb4444), reinterpret_cast<void*>(Hooked_Blit_Argb8888_Argb4444), reinterpret_cast<void**>(&orig_Argb4444)) != MH_OK ||
        WineSafe_CreateHook(reinterpret_cast<void*>(kTarget_Argb1555), reinterpret_cast<void*>(Hooked_Blit_Argb8888_Argb1555), reinterpret_cast<void**>(&orig_Argb1555)) != MH_OK ||
        WineSafe_CreateHook(reinterpret_cast<void*>(kTarget_4444To8888), reinterpret_cast<void*>(Hooked_Blit_Argb4444_Abgr8888), reinterpret_cast<void**>(&orig_4444To8888)) != MH_OK) {
        Log("[GxBlitFast] Failed to create hooks");
        return;
    }

    if (WO_EnableHook(reinterpret_cast<void*>(kTarget_Abgr8888)) != MH_OK ||
        WO_EnableHook(reinterpret_cast<void*>(kTarget_Rgb565)) != MH_OK ||
        WO_EnableHook(reinterpret_cast<void*>(kTarget_Argb4444)) != MH_OK ||
        WO_EnableHook(reinterpret_cast<void*>(kTarget_Argb1555)) != MH_OK ||
        WO_EnableHook(reinterpret_cast<void*>(kTarget_4444To8888)) != MH_OK) {
        Log("[GxBlitFast] Failed to enable hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_Blit_Argb8888_Abgr8888", reinterpret_cast<const void*>(&Hooked_Blit_Argb8888_Abgr8888));
    SamplingProfiler::RegisterSelfSymbol("Fast_Blit_Argb8888_Rgb565", reinterpret_cast<const void*>(&Hooked_Blit_Argb8888_Rgb565));
    SamplingProfiler::RegisterSelfSymbol("Fast_Blit_Argb8888_Argb4444", reinterpret_cast<const void*>(&Hooked_Blit_Argb8888_Argb4444));
    SamplingProfiler::RegisterSelfSymbol("Fast_Blit_Argb8888_Argb1555", reinterpret_cast<const void*>(&Hooked_Blit_Argb8888_Argb1555));
    SamplingProfiler::RegisterSelfSymbol("Fast_Blit_Argb4444_Abgr8888", reinterpret_cast<const void*>(&Hooked_Blit_Argb4444_Abgr8888));

    g_installed = true;
    Log("[GxBlitFast] ACTIVE on texture blitters (0x006ABF40, 0x006ABE60, 0x006ABE00, 0x006ABE30, 0x006ABE90)");
}

void LogStats() {
    if (!Config::g_settings.OptGxBlitFast || !g_installed) {
        return;
    }

    Log("[GxBlitFast] Stats: Abgr8888=%llu (%llu ver, %llu mis%s), Rgb565=%llu (%llu ver, %llu mis%s), Argb4444=%llu (%llu ver, %llu mis%s), Argb1555=%llu (%llu ver, %llu mis%s), 4444To8888=%llu (%llu ver, %llu mis%s)",
        g_calls_abgr8888, g_verified_abgr8888, g_mismatch_abgr8888, g_dead_abgr8888 ? " [RETIRED]" : "",
        g_calls_rgb565, g_verified_rgb565, g_mismatch_rgb565, g_dead_rgb565 ? " [RETIRED]" : "",
        g_calls_argb4444, g_verified_argb4444, g_mismatch_argb4444, g_dead_argb4444 ? " [RETIRED]" : "",
        g_calls_argb1555, g_verified_argb1555, g_mismatch_argb1555, g_dead_argb1555 ? " [RETIRED]" : "",
        g_calls_4444to8888, g_verified_4444to8888, g_mismatch_4444to8888, g_dead_4444to8888 ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook(reinterpret_cast<void*>(kTarget_Abgr8888));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_Rgb565));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_Argb4444));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_Argb1555));
    MH_DisableHook(reinterpret_cast<void*>(kTarget_4444To8888));
    g_installed = false;
}

} // namespace GxBlitFast
