#include "crt_wchar_fast.h"
#include "version.h"
#include "MinHook.h"
#include <cstdint>
#include <intrin.h>
#include <emmintrin.h>

extern "C" void Log(const char* fmt, ...);

#if !TEST_DISABLE_CRT_MEM_FASTPATHS  // use same toggle as CRT mem paths

// Thread-local recursion guard
static __declspec(thread) volatile LONG g_wcharInHook = 0;
#define WCHAR_ENTER() do { if (InterlockedExchange(&g_wcharInHook, 1) != 0) goto fallback; } while(0)
#define WCHAR_LEAVE() InterlockedExchange(&g_wcharInHook, 0)

// ====== wcslen ======
typedef size_t (__cdecl* wcslen_fn)(const wchar_t*);
static wcslen_fn orig_wcslen = nullptr;

static size_t __cdecl Hooked_wcslen(const wchar_t* s) {
    if (WOWOPT_FOREIGN_CALLER()) return orig_wcslen(s);
    if (!orig_wcslen) goto fallback;
    WCHAR_ENTER();
    if (!s) { WCHAR_LEAVE(); goto fallback; }
    if (((uintptr_t)s & 0xFFF) > 0xFF0) { WCHAR_LEAVE(); goto fallback; }
    __try {
        const __m128i zero = _mm_setzero_si128();
        size_t n = 0;
        while (true) {
            __m128i v = _mm_loadu_si128((const __m128i*)(s + n));
            // Compare 16-bit lanes against zero to handle wide chars correctly
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(v, zero));
            if (mask) {
                unsigned long idx; _BitScanForward(&idx, (unsigned long)mask);
                WCHAR_LEAVE(); return n + (idx / 2);
            }
            n += 8; // 8 wchar_t per 16-byte SSE2 register
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { WCHAR_LEAVE(); }
fallback:
    if (orig_wcslen) return orig_wcslen(s);
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

// ====== wcscpy ======
typedef wchar_t* (__cdecl* wcscpy_fn)(wchar_t*, const wchar_t*);
static wcscpy_fn orig_wcscpy = nullptr;

static wchar_t* __cdecl Hooked_wcscpy(wchar_t* dst, const wchar_t* src) {
    if (WOWOPT_FOREIGN_CALLER()) return orig_wcscpy(dst, src);
    if (!orig_wcscpy) goto fallback;
    WCHAR_ENTER();
    if (!dst || !src) { WCHAR_LEAVE(); goto fallback; }
    if (((uintptr_t)dst & 0xFFF) > 0xFF0 || ((uintptr_t)src & 0xFFF) > 0xFF0)
        { WCHAR_LEAVE(); goto fallback; }
    __try {
        wchar_t* ret = dst;
        const __m128i zero = _mm_setzero_si128();
        while (true) {
            __m128i v = _mm_loadu_si128((const __m128i*)src);
            _mm_storeu_si128((__m128i*)dst, v);
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(v, zero));
            if (mask) { WCHAR_LEAVE(); return ret; }
            src += 8; dst += 8;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { WCHAR_LEAVE(); }
fallback:
    if (orig_wcscpy) return orig_wcscpy(dst, src);
    if (!dst || !src) return dst;
    wchar_t* d = dst;
    const wchar_t* s = src;
    while ((*d++ = *s++)) {}
    return dst;
}

static void* g_target_wcslen = nullptr;
static void* g_target_wcscpy = nullptr;

bool InstallCrtWcharSSE2() {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (!crt) crt = GetModuleHandleA("ucrtbase.dll");
    if (!crt) { Log("[CrtWchar] msvcrt/ucrtbase not found"); return false; }

    int ok = 0;
    void* pWcslen = GetProcAddress(crt, "wcslen");
    void* pWcscpy = GetProcAddress(crt, "wcscpy");
    if (pWcslen && MH_CreateHook(pWcslen, (void*)Hooked_wcslen, (void**)&orig_wcslen) == MH_OK
        && MH_EnableHook(pWcslen) == MH_OK) {
        g_target_wcslen = pWcslen;
        ok++;
    }
    if (pWcscpy && MH_CreateHook(pWcscpy, (void*)Hooked_wcscpy, (void**)&orig_wcscpy) == MH_OK
        && MH_EnableHook(pWcscpy) == MH_OK) {
        g_target_wcscpy = pWcscpy;
        ok++;
    }
    if (ok > 0) {
        Log("[CrtWchar] Active: wcslen+wcscpy SSE2 (%d/2 hooked, page-boundary guarded)", ok);
        return true;
    }
    return false;
}

void ShutdownCrtWcharSSE2() {
    if (g_target_wcslen) MH_DisableHook(g_target_wcslen);
    if (g_target_wcscpy) MH_DisableHook(g_target_wcscpy);
}

#else
bool InstallCrtWcharSSE2() { return false; }
void ShutdownCrtWcharSSE2() {}
#endif