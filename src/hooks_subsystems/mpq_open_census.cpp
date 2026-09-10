// ============================================================================
// Module: mpq_open_census
// Description: Counts and times the client's archive file-open path, and says
//              how much of it is asking for the same name twice.
// Safety & Threading: Counting hook. Calls through on every path.
// ============================================================================
//
// Why this exists, in one measurement from the field
//
// Sicsoo's session, 2026-09-10, one loading screen:
//
//     Load took 45509 ms
//       89 ms (0%) inside ReadFile, 18483 reads, 267.8 MB
//       97 ms (0%) compiling Lua over 3496 chunks
//        0 ms (0%) inside the client's own file writes
//
// That accounts for 186 ms of 45509. Ninety-nine point six percent of a
// forty-five second loading screen is unexplained, and it is the largest single
// thing a player waits for anywhere in these logs. It is also not disk: 267.8 MB
// arrived in 89 ms, which is three gigabytes a second, so the bytes were already
// in the operating system's cache.
//
// So the time is going somewhere that is neither reading, nor writing, nor
// compiling. The obvious candidate has never been counted.
//
// ---------------------------------------------------------------------------
// What is being counted
//
// Storm is linked into wow.exe rather than shipped as storm.dll - two modules in
// this project look for a storm.dll that does not exist and quietly get nothing.
// Its assert strings put SFile2-Core.cpp between 0x00421950 and 0x00425010, and
// sub_424B50 in that range is __stdcall with four arguments, the second a char*,
// and has 257 callers, which is more than anything else there. That is
// SFileOpenFileEx: open a file by name, optionally in a named archive.
//
// A name that is not in the archive it is asked about is searched for in every
// open archive in turn, and a WoW install of this vintage carries a base set
// plus every patch plus whatever a private server adds. So a miss is not one
// hash lookup, it is one per archive, and the client asks for files that are not
// there constantly: optional textures, per-race variants, sounds an effect might
// not have.
//
// ---------------------------------------------------------------------------
// The number that decides what to build next
//
// Repeat misses. A name that has already been searched for and not found, being
// searched for again, is work that a negative cache would remove entirely and
// the only work here that is provably removable. Every other kind of open has to
// happen at least once.
//
// So this counts three things separately - found, missed for the first time, and
// missed again - and names the worst repeat offenders. If repeat misses are a
// large share of a loading screen then a negative cache is worth building and
// this says roughly what it would save. If they are not, the idea is dead for
// one log line and the forty-five seconds are somewhere else.
//
// Nothing is cached here and no call is skipped. Every call goes through to the
// client, in order, and the only thing that changes is that afterwards there is
// a number.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "mpq_open_census.h"
#include "loading_state.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace MpqOpenCensus {

namespace {

// SFileOpenFileEx. Identified by its assert strings, its four stdcall arguments
// with a char* second, and 257 callers - the most of anything in Storm's range.
constexpr uintptr_t kOpen = 0x00424B50;

typedef int (__stdcall* Open_t)(void* archive, const char* name, int scope,
                                void** out);
Open_t orig_Open = nullptr;

bool g_installed = false;

// Plain 32-bit, main thread. Lower bounds if that ever stops being true.
unsigned long g_calls      = 0;
unsigned long g_found      = 0;
unsigned long g_missFirst  = 0;
unsigned long g_missRepeat = 0;
unsigned long g_noName     = 0;

// Loading screens only, so gameplay is separated from the thing being explained.
unsigned long g_callsLoad      = 0;
unsigned long g_missRepeatLoad = 0;
double        g_msLoad         = 0.0;
double        g_msTotal        = 0.0;

double g_qpcPerMs = 0.0;

// Names that have already been searched for and not found. Direct-mapped, so a
// collision loses a name rather than inventing one: two names in one slot means
// the second is counted as a first miss, which under-reports repeats. That is
// the safe direction for a number whose whole purpose is to justify building
// something.
constexpr int kSlots = 8192;
constexpr int kMask  = kSlots - 1;

struct Slot {
    uint32_t hash;
    uint32_t misses;
    char     name[52];
};
Slot* g_seen = nullptr;

// MPQ names are case-insensitive and mix separators, so both are folded before
// hashing - otherwise the same file under two spellings looks like two names.
uint32_t HashName(const char* s) {
    uint32_t h = 2166136261u;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        else if (c == '/') c = '\\';
        h ^= (unsigned char)c;
        h *= 16777619u;
    }
    return h ? h : 1u;   // zero marks an empty slot
}

double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_qpcPerMs;
}

int __stdcall Hooked_Open(void* archive, const char* name, int scope, void** out) {
    // An unnamed open cannot be attributed to anything, so it is counted apart
    // rather than folded in.
    if (!name || !g_seen) {
        ++g_calls;
        ++g_noName;
        return orig_Open(archive, name, scope, out);
    }

    const bool loading = LoadingState::IsLoading();
    const double t0 = NowMs();
    const int r = orig_Open(archive, name, scope, out);
    const double dt = NowMs() - t0;

    ++g_calls;
    g_msTotal += dt;
    if (loading) { ++g_callsLoad; g_msLoad += dt; }

    if (r) { ++g_found; return r; }

    const uint32_t h = HashName(name);
    Slot& s = g_seen[h & kMask];
    if (s.hash == h) {
        ++s.misses;
        ++g_missRepeat;
        if (loading) ++g_missRepeatLoad;
    } else {
        ++g_missFirst;
        s.hash = h;
        s.misses = 1;
        // Kept only to name the worst offenders in the report.
        size_t n = strlen(name);
        const char* tail = (n >= sizeof(s.name)) ? name + (n - sizeof(s.name) + 1)
                                                 : name;
        strncpy(s.name, tail, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';
    }
    return r;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptMpqOpenCensus) return true;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (f.QuadPart == 0) {
        Log("[MpqOpen] NOT active: no performance counter.");
        return false;
    }
    g_qpcPerMs = (double)f.QuadPart / 1000.0;

    g_seen = (Slot*)VirtualAlloc(nullptr, sizeof(Slot) * kSlots,
                                 MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                 PAGE_READWRITE);
    if (!g_seen) {
        Log("[MpqOpen] NOT active: the %u KB name table could not be reserved.",
            (unsigned)(sizeof(Slot) * kSlots / 1024));
        return false;
    }

    if (WineSafe_CreateHook((void*)kOpen, (void*)&Hooked_Open,
                            (void**)&orig_Open) != MH_OK ||
        WO_EnableHook((void*)kOpen) != MH_OK) {
        Log("[MpqOpen] NOT active: could not hook 0x%08X.", (unsigned)kOpen);
        return false;
    }
    g_installed = true;

    Log("[MpqOpen] ACTIVE on sub_424B50, the archive open-by-name call - Storm "
        "is linked into wow.exe here rather than shipped as storm.dll, and this "
        "is the entry point with 257 callers. A loading screen in the field took "
        "45509 ms with 89 ms of it inside ReadFile and 97 ms compiling Lua, so "
        "99.6%% of it is unexplained and the bytes were already in the operating "
        "system's cache. This counts the opens, times them, and separates a name "
        "that was searched for and not found from one that had already been "
        "searched for and not found - the second is the only work here a cache "
        "could remove. Nothing is cached and no call is skipped.");
    return true;
}

void Shutdown() {
    if (g_installed) MH_DisableHook((void*)kOpen);
    g_installed = false;
}

void OnLoadBegin() {
    g_callsLoad = 0;
    g_missRepeatLoad = 0;
    g_msLoad = 0.0;
}

void ReportLoad(double loadMs) {
    if (!g_installed || !Config::g_settings.OptMpqOpenCensus) return;
    if (g_callsLoad == 0) {
        Log("[LoadingState]   measured and zero: the client opened no archive "
            "file by name inside this loading screen.");
        return;
    }
    Log("[LoadingState]   and %.0f ms (%.0f%%) opening %lu archive files by "
        "name, %lu of which were a name already searched for and not found. "
        "That last group is the only part a cache could remove.",
        g_msLoad, (loadMs > 0.0) ? (100.0 * g_msLoad / loadMs) : 0.0,
        g_callsLoad, g_missRepeatLoad);
}

void LogStats() {
    if (!Config::g_settings.OptMpqOpenCensus) return;
    if (!g_installed) {
        Log("[MpqOpen] switched on but not installed, so nothing here was "
            "measured.");
        return;
    }
    if (g_calls == 0) {
        Log("[MpqOpen] installed, and no archive file has been opened by name. "
            "This is measured and zero, not unmeasured.");
        return;
    }

    Log("[MpqOpen] %lu opens, %.0f ms. %lu found, %lu missed for the first "
        "time, %lu missed a name already missed (%.1f%% of all opens). Counts "
        "are lower bounds and the time includes this hook's own cost.",
        g_calls, g_msTotal, g_found, g_missFirst, g_missRepeat,
        100.0 * (double)g_missRepeat / (double)g_calls);
    if (g_noName > 0)
        Log("[MpqOpen]   %lu opens had no name to attribute and are counted "
            "apart.", g_noName);

    // The worst offenders, so a name in the report can be looked at directly.
    for (int round = 0; round < 5; ++round) {
        int best = -1;
        unsigned long bestN = 1;
        for (int i = 0; i < kSlots; ++i)
            if (g_seen[i].hash && g_seen[i].misses > bestN) {
                bestN = g_seen[i].misses; best = i;
            }
        if (best < 0) break;
        Log("[MpqOpen]   %8lu misses  %s", g_seen[best].misses,
            g_seen[best].name);
        g_seen[best].misses = 0;   // report-time only; the totals above stand
    }
}

}  // namespace MpqOpenCensus
