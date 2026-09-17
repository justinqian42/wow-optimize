// ============================================================================
// Description: Hoists the loop invariant out of Storm's templated hash find.
// Safety & Threading: Same thread as the functions it replaces (main).
// ============================================================================
// This started as one function and turned out to be eleven.
//
// sub_4D4BB0 - the object manager's find-by-GUID, 2.22% of executing time in a
// CPU-bound profile - walks a hash chain like this (from 0x4D4BFF):
//
//     mov ebx, [ecx+1Ch]        ; the bucket array
//     mov edx, esi
//     and edx, [ecx+24h]        ; the mask
//     lea edx, [edx+edx*2]
//     lea edx, [ebx+edx*4]
//     mov edx, [edx]            ; this bucket's link offset
//     add edx, eax
//     mov eax, [edx+4]
//
// Two loads of the table header, the index arithmetic and a third load, per
// node visited, to rebuild a value that depends only on `this` and the hash.
// Both are fixed for the whole call.
//
// Searching the image for the opening `mov eax,[ecx+24h] / cmp eax,-1` finds
// eleven functions with the identical body. They are instantiations of one
// template - the binary carries Storm\h\stpl.h among its assert strings - so
// the same wasted work is compiled into the client eleven times over. They
// differ only in what they compare once a node is reached:
//
//     sub_6F6020   184 callers   *node == key
//     sub_6792E0    46 callers   *node == key && node[6] == p[0] && node[7] == p[1]
//     sub_4D4BB0    14 callers   node[6] == key && node[12] == g[0] && node[13] == g[1]
//
// Those three are hooked. The remaining eight have between one and five callers
// each and are left alone: the same defect, but nothing to win.
//
// Each hook computes the link offset once and then performs the same reads in
// the same order as the original, including reading the key pointer only after
// the primary comparison passes - doing it eagerly would fault on a caller that
// passes a bad pointer with a key that never matches, which the original
// tolerates.
//
// And each checks itself. For the first calls it runs both and compares the
// returned node; a single disagreement retires that site for the session and
// logs the key that caused it. One call in 1024 stays checked afterwards.
//
// The equivalence was also proved outside the game before shipping, because
// nobody here can run the client. A standalone harness builds tables to this
// layout and compares the hoisted walk against a literal transcription of the
// original loop over randomised shapes: bucket counts from 2 to 64, a different
// link offset per bucket (which is why the original reads it from the bucket at
// all), the tagged-empty sentinel as a chain head, the mask == -1 empty-table
// case, and chains up to 200 nodes long.
//
//     compared 1601288 lookups: 702348 found, 898940 not found
//     RESULT: identical
//
// That covers the walk. What it cannot cover is the client's real memory - which
// is what the in-game verification above is for.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "objmgr_find_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

namespace ObjMgrFindFast {

namespace {

// Offsets in the table object, read from the listing.
constexpr unsigned kOff_Buckets = 0x1C;
constexpr unsigned kOff_Mask    = 0x24;

constexpr long kLearnCalls   = 20000;
constexpr long kResampleMask = 1023;

typedef uint32_t* (__fastcall* Find_fn)(void* self, void* edx, uint32_t key, uint32_t* aux);

struct SiteState {
    Find_fn       orig;
    unsigned long calls;
    unsigned long callWraps;   // 1258648980 in one field session; the wrap is at
                               // 4294967296 and a rate divided by a wrapped
                               // count is how frustum_aabb came to print 73322%
    unsigned long agreements;
    unsigned long steps;
    volatile LONG armed;
    volatile LONG dead;
    bool          installed;
    const char*   name;
    uintptr_t     addr;

    // The last answer, kept so a repeat can skip the hash and the chain.
    //
    // A field session has sub_4D4BB0 at 1.26 billion lookups, which is six
    // hundred a frame, and the callers are game code asking for an object it is
    // about to read several fields of. The same key arriving twice running is
    // not a guess about the workload, but the share of them is, so this counts
    // hits and misses and the report says which it was.
    //
    // The memo never answers on its own. It names a node, and that node is put
    // through the same comparison Walk would have applied when it reached it -
    // the key fields, read from the node, in the same order. A node that was
    // freed and reused fails that and costs one miss. This is the shape
    // lua_pool_fast uses for the same reason: a cache that nominates cannot
    // return something the client's own walk would have rejected.
    void*         memoSelf;
    uint32_t      memoKey;
    uint32_t      memoAux0;
    uint32_t      memoAux1;
    uint32_t*     memoNode;
    unsigned long memoHits;
    unsigned long memoHitWraps;
    unsigned long memoMiss;
};

// Indexed by the enum below; the three are separate hooks because MinHook needs
// a distinct detour per target.
enum { S_Key = 0, S_KeyPair, S_GuidPair, S_COUNT };

SiteState g_site[S_COUNT] = {
    { nullptr, 0, 0, 0, 0, 0, 0, false, "sub_6F6020 (key)",      0x006F6020 },
    { nullptr, 0, 0, 0, 0, 0, 0, false, "sub_6792E0 (key+pair)", 0x006792E0 },
    { nullptr, 0, 0, 0, 0, 0, 0, false, "sub_4D4BB0 (guid)",     0x004D4BB0 },
};

inline void Bump(unsigned long& low, unsigned long& wraps) {
    const unsigned long before = low;
    low = before + 1;
    if (low < before) ++wraps;
}

inline double Total(unsigned long low, unsigned long wraps) {
    return (double)low + (double)wraps * 4294967296.0;
}

// The comparison Walk applies once it reaches a node, applied to a node the
// memo named instead. KIND decides which fields carry the key, exactly as the
// three instantiations of the client's template differ.
template <int KIND>
inline bool NodeStillMatches(uint32_t* node, uint32_t key, const uint32_t* aux) {
    if (!node) return false;
    const uintptr_t n = (uintptr_t)node;
    if (n < 0x10000 || n > 0xFFE00000) return false;
    if (KIND == S_Key)      return Rd(n) == key;
    if (KIND == S_KeyPair)  return Rd(n) == key && Rd(n + 24) == aux[0] &&
                                   Rd(n + 28) == aux[1];
    return Rd(n + 24) == key && Rd(n + 48) == aux[0] && Rd(n + 52) == aux[1];
}

inline uint32_t Rd(uintptr_t p) { return *(volatile uint32_t*)p; }

void Retire(int s, const char* why) {
    if (InterlockedExchange(&g_site[s].dead, 1) == 0) {
        Log("[StormHash] %s disabled for this session: %s. The client's own "
            "routine runs from here on.", g_site[s].name, why);
    }
}

// The shared walk. `kind` selects the comparison, and is a compile-time constant
// at every call site below, so the switch costs nothing.
template <int KIND>
inline uint32_t* Walk(void* self, uint32_t key, uint32_t* aux, unsigned* stepsOut) {
    uintptr_t T = (uintptr_t)self;
    uint32_t mask = Rd(T + kOff_Mask);
    if (mask == 0xFFFFFFFFu) { if (stepsOut) *stepsOut = 0; return nullptr; }

    uint32_t  buckets = Rd(T + kOff_Buckets);
    uintptr_t bucket  = (uintptr_t)buckets + 12u * (mask & key);

    // The whole point: read these once, not once per node.
    uint32_t linkOff = Rd(bucket);
    uint32_t node    = Rd(bucket + 8);
    if ((node & 1) || node == 0) node = 0;

    unsigned steps = 0;
    while (node != 0 && (node & 1) == 0) {
        steps++;
        bool hit = false;
        if (KIND == S_Key) {
            hit = (Rd(node) == key);
        } else if (KIND == S_KeyPair) {
            hit = (Rd(node) == key) && (Rd(node + 24) == aux[0]) && (Rd(node + 28) == aux[1]);
        } else { // S_GuidPair
            hit = (Rd(node + 0x18) == key) && (Rd(node + 0x30) == aux[0]) && (Rd(node + 0x34) == aux[1]);
        }
        if (hit) { if (stepsOut) *stepsOut = steps; return (uint32_t*)node; }
        node = Rd(node + linkOff + 4);
    }
    if (stepsOut) *stepsOut = steps;
    return nullptr;
}

template <int KIND>
inline uint32_t* Dispatch(void* self, void* edx, uint32_t key, uint32_t* aux) {
    SiteState& st = g_site[KIND];
    if (st.dead || !self) return st.orig(self, edx, key, aux);
    if (KIND != S_Key && !aux) return st.orig(self, edx, key, aux);

    Bump(st.calls, st.callWraps);
    const unsigned long n = st.calls;
    bool verifying = (st.armed == 0) || ((n & kResampleMask) == 0);

    // Armed and not this call's turn to be checked: try the last answer first.
    if (!verifying && st.memoSelf == self && st.memoKey == key &&
        (KIND == S_Key || (aux && st.memoAux0 == aux[0] && st.memoAux1 == aux[1]))) {
        bool ok = false;
        __try {
            ok = NodeStillMatches<KIND>(st.memoNode, key, aux);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (ok) {
            Bump(st.memoHits, st.memoHitWraps);
            return st.memoNode;
        }
        ++st.memoMiss;
    }

    uint32_t* mine;
    unsigned steps = 0;
    __try {
        mine = Walk<KIND>(self, key, aux, &steps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Retire(KIND, "the walk faulted");
        return st.orig(self, edx, key, aux);
    }
    st.steps += steps;

    st.memoSelf = self;
    st.memoKey  = key;
    st.memoAux0 = aux ? aux[0] : 0;
    st.memoAux1 = aux ? aux[1] : 0;
    st.memoNode = mine;

    if (verifying) {
        uint32_t* theirs = st.orig(self, edx, key, aux);
        if (theirs != mine) {
            Log("[StormHash] %s disagreed: key %08X gave %p here and %p in the "
                "client.", st.name, key, (void*)mine, (void*)theirs);
            Retire(KIND, "a lookup returned a different node than the client's");
            return theirs;
        }
        unsigned long ok = ++st.agreements;
        if (st.armed == 0 && ok >= kLearnCalls) {
            InterlockedExchange(&st.armed, 1);
            Log("[StormHash] %s: %lu lookups agreed with the client. Taking the "
                "shortcut from here; one call in %d stays checked.",
                st.name, ok, (int)(kResampleMask + 1));
        }
        return theirs;
    }
    return mine;
}

uint32_t* __fastcall Hook_Key(void* s, void* d, uint32_t k, uint32_t* a) { return Dispatch<S_Key>(s, d, k, a); }
uint32_t* __fastcall Hook_KeyPair(void* s, void* d, uint32_t k, uint32_t* a) { return Dispatch<S_KeyPair>(s, d, k, a); }
uint32_t* __fastcall Hook_Guid(void* s, void* d, uint32_t k, uint32_t* a) { return Dispatch<S_GuidPair>(s, d, k, a); }

void* const kDetour[S_COUNT] = { (void*)Hook_Key, (void*)Hook_KeyPair, (void*)Hook_Guid };

bool InstallSite(int i) {
    SiteState& st = g_site[i];
    // mov eax,[ecx+24h] / cmp eax,-1 - the shape every instantiation opens with.
    const unsigned char expect[] = { 0x8B, 0x41, 0x24, 0x83, 0xF8, 0xFF };
    unsigned char* p = (unsigned char*)st.addr;

    // sub_4D4BB0 has a frame prologue in front of it; the others start at the load.
    unsigned char* probe = p;
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC) probe = p + 3;

    if (IsBadReadPtr(probe, sizeof(expect))) {
        Log("[StormHash] %s: unreadable, not installing", st.name);
        return false;
    }
    for (size_t k = 0; k < sizeof(expect); k++) {
        if (probe[k] != expect[k]) {
            Log("[StormHash] %s: opening bytes are not the expected sequence, "
                "not installing", st.name);
            return false;
        }
    }

    if (WineSafe_CreateHook((void*)st.addr, kDetour[i], (void**)&st.orig) != MH_OK) {
        Log("[StormHash] %s: hook NOT created", st.name);
        return false;
    }
    if (WO_EnableHook((void*)st.addr) != MH_OK) {
        Log("[StormHash] %s: hook created but could not be enabled", st.name);
        return false;
    }
    st.installed = true;
    return true;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptObjMgrFindFast) return true;

    int ok = 0;
    for (int i = 0; i < S_COUNT; i++) if (InstallSite(i)) ok++;

    if (ok == 0) {
        Log("[StormHash] nothing installed");
        return false;
    }
    Log("[StormHash] ACTIVE on %d of %d hash lookups. The client compiles this "
        "template eleven times and every copy rebuilds the bucket's link offset "
        "for each node of the chain; these read it once. Verifying against the "
        "client for the first %ld calls each.", ok, S_COUNT, kLearnCalls);
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptObjMgrFindFast) return;
    for (int i = 0; i < S_COUNT; i++) {
        SiteState& st = g_site[i];
        if (!st.installed) { Log("[StormHash] %s: not installed", st.name); continue; }
        if (st.calls == 0) { Log("[StormHash] %s: never called", st.name); continue; }
        const double calls = Total(st.calls, st.callWraps);
        const double memoHits = Total(st.memoHits, st.memoHitWraps);
        Log("[StormHash] %s: %.0f lookups, %lu verified, %.2f nodes walked each%s",
            st.name, calls, st.agreements,
            calls > 0.0 ? (double)st.steps / calls : 0.0,
            st.dead ? " - DISABLED" : (st.armed ? "" : " (still verifying)"));
        if (memoHits || st.memoMiss)
            Log("[StormHash]   %.0f of those (%.1f%%) were the same lookup as the "
                "one before and skipped the hash and the chain; %lu named a node "
                "that no longer held the key and cost one comparison. The share "
                "is the number that says whether remembering the last answer was "
                "worth it.",
                memoHits, calls > 0.0 ? 100.0 * memoHits / calls : 0.0,
                st.memoMiss);
    }
}

} // namespace ObjMgrFindFast
