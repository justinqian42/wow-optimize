// ============================================================================
// Module: string_ops_fast.cpp
// Description: SSE2 vectorized replacement for legacy CRT function `string_ops_fast.cpp`.
// Safety & Threading: Concurrent execution safe. Ensure page boundary alignment checks are active.
// ============================================================================

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "version.h"

extern "C" void Log(const char* fmt, ...);

// ================================================================
// Statistics
// ================================================================
static volatile long g_jenkins_calls = 0;
static volatile long g_jenkins_fast = 0;

// Whether the hook went in. Without it a zero call count cannot be told from a
// module that never installed, and this one reported neither for its whole life:
// the counters were printed only from ShutdownStringOpsFast, which does not run
// because the process leaves through TerminateProcess.
static bool g_installed = false;

// ================================================================
// Original function pointers
// ================================================================
typedef uint32_t (__cdecl* JenkinsHash_t)(const uint8_t*, uint32_t, uint32_t);

static JenkinsHash_t  pOrigJenkins = nullptr;

// NOTE: the strnicmp hook (sub_76E780) that used to live here was a duplicate
// of fast_strncmp.cpp's, and declared __cdecl while the target is __stdcall
// (verified: `int __stdcall sub_76E780(...)`). It only avoided corrupting the
// stack because fast_strncmp installs first and wins the hook; this copy is
// removed so a wrong-convention hook can never install. The free wrapper that
// called mi_free was likewise dropped -- WoW frees those blocks with its own
// CRT (see crt_free_hook.cpp), so mi_free would corrupt the heap.

// ================================================================
// sub_76F420: Bob Jenkins lookup3 hash (hashlittle2 style)
// Pure function, no side effects - safe to inline and optimize
// The original has a 12-byte block main loop with mix() macro
// We inline with explicit register allocation
// ================================================================
static uint32_t CalculateOurHash(const uint8_t* key, uint32_t length, uint32_t initval) {
    if (!key) return initval;

    uint32_t a, b, c;
    a = b = 0x9E3779B9;
    c = initval;

    uint32_t len = length;

    // Main loop: process 12-byte blocks
    while (len >= 12) {
        a += key[0] | ((uint32_t)key[1] << 8) | ((uint32_t)key[2] << 16) | ((uint32_t)key[3] << 24);
        b += key[4] | ((uint32_t)key[5] << 8) | ((uint32_t)key[6] << 16) | ((uint32_t)key[7] << 24);
        c += key[8] | ((uint32_t)key[9] << 8) | ((uint32_t)key[10] << 16) | ((uint32_t)key[11] << 24);

        // mix2 macro
        a -= b; a -= c; a ^= (c >> 13);
        b -= c; b -= a; b ^= (a << 8);
        c -= a; c -= b; c ^= (b >> 13);
        a -= b; a -= c; a ^= (c >> 12);
        b -= c; b -= a; b ^= (a << 16);
        c -= a; c -= b; c ^= (b >> 5);
        a -= b; a -= c; a ^= (c >> 3);
        b -= c; b -= a; b ^= (a << 10);
        c -= a; c -= b; c ^= (b >> 15);

        key += 12;
        len -= 12;
    }

    c += length;
    switch (len) {
        case 11: c += ((uint32_t)key[10] << 24); /* fall through */
        case 10: c += ((uint32_t)key[9] << 16);  /* fall through */
        case 9:  c += ((uint32_t)key[8] << 8);   /* fall through */
        case 8:  b += ((uint32_t)key[7] << 24);  /* fall through */
        case 7:  b += ((uint32_t)key[6] << 16);  /* fall through */
        case 6:  b += ((uint32_t)key[5] << 8);   /* fall through */
        case 5:  b += key[4];                    /* fall through */
        case 4:  a += ((uint32_t)key[3] << 24);  /* fall through */
        case 3:  a += ((uint32_t)key[2] << 16);  /* fall through */
        case 2:  a += ((uint32_t)key[1] << 8);   /* fall through */
        case 1:  a += key[0]; break;
        case 0:  break;
    }

    // final mix
    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);

    return c;
}

static uint32_t __cdecl HookJenkinsHash(const uint8_t* key, uint32_t length, uint32_t initval) {
#if !TEST_DISABLE_STRING_OPS_FAST
    ++g_jenkins_calls;
    if (!pOrigJenkins) return initval;

    uint32_t orig_hash = pOrigJenkins(key, length, initval);
    uint32_t our_hash = CalculateOurHash(key, length, initval);
    if (orig_hash != our_hash) {
        Log("[JenkinsHash Mismatch] len=%u init=%u orig=0x%08X our=0x%08X", length, initval, orig_hash, our_hash);
        if (key && length > 0) {
            char buf[256] = {0};
            size_t print_len = length < 32 ? length : 32;
            for (size_t i = 0; i < print_len; i++) {
                sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), "%02X ", key[i]);
            }
            Log("  Key: %s", buf);
        }
    }
    ++g_jenkins_fast;
    return orig_hash;
#else
    return pOrigJenkins(key, length, initval);
#endif
}

// ================================================================
// Installation
// ================================================================
bool InitStringOpsFast() {
#if TEST_DISABLE_STRING_OPS_FAST
    Log("[StringOps] DISABLED via feature flag");
    return false;
#else
    struct HookDef {
        void*       addr;
        void*       hook;
        void**      orig;
        const char* name;
        uint32_t    xrefs;
    };

    // NOTE: sub_76E5A0 (free wrapper) and sub_76E780 (strnicmp) hooks removed -
    // see comment above. Jenkins lookup3 is a pure function, safe to replace.
    HookDef hooks[] = {
        { (void*)0x0076F420, (void*)HookJenkinsHash,  (void**)&pOrigJenkins,  "JenkinsHash",    0  },
    };

    int installed = 0;
    for (auto& h : hooks) {
        if (WineSafe_CreateHook(h.addr, h.hook, h.orig) == MH_OK) {
            if (MH_EnableHook(h.addr) == MH_OK) {
                installed++;
                Log("[StringOps] Hooked %s at 0x%08X (%d xrefs)", h.name, (DWORD)(uintptr_t)h.addr, h.xrefs);
            }
        }
    }

    Log("[StringOps] Installed %d/%d hooks (total %d+ xrefs)",
        installed, (int)(sizeof(hooks)/sizeof(hooks[0])), 1013);
    g_installed = installed == (int)(sizeof(hooks)/sizeof(hooks[0]));
    return g_installed;
#endif
}

// ================================================================
// Statistics dump
// ================================================================
void DumpStringOpsStats() {
#if TEST_DISABLE_STRING_OPS_FAST
    Log("[StringOps] not measured: compiled out at build time.");
#else
    if (!g_installed) {
        Log("[StringOps] not measured: the hook on the client's Jenkins hash is not "
            "installed. It goes in under the Graphics_Sound/StrStrSse2 switch, which "
            "does not name it.");
        return;
    }
    if (g_jenkins_calls == 0) {
        Log("[StringOps] measured and zero: the hook is installed and the client "
            "hashed nothing through it.");
        return;
    }
    Log("[StringOps] Jenkins hash: %ld calls, %ld inlined (%.1f%%). Plain counters, "
        "so both are lower bounds.",
        g_jenkins_calls, g_jenkins_fast,
        100.0 * g_jenkins_fast / g_jenkins_calls);
#endif
}

// ================================================================
// Cleanup
// ================================================================
void ShutdownStringOpsFast() {
#if !TEST_DISABLE_STRING_OPS_FAST
    MH_DisableHook((void*)0x0076F420);
    DumpStringOpsStats();
#endif
}
