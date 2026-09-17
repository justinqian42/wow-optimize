#include "strtod_fast.h"
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// Statistics
static volatile long g_strtodCalls = 0;
static volatile long g_strtodFast  = 0;

typedef BOOL (__cdecl *strtod_helper_fn)(const char* String, double* a2);
static strtod_helper_fn orig_strtod_helper = nullptr;

// Fast integer parser that handles leading/trailing spaces and signs
static __forceinline bool FastParseInt(const char* s, double* out) {
    // Skip leading spaces
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    if (!*s) return false;
    
    // Handle sign
    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    if (!*s || *s < '0' || *s > '9') return false;
    
    // Parse digits
    uint64_t val = 0;
    int digitCount = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
        digitCount++;
        if (digitCount > 15) return false; // avoid float precision loss
    }
    
    // Skip trailing spaces
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    
    if (*s != '\0') return false; // has trailing characters (like '.', 'e', 'x', etc.) -> fallback
    
    *out = neg ? -(double)val : (double)val;
    return true;
}

static BOOL __cdecl Hooked_StrtodHelper(const char* String, double* a2) {
    ++g_strtodCalls;
    
    if (!String || !a2 || (uintptr_t)String < 0x10000 || (uintptr_t)String > 0xFFE00000) {
        return FALSE;
    }

    __try {
        double val;
        if (FastParseInt(String, &val)) {
            *a2 = val;
            ++g_strtodFast;
            return TRUE;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return orig_strtod_helper(String, a2);
}

bool InstallStrtodFast() {
    void* target = (void*)0x0084D480;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[StrtodFast] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }
    if (MH_CreateHook(target, (void*)Hooked_StrtodHelper, (void**)&orig_strtod_helper) != MH_OK) {
        Log("[StrtodFast] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[StrtodFast] MH_EnableHook FAILED");
        return false;
    }
    Log("[StrtodFast] ACTIVE: inline string-to-number parser (0x84D480)");
    CrashDumper::RegisterFeature("StrtodFast");
    CrashDumper::FeatureSetActive("StrtodFast", true);
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void StrtodFast_LogStats(void) {
    if (!orig_strtod_helper) {
        Log("[StrtodFast] not measured: the hook is not installed.");
        return;
    }
    const LONG64 total = g_strtodCalls, fast = g_strtodFast;
    if (total == 0) {
        Log("[StrtodFast] measured and zero: the client parsed no number "
            "through it.");
        return;
    }
    Log("[StrtodFast] %lld calls, %lld inline (%.1f%%).",
        (long long)total, (long long)fast,
        100.0 * (double)fast / (double)total);
}

void UninstallStrtodFast() {
    MH_DisableHook((void*)0x0084D480);
    MH_RemoveHook((void*)0x0084D480);
    CrashDumper::FeatureSetActive("StrtodFast", false);
    LONG64 total = g_strtodCalls;
    LONG64 fast  = g_strtodFast;
    if (total > 0) {
        Log("[StrtodFast] Stats: %lld calls, %lld inline (%.1f%%)",
            (long long)total, (long long)fast,
            100.0 * (double)fast / (double)total);
    }
}
