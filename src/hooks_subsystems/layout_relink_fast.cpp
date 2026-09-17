// ============================================================================
// Module: layout_relink_fast
//
// sub_489710, the UI layout dependency relink, is the largest single consumer of
// main-thread time in real gameplay: 590 samples, 9.06% of executing time, in a
// 28-minute ElvUI session. First place, more than double the next entry. It
// scales with the number of UI frames, so addon-heavy setups pay most.
//
// What it does, from its own disassembly:
//
//     v3 = dword_AC1020;                     // head of the GLOBAL layout list
//     while (v3 && !(v3 & 1)) {
//         // 3 iterations x 3 anchors = nine dereferences per node
//         if (a && !(*(a+12) & 0x800) && *(a+8) == this) v2 = v3;
//         ...
//         v3 = *(dword_AC1018 + v3 + 4);     // next
//     }
//
// It walks the entire list looking for a frame anchored to `this`. The expensive
// case is not finding one, because not finding one means having walked
// everything and dereferenced nine pointers per node on the way.
//
// The client already maintains the index that search reconstructs.
// sub_489C30 - RTTI in the cluster gives it as CLayoutFrame::FRAMENODE -
// registers "X anchors to me" in a list whose head is at `me + 0x38`, encoded
// empty as null or with the low bit set. Its two callers are SetPoint
// (sub_48A260) and sub_48A3E0; the de-registrar sub_489D70 has three, in
// SetPoint, sub_48A200 and the frame destructor sub_48B130. Nothing sets an
// anchor without registering it.
//
// Therefore `this+0x38` empty implies the scan cannot find a match, and that is
// exactly the case that costs the most.
//
// Why 0x38 and not 0x34 (checked against the disassembly, 2026-08-16). The
// dependants list has a root made of two adjacent dwords at this+0x34 and
// this+0x38, the same shape as the global list's off_AC101C / dword_AC1020.
// sub_489C30 inserts through the first of them - `*(this+13) = v5` in its
// decompilation, which is this+0x34 - and begins its own walk from the second,
// `result = *(this+14)`, which is this+0x38. sub_489710 does the same thing on
// the global root: its walk starts at `mov eax, dword_AC1020`, the second dword
// of that pair, not at off_AC101C. So 0x38 is the head of traversal and 0x34 is
// the insertion point, and reading 0x38 is reading what a walk would read.
//
// Where the profile's nine percent sits, also from the disassembly: 0x00489763
// is `test eax, eax` at the top of the inner anchor loop, right after
// `mov eax, [edx-4]`. That is inside the scan, not in the early-out at 0x489726
// that this module leaves to the client. The target is the right one.
// ---------------------------------------------------------------------------
// Why the case where a dependant IS found cannot be shortcut too
//
// The obvious next step is to answer the found case from the same index rather
// than only the not-found case, and it does not work. Decompiled, the client is:
//
//     while (node) {
//         for each of nine anchor slots a in node:
//             if (a && !(a->flags & 0x800) && a->owner == this) match = node;
//         if (match) { move this node to just before `match`; return; }
//         node = next(node);
//     }
//     move this node to the head of the list;
//
// The `if (match)` is inside the loop, so it stops at the FIRST NODE carrying an
// anchor that points at `this` - not the last match in the list. What it computes
// is therefore a position in the global list: put me immediately before the first
// thing that depends on me. It is one step of a topological sort.
//
// The dependants list at this+0x38 knows which anchors point at `this`, and that
// is what makes the empty case answerable. It does not know which of their owning
// nodes comes first in the global list, because it holds them in registration
// order. Recovering that would need an order key per node, and this very function
// reorders the list on every call, so any key it maintained would be invalidated
// by the next relink - including its own.
//
// Note that a node carries no owner field: sub_489C30 allocates sixteen bytes
// with `.?AUFRAMENODE@CLayoutFrame@@` and fills two link words, a frame at +8
// and a word at +12. That is about the node, not the anchor. Two different
// sixteen-byte objects in this cluster have a frame at +8 and a word at +0x0C.
// The anchor is a CFramePoint, built by sub_49CA40, sitting in one of nine slots
// at frame+0x0C..0x2C; its +8 is the frame it points at. The FRAMENODE is the
// dependants-index entry, chained off frame+0x38; its +8 is the DEPENDENT frame,
// which is precisely the owner the paragraph said did not exist. sub_48A260
// writes the anchor into `*(this + a2 + 3)` and then calls
// `sub_489C30(a3, this, 1 << a2)`, so the frame it hands the index is the frame
// whose slot it just filled.
//
// The found case is left to the client and counted as `deferred`; the counters
// below measure how often the single-candidate case arises.
//
// The word at the index entry's +0x0C is the point mask, not the anchor flags:
// sub_48A260 passes `1 << a2` for a point index in 0..8 and sub_48A3E0 passes
// 0x101, so the mask cannot exceed 0x1FF and bit 11 is unreachable through it.
//
// NoDependantCanMatch below does what that predicate was meant to do. For each
// frame the index names it applies the scan's own three tests to that frame's
// nine slots, and answers not-found when none of them holds an anchor the scan
// would accept. The field said how much is waiting: of 62782 verified calls in
// one session, 24691 - 39.3% - had a non-empty +0x38 and the client still found
// nothing. Those are 25.4 million deferred calls a session, each one walking an
// average of 43.3 nodes at nine dereferences apiece.
// ---------------------------------------------------------------------------
// Do not force the not-found path by zeroing dword_AC1020 for the duration of
// the call. Nothing reads it inside sub_489710, but off_AC101C at 0xAC101C and
// dword_AC1020 at 0xAC1020 are adjacent dwords forming one link pair - the root
// node of the list - and the not-found path writes to the second of them:
//
//     489827  mov esi, offset off_AC101C
//     48982c  mov edx, [esi]          ; edx = off_AC101C
//     489836  mov [edx+4], ecx        ; <-- when edx is the root, this IS AC1020
//
// Restoring afterwards clobbers the client's own write and the layout list loses
// a link: EIP 0x00489873, `mov [ebx], esi` with EBX = 0, on the next relink.
//
// So this writes to no client global. It reads one word to decide, and when it
// decides yes it performs the same pointer surgery the client would have,
// transcribed instruction by instruction from the tail below.
// ---------------------------------------------------------------------------
// And it does not believe itself. For the first calls of every session it takes
// no shortcut at all: it makes its prediction, calls the original, and then
// checks what the original actually did. The not-found tail ends with
// `off_AC101C = result`, so afterwards off_AC101C == result iff the not-found
// path ran, which is a one-word test. Only after a run of agreements does the
// fast path switch on, and one in every 1024 calls stays in shadow mode
// afterwards so a late divergence still gets caught.
// ---------------------------------------------------------------------------
// The check is deliberately one-sided, and the first version of it was not.
//
// Only one of the two ways the prediction can miss is dangerous:
//
//   predicted not-found, client found one   -> we would have run the not-found
//                                              tail on a frame that needed the
//                                              other one. Wrong. Retire.
//   predicted found, client found nothing   -> we defer to the original on a
//                                              non-empty +0x38 (see the
//                                              `if (!predictNotFound)` guard
//                                              below), so the client's own
//                                              routine ran and the result is
//                                              correct. We merely paid for a
//                                              scan we could have skipped.
//
// So retire on the first only. A divergence that cannot produce a wrong answer
// must not be able to switch the module off; it is counted instead, and a high
// count means the dependants list holds entries the scan rejects, which is win
// still on the table.
// ---------------------------------------------------------------------------
// ============================================================================

#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "layout_relink_fast.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace LayoutRelinkFast {

namespace {

constexpr uintptr_t kRelink        = 0x00489710;
constexpr uintptr_t kNodeOffsetVar = 0x00AC1018;  // member offset of the link pair
constexpr uintptr_t kListRoot      = 0x00AC101C;  // off_AC101C; AC1020 is its [1]
constexpr unsigned  kDependentsOff = 0x38;        // "who anchors to me" list head
constexpr unsigned  kStateByteOff  = 0x40;        // set to 6 by both tails

// How many calls to verify before trusting the prediction, and how often to
// keep checking afterwards.
constexpr long kLearnCalls   = 20000;
constexpr long kResampleMask = 1023;   // one in 1024

// What the deferred scan costs, measured rather than assumed.
// Shortcuts taken on the second predicate rather than on an empty list, and
// how many dependants were examined to reach them.
unsigned long g_rejectShortcut = 0;
double        g_rejectWalked   = 0.0;
// Of the calls that still defer, how many frames in the index actually carry an
// anchor the scan would accept. Exactly one is the interesting case: a single
// candidate is the match whatever order the global list is in, so this is the
// number that says whether the found path is worth answering from the index too.
unsigned long g_oneQualifier   = 0;
// Of the single-qualifier calls, the ones where that frame is actually in the
// global list. That is the population a found-path shortcut could serve, and it
// is smaller than g_oneQualifier by however many frames hold an anchor while
// unlinked.
unsigned long g_oneQualifierLinked = 0;
unsigned long g_manyQualifiers = 0;

unsigned long g_scanSample   = 0;
unsigned long g_scansSeen    = 0;
double        g_nodesToMatch = 0.0;   // nodes walked before the first match
double        g_nodesTotal   = 0.0;   // whole list length
unsigned long g_scanNoMatch  = 0;     // walked it all and found nothing
unsigned long g_scanLongest  = 0;

// Read-only replay of sub_489710's search: the first node in list order with an
// anchor slot pointing at `self` and without 0x800 set. Nothing is written.
void MeasureScan(uintptr_t self) {
    __try {
        uint32_t linkOff = *(const uint32_t*)kNodeOffsetVar;
        uint32_t node    = *(const uint32_t*)(kListRoot + 4);   // AC1020
        uint32_t walked  = 0;
        uint32_t matchAt = 0;
        bool     found   = false;

        while (node && (node & 1) == 0 && walked < 100000) {
            ++walked;
            if (!found) {
                for (unsigned s = 0; s < 9; s++) {
                    uint32_t fn = *(const uint32_t*)(node + 12 + s * 4);
                    if (!fn) continue;
                    if (*(const uint32_t*)(fn + 12) & 0x800) continue;
                    if (*(const uint32_t*)(fn + 8) == (uint32_t)self) {
                        found = true; matchAt = walked; break;
                    }
                }
            }
            node = *(const uint32_t*)(linkOff + node + 4);
        }

        g_scansSeen++;
        g_nodesTotal += (double)walked;
        if (found) g_nodesToMatch += (double)matchAt;
        else       g_scanNoMatch++;
        if (walked > g_scanLongest) g_scanLongest = walked;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

typedef uint32_t* (__fastcall* Relink_fn)(void* self, void* edx);
Relink_fn orig_Relink = nullptr;

// Plain, not Interlocked. This is the most expensive function in the client and
// it ran 16.9 million times in one session; every one of these was a locked
// read-modify-write on that path. They are statistics, one thread writes them,
// and a lost increment costs one count. Lower bounds, and the report says so.
long g_calls        = 0;
long g_agreements   = 0;
long g_fastTaken    = 0;
long g_deferred     = 0;
long g_disagreed    = 0;
long g_pessimistic  = 0;   // predicted found, client found nothing
// Every entry into the hook. g_calls below counts only those that get past the
// client's own early-out and reach the scan - the ones that cost anything - and
// it stops entirely once the module retires. This one never stops, so a log can
// say how often the function actually runs.
unsigned long g_invocations  = 0;
// Reached the hook while it was still live and stopped at the client's own
// early-out. Kept apart from the invocation count so that "retired early, so
// most invocations were never examined" cannot be misread as "most invocations
// take the early-out".
unsigned long g_earlyOut     = 0;
volatile LONG g_armed        = 0;   // 1 once the fast path is trusted
volatile LONG g_dead         = 0;   // 1 after a disagreement

// Set once at init when the A/B harness names this module. Tested before
// calling into it, so a session not testing this feature pays a predictable
// branch on a false global rather than a call.
bool          g_abSubject    = false;
unsigned long g_abOffCalls   = 0;   // reached the hook while the test had it off

inline uint32_t Rd(uintptr_t p)          { return *(volatile uint32_t*)p; }
inline void     Wr(uintptr_t p, uint32_t v) { *(volatile uint32_t*)p = v; }

// Empty is encoded two ways throughout this cluster: null, or a tagged sentinel
// with the low bit set.
inline bool IsEmptyLink(uint32_t v) { return v == 0 || (v & 1) != 0; }

// Two different objects, both sixteen bytes, both with a frame at +8 and a word
// at +0x0C. Conflating them is what made the second predicate dead code.
//
// An ANCHOR is a CFramePoint. It lives in one of nine slots at frame+0x0C..0x2C
// and the scan tests it with `a && !(*(a+0x0C) & 0x800) && *(a+8) == this`, so
// its +8 is the frame it points AT and its +0x0C is a packed word whose low
// byte is the relative point (sub_48A260 compares `*(char *)(v9+12) != a4`) and
// whose bit 11 is the reject flag.
//
// A DEPENDANTS ENTRY is the FRAMENODE sub_489C30 allocates,
// `sub_76E540(16, ".?AUFRAMENODE@CLayoutFrame@@", -2, 8)`. It is chained off
// frame+0x38 and sub_489C30 ends with `v6[2] = a2; v6[3] = a3`, so its +8 is the
// DEPENDENT frame and its +0x0C is the point mask.
//
// Both registrars call it as sub_489C30(target, dependent, mask):
// sub_48A260 passes `1 << a2` for a point index a2 in 0..8, and sub_48A3E0
// passes 0x101, which is points 0 and 8. The mask therefore cannot exceed
// 0x1FF, and bit 11 is not reachable through it at all.
//
// The old predicate read 0x800 out of the entry's +0x0C - the point mask - and
// so answered true only for a bit the registrar cannot set. It never fired once
// in a field session, and the line that would have said so was written behind
// `if (g_rejectShortcut > 0)`, which is how a dead predicate stayed invisible.
constexpr unsigned kAnchor0    = 0x0C;   // first of nine anchor slots in a frame
constexpr unsigned kAnchorCount= 9;
constexpr unsigned kAnchorTgt  = 0x08;   // the frame an anchor points at
constexpr unsigned kAnchorFlag = 0x0C;   // the word the scan masks with 0x800
constexpr unsigned kFN_next    = 0x04;   // dependants are chained through here
constexpr unsigned kFN_frame   = 0x08;   // the dependent frame
constexpr unsigned kScanReject = 0x800;

// How far to walk a dependants list before giving up and deferring. The list is
// short in practice; the cap is only there so a corrupted chain cannot turn a
// shortcut into an unbounded walk.
constexpr unsigned kMaxDependants = 64;

// Does this frame carry an anchor the scan would accept, pointing at `self`?
// The same three tests sub_489710 applies to each of the nine slots, in the same
// order, on one frame instead of every frame in the list.
inline bool FrameAnchorsTo(uint32_t frame, uint32_t self) {
    for (unsigned s = 0; s < kAnchorCount; ++s) {
        const uint32_t a = Rd(frame + kAnchor0 + 4 * s);
        if (!a) continue;
        if (Rd(a + kAnchorFlag) & kScanReject) continue;
        if (Rd(a + kAnchorTgt) == self) return true;
    }
    return false;
}

// What a walk of the dependants index found.
struct DepScan {
    unsigned entries;      // dependants examined
    unsigned qualifying;   // of those, frames that really do anchor to self
    uint32_t only;         // that frame, when there was exactly one
};

// True when no frame in the index carries an anchor the scan would accept, so
// the client is about to walk the whole global list and find nothing.
//
// This is the same premise the empty case already rests on - sub_489C30 and
// sub_489D70 are the only registrar and de-registrar, their five call sites are
// all in SetPoint, sub_48A200 and the frame destructor, and nothing sets an
// anchor without registering the frame that owns it. The empty case says the
// index names no frames; this says the frames it names do not, on inspection,
// hold a matching anchor.
//
// It reads all nine slots rather than only the ones the point mask names. The
// mask would usually save eight reads, but trusting it adds a second premise -
// that the mask is never stale - whose failure would point this at the wrong
// slot and produce a wrong not-found. Nine reads on a handful of frames against
// nine reads on 43 of them is not a trade worth another assumption.
//
// Read-only, bounded, and inside the caller's SEH. Anything unreadable or
// longer than the cap answers false, which defers exactly as before.
bool NoDependantCanMatch(uint32_t head, uint32_t self, DepScan* out) {
    unsigned n = 0, q = 0;
    uint32_t first = 0;
    uint32_t e = head;
    while (!IsEmptyLink(e)) {
        if (++n > kMaxDependants) {
            out->entries = n; out->qualifying = 2; out->only = 0;
            return false;
        }
        const uint32_t d = Rd(e + kFN_frame);
        if (d && (d & 1) == 0 && FrameAnchorsTo(d, self)) {
            if (++q == 1) first = d;
        }
        e = Rd(e + kFN_next);
    }
    out->entries    = n;
    out->qualifying = q;
    out->only       = (q == 1) ? first : 0;
    return n > 0 && q == 0;
}

// The not-found tail of sub_489710, transcribed from 0x004897CE to 0x0048983D.
// `result` is eax, `self` is ecx. There are no calls in it.
void RunNotFoundTail(uint32_t* result, uintptr_t self) {
    // 4897ce  mov esi,[eax] / test esi,esi / jz link
    uint32_t v14 = result[0];
    if (v14 != 0) {
        // 4897d4  mov ebx,[eax+4]
        uint32_t v15 = result[1];
        uint32_t* v16;
        if ((v15 & 1) == 0 && v15 != 0) {
            // 4897e5  mov edx,eax / sub edx,[esi+4] / add ebx,edx
            v16 = (uint32_t*)((uintptr_t)result + v15 - Rd((uintptr_t)v14 + 4));
        } else {
            // 4897e0  and ebx,0FFFFFFFEh
            v16 = (uint32_t*)(uintptr_t)(v15 & 0xFFFFFFFEu);
        }
        // 4897ec  mov [ebx],esi
        *v16 = v14;
        // 4897ee  mov edx,[eax] / mov esi,[eax+4] / mov [edx+4],esi
        //         (both re-read, exactly as the original does)
        uint32_t edx = result[0];
        uint32_t esi = result[1];
        Wr((uintptr_t)edx + 4, esi);
        // 4897f6 / 4897fc
        result[0] = 0;
        result[1] = 0;
    }

    // 489827  mov esi, offset off_AC101C
    // 48982c  mov edx,[esi]      ; edx = off_AC101C
    uint32_t v17 = Rd(kListRoot);
    // 48982e  mov [eax],edx
    result[0] = v17;
    // 489830  mov edi,[edx+4] / mov [eax+4],edi
    result[1] = Rd((uintptr_t)v17 + 4);
    // 489836  mov [edx+4],ecx    ; when edx is the root this writes AC1020
    Wr((uintptr_t)v17 + 4, (uint32_t)self);
    // 48983a  mov [esi],eax
    Wr(kListRoot, (uint32_t)(uintptr_t)result);
    // 48983d  mov byte ptr [ecx+40h], 6
    *(volatile uint8_t*)(self + kStateByteOff) = 6;
}

void Retire(const char* why) {
    if (InterlockedExchange(&g_dead, 1) != 0) return;
    Log("[LayoutRelink] Disabled for this session: %s. The client's own routine "
        "runs from here on; nothing is left half-applied.", why);
}

uint32_t* __fastcall Hooked_RelinkBody(void* self, void* edx) {
    (void)edx;
    // Counted before anything can return, including after this module has
    // retired, because every other counter here stops the moment it does.
    //
    // A session log read "5 calls" and it was taken - by me - as the call rate
    // of the hottest function in the client's profile, which made no sense and
    // led to the conclusion that the shortcut could never arm. The module had
    // retired three seconds in; from then on the first line returned without
    // counting, and the function went on being called for the rest of the
    // session with nothing recording it. The number was not a rate, it was
    // where the counting stopped.
    //
    // Plain increment, not interlocked: this is the client's layout relink and
    // it runs on the main thread, and a lock-prefixed read-modify-write on a
    // function that owns nine percent of executing time costs more than the
    // diagnostic is worth.
    g_invocations++;
    if (g_dead || !self) return orig_Relink(self, edx);


    uintptr_t This = (uintptr_t)self;
    uint32_t* result;
    bool predictNotFound;

    __try {
        result = (uint32_t*)(This + Rd(kNodeOffsetVar));
        // The whole body of the original is inside `if (!result[1])`. When that
        // is false it does nothing at all, so there is nothing to be clever
        // about and the original is the cheapest correct answer.
        if (result[1] != 0) { g_earlyOut++; return orig_Relink(self, edx); }
        uint32_t head = Rd(This + kDependentsOff);
        if (IsEmptyLink(head)) {
            predictNotFound = true;
        } else {
            // The list is not empty, which used to end the matter. Ask whether
            // any of the frames it names could match instead.
            DepScan ds = { 0, 0, 0 };
            predictNotFound = NoDependantCanMatch(head, (uint32_t)This, &ds);
            if (predictNotFound) {
                ++g_rejectShortcut;
                g_rejectWalked += (double)ds.entries;
            } else if (ds.qualifying == 1) {
                ++g_oneQualifier;
                // Is that one frame in the list the client is about to walk?
                //
                // The client's own membership test is in this function's
                // not-found tail: before unlinking itself it does `if (*result)`
                // on `result = frame + dword_AC1018`, so a non-zero first link
                // word is what "in the list" means here. It is one load.
                //
                // This is read-only and counts only. With exactly one frame in
                // the index carrying an anchor the scan would accept, and that
                // frame in the list, the client's loop must stop at it whatever
                // order the list is in - which is the case a found-path shortcut
                // would answer. Whether that case is common enough to be worth
                // the pointer surgery is what this counts, and the surgery is
                // not written until it says so: this module crashed the game on
                // login once already, doing exactly that kind of work on a
                // premise that had not been measured.
                const uint32_t linkOff = Rd(kNodeOffsetVar);
                if (Rd((uintptr_t)ds.only + linkOff) != 0) ++g_oneQualifierLinked;
            } else {
                ++g_manyQualifiers;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return orig_Relink(self, edx);
    }

    LONG n = ++g_calls;
    bool verifying = (g_armed == 0) || ((n & kResampleMask) == 0);

    if (verifying) {
        uint32_t rootBefore = 0;
        __try { rootBefore = Rd(kListRoot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return orig_Relink(self, edx); }

        uint32_t* r = orig_Relink(self, edx);

        // The not-found tail ends with off_AC101C = result. Nothing else in this
        // function writes that word, so this distinguishes the two paths.
        bool actualNotFound = false;
        __try {
            uint32_t rootAfter = Rd(kListRoot);
            actualNotFound = (rootAfter == (uint32_t)(uintptr_t)result)
                          && (rootAfter != rootBefore);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Retire("the list root became unreadable during verification");
            return r;
        }

        // The unsafe direction: we would have run the not-found tail on a frame
        // the client found a match for. This is the only one that invalidates
        // the optimisation.
        if (predictNotFound && !actualNotFound) {
            ++g_disagreed;
            Log("[LayoutRelink] Prediction was wrong: frame 0x%08X had nothing at "
                "+0x38 but the client took the found path. An empty dependants "
                "list does not imply the scan finds nothing, so this optimisation "
                "is not valid.", (unsigned)This);
            Retire("a prediction would have produced the wrong result");
            return r;
        }

        // The safe direction: a non-empty +0x38 where the client still found
        // nothing. We defer on non-empty, so the original ran and the answer is
        // correct - this is a skipped shortcut, not an error. Log the first one
        // so the asymmetry is visible in a session log, then just count them.
        if (!predictNotFound && actualNotFound) {
            if (++g_pessimistic == 1) {
                Log("[LayoutRelink] Frame 0x%08X had something at +0x38 but the "
                    "client found nothing. Correct either way - we defer on "
                    "non-empty - so this is a missed shortcut, not a divergence. "
                    "Counting the rest.", (unsigned)This);
            }
            return r;
        }

        LONG ok = ++g_agreements;
        if (g_armed == 0 && ok >= kLearnCalls) {
            InterlockedExchange(&g_armed, 1);
            Log("[LayoutRelink] %ld calls verified, no disagreement. Taking the "
                "shortcut from here; one call in %d stays checked.",
                (long)ok, (int)(kResampleMask + 1));
        }
        return r;
    }

    if (!predictNotFound) {
        ++g_deferred;
        // Nobody has measured the number the whole model rests on: how long the
        // list the client scans actually is, and how far into it the match
        // sits. If it is short, the scan cannot be where the time goes and this
        // module is aimed at the wrong thing. Sampled, because measuring means
        // walking the list a second time.
        if ((++g_scanSample & 255u) == 0) MeasureScan(This);
        return orig_Relink(self, edx);
    }

    __try {
        RunNotFoundTail(result, This);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Retire("the transcribed tail faulted");
        return orig_Relink(self, edx);
    }
    ++g_fastTaken;
    return result;
}

// The detour proper. Kept apart from the body because the A/B harness times the
// call, and a scope guard closing that sample on every return path cannot live
// in a function containing __try - MSVC refuses object unwinding alongside SEH.
// One pair of reads here covers every path the body can take.
uint32_t* __fastcall Hooked_Relink(void* self, void* edx) {
    if (!g_abSubject) return Hooked_RelinkBody(self, edx);
    unsigned long long t = AbTest::TickIn();
    uint32_t* r;
    if (AbTest::StandAside()) {
        g_abOffCalls++;
        r = orig_Relink(self, edx);
    } else {
        r = Hooked_RelinkBody(self, edx);
    }
    AbTest::TickOut(t);
    return r;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptLayoutRelinkFast) return true;

    // The prologue is `test ecx, ecx` / `jz short`, not a frame setup - this
    // function takes `this` in ecx and has no stack frame at all. Checking for
    // the usual 55 8B EC would have rejected the right address.
    unsigned char* p = (unsigned char*)kRelink;
    if (p[0] != 0x85 || p[1] != 0xC9) {
        Log("[LayoutRelink] Expected `test ecx,ecx` at 0x%08X and found %02X %02X "
            "- not installing", (unsigned)kRelink, p[0], p[1]);
        return false;
    }

    if (WineSafe_CreateHook((void*)kRelink, (void*)Hooked_Relink,
                            (void**)&orig_Relink) != MH_OK) {
        Log("[LayoutRelink] hook NOT installed at 0x%08X", (unsigned)kRelink);
        return false;
    }
    if (WO_EnableHook((void*)kRelink) != MH_OK) {
        Log("[LayoutRelink] hook created but could not be enabled");
        return false;
    }

    Log("[LayoutRelink] ACTIVE on sub_489710, the largest single entry in the "
        "main-thread profile (9.06%%). Verifying against the client for the first "
        "%ld calls before it changes anything.", (long)kLearnCalls);

    g_abSubject = AbTest::IsSubject("LayoutRelinkFast", &g_abSubject);
    if (g_abSubject) {
        Log("[LayoutRelink] under A/B test: the shortcut is taken only during the "
            "test's ON stints, and the frame times either side are reported by "
            "AbTest. The verification above is unaffected - it still runs, and a "
            "disagreement still retires this module whichever stint it happens "
            "in.");
    }
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptLayoutRelinkFast) return;
    if (g_calls == 0) return;
    Log("[LayoutRelink] %lu invocations, %ld of them reached the scan: %ld took "
        "the shortcut, %ld deferred (a dependant existed), %ld verified against "
        "the client, %ld disagreed%s",
        g_invocations, (long)g_calls, (long)g_fastTaken, (long)g_deferred,
        (long)g_agreements, (long)g_disagreed,
        g_dead ? " - DISABLED" : "");
    if (g_abSubject) {
        Log("[LayoutRelink]   %lu of those invocations were handed straight to the "
            "client because the A/B test had this feature off at the time. The "
            "shortcut counts above therefore describe the ON stints only.",
            g_abOffCalls);
    }
    // The gap between the two is the client's own early-out, which this module
    // deliberately leaves alone. If almost every invocation stops there, the
    // nine percent measured in the profile is not where this module is looking
    // and the whole approach needs rechecking.
    unsigned long examined = g_earlyOut + (unsigned long)g_calls;
    if (examined > 0) {
        Log("[LayoutRelink] of %lu invocations seen while live, %.1f%% stopped at the "
            "client's own early-out before any scan - those cost nothing and are not "
            "this module's target%s",
            examined, 100.0 * (double)g_earlyOut / (double)examined,
            g_dead ? "; the remaining invocations came after this module retired"
                   : "");
    }
    // Printed whether or not it fired. The previous version of this test was
    // dead - it masked 0x800 against a point mask that cannot exceed 0x1FF - and
    // the reason nobody saw that for a whole field session is that this line was
    // behind `if (g_rejectShortcut > 0)`. A zero here is a measurement.
    Log("[LayoutRelink] %lu shortcuts came from the second test - a dependants "
        "index naming frames that hold no anchor the scan would accept - after "
        "looking at %.1f entries on average. Those calls used to defer and the "
        "client walked the whole global list for nothing.",
        g_rejectShortcut,
        g_rejectShortcut ? g_rejectWalked / (double)g_rejectShortcut : 0.0);
    Log("[LayoutRelink] of the calls that still defer, %lu had exactly one frame "
        "in the index carrying an accepting anchor and %lu had more than one. A "
        "single candidate is the match whatever order the global list is in, so "
        "the first of those two numbers is what answering the found path from "
        "the index would be worth.",
        g_oneQualifier, g_manyQualifiers);
    Log("[LayoutRelink]   %lu of those single candidates were themselves in the "
        "global list, which is the population a found-path shortcut could serve. "
        "The client's own test for that is `if (*result)` on frame+dword_AC1018 "
        "in its not-found tail, so this is the same question it asks.",
        g_oneQualifierLinked);
    if (g_scansSeen > 0) {
        double avgLen   = g_nodesTotal / (double)g_scansSeen;
        unsigned long matched = g_scansSeen - g_scanNoMatch;
        Log("[LayoutRelink] the scan itself, sampled one deferred call in 256 "
            "over %lu of them: the list averages %.1f nodes and its longest was "
            "%lu. %lu found a match after %.1f nodes on average, %lu walked the "
            "whole list and found nothing.",
            g_scansSeen, avgLen, g_scanLongest, matched,
            matched ? g_nodesToMatch / (double)matched : 0.0, g_scanNoMatch);
        Log("[LayoutRelink]   this is the number the module rests on. A short "
            "list means the scan cannot be where the time goes and the target "
            "is wrong; a long one with the match near the end is what an index "
            "would be worth.");
    }
    if (g_pessimistic > 0) {
        Log("[LayoutRelink] %ld of the deferred calls had a non-empty +0x38 but "
            "the client found nothing anyway - shortcuts we could have taken and "
            "did not. A large share here means the dependants list holds entries "
            "the scan rejects, and there is more win available than we take.",
            (long)g_pessimistic);
    }
}

} // namespace LayoutRelinkFast
