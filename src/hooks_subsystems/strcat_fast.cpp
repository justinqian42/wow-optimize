#include "strcat_fast.h"
#include "version.h"
#include "MinHook.h"
#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <cstdint>
#include <atomic>

extern "C" void Log(const char* fmt, ...);

#if TEST_DISABLE_STRCAT_FAST == 0

static std::atomic<uint64_t> g_fast_paths{0};
static std::atomic<uint64_t> g_fallback_paths{0};

// Original: int __stdcall sub_76ED20(char* dst, const char* src, int maxlen)
typedef int (__stdcall *StrncpyLike_fn)(char* dst, const char* src, int maxlen);
static StrncpyLike_fn orig_Sub76ED20 = nullptr;

// Check if an SSE2 16-byte load starting at `addr` is entirely within one
// 4KB page (prevents fault when the next page is unmapped).
static inline bool PageSafe16(uintptr_t addr) {
    return (addr & 0xFFF) <= 0xFF0; // bytes 0xFF0-0xFFF are within page
}

// SSE2 16-byte copy chunk: loads from src, stores to dst.
// Both src and dst are assumed to have at least 16 readable/writable bytes.
static inline void Copy16(void* dst, const void* src) {
    __m128i v = _mm_loadu_si128((const __m128i*)src);
    _mm_storeu_si128((__m128i*)dst, v);
}

// Fast SSE2 copy loop for bulk data. src and dst must have >= copy_len bytes.
static inline void Sse2Copy(char* dst, const char* src, size_t copy_len) {
    size_t i = 0;
    while (i + 16 <= copy_len && PageSafe16((uintptr_t)(src + i))) {
        Copy16(dst + i, src + i);
        i += 16;
    }
    while (i < copy_len) {
        dst[i] = src[i];
        i++;
    }
}

// Disassembly-verified exact replacement for sub_76ED20.
static int __stdcall Hooked_Sub76ED20(char* dst, const char* src, int maxlen) {
    __try {
        // NULL check — match original exactly (0x76ED27-0x76ED30)
        if (!dst || !src) {
            g_fallback_paths++;
            SetLastError(0x57u);
            return 0;
        }

        // Bounded path: maxlen != 0x7FFFFFFF (0x76ED49)
        if (maxlen != 0x7FFFFFFF) {
            // Empty source: *src == 0 (0x76ED4B)
            if (*src == '\0') {
                *dst = '\0';
                g_fast_paths++;
                return 0;
            }

            // src_delta = src - dst — precomputed for the copy loop
            // (original does sub edx, esi at 0x76ED56)
            ptrdiff_t src_offset = src - dst;
            // dst_limit = dst + maxlen - 1 (original: lea edi, [esi+eax-1] at 0x76ED4E)
            char* dst_limit = dst + maxlen - 1;
            char* dst_ptr = dst;

            // Byte-by-byte copy until limit or null terminator
            while (dst_ptr < dst_limit) {
                char c = *(dst_ptr + src_offset);
                *dst_ptr = c;
                dst_ptr++;
                if (*(dst_ptr + src_offset) == '\0') {
                    *dst_ptr = '\0';
                    g_fast_paths++;
                    return (int)(dst_ptr - dst);
                }
            }
            // Hit limit — null-terminate and return
            *dst_ptr = '\0';
            g_fast_paths++;
            return (int)(dst_ptr - dst);
        }

        // Unbounded path: maxlen == 0x7FFFFFFF (0x76ED75)
        // Copy until source null byte, always null-terminate, no size limit
        char* dst_ptr = dst;
        ptrdiff_t src_offset = src - dst;

        char c = *src;
        while (c != '\0') {
            *dst_ptr = c;
            dst_ptr++;
            c = *(dst_ptr + src_offset);
        }
        *dst_ptr = '\0';
        g_fast_paths++;
        return (int)(dst_ptr - dst);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fallback_paths++;
        return orig_Sub76ED20(dst, src, maxlen);
    }
}

bool InstallStrcatFast() {
    void* target = (void*)0x0076ED20;

    if (MH_CreateHook(target, (void*)Hooked_Sub76ED20, (void**)&orig_Sub76ED20) != MH_OK) {
        Log("[StrcpyFast] MH_CreateHook FAILED at 0x%08X", (uintptr_t)target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[StrcpyFast] MH_EnableHook FAILED at 0x%08X", (uintptr_t)target);
        return false;
    }

    Log("[StrcpyFast] Disassembly-verified replacement at 0x%08X (890 xrefs), byte-exact + SEH-guarded",
        (uintptr_t)target);

    return true;
}

void StrcpyFast_GetStats(uint64_t* fast, uint64_t* fallback) {
    if (fast) *fast = g_fast_paths.load();
    if (fallback) *fallback = g_fallback_paths.load();
}

#else // TEST_DISABLE_STRCAT_FAST

bool InstallStrcatFast() {
    Log("[StrcpyFast] DISABLED (test toggle)");
    return false;
}

void StrcpyFast_GetStats(uint64_t* fast, uint64_t* fallback) {
    if (fast) *fast = 0;
    if (fallback) *fallback = 0;
}

#endif // TEST_DISABLE_STRCAT_FAST