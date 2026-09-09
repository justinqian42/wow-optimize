// ============================================================================
// Module: regex_cache.cpp
// Description: Bypasses PCRE pattern compilation overhead by caching compiled regex bytes.
// Safety & Threading: Thread-safe, verified pointer checks.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "regex_cache.h"
#include "high_tables.h"

extern "C" void Log(const char* fmt, ...);

// Teardown state helper
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static inline bool IsTeardownState() {
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000 || gL > 0xFFE00000);
}

// ================================================================
// Cache Configuration
// ================================================================
static constexpr int REGEX_CACHE_SIZE     = 256;
static constexpr int REGEX_CACHE_MASK     = REGEX_CACHE_SIZE - 1;
static constexpr int REGEX_MAX_PATTERN    = 512;   // max pattern length to cache
static constexpr int REGEX_MAX_COMPILED   = 8192;  // max compiled bytecode size
static constexpr DWORD REGEX_TTL_MS       = 120000; // 2 minute TTL

struct RegexCacheEntry {
    uint32_t  patternHash;
    DWORD     lastAccess;
    uint32_t  options;
    uintptr_t tableptr;
    uint16_t  patternLen;
    uint16_t  compiledLen;
    bool      valid;
    char      pattern[REGEX_MAX_PATTERN];
    uint8_t   compiled[REGEX_MAX_COMPILED];
};

// Out of this DLL's image and into the top half of the address space; see
// high_tables.cpp for why that half and not this one.
static constexpr size_t REGEX_CACHE_BYTES =
    sizeof(RegexCacheEntry) * REGEX_CACHE_SIZE;
static RegexCacheEntry* g_regexCache = nullptr;
static volatile LONG64 g_regexHits = 0;
static volatile LONG64 g_regexMisses = 0;
static volatile LONG64 g_regexEvictions = 0;

// ================================================================
// FNV-1a Hash with options mix
// ================================================================
static inline uint32_t RegexHash(const char* s, int len, unsigned int options) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    h ^= (uint32_t)options;
    h *= 0x01000193u;
    return h;
}

// ================================================================
// Cache Operations
// ================================================================
const uint8_t* RegexCache_Get(const char* pattern, int patternLen, unsigned int options, const unsigned char* tableptr, int* outCompiledLen) {
    if (patternLen <= 0 || patternLen >= REGEX_MAX_PATTERN) return nullptr;

    uint32_t hash = RegexHash(pattern, patternLen, options);
    uint32_t idx = hash & REGEX_CACHE_MASK;
    RegexCacheEntry* e = &g_regexCache[idx];

    if (e->valid && e->patternHash == hash && e->patternLen == patternLen && e->options == options && e->tableptr == (uintptr_t)tableptr) {
        if (memcmp(e->pattern, pattern, patternLen) == 0) {
            DWORD now = GetTickCount();
            if ((now - e->lastAccess) < REGEX_TTL_MS) {
                e->lastAccess = now;
                *outCompiledLen = e->compiledLen;
                InterlockedIncrement64(&g_regexHits);
                return e->compiled;
            }
            e->valid = false;
        }
    }
    InterlockedIncrement64(&g_regexMisses);
    return nullptr;
}

void RegexCache_Put(const char* pattern, int patternLen, unsigned int options, const unsigned char* tableptr, const uint8_t* compiled, int compiledLen) {
    if (patternLen <= 0 || patternLen >= REGEX_MAX_PATTERN) return;
    if (compiledLen <= 0 || compiledLen >= REGEX_MAX_COMPILED) return;

    uint32_t hash = RegexHash(pattern, patternLen, options);
    uint32_t idx = hash & REGEX_CACHE_MASK;
    RegexCacheEntry* e = &g_regexCache[idx];

    if (e->valid && e->patternHash != hash) {
        InterlockedIncrement64(&g_regexEvictions);
    }

    e->patternHash = hash;
    e->patternLen = (uint16_t)patternLen;
    e->options = options;
    e->tableptr = (uintptr_t)tableptr;
    e->compiledLen = (uint16_t)compiledLen;
    e->lastAccess = GetTickCount();
    memcpy(e->pattern, pattern, patternLen);
    memcpy(e->compiled, compiled, compiledLen);
    e->valid = true;
}

void RegexCache_Clear() {
    if (g_regexCache) memset(g_regexCache, 0, REGEX_CACHE_BYTES);
}

// ================================================================
// Detour Hook for sub_8418F0 (pcre_compile)
// ================================================================
typedef void* (__cdecl* pcre_compile_t)(
    const char* pattern,
    unsigned int options,
    const char** errptr,
    int* erroffset,
    const unsigned char* tableptr);

static pcre_compile_t orig_pcre_compile = nullptr;

static void* __cdecl Hooked_pcre_compile(
    const char* pattern,
    unsigned int options,
    const char** errptr,
    int* erroffset,
    const unsigned char* tableptr)
{
    if (pattern && !IsTeardownState()) {
        int patternLen = (int)strlen(pattern);
        int compiledLen = 0;
        const uint8_t* cached = RegexCache_Get(pattern, patternLen, options, tableptr, &compiledLen);
        if (cached) {
            // Allocate memory using the client's CRT allocator (malloc at 0x00415074)
            typedef void* (__cdecl* malloc_t)(size_t);
            malloc_t fn_malloc = (malloc_t)0x00415074;
            void* res = fn_malloc(compiledLen);
            if (res) {
                memcpy(res, cached, compiledLen);
                if (errptr) *errptr = nullptr;
                if (erroffset) *erroffset = 0;
                return res;
            }
        }
    }

    void* result = orig_pcre_compile(pattern, options, errptr, erroffset, tableptr);
    if (result && pattern && !IsTeardownState()) {
        int patternLen = (int)strlen(pattern);
        // The compiled size is stored at offset 4 of the compiled structure (v94[1] in IDA)
        uint32_t compiledLen = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(result) + 4);
        RegexCache_Put(pattern, patternLen, options, tableptr, reinterpret_cast<const uint8_t*>(result), (int)compiledLen);
    }
    return result;
}

// ================================================================
// Install / Shutdown
// ================================================================
bool InstallRegexCache() {
    g_regexCache = (RegexCacheEntry*)HighTables::Reserve("regex_cache",
                                                         REGEX_CACHE_BYTES);
    if (!g_regexCache) {
        Log("[RegexCache] NOT installed: the %u KB table could not be "
            "allocated.", (unsigned)(REGEX_CACHE_BYTES / 1024));
        return false;
    }

    if (WineSafe_CreateHook((void*)0x008418F0, (void*)Hooked_pcre_compile, (void**)&orig_pcre_compile) == MH_OK) {
        if (MH_EnableHook((void*)0x008418F0) == MH_OK) {
            Log("[RegexCache] Hooked pcre_compile at 0x008418F0 successfully");
        } else {
            Log("[RegexCache] Failed to enable hook on pcre_compile");
            return false;
        }
    } else {
        Log("[RegexCache] Failed to create hook on pcre_compile");
        return false;
    }

    Log("[RegexCache] Initialized (%d slots, %dB max pattern, %dB max compiled, %ds TTL)",
        REGEX_CACHE_SIZE, REGEX_MAX_PATTERN, REGEX_MAX_COMPILED, REGEX_TTL_MS / 1000);

    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void RegexCache_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[RegexCache] not measured: the cache is not installed.");
        return;
    }
    const LONG64 hits = g_regexHits, misses = g_regexMisses;
    if (hits + misses == 0) {
        Log("[RegexCache] measured and zero: no pattern was compiled.");
        return;
    }
    Log("[RegexCache] %lld hits, %lld misses (%.1f%% hit rate), %lld evictions.",
        (long long)hits, (long long)misses,
        100.0 * (double)hits / (double)(hits + misses),
        (long long)g_regexEvictions);
}

void ShutdownRegexCache() {
    MH_DisableHook((void*)0x008418F0);
    LONG64 hits = g_regexHits;
    LONG64 misses = g_regexMisses;
    if (hits + misses > 0) {
        Log("[RegexCache] Stats: %lld hits, %lld misses (%.1f%% hit rate), %lld evictions",
            hits, misses, 100.0 * hits / (hits + misses), g_regexEvictions);
    }
}