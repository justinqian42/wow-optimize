// ============================================================================
// The per-vertex and per-index fill in the client's UI batch draw, sub_484B00.
//
// UI_BatchDraw is 2.3% of executing time in the corrected combat profile, and
// nothing in this project touched it. The function gathers UI geometry into one
// locked vertex buffer and one locked index buffer per texture and state run,
// and its own time goes to two inner loops:
//
//   0x00484F16  per vertex, 24 bytes:
//                 +0   three dwords of position, from [item+14h], stride 12
//                 +12  colour from [item+1Ch] at index ([item+20h] * i) >> 2,
//                      red and blue swapped when the device asks for it, or
//                      FFFFFFFF when the item has no colour array
//                 +16  u and v, copied from [item+18h] with stride 8, or, when
//                      [item+2Ch] is set, u*s+ou and v*s+ov on the x87 stack
//                      with s, ou and ov at [item+30h], [item+34h], [item+38h]
//   0x00485010  per index: [item+24h][j] plus the running vertex base, 16 bits
//
// Every vertex also calls sub_532AF0, which is `lea eax, [ecx+214h]; retn` on
// the global device at dword_C5DF88, only to read one flag at +14h that nothing
// in the loop can change, and reloads four item fields and two frame cursors.
// The replacement reads all of that once per batch and writes the same bytes.
//
// Exactness. Position, colour and copied UV are moves and a byte swap with no
// arithmetic. The transformed UV is one multiply and one add per coordinate:
//
//   fld [esi+30h] / fld st / fmul [eax] / fld [eax+4] / fmulp st(2), st
//   fadd [esi+34h] / fstp var_68 / fadd [esi+38h] / fstp var_64
//
// that is (float)(s*u + ou) and (float)(s*v + ov) with the intermediates at the
// x87 precision control. This client runs at 53 bits, and at 53 bits a double
// multiply and add followed by one rounding to single on the store is the same
// computation. That is an argument, and nothing here relies on it: see the
// verification below.
//
// The patch. Two raw jumps into wow.exe, the way BoneMatrixUpload patches:
//
//   head 0x00484F05  33 FF 39 7E 10      xor edi, edi / cmp [esi+10h], edi
//   tail 0x0048502E  03 56 10 8B 46 28   add edx, [esi+10h] / mov eax, [esi+28h]
//
// The head is reached only after the client's own test that the item has an
// index array. Its thunk calls UiBatchFill_Head with the item (esi), the vertex
// cursor (ecx) and the frame (ebp). When that fills the batch it leaves the two
// cursors the client's loops would have left, in [ebp-0Ch] and [ebp-30h]; the
// thunk sets ebx = [ebp-34h] and edx = [ebp-18h], which is what 0x00484FFA
// reloads, and jumps to the tail. When it declines, every register comes back
// as it was, the two replaced instructions run, and control returns to the jbe
// at 0x00484F0A, so the client's loops run exactly as before. The tail thunk
// calls UiBatchFill_Tail, runs its two replaced instructions and returns to
// 0x00485034.
//
// What is live across those points was read off the disassembly. After the
// vertex loop, ebx and edx are reloaded and eax is cleared; ecx is reloaded at
// 0x00485010 or cleared at the outer loop head; edi is reloaded at 0x00485037.
// The frame cells [ebp-0Ch] and [ebp-30h] are the state that survives, because
// the path at 0x00484E91 reuses both cursors for the next item. The nine x87
// instructions in sub_484B00 are all inside the vertex loop, so the x87 stack
// is empty at the head. Nothing jumps into either patch from outside it, and
// the 303 bytes from the head to the end of the tail are hashed and compared
// with the build this was read from before anything is written.
//
// Verification, predict-then-compare. For the first 4096 batches, and one in
// 1024 after that, the head computes the batch into a private buffer and
// declines, so the client's own loops write the real buffers; the tail then
// compares the two byte for byte, cursors included. The first difference
// retires the module for the session and logs the vertex or index where it
// happened. The comparison is against what the client wrote, not against a
// transcription of it.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <emmintrin.h>

#include "ui_batch_fill_sse2.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"

extern "C" void Log(const char* fmt, ...);

namespace UiBatchFill {

// The batch item the loops read through esi.
struct Item {
    uint32_t        header[4];      // +00
    uint32_t        vertexCount;    // +10
    const uint8_t*  positions;      // +14, 12 bytes a vertex
    const uint8_t*  uvs;            // +18, 8 bytes a vertex
    const uint8_t*  colours;        // +1C, may be null
    uint32_t        colourStride;   // +20
    const uint16_t* indices;        // +24, not null at the head
    uint32_t        indexCount;     // +28
    uint32_t        uvTransform;    // +2C
    float           uvScale;        // +30
    float           uvOffsetU;      // +34
    float           uvOffsetV;      // +38
};
static_assert(offsetof(Item, vertexCount) == 0x10, "item layout");
static_assert(offsetof(Item, colours) == 0x1C, "item layout");
static_assert(offsetof(Item, indexCount) == 0x28, "item layout");
static_assert(offsetof(Item, uvOffsetV) == 0x38, "item layout");

const uintptr_t kHead       = 0x00484F05;
const uintptr_t kTail       = 0x0048502E;
const uintptr_t kRegionEnd  = 0x00485034;
const uint32_t  kRegionFnv  = 0x515B6439u;
const uintptr_t kDevicePtr  = 0x00C5DF88;

const unsigned char kHeadBytes[5] = { 0x33, 0xFF, 0x39, 0x7E, 0x10 };
const unsigned char kTailBytes[6] = { 0x03, 0x56, 0x10, 0x8B, 0x46, 0x28 };

// The scratch the learning phase fills. The client flushes a run before its
// vertex base would pass 0x10000 or its index total 0x20000, so no batch it
// keeps can be larger; a batch that is larger anyway is left to the client.
const uint32_t kMaxVertices = 0x10000;
const uint32_t kMaxIndices  = 0x20000;

const uint32_t kLearnBatches = 4096;
const uint32_t kResampleMask = 1023;

unsigned char g_savedHead[5] = {};
unsigned char g_savedTail[6] = {};
bool g_patchedHead = false;
bool g_patchedTail = false;
bool g_dead = false;
bool g_abSubject = false;

uint8_t*  g_scratchVertices = nullptr;
uint8_t*  g_scratchIndices  = nullptr;

struct Pending {
    bool         active;
    const Item*  item;
    uint8_t*     vertexStart;
    uint8_t*     indexStart;
    uint32_t     vertices;
    uint32_t     indices;
};
Pending g_pending = {};

// Main thread only, so plain. Lower bounds if that ever stops being true.
unsigned long long g_calls = 0;
unsigned long long g_filled = 0;
unsigned long long g_vertices = 0;
unsigned long long g_indices = 0;
unsigned long long g_control = 0;
unsigned long long g_tooLarge = 0;
unsigned g_verified = 0;
unsigned g_mismatches = 0;
unsigned g_pendingLost = 0;
uint32_t g_largestBatch = 0;

bool ColoursSwapped() {
    const uintptr_t device = *(const uintptr_t*)kDevicePtr;
    return *(const int32_t*)(device + 0x214 + 0x14) == 1;
}

void Fill(const Item* it, uint8_t* v, uint8_t* idx, uint16_t base) {
    const uint32_t n = it->vertexCount;
    if (n) {
        const bool haveColour = it->colours != nullptr;
        const bool swap = haveColour && ColoursSwapped();
        const uint32_t stride = it->colourStride;
        const bool transform = it->uvTransform != 0;
        const double s  = it->uvScale;
        const double ou = it->uvOffsetU;
        const double ov = it->uvOffsetV;
        const uint8_t* pos = it->positions;
        const uint8_t* uv = it->uvs;
        for (uint32_t i = 0; i < n; ++i, v += 24) {
            memcpy(v, pos + 12 * (size_t)i, 12);

            uint32_t c = 0xFFFFFFFFu;
            if (haveColour) {
                const uint32_t k = (stride * i) >> 2;
                memcpy(&c, it->colours + 4 * (size_t)k, 4);
                if (swap) {
                    c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
                }
            }
            memcpy(v + 12, &c, 4);

            if (transform) {
                float u0, v0;
                memcpy(&u0, uv + 8 * (size_t)i, 4);
                memcpy(&v0, uv + 8 * (size_t)i + 4, 4);
                const float u1 = (float)(s * (double)u0 + ou);
                const float v1 = (float)(s * (double)v0 + ov);
                memcpy(v + 16, &u1, 4);
                memcpy(v + 20, &v1, 4);
            } else {
                memcpy(v + 16, uv + 8 * (size_t)i, 8);
            }
        }
    }

    const uint32_t m = it->indexCount;
    const uint16_t* src = it->indices;
    const __m128i b = _mm_set1_epi16((short)base);
    uint32_t j = 0;
    for (; j + 8 <= m; j += 8) {
        const __m128i x = _mm_loadu_si128((const __m128i*)(src + j));
        _mm_storeu_si128((__m128i*)(idx + 2 * (size_t)j), _mm_add_epi16(x, b));
    }
    for (; j < m; ++j) {
        const uint16_t w = (uint16_t)(src[j] + base);
        memcpy(idx + 2 * (size_t)j, &w, 2);
    }
}

uint32_t RegionHash() {
    uint32_t h = 0x811C9DC5u;
    __try {
        for (uintptr_t a = kHead; a < kRegionEnd; ++a) {
            h ^= *(const unsigned char*)a;
            h *= 0x01000193u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h;
}

}  // namespace UiBatchFill

extern "C" int __cdecl UiBatchFill_Head(const void* itemPtr, uint8_t* vertexCursor,
                                        uintptr_t ebp) {
    using namespace UiBatchFill;
    const Item* item = (const Item*)itemPtr;

    ++g_calls;
    if (g_dead) return 0;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        return 0;
    }

    uint8_t* const indexCursor = *(uint8_t**)(ebp - 0x30);
    const uint16_t base = (uint16_t)*(const uint32_t*)(ebp - 0x18);
    const uint32_t n = item->vertexCount;
    const uint32_t m = item->indexCount;
    if (n > g_largestBatch) g_largestBatch = n;

    const bool learning = g_verified < kLearnBatches ||
                          ((uint32_t)g_calls & kResampleMask) == 0;
    if (learning) {
        if (n > kMaxVertices || m > kMaxIndices) {
            ++g_tooLarge;
            return 0;
        }
        Fill(item, g_scratchVertices, g_scratchIndices, base);
        g_pending.active      = true;
        g_pending.item        = item;
        g_pending.vertexStart = vertexCursor;
        g_pending.indexStart  = indexCursor;
        g_pending.vertices    = n;
        g_pending.indices     = m;
        return 0;
    }

    Fill(item, vertexCursor, indexCursor, base);
    *(uint8_t**)(ebp - 0x0C) = vertexCursor + 24 * (size_t)n;
    *(uint8_t**)(ebp - 0x30) = indexCursor + 2 * (size_t)m;
    ++g_filled;
    g_vertices += n;
    g_indices += m;
    return 1;
}

extern "C" void __cdecl UiBatchFill_Tail(const void* itemPtr, uintptr_t ebp) {
    using namespace UiBatchFill;
    if (!g_pending.active) return;
    const Pending p = g_pending;
    g_pending.active = false;

    if (p.item != (const Item*)itemPtr) {
        ++g_pendingLost;
        return;
    }

    char what[200] = "";
    const uint8_t* vertexEnd = *(uint8_t**)(ebp - 0x0C);
    const uint8_t* indexEnd  = *(uint8_t**)(ebp - 0x30);
    if (vertexEnd != p.vertexStart + 24 * (size_t)p.vertices) {
        _snprintf(what, sizeof(what) - 1, "the client left its vertex cursor at %p "
                  "and the prediction put it at %p", (void*)vertexEnd,
                  (void*)(p.vertexStart + 24 * (size_t)p.vertices));
    } else if (indexEnd != p.indexStart + 2 * (size_t)p.indices) {
        _snprintf(what, sizeof(what) - 1, "the client left its index cursor at %p "
                  "and the prediction put it at %p", (void*)indexEnd,
                  (void*)(p.indexStart + 2 * (size_t)p.indices));
    } else {
        for (uint32_t i = 0; i < p.vertices && !what[0]; ++i) {
            const uint8_t* a = p.vertexStart + 24 * (size_t)i;
            const uint8_t* b = g_scratchVertices + 24 * (size_t)i;
            if (memcmp(a, b, 24) == 0) continue;
            for (int f = 0; f < 24; f += 4) {
                uint32_t ca, cb;
                memcpy(&ca, a + f, 4);
                memcpy(&cb, b + f, 4);
                if (ca != cb) {
                    _snprintf(what, sizeof(what) - 1, "vertex %u of %u, bytes +%d: "
                              "client %08X, prediction %08X", i, p.vertices, f,
                              ca, cb);
                    break;
                }
            }
        }
        if (!what[0] && memcmp(p.indexStart, g_scratchIndices, 2 * (size_t)p.indices) != 0) {
            for (uint32_t j = 0; j < p.indices; ++j) {
                uint16_t ca, cb;
                memcpy(&ca, p.indexStart + 2 * (size_t)j, 2);
                memcpy(&cb, g_scratchIndices + 2 * (size_t)j, 2);
                if (ca != cb) {
                    _snprintf(what, sizeof(what) - 1, "index %u of %u: client %04X, "
                              "prediction %04X", j, p.indices, ca, cb);
                    break;
                }
            }
        }
    }
    what[sizeof(what) - 1] = '\0';

    if (!what[0]) {
        ++g_verified;
        return;
    }

    ++g_mismatches;
    g_dead = true;
    Log("[UiBatchFill] DISABLED for this session: a batch came out differently "
        "from the client's own loops - %s. The client's bytes are the ones in the "
        "buffer; every batch from here on is left to the client.", what);
    Verdict::Add(Verdict::Bad, "UiBatchFill predicted a UI batch that differed from "
                 "what the client wrote and retired itself for this session");
}

namespace UiBatchFill {

static void* g_toHeadResume = (void*)0x00484F0A;
static void* g_toTail       = (void*)0x0048502E;
static void* g_toTailResume = (void*)0x00485034;

// Register marshalling only; the contract is in the header comment.
static __declspec(naked) void ThunkHead() {
    __asm {
        pushad
        push ebp
        push ecx
        push esi
        call UiBatchFill_Head
        add  esp, 12
        test eax, eax
        popad                           // does not touch the flags
        jz   decline
        mov  ebx, [ebp-34h]
        mov  edx, [ebp-18h]
        jmp  dword ptr [g_toTail]
    decline:
        xor  edi, edi
        cmp  [esi+10h], edi
        jmp  dword ptr [g_toHeadResume]
    }
}

static __declspec(naked) void ThunkTail() {
    __asm {
        pushad
        push ebp
        push esi
        call UiBatchFill_Tail
        add  esp, 8
        popad
        add  edx, [esi+10h]
        mov  eax, [esi+28h]
        jmp  dword ptr [g_toTailResume]
    }
}

static bool BytesMatch(uintptr_t addr, const unsigned char* want, int n) {
    __try {
        return memcmp((const void*)addr, want, (size_t)n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteJump(uintptr_t at, void* to, int len, unsigned char* saved) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(saved, (const void*)at, (size_t)len);
    unsigned char patch[8];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)to - (at + 5));
    if (len > 5) memset(patch + 5, 0x90, (size_t)(len - 5));
    memcpy((void*)at, patch, (size_t)len);
    DWORD ignored = 0;
    VirtualProtect((void*)at, (SIZE_T)len, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, (SIZE_T)len);
    return true;
}

static void Restore(uintptr_t at, const unsigned char* saved, int len) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy((void*)at, saved, (size_t)len);
    DWORD ignored = 0;
    VirtualProtect((void*)at, (SIZE_T)len, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, (SIZE_T)len);
}

bool Init() {
    if (!Config::g_settings.OptUiBatchFill) return true;

    if (!BytesMatch(kHead, kHeadBytes, 5) || !BytesMatch(kTail, kTailBytes, 6) ||
        RegionHash() != kRegionFnv) {
        Log("[UiBatchFill] NOT active: the 303 bytes from 0x%08X to 0x%08X are not "
            "the loops this was read from, so nothing was written.",
            (unsigned)kHead, (unsigned)kRegionEnd);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kHead)) {
        Log("[UiBatchFill] NOT active: No Client Patches is on, and this writes two "
            "jumps into wow.exe.");
        return false;
    }

    // Without the scratch the verification cannot run, and without the
    // verification this does not patch.
    g_scratchVertices = (uint8_t*)VirtualAlloc(
        nullptr, (SIZE_T)kMaxVertices * 24 + (SIZE_T)kMaxIndices * 2,
        MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!g_scratchVertices) {
        Log("[UiBatchFill] NOT active: the %u KB verification buffer could not be "
            "committed, error %lu", (unsigned)(((SIZE_T)kMaxVertices * 24 +
            (SIZE_T)kMaxIndices * 2) / 1024), GetLastError());
        return false;
    }
    g_scratchIndices = g_scratchVertices + (SIZE_T)kMaxVertices * 24;

    // Tail first. With only the tail in place every batch runs the client's
    // loops and one empty call; with only the head, a batch filled here would
    // jump to a tail that does not call back.
    if (!WriteJump(kTail, (void*)&ThunkTail, 6, g_savedTail)) {
        Log("[UiBatchFill] NOT active: could not make 0x%08X writable", (unsigned)kTail);
        return false;
    }
    g_patchedTail = true;
    if (!WriteJump(kHead, (void*)&ThunkHead, 5, g_savedHead)) {
        Restore(kTail, g_savedTail, 6);
        g_patchedTail = false;
        Log("[UiBatchFill] NOT active: could not make 0x%08X writable", (unsigned)kHead);
        return false;
    }
    g_patchedHead = true;

    g_abSubject = AbTest::IsSubject("UiBatchFill", &g_abSubject);

    Log("[UiBatchFill] ACTIVE on the UI batch fill in sub_484B00 (UI_BatchDraw, 2.3%% "
        "of executing time in the corrected combat profile). A getter call, six "
        "reloads and an x87 transform per vertex become one read per batch. The "
        "first %u batches, and one in %u after, are predicted and compared byte "
        "for byte with what the client's own loops write; the first difference "
        "switches this off for the session.", kLearnBatches, kResampleMask + 1);
    if (g_abSubject)
        Log("[UiBatchFill]   under A/B test: the control half declines at the same "
            "patch, so both halves pay for the jump and only the fill differs.");
    return true;
}

void Shutdown() {
    if (g_patchedHead) {
        Restore(kHead, g_savedHead, 5);
        g_patchedHead = false;
    }
    if (g_patchedTail) {
        Restore(kTail, g_savedTail, 6);
        g_patchedTail = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptUiBatchFill) return;
    if (!g_patchedHead) {
        Log("[UiBatchFill] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[UiBatchFill] patched, and the fill was never reached. That is a "
            "measurement: no UI batch with an index array has been drawn yet, or "
            "this is not the path it was read from.");
        return;
    }
    Log("[UiBatchFill] %llu batch(es) reached the patch; %llu filled here (%llu "
        "vertices, %llu indices); largest batch %u vertices. Plain counters, lower "
        "bounds.", g_calls, g_filled, g_vertices, g_indices, g_largestBatch);
    if (g_mismatches) {
        Log("[UiBatchFill]   DISABLED after a difference from the client's own "
            "output; the line that says where is earlier in this log.");
    } else if (g_verified < kLearnBatches) {
        Log("[UiBatchFill]   %u of %u batches verified against the client so far; "
            "still leaving every batch to the client and comparing.",
            g_verified, kLearnBatches);
    } else {
        Log("[UiBatchFill]   %u batches verified byte for byte against the client, "
            "none differed; one in %u is still compared.", g_verified,
            kResampleMask + 1);
    }
    if (g_tooLarge)
        Log("[UiBatchFill]   %llu batch(es) were larger than the verification buffer "
            "and were left to the client.", g_tooLarge);
    if (g_pendingLost)
        Log("[UiBatchFill]   %u prediction(s) were dropped because the tail saw a "
            "different item than the head, so they were neither counted nor "
            "compared.", g_pendingLost);
    if (g_abSubject)
        Log("[UiBatchFill]   %llu batch(es) went through the client's loops as the "
            "A/B control half.", g_control);
}

}  // namespace UiBatchFill
