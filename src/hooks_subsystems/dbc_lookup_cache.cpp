// ============================================================================
// Module: dbc_lookup_cache.cpp
// Description: Fast O(1) transformed row cache for DBC database queries.
// Safety & Threading: Thread-safe, executes on main/render threads.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "MinHook.h"
#include "version.h"
#include "dbc_lookup_cache.h"

extern "C" void Log(const char* fmt, ...);
#include "crash_dumper.h"
#include "sampling_profiler.h"

// Geometry, chosen from the field rather than from a round number.
//
// The table used to be 4096 entries, direct-mapped, indexed by
// `((storeKey >> 2) ^ recordId) & 4095`. Three things about that turned out to
// be wrong together.
//
// There is only one store. All 345 call sites of sub_4CFD20 load ecx with
// `offset off_AD49D0` - the same constant object every time - so `storeKey` is
// invariant and the index reduced to `recordId & 4095` under a fixed xor. It
// was not a hash at all, it was the record number modulo the table size.
//
// That leaves the record ids to collide directly, and they do. The seen-bitmap
// counts 4331 distinct rows touched in one session and 1357 in another, against
// 4096 slots, so by the pigeonhole alone some ids share a slot - and the field
// says those shares are hot: 19637129 of 117085068 calls were rows that had
// been in the table and were pushed out, an 83.2% hit rate. The other session is
// worse and is the one that settles it: 1357 distinct rows, a third of the
// slots available, and still 6488363 evictions at a 62.7% hit rate. A working
// set that small cannot exhaust a table that large. Only collisions can do
// that.
//
// The last part is the one worth remembering. The instrument beside the cache -
// the seen bitmap - hashes with `recordId * 2654435761u` and mixes properly.
// The cache itself did not. The good hash went into the diagnostic and the raw
// xor into the thing being measured.
//
// So: 4096 sets of 4 ways, and a multiplicative index. At 4474 distinct rows
// that is about 1.09 rows per set against 4 ways, which puts the expected
// share of the working set in an over-subscribed set near 3% - the misses that
// remain should be close to the cold-miss floor of a few thousand a session.
// It costs 11.4 MB against 2.85 MB, committed MEM_TOP_DOWN into the half of
// the address space the client does not allocate from.
//
// Both indexes were run over three access patterns before this was written,
// counting the rows that do not fit their set. The middle row explains the
// field: a constant xor preserves stride, so ids walked at a fixed step collapse
// onto a fraction of the table however few of them there are.
//
//     4474 rows, pattern             old (direct)   new (4096 sets x 4 ways)
//     dense 0..4473                     8.4%           0.4%
//     stride 4 over 0..18000           77.2%           0.2%
//     random over 0..80000             37.8%           0.7%
//
// The two field sessions miss at 16.8% and 37.3%, which lands between the first
// and third rows. The model and the logs agree without either being fitted to
// the other.
static constexpr int CACHE_SETS = 4096;
static constexpr int CACHE_WAYS = 4;
static constexpr int CACHE_SIZE = CACHE_SETS * CACHE_WAYS;
static constexpr int CACHE_SET_MASK = CACHE_SETS - 1;

// Which set a record belongs to. Knuth's multiplicative constant, taking the
// high bits, so ids that differ by a multiple of the set count land apart -
// which is exactly what the old index could not do.
static inline uint32_t DbcSetOf(uintptr_t storeKey, int recordId) {
    uint32_t h = (uint32_t)recordId * 2654435761u;
    h ^= (uint32_t)(storeKey >> 4) * 2246822519u;
    h ^= h >> 15;
    return (h >> 8) & CACHE_SET_MASK;
}

struct DbcRowEntry {
    std::atomic<uint32_t> seq;
    uintptr_t storePtr;   // DBCStore* — identifies which DBC file
    uint32_t  recordId;
    uint8_t   rowData[0x2A8]; // Cache the actual 680-byte record data directly!
    bool      valid;
};

// A filter, not a source of truth.
//
// Four ways means up to four tag comparisons, and with 696-byte entries those
// are four separate cache lines on a path that runs 117 million times a
// session. This array holds the same two words in eight bytes, so one set's
// four tags sit inside a single 32-byte span and a lookup touches one line to
// find the way instead of four to rule them out.
//
// Every authoritative check still reads the entry's own storePtr and recordId
// inside the seqlock, exactly as before. A stale or torn tag here can only send
// the lookup to a way that then fails the real check and falls through to the
// client, which is the same outcome as a miss.
struct DbcTag {
    uint32_t storeLow;
    uint32_t recordId;
};
static DbcTag* g_tags = nullptr;

// Which way in each set to overwrite next, when no way is free. One byte per
// set; a lost increment costs one avoidable eviction and nothing else.
static uint8_t* g_victim = nullptr;

// Committed by InstallDbcLookupCache rather than living in BSS. The feature is
// off by default, so 2.7 MB of a 32-bit address space was being reserved at DLL
// load for every player who never enabled it - and ClearDbcLookupCache walked
// all 4096 slots with an atomic compare-exchange each, on the main thread, at
// every lua_State change, clearing a cache that was never filled.
static DbcRowEntry* g_cache = nullptr;
static uint64_t   g_hits = 0;
static int g_featureToken = -1;
static uint64_t   g_misses = 0;

// A hit rate does not say which of two problems a miss is, and they have
// opposite answers. A row never looked up before has to be decoded once
// whatever we do. A row that was looked up before and is no longer here was
// pushed out, and that is what a larger table would recover.
//
// The split matters because the price is known: sub_4CFBB0 walks 680 output
// bytes with a compare and an unpredictable branch each, so a tester's 2.8
// million misses are somewhere between one and one and a half seconds of CPU in
// a session. Whether any of that is recoverable is exactly this question.
//
// One bit per (store, row) ever seen. 65536 bits is 8 KB and costs one test per
// miss; a collision marks a new row as seen, which counts a cold miss as an
// eviction and overstates what a bigger table would win. That is the direction
// to be wrong in only if the number is read as an upper bound, which is how the
// report words it.
static constexpr int SEEN_BITS = 65536;
static uint32_t   g_seen[SEEN_BITS / 32] = {};
static uint64_t   g_missCold = 0;
static uint64_t   g_missEvicted = 0;
// Every clear turns the whole table into evictions, so the two figures cannot
// be read without knowing how many there were.
static uint64_t   g_clears = 0;
// Inserts that had to throw a live row out because all four ways of the set
// were taken. This is the number that says whether the geometry is right: it
// should be a small fraction of the misses, and if it tracks them the working
// set has outgrown four ways and the answer is more of them, not more sets.
static uint64_t   g_evictedWay = 0;
// Calls handed straight back because the client's own path was a plain memcpy
// that this cache cannot improve on. See the note in the hook.
static uint64_t   g_bypassedPlainCopy = 0;

typedef bool (__thiscall *orig_dbc_getrow_t)(void* store, int recordId, void* outBuf);
static orig_dbc_getrow_t g_orig = nullptr;

static bool __fastcall Hooked_DbcGetRow(void* store, void* /* edx */, int recordId, void* outBuf)
{
#if TEST_DISABLE_DBC_LOOKUP_CACHE
    return g_orig(store, recordId, outBuf);
#else
    if (!store) {
        return g_orig(store, recordId, outBuf);
    }

    // Whether this cache can win at all is decided by one byte in the client.
    //
    // sub_4CFD20 is three comparisons, an indexed load, and then one of two
    // things: if byte_C5DEA0 is set it runs sub_4CFBB0, a byte-at-a-time RLE
    // decode over 680 bytes, and caching that is a real saving. If it is clear
    // it is a single 680-byte memcpy - and this cache cannot beat a memcpy,
    // because a hit costs a hash, two atomic loads and *two* 680-byte copies,
    // one into a temporary for the seqlock and one out to the caller.
    //
    // A tester's profile put this function at 3.76% of executing main-thread
    // time with 17.6 million hits in 28 minutes, which is what being slower than
    // the thing you are caching looks like. One byte load per call is a cheap
    // price for not paying that.
    if (!*(volatile uint8_t*)0x00C5DEA0) {
        ++g_bypassedPlainCopy;
        return g_orig(store, recordId, outBuf);
    }

    uintptr_t storeKey = (uintptr_t)store;
    const uint32_t set  = DbcSetOf(storeKey, recordId);
    const uint32_t base = set * CACHE_WAYS;

    // Find the way holding this record, reading the compact tags rather than
    // four entries. A way that matches here is still checked properly below.
    const DbcTag* tset = &g_tags[base];
    int way = -1;
    for (int w = 0; w < CACHE_WAYS; ++w) {
        if (tset[w].recordId == (uint32_t)recordId &&
            tset[w].storeLow == (uint32_t)storeKey) { way = w; break; }
    }

    DbcRowEntry* e = (way >= 0) ? &g_cache[base + way] : nullptr;
    if (!e) goto miss;

    // Optimistic lock-free read using Sequence Lock.
    //
    // The payload goes straight to the caller rather than through a temporary.
    // The seqlock was reading into tempBuf, verifying, then copying tempBuf out,
    // which is 1360 bytes moved per hit for 680 bytes of result - and a tester
    // session took 9869554 hits, so that spare copy was about 6.7 GB of memcpy
    // on the main thread.
    //
    // Writing the caller's buffer before the sequence is verified is safe here
    // because of what happens next: if the sequence moved, control falls through
    // to g_orig, which fills that same buffer completely. The caller cannot
    // observe the discarded bytes. It only works in that order, so the fall-
    // through below must stay unconditional.
    //
    // The key comparison also moves ahead of the copy. It is not the
    // authoritative check - the one after the sequence still is - but a slot
    // holding a different record is the common collision case and there is no
    // reason to copy 680 bytes before noticing.
    {
    uint32_t s1 = e->seq.load(std::memory_order_acquire);
    if ((s1 & 1) == 0 && e->valid &&
        e->storePtr == storeKey && e->recordId == (uint32_t)recordId) {
        if (outBuf) memcpy(outBuf, e->rowData, 0x2A8);
        uint32_t s2 = e->seq.load(std::memory_order_acquire);

        // If sequence didn't change, the data we read is consistent and valid
        if (s1 == s2 && e->storePtr == storeKey && e->recordId == (uint32_t)recordId) {
            g_hits++;
            // Sampled, not per hit. This runs seventeen million times a session,
            // and a cross-module counter call on a path this hot costs a real
            // fraction of the work it is counting - the same mistake this project
            // found once already in the SSE2 matrix-vector hook. One in 1024 is
            // still far more than enough to answer "is it reached".
            if ((g_hits & 1023u) == 0u) CrashDumper::FeatureHit(g_featureToken);
            return true;   // already in the caller's buffer
        }
    }
    }

miss:
    g_misses++;
    {
        const uint32_t sb = (uint32_t)((storeKey >> 2) ^ (uint32_t)recordId * 2654435761u)
                            & (SEEN_BITS - 1);
        uint32_t& word = g_seen[sb >> 5];
        const uint32_t bit = 1u << (sb & 31);
        if (word & bit) ++g_missEvicted; else { ++g_missCold; word |= bit; }
    }
    // Call original function to load
    bool result = g_orig(store, recordId, outBuf);

    if (result) {
        // Safe extraction of direct record pointer from DBCStore fields
        __try {
            uint32_t minId = *reinterpret_cast<const uint32_t*>(storeKey + 0x10);
            uint32_t maxId = *reinterpret_cast<const uint32_t*>(storeKey + 0x0C);
            if (recordId >= (int)minId && recordId <= (int)maxId) {
                uintptr_t rowsArray = *reinterpret_cast<const uintptr_t*>(storeKey + 0x20);
                if (rowsArray) {
                    const void* rptr = *reinterpret_cast<const void**>(rowsArray + (recordId - minId) * 4);
                    if (rptr != nullptr) {
                        // Choose where to put it. A way already carrying this
                        // record is reused; otherwise a free way, and only if
                        // the set is full does anything get thrown away.
                        if (!e) {
                            int pick = -1;
                            for (int w = 0; w < CACHE_WAYS; ++w) {
                                if (!g_cache[base + w].valid) { pick = w; break; }
                            }
                            if (pick < 0) {
                                pick = g_victim[set] & (CACHE_WAYS - 1);
                                g_victim[set] = (uint8_t)(pick + 1);
                                ++g_evictedWay;
                            }
                            e = &g_cache[base + pick];
                            g_tags[base + pick].storeLow = (uint32_t)storeKey;
                            g_tags[base + pick].recordId = (uint32_t)recordId;
                        }
                        uint32_t s = e->seq.load(std::memory_order_relaxed);
                        if ((s & 1) == 0) {
                            if (e->seq.compare_exchange_strong(s, s + 1, std::memory_order_acquire)) {
                                // Once the CAS claims the slot (seq is odd = "write in
                                // progress"), the seq MUST be advanced back to even no
                                // matter what — otherwise a fault while copying the row
                                // (rptr can go stale if the DBC store reloads mid-copy)
                                // leaves this slot's seq stuck odd forever, and
                                // ClearDbcLookupCache()'s spin-wait below then loops on
                                // it for the rest of the process (observed as a hang/
                                // "crash" during loading screens, GitHub issue #35).
                                bool wrote = false;
                                __try {
                                    e->storePtr = storeKey;
                                    e->recordId = (uint32_t)recordId;
                                    if (*(unsigned char*)0x00C5DEA0) {
                                        typedef void* (__cdecl *rle_decompress_fn)(const void*, int, void*);
                                        ((rle_decompress_fn)0x004CFBB0)(rptr, 0x2A8, e->rowData);
                                    } else {
                                        memcpy(e->rowData, rptr, 0x2A8); // Store actual row data
                                    }
                                    wrote = true;
                                } __except(EXCEPTION_EXECUTE_HANDLER) {
                                    wrote = false;
                                }
                                e->valid = wrote;
                                e->seq.store(s + 2, std::memory_order_release); // Even: write complete (or aborted)
                            }
                        }
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    return result;
#endif
}

bool InstallDbcLookupCache()
{
    if (!g_cache) {
        // MEM_TOP_DOWN: 11.4 MB of a 32-bit address space, and without this it
        // is taken from the bottom - the half the client allocates from, which
        // a field session reports at a 5 MB largest free block. Nothing about
        // the table changes, only where it lands. The size matters more now
        // than it did at 2.7 MB, and so does the flag.
        g_cache = (DbcRowEntry*)VirtualAlloc(nullptr, sizeof(DbcRowEntry) * CACHE_SIZE,
                                             MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                             PAGE_READWRITE);
        if (!g_cache) {
            Log("[DbcLookupCache] Could not commit %zu KB for the row cache - disabled",
                (sizeof(DbcRowEntry) * CACHE_SIZE) / 1024);
            return false;
        }
        g_tags = (DbcTag*)VirtualAlloc(nullptr, sizeof(DbcTag) * CACHE_SIZE,
                                       MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                       PAGE_READWRITE);
        g_victim = (uint8_t*)VirtualAlloc(nullptr, CACHE_SETS,
                                          MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                          PAGE_READWRITE);
        if (!g_tags || !g_victim) {
            Log("[DbcLookupCache] Could not commit the tag or victim array - disabled");
            if (g_tags)   { VirtualFree(g_tags, 0, MEM_RELEASE);   g_tags = nullptr; }
            if (g_victim) { VirtualFree(g_victim, 0, MEM_RELEASE); g_victim = nullptr; }
            VirtualFree(g_cache, 0, MEM_RELEASE);
            g_cache = nullptr;
            return false;
        }
    }
    memset(g_tags, 0, sizeof(DbcTag) * CACHE_SIZE);
    memset(g_victim, 0, CACHE_SETS);
    for (int i = 0; i < CACHE_SIZE; i++) {
        g_cache[i].storePtr = 0;
        g_cache[i].recordId = 0;
        g_cache[i].valid = false;
        memset(g_cache[i].rowData, 0, 0x2A8);
        g_cache[i].seq.store(0, std::memory_order_relaxed);
    }
    g_hits = 0;
    g_misses = 0;
    g_missCold = 0;
    g_missEvicted = 0;
    g_clears = 0;
    g_evictedWay = 0;
    memset(g_seen, 0, sizeof(g_seen));

    void* target = reinterpret_cast<void*>(0x004CFD20);

    unsigned char prologue[3];
    __try {
        memcpy(prologue, target, 3);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[DbcLookupCache] Target 0x004CFD20 not readable.");
        return true;
    }

    if (prologue[0] != 0x55 || prologue[1] != 0x8B || prologue[2] != 0xEC) {
        Log("[DbcLookupCache] BAD PROLOGUE at 0x%08X (expected 55 8B EC)", (uintptr_t)target);
        return true;
    }

    if (WineSafe_CreateHook(target, (void*)Hooked_DbcGetRow, (void**)&g_orig) != MH_OK) {
        Log("[DbcLookupCache] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[DbcLookupCache] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    g_featureToken = CrashDumper::FeatureTokenForCounting("DbcLookupCache", 1024);
    SamplingProfiler::RegisterSelfSymbol("dbc_lookup_cache", (const void*)&Hooked_DbcGetRow);
    Log("[DbcLookupCache] Installed at 0x4CFD20: %d sets of %d ways, %d entries, "
        "%zu KB committed above 2GB. The previous table was %d entries indexed by "
        "the record number alone, which collided hard enough to cost a third of "
        "the lookups in one field session.",
        CACHE_SETS, CACHE_WAYS, CACHE_SIZE,
        (sizeof(DbcRowEntry) * CACHE_SIZE) / 1024, 4096);
    return true;
}

// Printed from the periodic report.
//
// The hit rate used to be reported only from UninstallDbcLookupCache, which is
// on the teardown path this DLL never reaches - the process exits through
// TerminateProcess. So a cache that ran seventeen million times in one session
// had never once reported whether it was hitting.
void DbcLookupCache_LogStats()
{
    uint64_t total = g_hits + g_misses;
    if (total == 0 && g_bypassedPlainCopy == 0) return;

    if (total > 0) {
        Log("[DbcLookupCache] %llu calls, %llu hits, %llu misses (%.1f%% hit rate)",
            total, g_hits, g_misses, 100.0 * g_hits / total);
        if (g_misses) {
            Log("[DbcLookupCache]   of those misses %llu were rows never looked "
                "up before, which have to be decoded once whatever we do, and "
                "%llu were rows that had been here and were pushed out. The "
                "second figure is an upper bound on what a larger table would "
                "recover - each one is a byte-at-a-time decode of 680 bytes.",
                g_missCold, g_missEvicted);
            if (g_clears)
                Log("[DbcLookupCache]   the table was cleared %llu time(s), and "
                    "every clear turns rows that were here into evictions, so "
                    "read the figure above with that in mind.", g_clears);
            // Printed whether or not it fired. Zero here with a high miss count
            // would mean the misses are cold rather than conflicts, and the
            // geometry is not the thing to change.
            Log("[DbcLookupCache]   %llu insert(s) threw a live row out because "
                "all %d ways of the set were taken. Read against the misses "
                "above: a small fraction means the table holds the working set, "
                "and a figure that tracks them means four ways is not enough and "
                "the answer is more ways, not more sets.",
                (unsigned long long)g_evictedWay, CACHE_WAYS);
        }
    }
    if (g_bypassedPlainCopy > 0) {
        Log("[DbcLookupCache] %llu calls handed straight back - the client's own path "
            "was a plain copy this cache cannot beat", g_bypassedPlainCopy);
    }
}

void UninstallDbcLookupCache()
{
    void* target = reinterpret_cast<void*>(0x004CFD20);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    DbcLookupCache_LogStats();
}

extern "C" void ClearDbcLookupCache()
{
    if (!g_cache) return;   // never installed - nothing to walk
    ++g_clears;
    for (int i = 0; i < CACHE_SIZE; i++) {
        DbcRowEntry* e = &g_cache[i];
        // Bounded retry: a slot's seq should always return to even quickly (the
        // writer critical section above is now guaranteed to advance it). Cap the
        // spin so a still-unforeseen stuck slot can no longer hang this thread
        // forever — skip it and move on instead.
        for (int attempt = 0; attempt < 10000; attempt++) {
            uint32_t s = e->seq.load(std::memory_order_relaxed);
            if ((s & 1) == 0) {
                if (e->seq.compare_exchange_strong(s, s + 1, std::memory_order_acquire)) {
                    e->storePtr = 0;
                    e->recordId = 0;
                    e->valid = false;
                    // The tag has to go with it. A tag left naming a record
                    // whose entry was just invalidated sends every later lookup
                    // for that record to this way, where the real check fails
                    // and it falls through - correct, but it would hide the way
                    // from the insert path and the set would look full forever.
                    g_tags[i].storeLow = 0;
                    g_tags[i].recordId = 0;
                    e->seq.store(s + 2, std::memory_order_release);
                    break;
                }
            } else {
                // If another thread is currently writing, yield CPU and try again
                Sleep(0);
            }
        }
    }
}
