// ============================================================================
// Module: m2_matrix_slot_sse2
//
// sub_82F0F0 is M2_AnimateModel. A tester's sampling profile puts roughly a
// fifth of main-thread execution in the animation family around it, the largest
// single target in the whole profile and about twice anything else.
//
// Two blocks inside it write a bone's finished 4x4 matrix into the model's
// matrix array, one float at a time, as sixteen fld/fstp pairs. They are the
// two arms of one if/else and both write the same slot:
//
//     0x0082FE54  the matrix sub_4C1F00 just produced, EAX -> the slot
//     0x0082FECB  the matrix at [EBX],                  EBX -> the slot
//
// The slot is [ESI+0x98] + (arg_10 << 6): a 64-byte entry in the model's matrix
// array, indexed by the bone number the loop at loc_83028C increments and
// compares against [[EBP-4]+0x2C].
//
// Thirty-two x87 instructions to move sixty-four bytes. Four movups loads and
// four stores do the same thing.
// ---------------------------------------------------------------------------
// Why this needs no precision measurement
//
// There is no arithmetic. `fld dword` widens a single to 80 bits and `fstp
// dword` narrows it back, which is exact for every finite value, every
// infinity, and every quiet NaN. `movups` copies the bits. The one input where
// the two differ is a signalling NaN, which fld quiets and a move does not - a
// matrix does not contain one, and a client that put one there would already be
// undefined.
//
// This is the same argument bone_matrix_upload_sse2 shipped on, and the same
// shape: a pure-move block found by the fld/fstp scan inside a function the
// profile had already named.
// ---------------------------------------------------------------------------
// What the profile did and did not say
//
// It named the function, not these blocks. The entry in the 2026-08-28 uncapped
// profile is `M2_AnimateModel+0x600` at 3.00%, and the fine histogram's buckets
// are 512 bytes, so what was measured is [+0x600, +0x800) - a stretch of
// branching with two fcom/fnstsw pairs in it, and none of the three sites here.
// Site C is at +0x264 and sites A and B at +0xD64 and +0xDDB.
//
// So the case for these three is that they are pure waste in a function the
// profile puts a fifth of the frame in, not that they are the bucket it named.
// They are off by default and an A/B subject for exactly that reason: the
// harness says what they were worth, and if the answer is nothing then nothing
// is what they cost to leave off.
// ---------------------------------------------------------------------------
// The register contracts, read off the disassembly rather than assumed
//
// Site A, 0x0082FE54 through 0x0082FEC3, falls through to 0x0082FEC4:
//
//     in   EAX = source matrix, ESI = model, EBP = frame
//     does the copy, and `add esp, 0Ch` - the cleanup for the three arguments
//          pushed to sub_4C1F00 just above
//     out  EDX = arg_10 << 6, ECX = slot address, ESP raised by twelve
//     x87  sixteen balanced pairs, so net zero
//
// ECX is dead: 0x0082FEC4 is `mov ecx, [ebp+0Ch]`. EAX is dead too. EDX is
// read at 0x0082FF4E on one path and overwritten before use on the other, so it
// is reproduced.
//
// Site B, 0x0082FECB through 0x0082FF39, falls through to 0x0082FF3A:
//
//     in   EBX = source matrix, ESI = model, EBP = frame
//     out  EAX = slot address, EDX = arg_10 << 6
//     x87  one `fstp st(0)` before the copy, so net minus one
//
// That single pop is part of the contract and the replacement performs it. The
// other arm reaches the same label having pushed with `fldz`, and the code at
// 0x0082FF46 pops once, which is what balances the pair of paths.
//
// Neither range is jumped into. Every cross-reference inside site A is ordinary
// flow, and site B has exactly one, the jump to its first byte from 0x0082F7B1.
// ---------------------------------------------------------------------------
// What is checked, and what is not
//
// Both ends of both blocks are compared against the bytes they were read from
// before anything is written, so a different client build refuses rather than
// jumps into the middle of an instruction. The five-byte jump is saved and put
// back on shutdown.
//
// The contract above is read, not measured, and a wrong reading would write
// sixty-four bytes somewhere else. So the first call through each site hands
// the slot address, the model and the frame to a plain C function that checks
// the slot is readable and that the bone index is below the count the client's
// own loop compares against. It cannot undo a patch from inside the code the
// patch redirects to, so it does not try: it records the failure and the report
// says the numbers from that session are not to be used.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "m2_matrix_slot_sse2.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace M2MatrixSlot {

namespace {

enum { kSiteA = 0, kSiteB = 1, kSiteC = 2, kSites = 3 };

const unsigned char kHeadA[8] = { 0xD9, 0x00, 0x8B, 0x8E, 0x98, 0x00, 0x00, 0x00 };
const unsigned char kTailA[6] = { 0xD9, 0x40, 0x3C, 0xD9, 0x59, 0x3C };
const unsigned char kHeadB[8] = { 0x8B, 0x86, 0x98, 0x00, 0x00, 0x00, 0xDD, 0xD8 };
const unsigned char kTailB[6] = { 0xD9, 0x43, 0x3C, 0xD9, 0x58, 0x3C };
const unsigned char kHeadC[6] = { 0xD9, 0x9E, 0x88, 0x00, 0x00, 0x00 };
const unsigned char kTailC[6] = { 0xD9, 0x95, 0x68, 0xFF, 0xFF, 0xFF };

// Row-major, which is what the thirty-five stores at site C write. Aligned
// so the loads are the cheap encoding; the destinations are frame slots and
// their alignment is the client's business, so the stores stay unaligned.
__declspec(align(16)) const float kIdentity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

struct Site {
    const char*          name;
    uintptr_t            head;
    uintptr_t            tailAddr;
    const unsigned char* headWant;
    int                  headLen;
    const unsigned char* tailWant;
    int                  tailLen;
    void*                thunk;
    void*                returnTo;
    bool                 patched;
};

void ThunkA();
void ThunkB();
void ThunkC();

Site g_site[kSites] = {
    { "sub_82F0F0 site A", 0x0082FE54, 0x0082FEBE, kHeadA, 8, kTailA, 6,
      nullptr, (void*)0x0082FEC4, false },
    { "sub_82F0F0 site B", 0x0082FECB, 0x0082FF34, kHeadB, 8, kTailB, 6,
      nullptr, (void*)0x0082FF3A, false },
    { "sub_82F0F0 site C", 0x0082F354, 0x0082F3FB, kHeadC, 6, kTailC, 6,
      nullptr, (void*)0x0082F401, false },
};

unsigned char g_saved[kSites][8] = {};

// Named one per site rather than indexed, because the thunks are naked and
// hand-computing a struct offset inside inline assembly is how a jump lands
// somewhere that was never checked.
void* g_retA = (void*)0x0082FEC4;
void* g_retB = (void*)0x0082FF3A;
void* g_retC = (void*)0x0082F401;

// Main thread only, so plain. Lower bounds if that ever stops being true.
unsigned long g_callsA = 0;
unsigned long g_callsB = 0;
unsigned long g_callsC = 0;

// The one-time contract check, per site.
unsigned char g_checkedA = 0;
unsigned char g_checkedB = 0;
bool          g_contractFailed  = false;
const char*   g_contractReason  = nullptr;

// The flag the A/B harness owns, and it means "I am the subject being measured
// right now" - not "run the fast path". The harness sets it true for whichever
// subject is under test and false for every other one, so a module that read it
// as a fast-path switch would run its control half through every other
// subject's stint and never run one during its own.
//
// So the thunks test it, and when it is set they ask StandAside() which half of
// the stint this is. That call is also how the harness learns the hot path was
// reached at all during an OFF stint, which is what its own warning about a
// meaningless null result depends on.
//
// The control half writes the slot too, four bytes at a time rather than
// sixteen floats, because two halves that do not both write it are not
// comparing the same frame.
bool          g_abSubject  = false;
unsigned char g_standAside = 0;
unsigned long g_scalarCalls = 0;

}  // namespace

// The thunks are naked, so the stint question goes through a byte they can test.
extern "C" void __cdecl M2MatrixSlot_StandAside(void) {
    g_standAside = AbTest::StandAside() ? 1 : 0;
}

namespace {

bool Readable(uintptr_t p) {
    if (p < 0x10000 || p > 0xFFE00000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    return (mbi.Protect & bad) == 0;
}

}  // namespace

// Called once per site, from the thunk, on its first call. Everything it needs
// is what the thunk already had in registers.
extern "C" void __cdecl M2MatrixSlot_CheckContract(int site, void* slot,
                                                   void* model, void* frame) {
    if (site < 0 || site >= kSites) return;
    if (site == 0) { if (g_checkedA) return; g_checkedA = 1; }
    else           { if (g_checkedB) return; g_checkedB = 1; }

    const uintptr_t s = (uintptr_t)slot;
    if (!Readable(s) || !Readable(s + 63)) {
        g_contractFailed = true;
        g_contractReason = "the slot address is not readable";
        Log("[M2Slot] CONTRACT CHECK FAILED at %s: the slot address 0x%08X is "
            "not readable memory, so [ESI+0x98] + (arg_10 << 6) is not what "
            "this thought it was.", g_site[site].name, (unsigned)s);
        return;
    }

    // The client's own loop at 0x0083028C compares the bone index against
    // [[EBP-4]+0x2C]. If the index this read is not below that, the index is
    // not the one the loop is counting.
    const uintptr_t ebp = (uintptr_t)frame;
    if (Readable(ebp - 4)) {
        const uintptr_t owner = *(const uintptr_t*)(ebp - 4);
        if (Readable(owner + 0x2C)) {
            const unsigned count = *(const unsigned*)(owner + 0x2C);
            const unsigned index = *(const unsigned*)(ebp + 0x18);
            if (count == 0 || index >= count) {
                g_contractFailed = true;
                g_contractReason = "the bone index is not below the bone count";
                Log("[M2Slot] CONTRACT CHECK FAILED at %s: bone index %u is not "
                    "below the count %u the client's own loop compares against.",
                    g_site[site].name, index, count);
                return;
            }
        }
    }

    (void)model;
    Log("[M2Slot] %s: contract checked on the first call - slot 0x%08X is "
        "readable and the bone index is inside the count.",
        g_site[site].name, (unsigned)s);
}

namespace {

// Both thunks are entered by a jump, not a call, so there is no return address
// of ours on the stack and every exit is a jump to the instruction after the
// block. Nothing is pushed that is not popped.
__declspec(naked) void ThunkA() {
    __asm {
        // EDX = arg_10 << 6, ECX = [ESI+0x98] + EDX, exactly as the block did.
        mov  edx, [ebp+18h]
        shl  edx, 6
        mov  ecx, [esi+98h]
        add  ecx, edx

        cmp  byte ptr [g_checkedA], 0
        jne  a_checked
        pushad
        push ebp
        push esi
        push ecx
        push 0
        call M2MatrixSlot_CheckContract
        add  esp, 16
        popad
    a_checked:

        cmp  byte ptr [g_abSubject], 0
        je   a_sse
        pushad
        call M2MatrixSlot_StandAside
        popad
        cmp  byte ptr [g_standAside], 0
        jne  a_scalar
    a_sse:

        movups xmm0, [eax]
        movups xmm1, [eax+16]
        movups xmm2, [eax+32]
        movups xmm3, [eax+48]
        movups [ecx], xmm0
        movups [ecx+16], xmm1
        movups [ecx+32], xmm2
        movups [ecx+48], xmm3
        inc  dword ptr [g_callsA]
        jmp  a_done

    a_scalar:
        // The control half of an A/B stint. Four bytes at a time, which is what
        // the client's x87 pair moved, without the x87.
        push esi
        push edi
        mov  esi, eax
        mov  edi, ecx
        mov  eax, 16
    a_loop:
        mov  edx, [esi]
        mov  [edi], edx
        add  esi, 4
        add  edi, 4
        dec  eax
        jnz  a_loop
        pop  edi
        pop  esi
        inc  dword ptr [g_scalarCalls]
        // ESI was restored, so recompute what the block promised.
        mov  edx, [ebp+18h]
        shl  edx, 6

    a_done:
        add  esp, 0Ch
        jmp  dword ptr [g_retA]
    }
}

__declspec(naked) void ThunkB() {
    __asm {
        fstp st(0)

        mov  edx, [ebp+18h]
        shl  edx, 6
        mov  eax, [esi+98h]
        add  eax, edx

        cmp  byte ptr [g_checkedB], 0
        jne  b_checked
        pushad
        push ebp
        push esi
        push eax
        push 1
        call M2MatrixSlot_CheckContract
        add  esp, 16
        popad
    b_checked:

        cmp  byte ptr [g_abSubject], 0
        je   b_sse
        pushad
        call M2MatrixSlot_StandAside
        popad
        cmp  byte ptr [g_standAside], 0
        jne  b_scalar
    b_sse:

        movups xmm0, [ebx]
        movups xmm1, [ebx+16]
        movups xmm2, [ebx+32]
        movups xmm3, [ebx+48]
        movups [eax], xmm0
        movups [eax+16], xmm1
        movups [eax+32], xmm2
        movups [eax+48], xmm3
        inc  dword ptr [g_callsB]
        jmp  b_done

    b_scalar:
        push esi
        push edi
        mov  esi, ebx
        mov  edi, eax
        mov  ecx, 16
    b_loop:
        mov  edx, [esi]
        mov  [edi], edx
        add  esi, 4
        add  edi, 4
        dec  ecx
        jnz  b_loop
        pop  edi
        pop  esi
        inc  dword ptr [g_scalarCalls]
        // ECX and EDX were used; EAX still holds the slot. Recompute EDX, and
        // ECX is dead here because 0x0082FF3A reloads it from the frame.
        mov  edx, [ebp+18h]
        shl  edx, 6

    b_done:
        jmp  dword ptr [g_retB]
    }
}

// Site C initialises two identity matrices and a zero vec3 in the frame, as
// thirty-five x87 stores of fld1 and fldz with an fxch in the middle. It runs
// once per call rather than once per bone, and a session on the animation hook
// counted 24.8 million calls.
//
// Everything about it is a store of a constant, so there is nothing to measure
// about precision: fld1 stores 0x3F800000 and fldz stores zero, and so does a
// move of the same bytes.
//
// Two things the replacement has to preserve besides the values. The block
// leaves both constants on the x87 stack - the stores are fst, not fstp - with
// 1.0 on top after the fxch, so it ends fldz then fld1. And the flags the jz at
// 0x0082F401 tests come from `cmp edx, edi` in the middle of the storm, so the
// compare is the last thing before the jump and nothing after it touches flags.
//
// The frame offsets were decoded from the instruction bytes rather than read off
// IDA's frame table: [ebp-0x50] for the first matrix, [ebp-0xD4] for the second,
// [ebp-0x5C] for the three zeros, [ebp+0x10] for the argument slot. The head and
// tail signatures are the check on those, because they are the same bytes the
// offsets came out of.
__declspec(naked) void ThunkC() {
    __asm {
        fstp dword ptr [esi+88h]

        cmp  byte ptr [g_abSubject], 0
        je   c_sse
        pushad
        call M2MatrixSlot_StandAside
        popad
        cmp  byte ptr [g_standAside], 0
        jne  c_scalar
    c_sse:
        movups xmm0, [kIdentity]
        movups xmm1, [kIdentity+16]
        movups xmm2, [kIdentity+32]
        movups xmm3, [kIdentity+48]
        movups [ebp-50h], xmm0
        movups [ebp-40h], xmm1
        movups [ebp-30h], xmm2
        movups [ebp-20h], xmm3
        movups [ebp-0D4h], xmm0
        movups [ebp-0C4h], xmm1
        movups [ebp-0B4h], xmm2
        movups [ebp-0A4h], xmm3
        xorps  xmm4, xmm4
        movlps qword ptr [ebp-5Ch], xmm4
        movss  dword ptr [ebp-54h], xmm4
        inc  dword ptr [g_callsC]
        jmp  c_done

    c_scalar:
        push edi
        push esi
        lea  edi, [ebp-50h]
        mov  esi, offset kIdentity
        mov  ecx, 16
    c_l1:
        mov  eax, [esi]
        mov  [edi], eax
        add  esi, 4
        add  edi, 4
        dec  ecx
        jnz  c_l1
        lea  edi, [ebp-0D4h]
        mov  esi, offset kIdentity
        mov  ecx, 16
    c_l2:
        mov  eax, [esi]
        mov  [edi], eax
        add  esi, 4
        add  edi, 4
        dec  ecx
        jnz  c_l2
        xor  eax, eax
        mov  [ebp-5Ch], eax
        mov  [ebp-58h], eax
        mov  [ebp-54h], eax
        pop  esi
        pop  edi
        inc  dword ptr [g_scalarCalls]

    c_done:
        // Both constants back on the x87 stack, 1.0 on top, as the block left
        // them.
        fldz
        fld1
        mov  edx, [esi+64h]
        mov  [ebp+10h], edi
        // Last, because the jz at the return address reads these flags.
        cmp  edx, edi
        jmp  dword ptr [g_retC]
    }
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int len) {
    if (!Readable(addr) || !Readable(addr + (uintptr_t)len - 1)) return false;
    return memcmp((const void*)addr, want, (size_t)len) == 0;
}

bool PatchSite(int i) {
    Site& s = g_site[i];
    if (!BytesMatch(s.head, s.headWant, s.headLen)) {
        Log("[M2Slot] %s NOT patched: the bytes at 0x%08X are not the copy this "
            "was read from.", s.name, (unsigned)s.head);
        return false;
    }
    if (!BytesMatch(s.tailAddr, s.tailWant, s.tailLen)) {
        Log("[M2Slot] %s NOT patched: the head at 0x%08X matches but the tail at "
            "0x%08X does not, so the block between them is not the one being "
            "replaced.", s.name, (unsigned)s.head, (unsigned)s.tailAddr);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)s.head)) {
        Log("[M2Slot] %s NOT patched: No Client Patches is on, and this writes "
            "five bytes into wow.exe like every hook here does.", s.name);
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)s.head, (SIZE_T)s.headLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[M2Slot] %s NOT patched: could not make 0x%08X writable",
            s.name, (unsigned)s.head);
        return false;
    }
    memcpy(g_saved[i], (const void*)s.head, (size_t)s.headLen);

    unsigned char patch[8];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)s.thunk - (s.head + 5));
    // Whatever follows the jump inside the saved run is the tail of the
    // instruction it split. Unreachable either way; filled so anything reading
    // the code sees nops rather than half of a mov.
    memset(patch + 5, 0x90, (size_t)(s.headLen - 5));
    memcpy((void*)s.head, patch, (size_t)s.headLen);

    DWORD ignored = 0;
    VirtualProtect((void*)s.head, (SIZE_T)s.headLen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)s.head, (SIZE_T)s.headLen);

    s.patched = true;
    return true;
}

}  // namespace

bool Install() {
    if (!Config::g_settings.OptM2MatrixSlotSse2) {
        Log("[M2Slot] not installed: switched off.");
        return false;
    }

    g_site[kSiteA].thunk = (void*)&ThunkA;
    g_site[kSiteB].thunk = (void*)&ThunkB;
    g_site[kSiteC].thunk = (void*)&ThunkC;

    int done = 0;
    for (int i = 0; i < kSites; ++i)
        if (PatchSite(i)) ++done;

    if (done == 0) {
        Log("[M2Slot] not installed: neither copy block matched the bytes it "
            "was read from.");
        return false;
    }

    // The return is the flag's initial value, which is what the harness will
    // drive from here on. The report reads the scalar count rather than a
    // snapshot taken at install, because a rotating run reaches this subject
    // long after this line.
    g_abSubject = AbTest::IsSubject("M2MatrixSlotSse2", &g_abSubject);
    Log("[M2Slot] ACTIVE on %d of %d sites in sub_82F0F0. Two replace sixteen "
        "fld/fstp pairs each with four SSE2 loads and four stores; the third "
        "replaces thirty-five stores of 1.0 and 0.0 that build two identity "
        "matrices. There is no arithmetic in any of them, so the bytes written "
        "are the bytes read.%s",
        done, (int)kSites,
        Config::g_settings.OptAbTest
            ? " The A/B harness owns the switch between the two halves."
            : "");
    return true;
}

void Shutdown() {
    for (int i = 0; i < kSites; ++i) {
        Site& s = g_site[i];
        if (!s.patched) continue;
        DWORD old = 0;
        if (VirtualProtect((void*)s.head, (SIZE_T)s.headLen,
                           PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)s.head, g_saved[i], (size_t)s.headLen);
            DWORD ignored = 0;
            VirtualProtect((void*)s.head, (SIZE_T)s.headLen, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), (void*)s.head,
                                  (SIZE_T)s.headLen);
        }
        s.patched = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptM2MatrixSlotSse2) {
        Log("[M2Slot] not measured: switched off.");
        return;
    }
    int patched = 0;
    for (int i = 0; i < kSites; ++i) if (g_site[i].patched) ++patched;
    if (patched == 0) {
        Log("[M2Slot] not measured: neither site is patched.");
        return;
    }

    if (g_contractFailed) {
        Log("[M2Slot] DO NOT USE THIS SESSION'S NUMBERS: the first-call contract "
            "check failed because %s. The register contract was read off the "
            "disassembly and something about it is wrong.", g_contractReason);
    }

    const unsigned long total = g_callsA + g_callsB + g_callsC;
    if (total == 0 && g_scalarCalls == 0) {
        Log("[M2Slot] measured and zero: %d of %d sites patched and neither was "
            "reached. The client animated no model through this path.",
            patched, (int)kSites);
        return;
    }

    Log("[M2Slot] %lu block(s) written: %lu bone matrix slots at site A, %lu at "
        "site B, and %lu frame initialisations at site C, over %d of %d sites "
        "patched. A and B run once per bone and C once per call, so they are not "
        "the same unit. Counts are plain increments on the animation path and "
        "are lower bounds.",
        total, g_callsA, g_callsB, g_callsC, patched, (int)kSites);
    if (g_site[kSiteA].patched && g_callsA == 0)
        Log("[M2Slot]   %s is patched and was never reached.", g_site[kSiteA].name);
    if (g_site[kSiteB].patched && g_callsB == 0)
        Log("[M2Slot]   %s is patched and was never reached.", g_site[kSiteB].name);
    if (g_site[kSiteC].patched && g_callsC == 0)
        Log("[M2Slot]   %s is patched and was never reached.", g_site[kSiteC].name);
    if (g_scalarCalls) {
        Log("[M2Slot]   the A/B control half moved %lu slot(s) four bytes at a "
            "time, so both halves wrote the slot and the comparison is between "
            "two ways of doing it rather than doing it and not.", g_scalarCalls);
    } else if (Config::g_settings.OptAbTest) {
        Log("[M2Slot]   the A/B harness never handed this an OFF stint, so "
            "every slot above went out through the SSE2 path and there is no "
            "control half to compare against.");
    }
}

}  // namespace M2MatrixSlot
