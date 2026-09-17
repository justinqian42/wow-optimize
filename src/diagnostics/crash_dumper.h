#pragma once

#ifndef CRASH_DUMPER_H
#define CRASH_DUMPER_H

// Each optimization registers itself, so a crash dump says what was active.
// Overflow is logged; raise this when that line appears.
#define MAX_TRACKED_FEATURES 192

struct FeatureState {
    const char*  name;        // Feature name (e.g., "AdaptiveGC", "GetStrInline")
    bool         active;      // Currently enabled
    bool         counted;     // Something calls FeatureHit for this one
    unsigned int hits;        // Times FeatureHit was called (32-bit: see FeatureHit)
    unsigned int hitStride;   // Calls per FeatureHit. See FeatureTokenForCounting.
    long long    callCount;   // Total invocations via the legacy by-name API
    long long    errorCount;  // SEH exceptions caught
    DWORD        lastCallTick;// GetTickCount of last invocation
    const char*  lastError;   // Last error description (static string)
};

namespace CrashDumper {
    bool Init();
    void Shutdown();

    // Register a tracked feature for crash diagnostics. Returns a token for
    // FeatureHit, or -1 if the table is full.
    int RegisterFeature(const char* name);

    // Claims a token for counting, by the name dllmain already registered, and
    // registers it if absent. Claiming marks the feature as reporting its
    // activity, so a zero count means it never ran rather than that nobody
    // instrumented it. hitStride is how many calls each FeatureHit stands for:
    // pass the divisor a sampled call site uses, or 1 when it counts every call.
    int FeatureTokenForCounting(const char* name, unsigned hitStride = 1);

    // O(1) increment at a known index. Not atomic and 32-bit on purpose: this
    // sits on paths that run millions of times a session, where a locked 64-bit
    // add costs more than the work being counted and can tear.
    void FeatureHit(int token);

// How many features are registered. Not the cap: printing the cap here read as
// a registry overflowing on every startup.
int RegisteredFeatureCount();

    // Writes the "what actually ran" section to the log.
    void ReportFeatureActivity();

// States how many handled fatal-class exceptions were seen, without printing one
// line each - a single third-party module produced 8889 in one session.
void ReportFirstChanceSummary();

// Caches the address range of modules whose deterministic exceptions are known
// to be benign, so the first-chance probe can recognise them without taking the
// loader lock. Call from the main thread - Init and periodic maintenance - since
// such a module can load after we do.
void RefreshBenignModuleRanges();

    // Update feature state (call from hooks/fast-paths)
    void FeatureCall(const char* name);
    void FeatureError(const char* name, const char* desc);
    void FeatureSetActive(const char* name, bool active);

    // Get current feature states for crash dump
    int GetFeatureStates(FeatureState* out, int maxCount);

    // Record last hook call for crash context (ring buffer, lock-free)
    void RecordHookCall(const char* hookName, uintptr_t addr);

    // Hot-path variant for very high-frequency hooks (UI accessors etc.).
    // Samples ~1/64 calls so the per-call InterlockedIncrement stays off the
    // frame critical path, and so one hot hook can't flood the 256-slot ring
    // and evict the rarer, riskier hooks we actually want in a crash trace.
    void RecordHookCallHot(const char* hookName, uintptr_t addr);

    // Event trace. Records state transitions - loading screen boundaries,
    // lua_State swaps, D3D9 device resets, cache invalidations, watchdogs -
    // unconditionally, so the trail exists whatever is enabled. Safe from any
    // thread, never allocates. Keep messages short and factual.
    void Trace(const char* fmt, ...);

    // Writes the most recent `count` events to the log, newest first.
    //
    // maxAgeMs bounds how far back an event may be and still be printed. A caller
    // explaining something that just happened - a slow frame - must pass the
    // window it actually covers, or the newest three entries in the ring get
    // presented as the cause when they are minutes old and unrelated. 0 means no
    // bound, which is what the crash handler wants: there, the whole trail is the
    // point. Returns the number of events printed.
    int DumpTrace(int count, DWORD maxAgeMs = 0);
}

// Times a scope and traces it ONLY if it ran longer than thresholdMs.
//
// The event ring is fed by state transitions, which are rare - a session can go
// seventeen minutes recording three of them. That is fine for a crash trail and
// useless for explaining a 100 ms frame, because the newest entries are minutes
// old. This fills the gap without flooding: a probe that stays under its
// threshold writes nothing, so every line it does produce is a stall long enough
// to matter, on work known to run on the main thread.
class StallProbe {
public:
    StallProbe(const char* what, double thresholdMs);
    ~StallProbe();
private:
    const char*   m_what;
    double        m_thresholdMs;
    LARGE_INTEGER m_start;
};

// Same measurement without RAII. MSVC rejects objects requiring unwinding in any
// function that uses __try, and several of the places worth probing are SEH
// guarded, so those call StallProbeBegin/End directly.
// Printed from the periodic report. Stalls used to reach a log only through
// our own crash dump, so a session that did not crash recorded none.
void StallProbe_LogStats();

LARGE_INTEGER StallProbeBegin();
void StallProbeEnd(const char* what, const LARGE_INTEGER& start, double thresholdMs);

// C-callable wrapper for modules that only see the C interface.
extern "C" void CrashDumper_Trace(const char* fmt, ...);

#endif
