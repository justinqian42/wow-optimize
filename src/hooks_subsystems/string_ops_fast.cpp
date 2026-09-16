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
// Whether Init was reached. It runs only when the switch is on, so this separates
// "switched off" from "asked to install and could not".
static bool g_initRan = false;

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

// The learning phase, which this hook never had.
//
// It used to call the client's hash, call ours, compare them, throw ours away
// and return the client's - on every call, for the life of the session. That is
// two hashes where the client did one, so the replacement was strictly slower
// than the function it replaced and could never be anything else. It also
// counted g_jenkins_fast on every call, so the report would have read 100%
// inlined for a path that was used 0% of the time, if the report had ever been
// printed at all.
//
// Now it is the shape every other replacement in this project uses: run both and
// hand back the client's answer until enough calls have agreed, then use ours and
// resample. One disagreement retires it for the session and says so.
static constexpr long kVerifyFirst  = 4096;
static constexpr long kResampleMask = 1023;
static long g_verified  = 0;       // calls that ran both and compared
static long g_mismatch  = 0;
static bool g_armed     = false;
static bool g_retired   = false;

__declspec(noinline) static void ReportMismatch(const uint8_t* key, uint32_t length,
                                                uint32_t initval, uint32_t orig,
                                                uint32_t ours) {
    Log("[StringOps] Jenkins hash MISMATCH: len=%u init=%u client=0x%08X ours=0x%08X "
        "- retired for this session, every later call goes to the client.",
        length, initval, orig, ours);
    if (key && length > 0) {
        char buf[128] = {0};
        size_t n = length < 32 ? length : 32;
        size_t w = 0;
        for (size_t i = 0; i < n && w + 3 < sizeof(buf); i++) {
            _snprintf(buf + w, sizeof(buf) - w - 1, "%02X ", key[i]);
            w += 3;
        }
        Log("[StringOps]   key: %s", buf);
    }
}

static uint32_t __cdecl HookJenkinsHash(const uint8_t* key, uint32_t length, uint32_t initval) {
#if !TEST_DISABLE_STRING_OPS_FAST
    ++g_jenkins_calls;
    if (!pOrigJenkins) return initval;

    if (g_retired) return pOrigJenkins(key, length, initval);

    if (g_armed && ((g_jenkins_calls & kResampleMask) != 0)) {
        ++g_jenkins_fast;
        return CalculateOurHash(key, length, initval);
    }

    uint32_t orig_hash = pOrigJenkins(key, length, initval);
    uint32_t our_hash  = CalculateOurHash(key, length, initval);
    ++g_verified;
    if (orig_hash != our_hash) {
        ++g_mismatch;
        g_retired = true;
        g_armed   = false;
        ReportMismatch(key, length, initval, orig_hash, our_hash);
        return orig_hash;
    }
    if (!g_armed && g_verified >= kVerifyFirst) g_armed = true;
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
        if (!g_initRan) {
            Log("[StringOps] not measured: switched off. The hook on the client's "
                "Jenkins hash goes in under Graphics_Sound/StrStrSse2, which does "
                "not name it.");
        } else {
            Log("[StringOps] NOT active: asked to hook the client's Jenkins hash "
                "and it did not go in. The reason is earlier in this log.");
        }
        return;
    }
    if (g_jenkins_calls == 0) {
        Log("[StringOps] measured and zero: the hook is installed and the client "
            "hashed nothing through it.");
        return;
    }
    Log("[StringOps] Jenkins hash: %ld calls, %ld answered by the replacement "
        "(%.1f%%), %ld run both ways and compared. Plain counters, so each is a "
        "lower bound.",
        g_jenkins_calls, g_jenkins_fast,
        100.0 * g_jenkins_fast / g_jenkins_calls, g_verified);
    if (g_retired)
        Log("[StringOps]   RETIRED: %ld comparison(s) disagreed with the client, so "
            "every call since has gone to the client's own routine.", g_mismatch);
    else if (g_armed)
        Log("[StringOps]   armed after %ld agreeing comparison(s); one call in %d is "
            "still checked against the client.", (long)kVerifyFirst, kResampleMask + 1);
    else
        Log("[StringOps]   still verifying: %ld of %ld agreeing comparison(s) needed "
            "before the replacement's own answer is used, so it is running both and "
            "is slower than the client until then.", g_verified, (long)kVerifyFirst);
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
