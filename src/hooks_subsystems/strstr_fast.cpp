#include "strstr_fast.h"
#include "version.h"
#include "MinHook.h"
#include <cstdint>
#include <intrin.h>

extern "C" void Log(const char* fmt, ...);

#if !TEST_DISABLE_STRSTR_SSE2

typedef const char* (__cdecl *strstr_fn)(const char*, const char*);
static strstr_fn orig_strstr = nullptr;

static volatile LONG64 g_calls = 0, g_fast = 0;

static const char* __cdecl Hooked_strstr(const char* haystack, const char* needle) {
    if (WOWOPT_FOREIGN_CALLER()) return orig_strstr(haystack, needle);
    if (!haystack || !needle) return nullptr;
    if (!*needle) return haystack;

    g_calls++;

    size_t needleLen = strlen(needle);
    if (needleLen == 0) return haystack;
    if (needleLen > 16) return orig_strstr(haystack, needle);

    char first = needle[0];
    size_t hayLen = strlen(haystack);
    if (hayLen < needleLen) return nullptr;

    // SSE2 Boyer-Moore-Horspool for short patterns
    const char* end = haystack + hayLen - needleLen;
    
    // Build bad-char skip table (simple, not SSE for tiny patterns)
    __declspec(align(16)) char skip[256];
    for (int i = 0; i < 256; i++) skip[i] = (char)(needleLen > 255 ? 255 : needleLen);
    for (size_t i = 0; i < needleLen - 1; i++) skip[(unsigned char)needle[i]] = (char)(needleLen - 1 - i);

    const char* p = haystack;
    
    if (needleLen <= 4) {
        // Tiny pattern - unrolled comparison
        while (p <= end) {
            if (p[0] == first) {
                bool match = true;
                for (size_t i = 1; i < needleLen; i++) {
                    if (p[i] != needle[i]) { match = false; break; }
                }
                if (match) { g_fast++; return p; }
            }
            p++;
        }
    } else if (needleLen <= 8) {
        // Medium pattern - use uint64 comparison for speed
        uint64_t needle64 = 0;
        memcpy(&needle64, needle, needleLen);
        while (p <= end) {
            if (p[0] == first) {
                uint64_t hay64 = 0;
                memcpy(&hay64, p, needleLen);
                if (hay64 == needle64) { g_fast++; return p; }
                unsigned char skipChar = (unsigned char)p[needleLen > 0 ? needleLen - 1 : 0];
                p += skip[skipChar];
                continue;
            }
            p++;
        }
    } else {
        // 9-16 byte pattern - use SSE2
        __m128i needleXmm = _mm_loadu_si128((__m128i*)needle);
        __m128i mask = _mm_cmpeq_epi8(needleXmm, _mm_set1_epi8(first));
        int maskBits = _mm_movemask_epi8(mask);
        
        while (p <= end) {
            __m128i hayXmm = _mm_loadu_si128((__m128i*)p);
            __m128i cmp = _mm_cmpeq_epi8(hayXmm, needleXmm);
            int bits = _mm_movemask_epi8(cmp);
            // Check if all needleLen bytes match
            int needleMask = (1 << needleLen) - 1;
            if ((bits & needleMask) == needleMask) { g_fast++; return p; }
            // Skip using bad-char heuristic
            unsigned char lastChar = (unsigned char)p[needleLen - 1];
            int step = skip[lastChar];
            if (step < 1) step = 1;
            if (step > (end - p)) break;
            p += step;
        }
    }

    return nullptr;
}

static void* g_target_strstr = nullptr;

bool InstallStrstrSSE2() {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (!crt) crt = GetModuleHandleA("ucrtbase.dll");
    if (!crt) {
        void* addr = GetProcAddress(GetModuleHandleA("ntdll.dll"), "strstr");
        if (addr) {
            if (MH_CreateHook(addr, (void*)Hooked_strstr, (void**)&orig_strstr) == MH_OK) {
                if (MH_EnableHook(addr) == MH_OK) {
                    g_target_strstr = addr;
                    Log("[StrstrSSE2] Active via ntdll!strstr");
                    return true;
                }
            }
        }
        return false;
    }
    void* addr = GetProcAddress(crt, "strstr");
    if (!addr) return false;
    if (MH_CreateHook(addr, (void*)Hooked_strstr, (void**)&orig_strstr) == MH_OK) {
        if (MH_EnableHook(addr) == MH_OK) {
            g_target_strstr = addr;
            Log("[StrstrSSE2] Active: SSE2 Boyer-Moore-Horspool for patterns upto 16 bytes");
            return true;
        }
    }
    return false;
}

void ShutdownStrstrSSE2() {
    if (g_target_strstr) {
        MH_DisableHook(g_target_strstr);
    }
    Log("[StrstrSSE2] Calls: %lld fast, SSE2 hit rate: %.1f%%", g_fast, g_calls ? 100.0 * g_fast / g_calls : 0.0);
}

#else
bool InstallStrstrSSE2() { return false; }
void ShutdownStrstrSSE2() {}
#endif
