// ============================================================================
// Description: SSE2 replacement for the client's own memcpy at 0x0040CB10.
//              NOT INSTALLED. It corrupted objects and crashed the game.
// ============================================================================
// This module is complete, compiles, and is never installed. The call in
// dllmain is commented out and the linker drops the whole object, which is why
// nothing in a field log mentions it.
//
// The reason is not that it was abandoned half-written. Commit 6d71ec19 turned
// it off, and its message says what happened: "Disable W12 AllocWrapper hook
// and FastMemcpy hook to prevent uninitialized object corruptions and crashes
// in combat."
//
// That is written here because it was not written anywhere a reader of this
// file would see it. What is below looks finished and plausible, the switch it
// used to hang off was OptStrStrSse2 - a name belonging to strstr, not to
// memcpy - and the line in dllmain is a bare `false` with no explanation. Any
// of those three would invite someone to turn it back on.
//
// 0x0040CB10 has 719 xrefs and carries live game objects between allocations.
// Whatever the defect was, it produced corruption rather than a fault, which is
// the hardest kind to attribute and the reason this needs a proof rather than a
// retry. Do not re-enable it without one.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include "MinHook.h"
#include "version.h"
#include "crt_memcpy_fast.h"

extern "C" void Log(const char* fmt, ...);

static uint64_t g_total_calls = 0;
static uint64_t g_sse2_path = 0;
static uint64_t g_nt_path = 0;
static uint64_t g_fallback_path = 0;

static const size_t NT_THRESHOLD = 256 * 1024;

typedef void* (__cdecl *orig_memcpy_t)(void*, const void*, size_t);
static orig_memcpy_t g_orig_memcpy = nullptr;

static bool ranges_overlap_up(const unsigned char* dst, const unsigned char* src, size_t size)
{
    return (dst > src) && (dst < src + size);
}

static bool ranges_overlap_down(const unsigned char* dst, const unsigned char* src, size_t size)
{
    return (src > dst) && (src < dst + size);
}

static void* __cdecl Hooked_memcpy(void* dest, const void* src, size_t Size)
{
    if (!g_orig_memcpy) {
        if (dest && src && Size > 0) {
            __movsb((unsigned char*)dest, (const unsigned char*)src, Size);
        }
        return dest;
    }

    if (!dest || !src || Size == 0) return g_orig_memcpy(dest, src, Size);

    const unsigned char* d = (const unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (ranges_overlap_up(d, s, Size) || ranges_overlap_down(d, s, Size)) {
        g_fallback_path++;
        return g_orig_memcpy(dest, src, Size);
    }

    if (Size < 16) {
        g_fallback_path++;
        return g_orig_memcpy(dest, src, Size);
    }

    if (Size >= NT_THRESHOLD) {
        g_fallback_path++;
        return g_orig_memcpy(dest, src, Size);
    }

    if (Size >= 256) {
        g_fallback_path++;
        return g_orig_memcpy(dest, src, Size);
    }

    g_total_calls++;
    g_sse2_path++;

    unsigned char* pd = (unsigned char*)dest;
    const unsigned char* ps = (const unsigned char*)src;
    size_t len = Size;

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        _mm_storeu_si128((__m128i*)(pd + i), _mm_loadu_si128((const __m128i*)(ps + i)));
    }
    if (i < len) {
        __movsb(pd + i, ps + i, len - i);
    }

    return dest;
}

bool InstallMemcpyFast()
{
    void* target = reinterpret_cast<void*>(0x0040CB10);

    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[FastMemcpy] BAD PROLOGUE at 0x%08X (expected 55 8B EC)", (uintptr_t)target);
        return false;
    }

    if (WineSafe_CreateHook(target, (void*)Hooked_memcpy, (void**)&g_orig_memcpy) != MH_OK) {
        Log("[FastMemcpy] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[FastMemcpy] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    Log("[FastMemcpy] Installed: SSE2 memcpy for 16-255B range at 0x40CB10 (719 xrefs, memmove-safe)");
    return true;
}

void UninstallMemcpyFast()
{
    void* target = reinterpret_cast<void*>(0x0040CB10);
    MH_DisableHook(target);
    MH_RemoveHook(target);

    uint64_t total = g_total_calls;
    if (total > 0) {
        Log("[FastMemcpy] Stats: %llu total, %llu SSE2, %llu NT, %llu fallback (%.1f%% SSE2)",
            total, g_sse2_path, g_nt_path, g_fallback_path, 100.0 * g_sse2_path / total);
    }
}


