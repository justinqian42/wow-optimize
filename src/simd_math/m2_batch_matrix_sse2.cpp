// ============================================================================
// Module: m2_batch_matrix_sse2
//
// sub_823130 is M2_RenderModelBatch / M2_DrawModel, called from every single
// model rendering path in the client (world objects, characters, creatures,
// doodads, particles, and missiles).
//
// Sampling profiler records:
//     { 0x00823130, 2909, "PureFloatMove_sub823130" }, // 32/32 block at 0x008236B3
//
// Inside sub_823130:
//
// Site 1 (0x008236B3 - 0x0082371F, 108 bytes):
//     Duplicates the top matrix on stack slot 8 (depth - 1 to depth) as sixteen
//     fld/fstp pairs from [EAX-38h] to [EAX+8], while pushing the constant
//     identity matrix pointer 0x00AF58A8 for the call at 0x00823741.
//     Replaced by four unaligned 128-bit SSE2 vector loads and stores.
//
// Site 2 (0x00823762 - 0x00823803, 161 bytes):
//     Initializes the top matrix on slot 8 with the 4x4 identity matrix from
//     0x00AF58A8 as sixteen fld/fstp pairs.
//     Replaced by four 128-bit SSE2 vector loads from an aligned identity
//     constant and four unaligned stores into [EAX].
//
// Why this needs no precision measurement:
// Pure 64-byte float bit copies with no arithmetic whatsoever.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "m2_batch_matrix_sse2.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace M2BatchMatrix {

namespace {

enum { kSite1 = 0, kSite2 = 1, kSites = 2 };

const unsigned char kHead1[8] = { 0x89, 0x01, 0xC1, 0xE0, 0x06, 0xD9, 0x44, 0x08 };
const unsigned char kTail1[6] = { 0xD9, 0x40, 0x04, 0xD9, 0x58, 0x44 };

const unsigned char kHead2[8] = { 0xD9, 0x05, 0xA8, 0x58, 0xAF, 0x00, 0x8B, 0x10 };
const unsigned char kTail2[9] = { 0xD9, 0x05, 0xE4, 0x58, 0xAF, 0x00, 0xD9, 0x58, 0x3C };

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

void ThunkSite1();
void ThunkSite2();

Site g_site[kSites] = {
    { "sub_823130 site 1 (slot 8 push)",     0x008236B3, 0x00823719, kHead1, 5, kTail1, 6,
      nullptr, (void*)0x0082371F, false },
    { "sub_823130 site 2 (slot 8 identity)", 0x00823762, 0x008237FA, kHead2, 6, kTail2, 9,
      nullptr, (void*)0x00823803, false },
};

unsigned char g_saved[kSites][8] = {};

void* g_retSite1 = (void*)0x0082371F;
void* g_retSite2 = (void*)0x00823803;

unsigned long g_callsSite1 = 0;
unsigned long g_callsSite2 = 0;

unsigned char g_checkedSite1 = 0;
unsigned char g_checkedSite2 = 0;
bool          g_contractFailed  = false;
const char*   g_contractReason  = nullptr;

bool          g_abSubject   = false;
unsigned char g_standAside  = 0;
unsigned long g_scalarCalls = 0;

bool Readable(uintptr_t p) {
    if (p < 0x10000 || p > 0xFFE00000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    return (mbi.Protect & bad) == 0;
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int len) {
    if (!Readable(addr) || !Readable(addr + len - 1)) return false;
    return memcmp((const void*)addr, want, (size_t)len) == 0;
}

}  // namespace

extern "C" void __cdecl M2BatchMatrix_StandAside(void) {
    g_standAside = AbTest::StandAside() ? 1 : 0;
}

extern "C" void __cdecl M2BatchMatrix_CheckContract(int site, void* dest, void* src) {
    if (site < 0 || site >= kSites) return;
    if (site == 0) { if (g_checkedSite1) return; g_checkedSite1 = 1; }
    else           { if (g_checkedSite2) return; g_checkedSite2 = 1; }

    const uintptr_t d = (uintptr_t)dest;
    const uintptr_t s = (uintptr_t)src;

    if (!Readable(d) || !Readable(d + 63)) {
        g_contractFailed = true;
        g_contractReason = "the destination address is not readable/writable";
        Log("[M2BatchMatrix] CONTRACT CHECK FAILED at %s: dest 0x%08X is not valid memory.",
            g_site[site].name, (unsigned)d);
        return;
    }

    if (!Readable(s) || !Readable(s + 63)) {
        g_contractFailed = true;
        g_contractReason = "the source address is not readable";
        Log("[M2BatchMatrix] CONTRACT CHECK FAILED at %s: src 0x%08X is not valid memory.",
            g_site[site].name, (unsigned)s);
        return;
    }

    Log("[M2BatchMatrix] %s: contract checked on first call - dest 0x%08X, src 0x%08X are valid memory.",
        g_site[site].name, (unsigned)d, (unsigned)s);
}

namespace {

__declspec(naked) void ThunkSite1() {
    __asm {
        mov  [ecx], eax
        shl  eax, 6
        add  eax, ecx

        push 00AF58A8h

        cmp  byte ptr [g_checkedSite1], 0
        jne  site1_checked
        pushad
        mov  edx, eax
        sub  edx, 38h
        lea  eax, [eax+8]
        push edx
        push eax
        push 0
        call M2BatchMatrix_CheckContract
        add  esp, 12
        popad
    site1_checked:

        cmp  byte ptr [g_abSubject], 0
        je   site1_sse
        pushad
        call M2BatchMatrix_StandAside
        popad
        cmp  byte ptr [g_standAside], 0
        jne  site1_scalar

    site1_sse:
        movups xmm0, [eax - 38h]
        movups xmm1, [eax - 28h]
        movups xmm2, [eax - 18h]
        movups xmm3, [eax - 08h]
        movups [eax + 08h], xmm0
        movups [eax + 18h], xmm1
        movups [eax + 28h], xmm2
        movups [eax + 38h], xmm3
        inc  dword ptr [g_callsSite1]
        jmp  dword ptr [g_retSite1]

    site1_scalar:
        push esi
        push edi
        push ecx
        lea  esi, [eax - 38h]
        lea  edi, [eax + 08h]
        mov  ecx, 16
    site1_loop:
        mov  edx, [esi]
        mov  [edi], edx
        add  esi, 4
        add  edi, 4
        dec  ecx
        jnz  site1_loop
        pop  ecx
        pop  edi
        pop  esi
        inc  dword ptr [g_scalarCalls]
        jmp  dword ptr [g_retSite1]
    }
}

__declspec(naked) void ThunkSite2() {
    __asm {
        mov  edx, [eax]
        lea  ecx, [eax+ecx*4+108h]
        shl  edx, 6
        lea  eax, [edx+eax+8]

        cmp  byte ptr [g_checkedSite2], 0
        jne  site2_checked
        pushad
        push 00AF58A8h
        push eax
        push 1
        call M2BatchMatrix_CheckContract
        add  esp, 12
        popad
    site2_checked:

        cmp  byte ptr [g_abSubject], 0
        je   site2_sse
        pushad
        call M2BatchMatrix_StandAside
        popad
        cmp  byte ptr [g_standAside], 0
        jne  site2_scalar

    site2_sse:
        movaps xmm0, [kIdentity]
        movaps xmm1, [kIdentity+16]
        movaps xmm2, [kIdentity+32]
        movaps xmm3, [kIdentity+48]
        movups [eax], xmm0
        movups [eax+16], xmm1
        movups [eax+32], xmm2
        movups [eax+48], xmm3
        inc  dword ptr [g_callsSite2]
        jmp  dword ptr [g_retSite2]

    site2_scalar:
        push esi
        push edi
        push ecx
        mov  esi, 00AF58A8h
        mov  edi, eax
        mov  ecx, 16
    site2_loop:
        mov  edx, [esi]
        mov  [edi], edx
        add  esi, 4
        add  edi, 4
        dec  ecx
        jnz  site2_loop
        pop  ecx
        pop  edi
        pop  esi
        inc  dword ptr [g_scalarCalls]
        jmp  dword ptr [g_retSite2]
    }
}

bool PatchSite(int i) {
    Site& s = g_site[i];
    if (!BytesMatch(s.head, s.headWant, s.headLen)) {
        Log("[M2BatchMatrix] %s NOT patched: head bytes at 0x%08X do not match.",
            s.name, (unsigned)s.head);
        return false;
    }
    if (!BytesMatch(s.tailAddr, s.tailWant, s.tailLen)) {
        Log("[M2BatchMatrix] %s NOT patched: tail bytes at 0x%08X do not match.",
            s.name, (unsigned)s.tailAddr);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)s.head)) {
        Log("[M2BatchMatrix] %s NOT patched: No Client Patches is active.", s.name);
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)s.head, (SIZE_T)s.headLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[M2BatchMatrix] %s NOT patched: VirtualProtect failed at 0x%08X.",
            s.name, (unsigned)s.head);
        return false;
    }

    memcpy(g_saved[i], (const void*)s.head, (size_t)s.headLen);

    unsigned char patch[8];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)s.thunk - (s.head + 5));
    if (s.headLen > 5) {
        memset(patch + 5, 0x90, (size_t)(s.headLen - 5));
    }
    memcpy((void*)s.head, patch, (size_t)s.headLen);

    DWORD ignored = 0;
    VirtualProtect((void*)s.head, (SIZE_T)s.headLen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)s.head, (SIZE_T)s.headLen);

    s.patched = true;
    return true;
}

}  // namespace

bool Install() {
    if (!Config::g_settings.OptM2BatchMatrixSse2) {
        Log("[M2BatchMatrix] not installed: switched off.");
        return false;
    }

    g_site[kSite1].thunk = (void*)&ThunkSite1;
    g_site[kSite2].thunk = (void*)&ThunkSite2;

    int done = 0;
    for (int i = 0; i < kSites; ++i) {
        if (PatchSite(i)) ++done;
    }

    if (done == 0) {
        Log("[M2BatchMatrix] not installed: neither copy block matched expected bytes.");
        return false;
    }

    g_abSubject = AbTest::IsSubject("M2BatchMatrixSse2", &g_abSubject);
    Log("[M2BatchMatrix] ACTIVE on %d of %d sites in sub_823130. Replaces sixteen "
        "fld/fstp pairs each with four SSE2 vector moves. Bit-exact bitwise copy with "
        "zero arithmetic.%s",
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
        if (VirtualProtect((void*)s.head, (SIZE_T)s.headLen, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)s.head, g_saved[i], (size_t)s.headLen);
            DWORD ignored = 0;
            VirtualProtect((void*)s.head, (SIZE_T)s.headLen, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), (void*)s.head, (SIZE_T)s.headLen);
        }
        s.patched = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptM2BatchMatrixSse2) {
        Log("[M2BatchMatrix] not measured: switched off.");
        return;
    }
    int patched = 0;
    for (int i = 0; i < kSites; ++i) {
        if (g_site[i].patched) ++patched;
    }
    if (patched == 0) {
        Log("[M2BatchMatrix] not measured: neither site is patched.");
        return;
    }

    if (g_contractFailed) {
        Log("[M2BatchMatrix] WARNING: first-call contract check failed because %s.",
            g_contractReason);
    }

    const unsigned long total = g_callsSite1 + g_callsSite2;
    if (total == 0 && g_scalarCalls == 0) {
        Log("[M2BatchMatrix] measured and zero: %d of %d sites patched and neither was reached.",
            patched, (int)kSites);
        return;
    }

    Log("[M2BatchMatrix] %lu matrix setup block(s) written: %lu stack push copies at site 1, "
        "%lu identity resets at site 2, over %d of %d sites patched.",
        total, g_callsSite1, g_callsSite2, patched, (int)kSites);

    if (g_scalarCalls) {
        Log("[M2BatchMatrix]   the A/B control half moved %lu block(s) via scalar path.",
            g_scalarCalls);
    }
}

}  // namespace M2BatchMatrix
