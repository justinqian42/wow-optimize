// ============================================================================
// Module: m2_anim_reuse
//
// Skips the bone loop in sub_82F0F0 when the model is being asked for an
// animation state it was already asked for, and the bone array it produced is
// still sitting there from last time.
//
// ---------------------------------------------------------------------------
// The number this exists for
//
// The animation census measured it in a real session on a real machine:
//
//     of 256456 calls: 256277 rebuilt bones, 179 returned on the instance flag,
//     0 returned because the per-tick stamp already matched
//     234662 of those 256277 (91.6%) were the same model asked for the same
//     animation state as in an earlier frame
//
// The client's own two early exits took 179 calls of a quarter million. The
// per-tick stamp, the one thing in the client that exists to stop this work
// being repeated, fired zero times. Meanwhile nine calls in ten were a repeat.
//
// The census called that a ceiling rather than a promise, because a matching
// argument tuple is evidence the bones match and not proof, and said what
// proving it needs: the bone array compared. That is what this does.
//
// ---------------------------------------------------------------------------
// Why this is not the stride again
//
// M2AnimStride cuts at the same instruction and jumps to the same place. It was
// marked as tried and failed because it chose what to hold by distance, and a
// large animation fills the screen at any range, so it stepped visibly on the
// lava in Ironforge.
//
// Distance is a guess about whether anyone would notice a stale pose. This does
// not guess. It holds a pose only when the inputs that produced it are
// identical, so the pose is not stale - it is the pose the loop would have
// recomputed. There is no visual difference to notice, and unlike a distance
// rule that claim is checkable, which is what the whole verification below is.
//
// The two cannot both be installed; they overwrite the same five bytes.
//
// ---------------------------------------------------------------------------
// The cut, which the client already has
//
//     0x0082F418  mov  edx, [ebx+2Ch]      ; the bone count
//     0x0082F41B  cmp  edx, edi            ; edi is the loop start, zero
//     0x0082F41D  mov  [ebp+arg_10], edi
//     0x0082F420  jbe  loc_8302A3          ; no bones -> straight to the tail
//
// Taking that branch early is a path the client takes itself, so the frame, the
// x87 depth and every register the tail reads are already right. The tail is
// attachments, particle and ribbon emitters, lights and materials, and it still
// runs - holding the skeleton does not hold the texture animation.
//
// The x87 half of that sentence is load-bearing and was worth checking rather
// than asserting. Three instructions before the cut the client has
//
//     0x0082F3FB  fst  [ebp+var_98]      ; fst, not fstp
//
// so exactly one value is live on the x87 stack when control reaches 0x0082F418
// and it is still live through the jbe. At the other end,
//
//     0x008302AE  jbe  loc_830363
//     0x008302B4  fstp st                ; discards it
//
// the tail pops that value. Arriving there with an empty stack is an underflow,
// and with exceptions masked it does not fault - it writes the indefinite value
// and leaves the tag word wrong, so every float in the rest of the frame is
// quietly wrong. That is a corrupted-skeleton bug, not a crash.
//
// Which means nothing between the cut and the jump may touch the x87 stack.
// Today nothing does: every one of the 31 functions in this object was checked
// for x87 opcodes in the built object, _M2AnimReuse_Decide included, and all are
// clean. Re-run it after editing this file, because the compiler will reach for
// the x87 stack the moment a float or double appears in the decision path and
// nothing in C++ will warn:
//
//     dumpbin /DISASM:BYTES m2_anim_reuse.obj | findstr /R "\<f[a-z]*\>"
//
// A source check is not enough on its own, so the thunk also reads the x87 TOP
// field either side of the call and refuses to hold when it moved. That turns a
// future edit from silently wrong geometry into a counted refusal.
//
// ---------------------------------------------------------------------------
// What the loop writes, and the models that must never be held
//
// The loop walks bone records at [esi+0x94], stride 0xAC, for [ebx+0x2C] of
// them. That whole array is what it produces and what gets hashed here.
//
// Two of its stores are not stores:
//
//     0x0082F455  cmp  dword ptr [esi+64h], 0
//     0x0082F459  jz   short loc_82F464
//     0x0082F45B  mov  eax, [ebp+arg_8]
//     0x0082F45E  add  [edi+4Ch], eax
//     0x0082F461  add  [edi+50h], eax
//
// They accumulate. On a model with [esi+64h] set, running the loop twice with
// identical arguments does not produce an identical array, because two of the
// fields advance by arg_8 every time - that is an animation clock being carried
// forward, and skipping the call would stop it. Those models are refused here
// and counted, rather than being allowed to fail the hash check and take the
// whole module down with them.
//
// This is the fourth time in this project that a function turned out to do
// something besides the work being skipped. It is the reason the rule in
// CLAUDE.md exists, and the reason the hash covers the entire record rather
// than just the matrices.
//
// ---------------------------------------------------------------------------
// Proving a repeat really is a repeat, from one patch site
//
// The array as it stands when a call arrives is the result of the previous call
// for that model. So hashing at entry gives the output of the previous tuple,
// with no second patch after the loop and nothing to snapshot or restore.
//
// A slot remembers the last tuple, the hash taken at the last entry, and
// whether that hash was itself produced by that same tuple. On a call whose
// tuple matches the slot's, the hash taken now is the result of that tuple; if
// the stored one was too, the two are compared. Equal means the loop computed
// the same array twice from the same arguments, which is exactly the claim.
// Unequal means the tuple is not a sufficient key, and the module retires
// without ever having skipped anything.
//
// Arming has to be careful about exactly that word "previous". A held call
// leaves the array alone, so once holding starts, two hashes taken either side
// of a hold are the same value read twice and comparing them proves nothing
// while looking like a passing check. So an armed session lets two calls
// through: one repeat in kResample runs the loop, and the repeat after it is
// the one that compares. The comparison then straddles a loop that actually
// ran, which is the only arrangement in which it means anything.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "m2_anim_reuse.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);
MH_STATUS WO_DisableHook(void* target);

namespace M2AnimReuse {

namespace {

// The five bytes the jump replaces: mov edx,[ebx+2Ch] ; cmp edx,edi
const unsigned char kHead[5] = { 0x8B, 0x53, 0x2C, 0x3B, 0xD7 };
// The client's own no-bones branch, immediately after them.
const unsigned char kTail[6] = { 0x0F, 0x86, 0x7D, 0x0E, 0x00, 0x00 };

constexpr uintptr_t kEntryAddr = 0x0082F0F0;   // the function itself
constexpr uintptr_t kHeadAddr = 0x0082F418;
constexpr uintptr_t kTailAddr = 0x0082F420;   // the jbe, for the signature
constexpr uintptr_t kRunAddr  = 0x0082F41D;   // mov [ebp+arg_10],edi ; jbe
constexpr uintptr_t kHoldAddr = 0x008302A3;   // where the client's own jbe goes

// Read by the naked thunk, so plain addresses rather than a struct.
void* g_retRun  = (void*)kRunAddr;
void* g_retHold = (void*)kHoldAddr;

unsigned char g_saved[5] = {};
bool g_patched = false;

// On the model.
constexpr unsigned kM_bones     = 0x94;   // bone record array
constexpr unsigned kM_accum     = 0x64;   // non-zero: the loop accumulates a clock
constexpr unsigned kBoneStride  = 0xAC;
// On the object in EBX.
constexpr unsigned kB_boneCount = 0x2C;

// A bone count past this is not something to hash on a hot path, and a model
// with one is refused rather than trusted.
constexpr uint32_t kMaxBones = 8192;

constexpr int kSlots     = 4096;          // the census saw 1.5% eviction at 512
constexpr int kSlotMask  = kSlots - 1;
constexpr long kLearn    = 5000;          // hash comparisons before arming
constexpr unsigned kResample = 4096;      // one call in this many rechecks

struct Slot {
    void*    model;
    uint32_t a[5];        // the five arguments, by bit pattern
    uint64_t hash;        // the array as seen at this slot's last entry
    uint64_t frame;
    bool     hashFromSameTuple;
    // True when the previous call for this slot ran the loop. A hold leaves the
    // array untouched, so hashing across two holds compares a value with
    // itself; a comparison only means something when a loop ran between the two
    // hashes. See the note above the comparison.
    bool     checkNext;
};
Slot g_slot[kSlots];

// The byte the thunk tests. A naked thunk cannot read EAX across the popad that
// protects the client's registers, so the answer comes back in memory.
unsigned char g_hold = 0;

// The x87 TOP field on entry to the decision, and how often it came back
// different. See the note on the cut: the tail pops a value the client left on
// the stack, so anything of ours that pushed or popped one would corrupt every
// float in the rest of the frame rather than fault. Zero is the expected count
// and the report says so either way.
unsigned short g_topBefore = 0;
unsigned long  g_fpuMoved  = 0;

// The five arguments, captured where they still are what the caller passed.
//
// They cannot be read at the cut. The client overwrites one of its own at
// 0x0082F364 - `xor edi, edi` then `mov [ebp+arg_8], edi` - so by 0x0082F418
// arg_8 is a scratch zero and a tuple built there is missing a fifth of its
// key. That is not a theory: this module retired in six field sessions with
// "the same model and the same arguments produced two different bone arrays",
// and they were not the same arguments.
//
// So the entry is hooked as well, purely to record them. A stack rather than a
// single slot because the tail of this function animates attached models and
// can re-enter it; each call reads the entry it pushed, matched on the model so
// a mismatched depth cannot hand one call another's arguments.
typedef int (__fastcall* Animate_fn)(void* This, void* edx, int a0, int a1,
                                     int a2, float a3, float a4);
Animate_fn orig_Animate = nullptr;

constexpr int kMaxDepth = 8;
struct Stash { void* model; uint32_t a[5]; };
// A guard band on each side of the stash, and the reason it is here.
//
// A tester on build 65ddf5e0 crashed entering the world with EIP and EAX both
// 0x3F800000 - the bit pattern of 1.0f - and the return address on the stack
// was Hooked_Animate+0xC2, which is the instruction after
// `call dword ptr [orig_Animate]`. A float had been written over that function
// pointer, and orig_Animate is declared immediately above this array.
//
// The write came from here. The entry hook indexed g_stash[g_depth] after
// checking only that g_depth was below kMaxDepth, never that it was not
// negative, and s.a[3] and s.a[4] are the two float arguments copied in as bit
// patterns. One negative index puts a float on the pointer the very next call
// goes through.
//
// The index is bounded at both ends now, which is the actual fix. The bands are
// here so that a future mistake of the same shape lands on eight words of
// nothing instead of on whatever the linker put next to the array.
uint32_t g_stashGuardLow[8] = {0};
Stash g_stash[kMaxDepth];
uint32_t g_stashGuardHigh[8] = {0};

// Depth, and the thread allowed to use the stash.
//
// This is a plain int and two threads incrementing it can lose an update, which
// is how it reaches a value its own bounds check was not written for. Rather
// than make it atomic on a path this hot, the stash belongs to one thread: the
// first to arrive owns it, and every other thread passes straight through
// without reading or writing either the stash or the depth. That is not a
// restriction in practice - the cut this module installs only ever runs on the
// thread the client animates on - and it removes the race rather than tightening
// it.
int   g_depth = 0;
volatile LONG g_stashOwner = 0;   // thread id, 0 until claimed
unsigned long g_offThreadCalls = 0;
unsigned long g_badDepth = 0;     // the index was out of range on entry
unsigned long g_tooDeep = 0;   // re-entered past kMaxDepth; those are refused

bool g_armed     = false;
bool g_dead      = false;
bool g_abSubject = false;

// Plain 32-bit, main thread only, and lower bounds if that ever stops being
// true. Counted before any early return so a frozen number cannot read as a
// rate.
unsigned long g_calls        = 0;
unsigned long g_held         = 0;
unsigned long g_repeats      = 0;   // tuple matched the previous call
unsigned long g_compared     = 0;
unsigned long g_refusedAccum = 0;   // [esi+64h] set: the loop carries a clock
unsigned long g_refusedShape = 0;   // unreadable, or too many bones
unsigned long g_evictions    = 0;
unsigned long g_stoodAside   = 0;
unsigned long g_frames       = 0;

unsigned long g_heldBones      = 0;  // bone iterations not run
unsigned long g_heldBoneWraps  = 0;

void AddHeldBones(uint32_t n) {
    const unsigned long before = g_heldBones;
    g_heldBones = before + n;
    if (g_heldBones < before) ++g_heldBoneWraps;
}

bool Readable(uintptr_t p) {
    return p >= 0x10000 && p < 0xFFE00000;
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int n) {
    __try {
        return memcmp((const void*)addr, want, (size_t)n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FNV-1a over the whole bone array. Not a checksum anybody has to trust for
// long: a collision here would let one wrong array pass one comparison, and
// kLearn of them have to agree before anything is skipped.
uint64_t HashBones(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    const uint64_t* q = (const uint64_t*)p;
    const size_t words = n >> 3;
    for (size_t i = 0; i < words; ++i) {
        h ^= q[i];
        h *= 1099511628211ULL;
    }
    for (size_t i = words << 3; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Everything is read behind this, because a model can be torn down between the
// census counting it and this reading it.
bool ReadShape(void* model, void* ebx, const uint8_t** bones, uint32_t* count,
               bool* accumulates) {
    __try {
        const uintptr_t m = (uintptr_t)model;
        const uintptr_t b = (uintptr_t)ebx;
        if (!Readable(m + kM_bones + 3) || !Readable(b + kB_boneCount + 3))
            return false;
        *accumulates = *(const uint32_t*)(m + kM_accum) != 0u;
        *count = *(const uint32_t*)(b + kB_boneCount);
        const uintptr_t base = *(const uintptr_t*)(m + kM_bones);
        if (*count == 0 || *count > kMaxBones) return false;
        if (!Readable(base) || !Readable(base + (uintptr_t)*count * kBoneStride))
            return false;
        *bones = (const uint8_t*)base;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Retire(const char* why, uint64_t a, uint64_t b) {
    g_dead = true;
    Log("[M2AnimReuse] Retired: %s (0x%016llX against 0x%016llX). Nothing was "
        "held before this - the loop ran on every call so far - so the session "
        "is unaffected. It means the arguments are not enough to say the bone "
        "array will come out the same, and this module will not skip anything.",
        why, (unsigned long long)a, (unsigned long long)b);
}

}  // namespace

// Called from the naked thunk with the model, the frame pointer and the object
// that carries the bone count. Sets g_hold.
extern "C" void __cdecl M2AnimReuse_Decide(void* model, void* framePtr, void* ebx) {
    g_hold = 0;
    ++g_calls;

    if (g_dead) return;
    if (g_abSubject && AbTest::StandAside()) { ++g_stoodAside; return; }

    const uint8_t* bones = nullptr;
    uint32_t count = 0;
    bool accumulates = false;
    if (!ReadShape(model, ebx, &bones, &count, &accumulates)) {
        ++g_refusedShape;
        return;
    }
    if (accumulates) {
        // Two fields of every bone advance by arg_8 on this model. Holding it
        // would stop a clock rather than repeat a result.
        ++g_refusedAccum;
        return;
    }

    // From the entry hook, not from the frame: see the note on g_stash.
    if (g_depth <= 0 || g_depth > kMaxDepth) { ++g_refusedShape; return; }
    const Stash& st = g_stash[g_depth - 1];
    if (st.model != model) { ++g_refusedShape; return; }
    uint32_t arg[5];
    memcpy(arg, st.a, sizeof(arg));
    (void)framePtr;

    Slot& s = g_slot[(unsigned)(((uintptr_t)model >> 4) & kSlotMask)];

    if (s.model != model) {
        if (s.model) ++g_evictions;
        s.model = model;
        memcpy(s.a, arg, sizeof(arg));
        s.hash = 0;
        s.hashFromSameTuple = false;
        s.checkNext = false;
        s.frame = g_frames;
        return;
    }

    const bool sameTuple = memcmp(s.a, arg, sizeof(arg)) == 0;

    // A second call inside one frame is work the client's own stamp is supposed
    // to remove. Counting it as a repeat would inflate this with calls nobody
    // would have made twice.
    const bool laterFrame = s.frame != g_frames;
    s.frame = g_frames;

    if (!sameTuple) {
        memcpy(s.a, arg, sizeof(arg));
        s.hash = 0;
        s.hashFromSameTuple = false;
        s.checkNext = false;
        return;
    }
    if (!laterFrame) return;

    ++g_repeats;

    // Armed: the array in front of us is the result of these same arguments, so
    // the loop would rewrite it with what it already holds. Two calls are let
    // through anyway - one in kResample, and the one after any call that ran -
    // because the check below is only worth anything when a loop ran between
    // the two hashes it compares.
    const bool sampled = (g_repeats % kResample) == 0;
    if (g_armed && !sampled && !s.checkNext) {
        g_hold = 1;
        ++g_held;
        AddHeldBones(count);
        s.checkNext = false;   // nothing ran, so nothing to compare next time
        return;
    }

    // This call runs the loop. Hash what the previous run left behind: that is
    // the output of the previous tuple, which is this one.
    uint64_t h;
    __try {
        h = HashBones(bones, (size_t)count * kBoneStride);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_refusedShape;
        return;
    }

    if (s.hashFromSameTuple && s.checkNext) {
        ++g_compared;
        if (h != s.hash) {
            Retire("the same model and the same arguments produced two "
                   "different bone arrays", s.hash, h);
            return;
        }
        if (!g_armed && g_compared >= kLearn) {
            g_armed = true;
            Log("[M2AnimReuse] Taking over after %lu comparisons of the bone "
                "array agreed. From here a repeated animation state holds the "
                "pose the loop would have recomputed. One repeat in %u still "
                "runs the loop, and the repeat after it compares what that run "
                "produced, so the checking continues against a loop that "
                "actually ran rather than against a held array compared with "
                "itself.", g_compared, kResample);
        }
        // Unarmed, every call runs the loop and the chain continues. Armed, the
        // pair is closed and holding resumes until the next sample.
        s.checkNext = !g_armed;
    } else {
        s.checkNext = true;
    }

    s.hash = h;
    s.hashFromSameTuple = true;
}

namespace {

// Entered by a jump, not a call. Every exit is a jump to an address the client
// would have reached anyway.
int __fastcall Hooked_Animate(void* This, void* edx, int a0, int a1, int a2,
                             float a3, float a4) {
    // Only the thread that owns the stash may touch it or the depth. See the
    // note on g_stashOwner.
    const LONG me = (LONG)GetCurrentThreadId();
    if (g_stashOwner == 0) InterlockedCompareExchange(&g_stashOwner, me, 0);
    if (g_stashOwner != me) {
        ++g_offThreadCalls;
        return orig_Animate(This, edx, a0, a1, a2, a3, a4);
    }

    // Both ends. Checking only the top is what let a negative index write a
    // float over orig_Animate; see the note on g_stashGuardLow.
    if (g_depth < 0 || g_depth >= kMaxDepth) {
        if (g_depth < 0) { ++g_badDepth; g_depth = 0; }
        else ++g_tooDeep;
        // Not stashing on this call, so the depth is left exactly as it is and
        // the cut below will refuse rather than read a stale entry.
        return orig_Animate(This, edx, a0, a1, a2, a3, a4);
    }
    Stash& s = g_stash[g_depth];
    s.model = This;
    s.a[0] = (uint32_t)a0;
    s.a[1] = (uint32_t)a1;
    s.a[2] = (uint32_t)a2;
    memcpy(&s.a[3], &a3, 4);
    memcpy(&s.a[4], &a4, 4);
    ++g_depth;
    const int r = orig_Animate(This, edx, a0, a1, a2, a3, a4);
    --g_depth;
    return r;
}

__declspec(naked) void Thunk() {
    __asm {
        pushad
        // TOP, bits 11..13 of the status word. Inside pushad, so ax is free.
        fnstsw ax
        and  ax, 3800h
        mov  word ptr [g_topBefore], ax

        push ebx
        push ebp
        push esi
        call M2AnimReuse_Decide
        add  esp, 12

        // If anything in there moved the x87 stack, the tail's fstp would pop
        // the wrong thing. Refuse to hold rather than hand the client a frame
        // whose floats are all indefinite.
        fnstsw ax
        and  ax, 3800h
        cmp  ax, word ptr [g_topBefore]
        je   top_unchanged
        mov  byte ptr [g_hold], 0
        inc  dword ptr [g_fpuMoved]
    top_unchanged:
        popad

        cmp  byte ptr [g_hold], 0
        je   run_bones

        // The client's own no-bones path, byte for byte: set the loop counter
        // and go where its jbe goes.
        mov  [ebp+18h], edi
        jmp  dword ptr [g_retHold]

    run_bones:
        // The two instructions the jump replaced, then back to the third. The
        // jmp does not touch flags, so the client's jbe reads this compare.
        mov  edx, [ebx+2Ch]
        cmp  edx, edi
        jmp  dword ptr [g_retRun]
    }
}

}  // namespace

void OnFrame() { ++g_frames; }

bool Init() {
    if (!Config::g_settings.OptM2AnimReuse) return true;

    if (Config::g_settings.OptAnimLod) {
        Log("[M2AnimReuse] NOT installed: Animation LOD is on and hooks the "
            "entry at 0x%08X, which this needs to read the arguments before the "
            "client overwrites one of them. They cannot both have it.",
            (unsigned)kEntryAddr);
        return false;
    }
    if (Config::g_settings.OptM2AnimStride) {
        Log("[M2AnimReuse] NOT installed: Model Animation Stride is on and cuts "
            "the same five bytes at 0x%08X. Turn that one off - it is marked as "
            "tried and failed - and this one can take the cut.",
            (unsigned)kHeadAddr);
        return false;
    }
    if (!BytesMatch(kHeadAddr, kHead, 5)) {
        Log("[M2AnimReuse] NOT installed: the bytes at 0x%08X are not the bone "
            "count test this was read from.", (unsigned)kHeadAddr);
        return false;
    }
    if (!BytesMatch(kTailAddr, kTail, 6)) {
        Log("[M2AnimReuse] NOT installed: the bone count test matches but the "
            "branch after it at 0x%08X is not the client's own no-bones exit, "
            "so the address a held model would jump to is not confirmed.",
            (unsigned)kTailAddr);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kHeadAddr)) {
        Log("[M2AnimReuse] NOT installed: No Client Patches is on, and this "
            "writes five bytes into wow.exe like every hook here does.");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)kHeadAddr, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[M2AnimReuse] NOT installed: could not make 0x%08X writable",
            (unsigned)kHeadAddr);
        return false;
    }
    // The entry hook goes in first. Without it the cut has no arguments to key
    // on, and a cut installed alone would run on a tuple with a hole in it -
    // which is exactly what retired this module in six field sessions.
    if (WineSafe_CreateHook((void*)kEntryAddr, (void*)&Hooked_Animate,
                            (void**)&orig_Animate) != MH_OK ||
        WO_EnableHook((void*)kEntryAddr) != MH_OK) {
        Log("[M2AnimReuse] NOT installed: could not hook the entry at 0x%08X, "
            "and the cut is useless without the arguments it reads there.",
            (unsigned)kEntryAddr);
        return false;
    }

    memcpy(g_saved, (const void*)kHeadAddr, 5);
    unsigned char patch[5];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&Thunk - (kHeadAddr + 5));
    memcpy((void*)kHeadAddr, patch, 5);
    DWORD ignored = 0;
    VirtualProtect((void*)kHeadAddr, 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)kHeadAddr, 5);
    g_patched = true;

    g_abSubject = AbTest::IsSubject("M2AnimReuse", &g_abSubject);

    Log("[M2AnimReuse] ACTIVE. The animation census measured 91.6%% of a "
        "quarter million bone rebuilds in one session as the same model asked "
        "for the same animation state as in an earlier frame, while the "
        "client's own per-tick stamp removed none of them. This holds the "
        "skeleton when the arguments are identical, so the pose held is the "
        "pose the loop would have recomputed. The tail still runs, so "
        "attachments, emitters, lights and materials keep animating. Models "
        "whose loop accumulates a clock into every bone are refused outright. "
        "Nothing is skipped until %ld hash comparisons of the bone array agree.",
        kLearn);
    if (g_abSubject)
        Log("[M2AnimReuse]   under A/B test: both halves take the same patch "
            "and the same call, and only the hold differs.");
    return true;
}

void Shutdown() {
    // The entry hook goes first: the cut reads what it records, so a cut left
    // running against an unhooked entry would read a stale depth.
    if (orig_Animate) {
        MH_DisableHook((void*)kEntryAddr);
        orig_Animate = nullptr;
    }
    if (!g_patched) return;
    DWORD old = 0;
    if (VirtualProtect((void*)kHeadAddr, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void*)kHeadAddr, g_saved, 5);
        DWORD ignored = 0;
        VirtualProtect((void*)kHeadAddr, 5, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), (void*)kHeadAddr, 5);
    }
    g_patched = false;
}

void LogStats() {
    if (!Config::g_settings.OptM2AnimReuse) return;
    if (!g_patched) {
        Log("[M2AnimReuse] switched on but not installed, so nothing here was "
            "measured.");
        return;
    }
    if (g_calls == 0) {
        Log("[M2AnimReuse] installed, and the bone loop has not run yet. This is "
            "measured and zero, not unmeasured.");
        return;
    }

    // The frame clock, printed before anything that depends on it.
    //
    // A repeat here is defined as the same tuple in a LATER frame, so every
    // number below is multiplied by whether g_frames advances. When this module
    // was on MainThreadPump's clock it reported 2 repeats in 454627333 calls
    // against the census's 91.6% on the same workload, and nothing in the report
    // could tell "these arguments never repeat" from "the clock that decides
    // what a repeat is never moved". Six field sessions read the first when the
    // truth was the second.
    if (g_frames == 0) {
        Log("[Wrong] [M2AnimReuse] the bone loop ran %lu time(s) and the frame "
            "counter is still zero. A repeat is defined as a later frame, so "
            "nothing below can ever be non-zero. OnFrame is not being called.",
            g_calls);
    } else {
        Log("[M2AnimReuse] %lu frame(s) seen, %.1f call(s) per frame. A repeat "
            "below means a later frame, so this is the clock the rest of these "
            "numbers are measured against.",
            g_frames, (double)g_calls / (double)g_frames);
    }

    const double heldBones =
        (double)g_heldBones + (double)g_heldBoneWraps * 4294967296.0;

    Log("[M2AnimReuse] %lu calls, %lu repeats (%.1f%%), %lu held (%.1f%% of "
        "calls), %.0f bone iterations not run. %s",
        g_calls, g_repeats, 100.0 * (double)g_repeats / (double)g_calls,
        g_held, 100.0 * (double)g_held / (double)g_calls, heldBones,
        g_dead ? "Retired." :
        g_armed ? "Armed." : "Still checking; nothing has been held.");

    Log("[M2AnimReuse]   %lu bone arrays compared, all equal. %s",
        g_compared,
        g_armed ? "One repeat in 4096 runs the loop and the repeat after it "
                  "compares what that run produced, so a held array is never "
                  "compared with itself."
                : "Comparisons still short of the count that arms this.");

    if (g_refusedAccum > 0)
        Log("[M2AnimReuse]   %lu calls refused: the loop adds an animation "
            "clock into every bone on that model, so a second run does not "
            "reproduce the first and holding it would stop the clock.",
            g_refusedAccum);
    // Printed whether or not it fired, because zero is the answer that says the
    // decision path is still free of x87 and the hold is safe to take.
    Log("[M2AnimReuse]   the x87 stack depth was unchanged across the decision "
        "on every call but %lu. The tail pops a value the client left on the "
        "stack, so a non-zero figure here means something in the decision now "
        "uses the FPU and those calls refused to hold rather than corrupt the "
        "frame.", g_fpuMoved);
    // Printed whether or not they fired. A non-zero bad-depth count means the
    // index that once wrote a float over orig_Animate went out of range again
    // and was caught this time; a non-zero off-thread count means a second
    // thread reaches this hook, which the stash is no longer shared with.
    Log("[M2AnimReuse]   %lu call(s) arrived with the stash index out of range "
        "and were refused, %lu arrived on a thread that does not own the stash "
        "and were handed straight to the client.", g_badDepth, g_offThreadCalls);
    // The guard bands either side of the stash. Both are written by nothing, so
    // anything but zero means an index ran past the array again.
    {
        uint32_t dirty = 0;
        for (int i = 0; i < 8; ++i) dirty |= g_stashGuardLow[i] | g_stashGuardHigh[i];
        if (dirty)
            Log("[Wrong] [M2AnimReuse] the guard words either side of the stash "
                "are not zero, so something indexed past it. Nothing writes them "
                "on purpose.");
    }
    if (g_tooDeep > 0)
        Log("[M2AnimReuse]   %lu calls re-entered past %d deep - an attached "
            "model animating from inside the tail - and were refused rather "
            "than given another call's arguments.", g_tooDeep, kMaxDepth);
    if (g_refusedShape > 0)
        Log("[M2AnimReuse]   %lu calls refused: the bone array could not be "
            "read, or the model reported more bones than this will hash.",
            g_refusedShape);
    if (g_evictions > 0)
        Log("[M2AnimReuse]   %lu calls landed on a slot held by a different "
            "model and could not be judged. More slots would recover those.",
            g_evictions);
    if (g_stoodAside > 0)
        Log("[M2AnimReuse]   %lu calls stood aside for the A/B off stint.",
            g_stoodAside);
    if (g_repeats > 0 && g_held == 0 && g_armed)
        Log("[Wrong] [M2AnimReuse] armed with repeats counted and nothing held. "
            "That combination should not occur.");
}

}  // namespace M2AnimReuse
