// ============================================================================
// Module: lua_gc_pace.cpp
//
// The client's Lua collector is the largest single entry in the profile, and
// the numbers that pace it were never measured. This measures them.
//
// What the log says. In a session on 2026-09-20 that ran 90 to 92 percent
// executing - not a capped one, where shares are shares of a spin - the
// sampling profiler put luaC_sweeplist first at 7.04% of executing main-thread
// time and luaC_traversetable second at 5.33%, 12.37% together. That is more
// than d3d9.dll, more than the frustum test, more than every maths replacement
// in this project put together. Both are the client's own collector, in the
// client's own functions, and nothing here counts them.
//
// What paces it. Two numbers in lua_optimize.cpp, applied to every VM this DLL
// sees: a pause of 110 and a step multiplier of 300. The client's own values
// were 110 and 200, so the pause is the client's and the step multiplier is
// ours - we raised the work the collector does per unit of allocation by half,
// and no log has ever said what that cost or bought.
//
// A pause of 110 means a new collection cycle begins once the heap has grown a
// tenth past what was live at the end of the last one, which is close to
// collecting continuously. Stock Lua is 200: collect when the heap has doubled.
// The trade is plain enough to state and impossible to settle by stating it -
// fewer cycles cost less processor and hold a larger heap, and this is a 32-bit
// client where the low half of the address space is already the scarce
// resource. So it gets an experiment rather than an opinion.
//
// What this does. It registers as an A/B subject. During an ON stint the
// collector runs on our pacing; during an OFF stint it runs on the values the
// client itself had before this DLL touched them. The harness already splits
// frame times by stint and reports mean, p50, p95 and p99 for each half, which
// is the answer about frames. What it cannot show is the other half of the
// trade, so this counts that: the Lua heap every frame, its mean and its peak
// in each half, and how many collection cycles each half actually saw.
//
// The cycle count is what says whether a stint was long enough to mean
// anything. A pause of 200 makes cycles rarer, and a comparison across stints
// that saw one cycle each is a comparison of two accidents. The report prints
// the count beside every other number so a reader can throw the run out.
//
// Both settings are re-applied every frame rather than on the phase change
// alone. A VM reload re-runs the tuning in lua_optimize.cpp and would put our
// values back in the middle of an OFF stint, and lua_gc with LUA_GCSETPAUSE is
// a field write, so paying for it once a frame is cheaper than being wrong.
//
// Three states, not two. If lua_optimize.cpp never tuned this VM, or if the
// values it set are the ones the client already had, the two halves are
// identical and the run measures nothing; the report says so instead of
// printing a difference between two copies of the same thing. If the adaptive
// governor is on, it rewrites the pacing itself on every frame and this
// declines to install at all rather than fight it for the last write.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "lua_gc_pace.h"
#include "lua_optimize.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace LuaGcPace {

namespace {

typedef int (__cdecl* lua_gc_fn)(void* L, int what, int data);
lua_gc_fn g_lua_gc = (lua_gc_fn)0x0084ED50;

void** const kLuaStateSlot = (void**)0x00D3F78C;

constexpr int kGcCount     = 3;   // LUA_GCCOUNT, kilobytes
constexpr int kGcCountB    = 4;   // LUA_GCCOUNTB, the remainder in bytes
constexpr int kGcSetPause  = 6;
constexpr int kGcSetStepMul = 7;

// The client's collector, from the profiler's symbol table: luaC_traversetable
// at 0x0085A960 is 432 bytes and luaC_sweeplist at 0x0085B200 is 143.
constexpr uintptr_t kTraverseLo = 0x0085A960, kTraverseHi = 0x0085A960 + 432;
constexpr uintptr_t kSweepLo    = 0x0085B200, kSweepHi    = 0x0085B200 + 143;

// A trough in the heap's sawtooth is a cycle that finished sweeping. Ten
// percent below the running peak is well clear of the noise a single frame's
// allocation makes and well inside the drop a real collection produces.
constexpr double kTroughFraction = 0.90;

struct Half {
    unsigned long long frames = 0;
    double   heapSumKB = 0.0;
    double   heapPeakKB = 0.0;
    unsigned long cycles = 0;
    double   peakSinceTrough = 0.0;
};

Half g_ours;      // our pacing
Half g_theirs;    // the client's own

bool g_installed = false;
bool g_resolved = false;    // the two pacings are known and the halves are live
bool g_abSubject = false;
int  g_lastPhase = -1;
unsigned long long g_beforeTuning = 0;

int g_pauseOurs = 0, g_stepOurs = 0;
int g_pauseTheirs = 0, g_stepTheirs = 0;
bool g_identical = false;

unsigned long long g_noState = 0;     // frames with no lua_State to look at
unsigned long long g_reloading = 0;   // frames during a VM reload or swap
unsigned long g_transitions = 0;

inline double HeapKB(void* L) {
    const int kb = g_lua_gc(L, kGcCount, 0);
    const int b  = g_lua_gc(L, kGcCountB, 0);
    if (kb < 0) return -1.0;
    return (double)kb + (double)b / 1024.0;
}

void Account(Half& h, double kb) {
    ++h.frames;
    h.heapSumKB += kb;
    if (kb > h.heapPeakKB) h.heapPeakKB = kb;
    if (kb > h.peakSinceTrough) h.peakSinceTrough = kb;
    else if (h.peakSinceTrough > 0.0 && kb < h.peakSinceTrough * kTroughFraction) {
        ++h.cycles;
        h.peakSinceTrough = kb;
    }
}

void ReportHalf(const char* label, int pause, int stepMul, const Half& h) {
    if (h.frames == 0) {
        Log("[LuaGcPace]   %-6s pause %d stepmul %d: no frame ran in this half.",
            label, pause, stepMul);
        return;
    }
    Log("[LuaGcPace]   %-6s pause %d stepmul %d: %llu frame(s), heap mean %.0f KB, "
        "peak %.0f KB, %lu collection cycle(s) seen.",
        label, pause, stepMul, h.frames, h.heapSumKB / (double)h.frames,
        h.heapPeakKB, h.cycles);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptLuaGcPace) return true;

    if (Config::g_settings.OptLuaGcCoalesce) {
        Log("[LuaGcPace] NOT active: the adaptive GC governor is on, and it writes "
            "the pause and step multiplier itself on every frame. Two modules "
            "arguing over the last write would measure neither. Turn the governor "
            "off to run this.");
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("LuaGcPace", &g_abSubject);

    // The pair cannot be read here. Features initialise before a Lua VM exists,
    // and the tuning in lua_optimize.cpp writes its values when one appears, so
    // asking now would always find nothing and this would always decline. The
    // first frame that finds the tuning applied resolves it and says so.
    Log("[LuaGcPace] ACTIVE, waiting for the first Lua VM. The collector this paces "
        "was the top two entries of a 2026-09-20 profile at 12.37%% of executing "
        "main-thread time together, and neither number pacing it was ever measured.");
    return true;
}

namespace {

// Called once, on the first frame where the tuning in lua_optimize.cpp has run.
void Resolve() {
    const LuaOpt::Stats s = LuaOpt::GetStats();
    g_pauseOurs   = s.gcPause;
    g_stepOurs    = s.gcStepMul;
    g_pauseTheirs = s.gcPauseOrig;
    g_stepTheirs  = s.gcStepMulOrig;
    g_identical   = (g_pauseOurs == g_pauseTheirs && g_stepOurs == g_stepTheirs);
    g_resolved    = true;

    Log("[LuaGcPace] this DLL runs the collector at pause %d stepmul %d; the client "
        "itself had %d and %d. %llu frame(s) went by before the tuning was applied "
        "and are not in any half below.",
        g_pauseOurs, g_stepOurs, g_pauseTheirs, g_stepTheirs, g_beforeTuning);
    if (g_identical) {
        Log("[LuaGcPace]   those two pairs are the same, so an A/B run would compare "
            "a setting with itself. The heap and cycle counts are still real; the "
            "comparison is not.");
    }
    if (g_abSubject) {
        Log("[LuaGcPace]   under A/B test: ON stints run our pacing, OFF stints run "
            "the client's own. Frame times for each half are in the AbTest report; "
            "the heap and the cycle count for each half are here. Read the cycle "
            "count first - halves that saw one cycle each compared two accidents.");
    } else {
        Log("[LuaGcPace]   not the A/B subject this session, so our pacing is left "
            "in place throughout and the numbers describe it alone. Set "
            "AbTestSubject to LuaGcPace to get the comparison.");
    }
}

}  // namespace

void OnFrame() {
    if (!g_installed) return;
    if (LuaOpt::IsReloading() || LuaOpt::IsSwapping()) { ++g_reloading; return; }

    void* L = *kLuaStateSlot;
    if (!L) { ++g_noState; return; }

    if (!g_resolved) {
        if (!LuaOpt::GetStats().gcOptimized) { ++g_beforeTuning; return; }
        Resolve();
    }

    // StandAside, not FeatureOn. Both read the phase, but only StandAside tells
    // the harness the subject was reached. With FeatureOn a three-hour run of
    // 279 stints each way was reported as never reached during an OFF stint,
    // and the harness, correctly given what it could see, printed no result.
    const int phase = (g_abSubject && AbTest::StandAside()) ? 0 : 1;
    if (phase != g_lastPhase) {
        g_lastPhase = phase;
        ++g_transitions;
        // The trough detector carries a peak across the boundary otherwise, and
        // would score the first dip of the new half as a cycle of the old one.
        g_ours.peakSinceTrough = 0.0;
        g_theirs.peakSinceTrough = 0.0;
    }

    // Written every frame, not once per stint. A VM reload re-runs the tuning in
    // lua_optimize.cpp and would restore our values mid-stint.
    if (phase == 0) {
        g_lua_gc(L, kGcSetPause, g_pauseTheirs);
        g_lua_gc(L, kGcSetStepMul, g_stepTheirs);
    } else {
        g_lua_gc(L, kGcSetPause, g_pauseOurs);
        g_lua_gc(L, kGcSetStepMul, g_stepOurs);
    }

    const double kb = HeapKB(L);
    if (kb < 0.0) return;
    Account(phase == 0 ? g_theirs : g_ours, kb);
}

void Shutdown() {
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptLuaGcPace) return;
    if (!g_installed) {
        Log("[LuaGcPace] not installed - the reason is at the top of this log");
        return;
    }

    if (!g_resolved) {
        Log("[LuaGcPace] installed and still waiting: %llu frame(s) had no "
            "lua_State, %llu were inside a VM reload, and %llu found a VM whose "
            "collector this DLL had not tuned. Nothing was measured.",
            g_noState, g_reloading, g_beforeTuning);
        return;
    }

    const unsigned long long seen = g_ours.frames + g_theirs.frames;
    if (seen == 0) {
        Log("[LuaGcPace] no frame reached the collector: %llu had no lua_State and "
            "%llu were inside a VM reload or swap. Nothing was measured.",
            g_noState, g_reloading);
        return;
    }

    Log("[LuaGcPace] %llu frame(s) measured; %llu had no lua_State, %llu were "
        "inside a VM reload and %llu came before the tuning was applied. Plain "
        "counters, lower bounds.",
        seen, g_noState, g_reloading, g_beforeTuning);
    ReportHalf("ours", g_pauseOurs, g_stepOurs, g_ours);
    ReportHalf("client", g_pauseTheirs, g_stepTheirs, g_theirs);

    if (g_abSubject) {
        Log("[LuaGcPace]   %lu phase change(s). The frame times for the two halves "
            "are in the AbTest report; this module deliberately does not restate "
            "them, because two instruments printing one number is how they start "
            "disagreeing.", g_transitions);
        const unsigned long fewest =
            g_ours.cycles < g_theirs.cycles ? g_ours.cycles : g_theirs.cycles;
        if (fewest < 4)
            Log("[LuaGcPace]   one half saw only %lu collection cycle(s). That is "
                "too few to compare; the run needs longer stints or a busier "
                "session before either half's frame time means anything.", fewest);
    }

    double tp = 0.0, sp = 0.0;
    unsigned long ts = 0, ss = 0, win = 0;
    const bool haveT = SamplingProfiler::ShareForRange(kTraverseLo, kTraverseHi,
                                                       20000, &tp, &ts, &win);
    const bool haveS = SamplingProfiler::ShareForRange(kSweepLo, kSweepHi,
                                                       20000, &sp, &ss, &win);
    if (haveT && haveS) {
        Log("[LuaGcPace]   the collector itself, over the last %lu profiler "
            "sample(s): luaC_traversetable %.2f%%, luaC_sweeplist %.2f%%, %.2f%% "
            "together. That is the most recent samples the ring still holds, "
            "which on a long session is its last stretch and not all of it, and "
            "it is not split by half - it says what the collector costs, not "
            "which pacing costs less.", win, tp, sp, tp + sp);
    } else {
        Log("[LuaGcPace]   what the collector costs is not measured this session: "
            "the sampling profiler is off or has too few samples. That is a "
            "missing measurement, not a zero.");
    }
}

}  // namespace LuaGcPace
