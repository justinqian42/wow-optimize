#include "perf_diagnostics.h"
#include "../core/world_position.h"
#include "version.h"
#include "crash_dumper.h"
#include "mimalloc_high_arena.h"
#include "high_placement.h"
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <atomic>

extern "C" void Log(const char* fmt, ...);
extern void CrashDumper_DumpHookTrace(int count);
// Whether an address belongs to this project's allocator.
//
// The stutter snapshot has been printing "private (heap/allocator)" against the
// largest reservations for as long as it has existed, and a tester log finally
// showed why that is not enough: 128 MB, 128 MB, 51 MB, 32 MB, 32 MB, every one
// of them below 2GB and every one of them described with the same six words. The
// question the whole low-address-space investigation turns on is whether those
// are the client's or ours, and mimalloc can answer it directly.
extern "C" bool mi_is_in_heap_region(const void* p);

namespace PerfDiagnostics {

static DWORD g_lastDiagTick = 0;
static std::atomic<long> g_stutterCount{0};


// Insert one allocation into a descending top-N list, dropping the smallest.
template <typename T>
static void TrackTopReservation(T* top, int n, uintptr_t base, SIZE_T size, DWORD type) {
    if (size <= top[n - 1].size) return;
    int i = n - 1;
    while (i > 0 && top[i - 1].size < size) { top[i] = top[i - 1]; i--; }
    top[i].base = base; top[i].size = size; top[i].type = type;
}

// Name an allocation so the log says "d3d9.dll" instead of a bare address.
static void DescribeAllocation(uintptr_t base, DWORD type, char* out, size_t outSize) {
    if (type == MEM_IMAGE) {
        char path[MAX_PATH];
        if (GetModuleFileNameA((HMODULE)base, path, MAX_PATH)) {
            const char* leaf = strrchr(path, '\\');
            lstrcpynA(out, leaf ? leaf + 1 : path, (int)outSize);
            return;
        }
        lstrcpynA(out, "image", (int)outSize);
        return;
    }
    if (type == MEM_MAPPED) { lstrcpynA(out, "mapped file/section", (int)outSize); return; }
    // "private" covers both heaps in this process, and which one it is decides
    // whether the fix is ours to make.
    //
    // The high arena's blocks are checked first, by address. This project
    // reserves them and hands them to mimalloc, but mi_is_in_heap_region answers
    // from mimalloc's page map, which has no entry for address space it has not
    // put to use. A stutter snapshot listed seven 256 MB blocks of ours as "the
    // client's own heap, or something else", which is the one question this
    // label exists to answer, answered backwards.
    if (MimallocHighArena::Contains((const void*)base)) {
        lstrcpynA(out, "reserved - THIS TOOL'S HIGH ARENA (handed to mimalloc)",
                  (int)outSize);
        return;
    }
    bool ours = false;
    __try { ours = mi_is_in_heap_region((const void*)base); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ours = false; }
    lstrcpynA(out, ours ? "private - THIS TOOL'S ALLOCATOR (mimalloc)"
                        : "private - the client's own heap, or something else",
              (int)outSize);
}

// Who is holding the low 2GB.
//
// The one question nobody has been able to answer. A tester session ends with
// "VA Space (below 2GB): Free=16MB LargestBlock=1MB" while the working set is
// 903MB, so a gigabyte of the half the client allocates from is *reserved* and
// not resident - and nothing said by what. That is the state in which a
// SavedVariables filename comes out as ")_.lua".
//
// The snapshot above already collects exactly this, but over the whole address
// space, where a 3GB machine's high half swamps the list. Restricted to the low
// half it names the owner instead: a module by filename, a mapped section, or
// private memory, which on this client means the allocator.
//
// Called from the heap compactor's monitor thread, so the walk is not on the
// main thread - the whole point of the last release's work. Rate limited by the
// caller, because it is a full VirtualQuery walk.
void LogLowHalfOccupancy(const char* why) {
    static const uintptr_t kLowEnd = 0x80000000u;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    SIZE_T commitPrivate = 0, commitMapped = 0, commitImage = 0, reservedOnly = 0;
    SIZE_T totalFree = 0, largestFree = 0, currentFree = 0;
    // Split by owner, which is the whole point of looking at this half.
    SIZE_T oursBytes = 0, theirsBytes = 0;
    unsigned oursRegions = 0;

    struct TopEntry { uintptr_t base; SIZE_T size; DWORD type; };
    const int kTopN = 10;
    TopEntry top[kTopN] = {};
    uintptr_t runBase = 0; SIZE_T runSize = 0; DWORD runType = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    unsigned regions = 0;

    while (addr < kLowEnd && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if (base >= kLowEnd) break;

        // A region straddling the boundary counts only its low part, or the
        // totals inflate and the answer is about a range nobody asked about.
        SIZE_T size = mbi.RegionSize;
        if (base + size > kLowEnd) size = (SIZE_T)(kLowEnd - base);

        if (mbi.State == MEM_FREE) {
            totalFree += size;
            currentFree += size;
            if (currentFree > largestFree) largestFree = currentFree;
        } else {
            currentFree = 0;
            if (mbi.State == MEM_COMMIT) {
                if      (mbi.Type == MEM_IMAGE)  commitImage   += size;
                else if (mbi.Type == MEM_MAPPED) commitMapped  += size;
                else                             commitPrivate += size;
            } else {
                reservedOnly += size;
            }
            if (mbi.Type == MEM_PRIVATE) {
                bool ours = false;
                __try { ours = mi_is_in_heap_region((const void*)base); }
                __except (EXCEPTION_EXECUTE_HANDLER) { ours = false; }
                if (ours) { oursBytes += size; ++oursRegions; }
                else      { theirsBytes += size; }
            }
            uintptr_t allocBase = (uintptr_t)mbi.AllocationBase;
            if (allocBase != runBase) {
                if (runSize > 0) TrackTopReservation(top, kTopN, runBase, runSize, runType);
                runBase = allocBase; runSize = 0; runType = mbi.Type;
            }
            runSize += size;
        }

        ++regions;
        addr = base + mbi.RegionSize;
        if (mbi.RegionSize == 0) addr += 0x10000;
        if (addr < base) break;                       // overflow
    }
    if (runSize > 0) TrackTopReservation(top, kTopN, runBase, runSize, runType);

    QueryPerformanceCounter(&t1);
    double walkMs = freq.QuadPart
                  ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
                  : 0.0;

    Log("[LowHalf] === WHAT IS HOLDING THE LOW 2GB === (%s)", why ? why : "no reason given");
    Log("[LowHalf]   free %.0f MB in %u region(s), largest run %.0f MB",
        totalFree / (1024.0 * 1024.0), regions, largestFree / (1024.0 * 1024.0));
    Log("[LowHalf]   in use: private %.0f MB, mapped %.0f MB, image %.0f MB, "
        "reserved but never committed %.0f MB",
        commitPrivate / (1024.0 * 1024.0), commitMapped / (1024.0 * 1024.0),
        commitImage / (1024.0 * 1024.0), reservedOnly / (1024.0 * 1024.0));

    // This used to print "mimalloc holds N MB committed" from mi_process_info,
    // which on Windows returns the private bytes of the whole process. It told
    // the reader to compare the low half against this tool's allocator and gave
    // them the client's own total to compare with. These are the two figures
    // those words meant.
    {
        PROCESS_MEMORY_COUNTERS pmc = {};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            Log("[LowHalf]   the whole process has %.0f MB of private bytes (peak "
                "%.0f MB), the client and this tool together.",
                pmc.PagefileUsage / (1024.0 * 1024.0),
                pmc.PeakPagefileUsage / (1024.0 * 1024.0));
        } else {
            Log("[LowHalf]   the process's private bytes could not be read.");
        }
        unsigned long long arenaCommitted = 0, arenaHanded = 0;
        if (MimallocHighArena::CommittedInArena(&arenaCommitted, &arenaHanded)) {
            Log("[LowHalf]   the high arena has %.0f MB committed of the %.0f MB "
                "handed to mimalloc above 2GB.",
                arenaCommitted / (1024.0 * 1024.0), arenaHanded / (1024.0 * 1024.0));
        } else {
            Log("[LowHalf]   the high arena is not active, so none of this tool's "
                "allocator is being kept above 2GB.");
        }
    }

    // The one line this dump exists to produce.
    Log("[LowHalf]   of the private memory below 2GB, %.0f MB in %u region(s) "
        "belongs to this tool's allocator and %.0f MB does not. If the first "
        "number is the larger one, Keep the Allocator Above 2GB is the fix; if "
        "the second is, it is not.",
        oursBytes / (1024.0 * 1024.0), oursRegions,
        theirsBytes / (1024.0 * 1024.0));

    Log("[LowHalf]   largest %d reservations below 2GB:", kTopN);
    for (int i = 0; i < kTopN && top[i].size > 0; ++i) {
        char owner[MAX_PATH];
        DescribeAllocation(top[i].base, top[i].type, owner, sizeof(owner));
        Log("[LowHalf]     0x%08X  %7.1f MB  %s",
            (unsigned)top[i].base, top[i].size / (1024.0 * 1024.0), owner);
    }
    // The owner of the private figure, which the list above cannot give: it
    // knows images and mapped files by name and everything else only as
    // "private". Not measured unless the address space census is on.
    HighPlacement::LogLiveByCaller(true, "the private figure above");
    Log("[LowHalf]   walk took %.1f ms on the heap monitor thread, not on the "
        "main one.", walkMs);
    Log("[LowHalf] ====================================");
}

void LogPerformanceSnapshot(double elapsedMs) {
    DWORD now = GetTickCount();
    if (now - g_lastDiagTick < 5000) return; // Rate-limit to once every 5 seconds
    g_lastDiagTick = now;

    // This snapshot is expensive and it runs on the main thread, inside the
    // frame it is describing - the address-space walk below is VirtualQuery once
    // per region and costs tens of milliseconds on a fragmented 3GB space. That
    // is the right trade for a stutter that has already happened, but only if
    // the log says how much of the reported frame was this. Unsaid, the next
    // reader subtracts nothing and treats the whole spike as the client's.
    LARGE_INTEGER snapFreq, snapStart;
    QueryPerformanceFrequency(&snapFreq);
    QueryPerformanceCounter(&snapStart);
    
    g_stutterCount.fetch_add(1, std::memory_order_relaxed);
    
    Log("[PerfDiag] === STUTTER DETECTED (Frame duration: %.1f ms) ===", elapsedMs);
    
    // 1. Where this happened. Not a player position: the client's terrain
    //    streaming centre, the only world coordinate available here that the
    //    client actually writes. Said plainly when it cannot be read - a
    //    stutter at the origin and a stutter with no world are different facts,
    //    and the line this replaces printed 0.00, 0.00 for both, always.
    float pos[3];
    if (WowWorld::StreamCentre(pos)) {
        Log("[PerfDiag]   World position: X=%.2f, Y=%.2f, Z=%.2f",
            pos[0], pos[1], pos[2]);
    } else {
        Log("[PerfDiag]   World position: no world loaded, nothing to report");
    }
    
    // 2. Memory State
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        Log("[PerfDiag]   Working Set: %.1f MB  Private Bytes: %.1f MB", 
            pmc.WorkingSetSize / (1024.0 * 1024.0), 
            pmc.PagefileUsage / (1024.0 * 1024.0));
    }
    
    // 3. Virtual Address Space Check
    //    We walk every region once; while we're here, bucket the FREE regions by
    //    size so we can see the *shape* of the fragmentation, not just the single
    //    largest block. The key number is "usable" free space (blocks big enough
    //    to satisfy a real allocation) vs total free — a large gap between them
    //    means the free space is chopped into unusable slivers, which is what a
    //    segregating VA arena would fix. Buckets are the upper bound of each band.
    static const SIZE_T kBucketMax[] = {
        64 * 1024, 256 * 1024, 1 * 1024 * 1024, 4 * 1024 * 1024,
        16 * 1024 * 1024, 64 * 1024 * 1024, (SIZE_T)-1
    };
    static const char* const kBucketName[] = {
        "<64K", "64K-256K", "256K-1M", "1M-4M", "4M-16M", "16M-64M", ">=64M"
    };
    const int kNumBuckets = 7;
    int   freeCount[7]  = {0};
    SIZE_T freeBytes[7] = {0};

    // The scan must cover the whole user-mode range. Hardcoding 0x7FFF0000 was
    // wrong on a large-address-aware client (WoW is /LARGEADDRESSAWARE, so on
    // 64-bit Windows it gets ~4GB): we stopped at 2GB but still added each
    // region's *full* size, so a free block straddling the 2GB line inflated the
    // totals and hid all fragmentation above 2GB. Ask the OS for the real limit.
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t kScanEnd = (uintptr_t)si.lpMaximumApplicationAddress;

    // Where the address space actually went. Free space alone can't tell us
    // whether we're out of VA because something committed it or because
    // something reserved a huge block and never touched it.
    SIZE_T commitPrivate = 0, commitMapped = 0, commitImage = 0, reservedOnly = 0;

    // Biggest single reservations, tracked without allocating: regions of one
    // VirtualAlloc share an AllocationBase and are walked consecutively, so we
    // accumulate the run and only keep the top few.
    struct TopEntry { uintptr_t base; SIZE_T size; DWORD type; };
    const int kTopN = 8;
    TopEntry top[kTopN] = {};
    uintptr_t runBase = 0; SIZE_T runSize = 0; DWORD runType = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    SIZE_T largestFree = 0, totalFree = 0, usableFree = 0; // usableFree = blocks >= 1MB
    while (addr < kScanEnd) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) { addr += 0x10000; continue; }

        // Never count past the end of the range we're scanning.
        SIZE_T size = mbi.RegionSize;
        if (addr + size > kScanEnd) size = kScanEnd - addr;

        if (mbi.State == MEM_FREE) {
            if (size > largestFree) largestFree = size;
            totalFree += size;
            if (size >= 1 * 1024 * 1024) usableFree += size;
            for (int b = 0; b < kNumBuckets; b++) {
                if (size <= kBucketMax[b]) { freeCount[b]++; freeBytes[b] += size; break; }
            }
        } else {
            if (mbi.State == MEM_COMMIT) {
                if      (mbi.Type == MEM_IMAGE)  commitImage  += size;
                else if (mbi.Type == MEM_MAPPED) commitMapped += size;
                else                             commitPrivate += size;
            } else {
                reservedOnly += size;
            }

            uintptr_t base = (uintptr_t)mbi.AllocationBase;
            if (base != runBase) {
                if (runSize > 0) TrackTopReservation(top, kTopN, runBase, runSize, runType);
                runBase = base; runSize = 0; runType = mbi.Type;
            }
            runSize += size;
        }

        addr += mbi.RegionSize;
        if (mbi.RegionSize == 0) addr += 0x10000;
    }
    if (runSize > 0) TrackTopReservation(top, kTopN, runBase, runSize, runType);

    Log("[PerfDiag]   VA Total Free: %.1f MB  Largest Block: %.1f MB%s",
        totalFree / (1024.0 * 1024.0),
        largestFree / (1024.0 * 1024.0),
        (largestFree < 64 * 1024 * 1024) ? " [WARNING: FRAGMENTED]" : "");
    Log("[PerfDiag]   VA in use: private %.1f MB, mapped %.1f MB, image %.1f MB, reserved-only %.1f MB",
        commitPrivate / (1024.0 * 1024.0), commitMapped / (1024.0 * 1024.0),
        commitImage / (1024.0 * 1024.0), reservedOnly / (1024.0 * 1024.0));
    Log("[PerfDiag]   Largest VA reservations (who ate the address space):");
    for (int i = 0; i < kTopN && top[i].size > 0; i++) {
        char owner[MAX_PATH];
        DescribeAllocation(top[i].base, top[i].type, owner, sizeof(owner));
        Log("[PerfDiag]     0x%08X  %8.1f MB  %s",
            (unsigned)top[i].base, top[i].size / (1024.0 * 1024.0), owner);
    }

    // How much of "reserved-only" above is ours. This printed "mimalloc: commit
    // ..., rss ..." from mi_process_info, which on Windows returns the whole
    // process's private bytes and working set. In a tester's report its commit
    // matched the Private Bytes line at the top of the same snapshot to the tenth
    // of a megabyte, 1511.2 and 1511.2, under a name that said it was the
    // allocator's.
    {
        unsigned long long arenaCommitted = 0, arenaHanded = 0;
        if (MimallocHighArena::CommittedInArena(&arenaCommitted, &arenaHanded)) {
            Log("[PerfDiag]   high arena: %.1f MB committed of %.1f MB handed to "
                "mimalloc, so %.1f MB of the reserved-only figure above is this "
                "tool's",
                arenaCommitted / (1024.0 * 1024.0), arenaHanded / (1024.0 * 1024.0),
                (arenaHanded - arenaCommitted) / (1024.0 * 1024.0));
        } else {
            Log("[PerfDiag]   high arena: not active");
        }
    }
    Log("[PerfDiag]   VA Usable Free (>=1MB blocks): %.1f MB of %.1f MB  (%.0f%% lost to slivers)",
        usableFree / (1024.0 * 1024.0),
        totalFree / (1024.0 * 1024.0),
        totalFree ? (100.0 * (double)(totalFree - usableFree) / (double)totalFree) : 0.0);
    for (int b = 0; b < kNumBuckets; b++) {
        if (freeCount[b] > 0) {
            Log("[PerfDiag]     free[%-8s] count=%-5d total=%.1f MB",
                kBucketName[b], freeCount[b], freeBytes[b] / (1024.0 * 1024.0));
        }
    }
        
    // 4. What happened inside the spike. A state transition landing in a stutter
    // (loading boundary, device reset, cache invalidation) is almost always the
    // explanation, and it is the one thing a raw frame time cannot tell us.
    //
    // Bounded to the stutter itself. Unbounded, this printed whatever was newest
    // in the ring, which in a quiet session is a loading screen from minutes ago -
    // an unrelated event presented as the cause.
    Log("[PerfDiag]   Events within the stutter:");
    CrashDumper::DumpTrace(16, (DWORD)(elapsedMs + 0.5) + 50);

    // 5. Feature usage. Only features that recorded activity are listed - printing
    // every active feature meant ~70 lines of "calls=0" per stutter, because
    // FeatureCall() is wired into almost nothing.
    Log("[PerfDiag]   Features with recorded activity:");
    FeatureState features[MAX_TRACKED_FEATURES];
    int fcount = CrashDumper::GetFeatureStates(features, MAX_TRACKED_FEATURES);
    int reported = 0;
    for (int i = 0; i < fcount; i++) {
        if (features[i].callCount == 0 && features[i].errorCount == 0) continue;
        Log("[PerfDiag]     %-28s active=%d calls=%lld errors=%lld",
            features[i].name ? features[i].name : "(null)",
            features[i].active ? 1 : 0,
            features[i].callCount,
            features[i].errorCount);
        reported++;
    }
    if (reported == 0) Log("[PerfDiag]     (none)");

    // 6. Dump last hook trace to pinpoint exactly what ran during this lag spike
    Log("[PerfDiag]   Last 16 hook calls before stutter:");
    CrashDumper_DumpHookTrace(16);
    
    if (snapFreq.QuadPart) {
        LARGE_INTEGER snapEnd;
        QueryPerformanceCounter(&snapEnd);
        double snapMs = (double)(snapEnd.QuadPart - snapStart.QuadPart) * 1000.0
                      / (double)snapFreq.QuadPart;
        Log("[PerfDiag]   Collecting this snapshot took %.1f ms on the main "
            "thread. The %.1f ms frame above was measured before it ran, so "
            "this is not part of that number - but it is part of the next "
            "frame, and most of it is the address-space walk.",
            snapMs, elapsedMs);
    }
    Log("[PerfDiag] ==================================================");
}

// A full snapshot is about twenty lines - sixteen hook calls, the VA reservation
// table, player position. Printing one per stutter was fine in a smooth session
// and ruinous in a rough one: a 7MB tester log contains 829 of them, and writing
// them is itself work on the main thread, so the diagnostic was making the
// stutters it reports worse.
//
// The first few say everything the later ones do. After that a stutter is worth
// counting, not describing again.
static constexpr DWORD STUTTER_QUIET_MS   = 30000;
static constexpr LONG  STUTTER_MAX_REPORTS = 12;

static DWORD s_lastStutterReport = 0;
static LONG  s_stutterReports    = 0;
static LONG  s_stuttersSeen      = 0;
static LONG  s_gapsIgnored       = 0;

// Above this, the number stops describing a frame.
//
// One log reported "STUTTER DETECTED (Frame duration: 179395.5 ms)" - three
// minutes. Nothing stalled for three minutes; the window was not on screen, so
// Present was not called, and the gap between two Presents was measured as
// though it were one frame. Alt-tabbing, minimising and sitting on a loading
// screen all produce it, and each one burned a snapshot out of the twelve this
// module is allowed to write - snapshots that describe an idle process.
//
// FrameBench already draws this line at two seconds for its percentiles. Here
// the line is higher, because a genuine multi-second freeze is exactly what this
// module exists to catch and must still be described. Thirty seconds is past
// anything a player sits through and calls a stutter.
static constexpr double STUTTER_CEILING_MS = 30000.0;

void OnFrame(double elapsedMs) {
    #if !TEST_DISABLE_SAMPLING_PROFILER
    if (elapsedMs > STUTTER_CEILING_MS) {
        ++s_gapsIgnored;
        return;
    }

    // If a frame takes longer than 100ms (10 FPS or below), it's a severe stutter
    if (elapsedMs > 100.0) {
        ++s_stuttersSeen;

        if (s_stutterReports >= STUTTER_MAX_REPORTS) return;

        DWORD now = GetTickCount();
        if (s_lastStutterReport != 0 && now - s_lastStutterReport < STUTTER_QUIET_MS)
            return;

        s_lastStutterReport = now;
        ++s_stutterReports;
        LogPerformanceSnapshot(elapsedMs);

        if (s_stutterReports == STUTTER_MAX_REPORTS) {
            Log("[PerfDiag] That is %d snapshots; further stutters are counted "
                "only. See the summary at the end.", (int)STUTTER_MAX_REPORTS);
        }
    }
    #endif
}

void LogStats() {
    if (s_stuttersSeen > 0) {
        Log("[PerfDiag] %ld frames over 100ms this session, %ld described in full",
            (long)s_stuttersSeen, (long)s_stutterReports);
    }
    if (s_gapsIgnored > 0) {
        Log("[PerfDiag] %ld gaps over %.0f s ignored - the window was not being "
            "drawn (alt-tab, minimise, loading), so they are not frames",
            (long)s_gapsIgnored, STUTTER_CEILING_MS / 1000.0);
    }
}

bool Init() {
    g_lastDiagTick = 0;
    g_stutterCount.store(0);
    Log("[PerfDiag] Performance Diagnostic Monitor Active (100ms stutter trigger)");
    return true;
}

void Shutdown() {
}

} // namespace PerfDiagnostics
