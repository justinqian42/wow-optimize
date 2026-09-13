// ============================================================================
// Module: mimalloc_high_arena.cpp
//
// A 32-bit client allocates from below 2GB. Everything it asks the OS for -
// textures, model data, the Lua heap, the filename buffer it writes
// SavedVariables with - has to come from there, and when the largest free run
// there falls to a megabyte those requests start failing. Two tester sessions
// ended in that state, one of them writing ElvUI.lua to disk as ")_.lua".
//
// In both, the working set was under a gigabyte while the low half was full.
// Most of it was reserved and not resident, and the allocator holding it is this
// project's: mimalloc, which reserves arenas from the OS and, because purging
// here is by MEM_RESET rather than MEM_DECOMMIT, never gives the address space
// back. That trade is deliberate and documented in ConfigureMimalloc - decommit
// unmapped buffers a GL driver was still reading - but its cost lands entirely
// on the half the client needs.
//
// The address space above 2GB is what a large-address-aware client gets on
// 64-bit Windows and it is nearly empty. mimalloc will take memory it is handed
// in preference to asking the OS, so handing it a block up there moves the
// allocator out of the client's way without changing how either of them works.
//
// One VirtualAlloc with MEM_TOP_DOWN, one mi_manage_os_memory. No hooks, no
// patches, nothing to go wrong at runtime.
//
// ---------------------------------------------------------------------------
// Why it can be handed memory this project keeps
//
// mi_manage_os_memory records the block as MI_MEM_EXTERNAL, which sits below
// MI_MEM_STATIC in mimalloc's memkind enum. mi_memkind_is_os is false for it, so
// _mi_os_free_ex returns without calling VirtualFree, and mi_memkind_needs_no_free
// is true. mimalloc will use the block and never release it, which is what a
// reservation this module owns requires. Checked against mimalloc 3.3.2's
// arena.c, os.c and types.h rather than assumed.
//
// ---------------------------------------------------------------------------
// The guard that matters
//
// MEM_TOP_DOWN is a request, not a guarantee. If Windows hands back a block
// below 2GB the reservation has taken the exact resource this exists to protect,
// so the block is returned and the feature does nothing. Anything else would be
// worse than not running.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "mimalloc_high_arena.h"
#include "version.h"
#include "config.h"
#include <mimalloc.h>

extern "C" void Log(const char* fmt, ...);

#if TEST_DISABLE_MIMALLOC_HIGH_ARENA == 0

namespace MimallocHighArena {
namespace {

// Below this there is no high half worth moving into.
//
// A client without /LARGEADDRESSAWARE tops out at 0x7FFEFFFF and there is
// nothing above 2GB to move into at all. One with it, on 32-bit Windows with
// the /3GB switch, tops out at 0xBFFEFFFF - a whole gigabyte above 2GB, which is
// exactly the case this module is for.
//
// This was 0xC0000000 and refused that client by one byte. Half a gigabyte of
// high space is the bar now: enough to be worth the reservation, and still far
// above anything a 2GB process can offer.
const uintptr_t kMinTopAddress = 0xA0000000u;
const uintptr_t kLowHalfEnd    = 0x80000000u;

void*  g_base = nullptr;          // the first block, for the log
SIZE_T g_size = 0;               // and its size
SIZE_T g_handed = 0;             // everything handed over, across all blocks
unsigned g_blocks = 0;
unsigned g_growFailed = 0;
unsigned g_growLow = 0;          // refused because Windows placed it below 2GB
SIZE_T g_highTotal = 0;          // how much address space exists above 2GB
SIZE_T g_maxHanded = 0;          // and the most this module will ever take of it

// One reservation, handed over, with every guard the first one has.
//
// Returns the bytes handed to mimalloc, or zero. A block that comes back below
// 2GB is released rather than used: reserving low takes the exact resource this
// exists to protect, and doing it while trying to protect it would be worse than
// not running at all.
SIZE_T ReserveAndHand(SIZE_T want, const char* why) {
    if (want == 0) return 0;
    const SIZE_T align = mi_arena_min_alignment();
    if (align > 1) want = (want + align - 1) & ~(align - 1);

    void* base = VirtualAlloc(nullptr, want, MEM_RESERVE | MEM_TOP_DOWN,
                              PAGE_READWRITE);
    if (!base) {
        ++g_growFailed;
        Log("[HighArena] could not reserve another %u MB (%s, error %lu). What is "
            "already handed over stays; the allocator will go to the OS for the "
            "rest, which is where it was before this module existed.",
            (unsigned)(want / (1024 * 1024)), why, GetLastError());
        return 0;
    }
    if ((uintptr_t)base < kLowHalfEnd) {
        VirtualFree(base, 0, MEM_RELEASE);
        ++g_growLow;
        Log("[HighArena] Windows placed a %u MB reservation at 0x%08X, below 2GB. "
            "Released it - taking the low half is the one thing this must not do.",
            (unsigned)(want / (1024 * 1024)), (unsigned)(uintptr_t)base);
        return 0;
    }
    if (!mi_manage_os_memory(base, want, false, false, false, -1)) {
        VirtualFree(base, 0, MEM_RELEASE);
        ++g_growFailed;
        Log("[HighArena] mimalloc declined a %u MB block at 0x%08X (%s). Released "
            "it.", (unsigned)(want / (1024 * 1024)),
            (unsigned)(uintptr_t)base, why);
        return 0;
    }
    g_handed += want;
    ++g_blocks;
    if (!g_base) { g_base = base; g_size = want; }
    return want;
}

}  // namespace

// Hand over more before the allocator has to ask the OS for it.
//
// One block was the first shape of this and it only postpones the problem: when
// mimalloc has used what it was given it reserves from the OS again, bottom-up,
// and the low half starts filling exactly as before. A tester session ended with
// 890 MB reserved and never committed, all of it below 2GB, in blocks of 128 MB.
//
// So this runs from the heap monitor thread and watches what mimalloc has
// committed against what it has been handed. Within a block's worth of the end,
// it reserves another one high and hands that over too. The allocator never has
// a reason to go to the OS, and the low half stays the client's.
//
// Bounded twice: by the configured maximum and by half of what exists above 2GB.
// The renderer and the translation layer want space up there as well, and a
// client that cannot get a texture buffer high is no better off than one that
// cannot get it low.
void Grow() {
    if (!Config::g_settings.OptMimallocHighArena) return;
    if (g_handed == 0 || g_handed >= g_maxHanded) return;

    size_t elapsed = 0, userMs = 0, sysMs = 0, rss = 0, peakRss = 0,
           commit = 0, peakCommit = 0, faults = 0;
    mi_process_info(&elapsed, &userMs, &sysMs, &rss, &peakRss,
                    &commit, &peakCommit, &faults);

    const SIZE_T step = (SIZE_T)Config::g_settings.MimallocHighArenaMB * 1024 * 1024;
    // The headroom is one step: by the time the allocator is within a block of
    // the end it is about to need the next one, and reserving after it has
    // already gone to the OS would be too late to matter.
    if ((SIZE_T)commit + step < g_handed) return;

    SIZE_T want = step;
    if (g_handed + want > g_maxHanded) want = g_maxHanded - g_handed;
    const SIZE_T got = ReserveAndHand(want, "the allocator is close to the end of "
                                            "what it has been given");
    if (got) {
        Log("[HighArena] handed over another %u MB above 2GB - %u MB in %u "
            "block(s) now, against %u MB the allocator has committed. The ceiling "
            "is %u MB.",
            (unsigned)(got / (1024 * 1024)),
            (unsigned)(g_handed / (1024 * 1024)), g_blocks,
            (unsigned)(commit / (1024 * 1024)),
            (unsigned)(g_maxHanded / (1024 * 1024)));
    }
}

bool Init() {
    if (!Config::g_settings.OptMimallocHighArena) return true;

    // The one combination this must never be in.
    //
    // hooked_malloc and hooked_calloc in dllmain hand mimalloc blocks to the
    // client, and both end with the same guard: if the pointer is at or above
    // 0x80000000 they free it again and fall back to the CRT, because a 32-bit
    // client that was never built for a large address space can treat a pointer
    // above 2GB as negative.
    //
    // This module exists to put mimalloc's memory above 2GB. Together they turn
    // every client allocation into an allocate, a free and a fallback - slower
    // than either feature alone and silent about it, because both would report
    // themselves as working.
    //
    // Our own allocations are fine up there; the client's are not. Separating
    // them properly needs a second mimalloc heap bound to the arena, which is a
    // larger change than refusing the combination.
    if (Config::g_settings.OptCrtMimalloc || Config::g_settings.OptMimallocLarge) {
        Log("[HighArena] NOT active: %s hands mimalloc blocks to the client, and "
            "the client is not built for addresses above 2GB - those paths free "
            "any block they get from up there and fall back to the CRT. With "
            "this module on, that would be every allocation. Turn one of them "
            "off; they cannot both be right at once.",
            Config::g_settings.OptCrtMimalloc ? "CrtMimalloc" : "MimallocLarge");
        return false;
    }

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t top = (uintptr_t)si.lpMaximumApplicationAddress;
    if (top < kMinTopAddress) {
        Log("[HighArena] NOT active: user address space ends at 0x%08X, so there "
            "is no high half to move the allocator into. This needs a "
            "large-address-aware client on 64-bit Windows.", (unsigned)top);
        return false;
    }

    SIZE_T want = (SIZE_T)Config::g_settings.MimallocHighArenaMB * 1024 * 1024;
    const SIZE_T minSize = mi_arena_min_size();
    const SIZE_T align   = mi_arena_min_alignment();
    if (want < minSize) want = minSize;
    if (align > 1) want = (want + align - 1) & ~(align - 1);

    // The ceiling, set once: a fraction of what lies above 2GB, or the
    // configured maximum, whichever is smaller.
    //
    // It used to be half, and half was not enough. Two field sessions say so
    // exactly. This client is large-address-aware on 64-bit Windows, so it gets
    // four gigabytes and the region above 2GB is 2047 MB - the arena in those
    // logs sits at 0xEFCB0000, which is 3.75 GB, so the space is certainly
    // there. Half of 2047 is the "ceiling 1023 MB" the report kept printing,
    // while the line beside it read:
    //
    //     1023 MB handed to mimalloc in 4 block(s), ceiling 1023 MB. The
    //     allocator has 1552 MB committed, so it has 0 MB of high address space
    //     left before it would have to ask the OS.
    //
    // and 1285, and 1434, and 1513, climbing all session. So the allocator ran
    // out of arena early and spent the rest of the session reserving from the
    // OS bottom-up, which is the only thing that explains the other number in
    // the same report: "free 85 MB in 6183 region(s), largest run 9 MB". The
    // low half was not full, it was shredded, and the shredding was overflow
    // this module exists to prevent.
    //
    // Seven eighths instead. That is 1791 MB of a 2047 MB region, against a
    // measured need of 1552 MB and rising, and it still leaves 256 MB above 2GB
    // for anything else that wants to live up there. Nothing is reserved up
    // front by raising it - blocks are handed over only as the allocator fills
    // what it already has - so the cost of the higher ceiling is nothing until
    // the allocator actually needs it.
    g_highTotal = (SIZE_T)(top - kLowHalfEnd);
    g_maxHanded = (SIZE_T)Config::g_settings.MimallocHighArenaMaxMB * 1024 * 1024;
    const SIZE_T ceilingCap = g_highTotal / 8 * 7;
    if (g_maxHanded > ceilingCap) g_maxHanded = ceilingCap;
    if (want > g_maxHanded) want = g_maxHanded;

    if (!ReserveAndHand(want, "first block")) {
        Log("[HighArena] NOT active - the first reservation is described above "
            "and nothing was changed.");
        return false;
    }

    Log("[HighArena] ACTIVE: %u MB reserved at 0x%08X-0x%08X and handed to "
        "mimalloc as an arena. It takes memory it has been given before asking "
        "the OS, so the allocator grows up there instead of into the half the "
        "client allocates from. Nothing is committed yet - this is address "
        "space, not memory. More is handed over as it fills, up to %u MB of the "
        "%u MB that exists above 2GB.",
        (unsigned)(g_size / (1024 * 1024)), (unsigned)(uintptr_t)g_base,
        (unsigned)((uintptr_t)g_base + g_size - 1),
        (unsigned)(g_maxHanded / (1024 * 1024)),
        (unsigned)(g_highTotal / (1024 * 1024)));
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptMimallocHighArena) return;
    if (!g_base) {
        Log("[HighArena] not active - the reason is at the top of this log");
        return;
    }
    // What mimalloc has done with it is not directly queryable; the figure that
    // answers the question is the low half, which the heap monitor reports and
    // the occupancy dump breaks down by owner.
    size_t elapsed = 0, userMs = 0, sysMs = 0, rss = 0, peakRss = 0,
           commit = 0, peakCommit = 0, faults = 0;
    mi_process_info(&elapsed, &userMs, &sysMs, &rss, &peakRss,
                    &commit, &peakCommit, &faults);
    Log("[HighArena] %u MB handed to mimalloc in %u block(s), first at 0x%08X, "
        "ceiling %u MB. The allocator has %u MB committed, so it has %u MB of "
        "high address space left before it would have to ask the OS.",
        (unsigned)(g_handed / (1024 * 1024)), g_blocks,
        (unsigned)(uintptr_t)g_base,
        (unsigned)(g_maxHanded / (1024 * 1024)),
        (unsigned)(commit / (1024 * 1024)),
        (unsigned)(g_handed > (SIZE_T)commit
                   ? (g_handed - (SIZE_T)commit) / (1024 * 1024) : 0));
    // What running out actually costs, which is not obvious from the line above.
    //
    // Past the ceiling mimalloc asks the OS directly, and the size of each of
    // those asks is mi_option_arena_reserve. mimalloc's own default for a
    // 32-bit build is 128 MiB (MI_DEFAULT_ARENA_RESERVE in options.c, taken
    // when MI_INTPTR_SIZE is 4), and nothing here overrides it. Those
    // reservations are placed bottom-up, so every one of them lands in the half
    // the client allocates from.
    //
    // A field session shows what that does: the allocator wanted 1648 MB
    // against a 1023 MB ceiling, and the 625 MB difference arrived as a handful
    // of 128 MB reservations into the low 2GB, which ended that session with a
    // 13 MB largest free block and the client refusing an 8788240 byte model.
    Log("[HighArena]   past the ceiling the allocator asks the OS directly, and "
        "each of those asks is %u MB placed bottom-up - into the half the client "
        "allocates from. That is the cost of the ceiling being too low, and it "
        "arrives in whole blocks rather than gradually.",
        (unsigned)(mi_option_get_size(mi_option_arena_reserve) / (1024 * 1024)));
    if (g_growLow || g_growFailed) {
        Log("[HighArena]   %u later reservation(s) came back below 2GB and were "
            "released, %u failed outright. Both mean the allocator went to the "
            "OS for that memory instead, which is where it was going before.",
            g_growLow, g_growFailed);
    }
    Log("[HighArena]   whether it helped is the largest free block below 2GB "
        "elsewhere in this report, not a number here.");
}

}  // namespace MimallocHighArena

#endif  // TEST_DISABLE_MIMALLOC_HIGH_ARENA == 0
