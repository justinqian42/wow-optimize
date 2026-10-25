// ============================================================================
// sub_873060 is called from the M2 draw path (sub_8203B0 and sub_820AE0, plus
// sub_7A84D0 and sub_873160). It does two things:
//
//   if (dword_D43020) {
//       sub_685F50(dword_C5DF88, 77, *(dword_D43024 + 4*a1 +  44));
//       sub_685F50(dword_C5DF88, 78, *(dword_D43024 + 4*a2 + 404));
//       v2 = (a2 & 8) == 0;
//   } else v2 = 1;
//   if (v2 != dword_D43008) { dword_D43008 = v2; ...apply the mode change... }
//
// The first half sets two per-draw parameters indexed by this model's own a1
// and a2. The second half is a cached two-state mode toggle.
//
// This guard exists because the second half dereferences dword_C5DF88
// unconditionally once the mode differs, and the first half indexes
// dword_D43024, so a null in either is a crash.
//
// What it must not do is skip more than it has to. Suppressing a call does not
// lose the mode change - dword_D43008 is left alone, so the next call that gets
// through still sees a difference and applies it. But it does lose the two
// per-draw parameters, and that model then draws with whichever model set them
// last. Once, for one frame, on one model, is a flicker: intermittent, hard to
// attribute, and exactly the kind of report that arrives as "the screen flickers
// occasionally after changing a graphics setting".
//
// So the D43024 test matches what the original actually dereferences: it only
// indexes that when D43020 is set. And every suppression is counted and
// attributed, because otherwise this can fire on every draw call in the game
// with nothing anywhere saying so.
//
// The IsDeviceReady check is left as it was. sub_873060 never touches the D3D9
// device pointer itself, so on the face of it that check guards something this
// function does not do - but the mode change makes a CGxDevice virtual call at
// vtable+280, which may well reach the device, and loosening a crash guard on a
// hypothesis is not a trade worth making blind. It is counted separately
// instead, so the next log says whether it is the one firing.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "render_null_guard.h"

extern "C" void Log(const char* fmt, ...);

// Global pointers that sub_873060 reads
static uint32_t* const D43020 = (uint32_t*)0x00D43020; // device-ready flag
static uint32_t* const D43024 = (uint32_t*)0x00D43024; // per-draw parameter table
static uint32_t* const C5DF88 = (uint32_t*)0x00C5DF88; // global CGxDevice

// Original function
typedef int (__cdecl *Sub873060_t)(int a1, int a2);
static Sub873060_t g_orig873060 = nullptr;

static bool g_active = false;

// Suppression accounting. This runs on the render thread only, and these are
// counters read after the fact, so plain increments are enough - an interlocked
// pair on a per-draw path would cost more than the call being guarded.
// Sixty-four bits, and not for tidiness. A field session has g_calls at
// 1097427112 and these were signed 32-bit, where the wrap is at 2147483648 - so
// a session about twice as long prints a negative number of draw calls. This
// project has been there: fifteen signed counters in matrix_copy_sse2 did
// exactly that, one of them ran out inside the second hour and took the total
// with it.
//
// The width was chosen by measurement rather than by the rule of thumb. On this
// target a volatile 64-bit increment is 1.03 ns against 1.24 for the
// plain-32-with-a-wrap-counter pattern, because the 64-bit form is two
// dependent ALU operations and the other one has a compare and a branch. The
// rule in CLAUDE.md about never using a plain 64-bit counter is about a value
// tearing when another thread reads it; these are written and read on the main
// thread, and the report says the counts are lower bounds.
static volatile LONG64 g_calls        = 0;
static volatile LONG64 g_suppressed   = 0;
static volatile LONG64 g_noParamTable = 0;   // D43020 set but D43024 null
static volatile LONG64 g_noGxDevice   = 0;   // C5DF88 null
static volatile LONG64 g_deviceNotReady = 0; // D3D9 device missing or vtable bad

// The last D3D9 device whose vtable word was checked and found sane.
//
// Not a cache of the answer - a cache of the last step of it. The walk from the
// global to the vtable is three dependent loads, and this hook takes 1097427112
// calls in a field session and has suppressed none of them, so all three run a
// billion times to reach the same conclusion.
//
// Keyed on the device pointer itself, so a device that is torn down and replaced
// is checked again. That is no weaker than checking every time: if the object at
// that address were freed and the address reused, the unconditional version
// would read the recycled memory and range-check whatever it found, and pass as
// well.
static uintptr_t g_lastGoodDevice = 0;

// Takes the CGxDevice the caller has already loaded rather than reading the
// global a second time. It was read two lines above the call.
static bool IsDeviceReady(uintptr_t pGxDevice) {
    if (pGxDevice < 0x10000 || pGxDevice > 0xFFE00000) return false;

    const uintptr_t pD3d9Device = *(uintptr_t*)(pGxDevice + 0x397C);
    if (pD3d9Device < 0x10000 || pD3d9Device > 0xFFE00000) return false;
    if (pD3d9Device == g_lastGoodDevice) return true;

    const uintptr_t pVtable = *(uintptr_t*)pD3d9Device;
    if (pVtable < 0x10000 || pVtable > 0xFFE00000) return false;

    g_lastGoodDevice = pD3d9Device;
    return true;
}

static int __cdecl Hooked_873060(int a1, int a2)
{
    ++g_calls;

    // The original indexes dword_D43024 only inside the `if (dword_D43020)`
    // block. Demanding it when D43020 is clear suppressed calls the original
    // would have run safely, and every one of those cost a model its two
    // per-draw parameters.
    if (*D43020 && !*D43024) {
        ++g_noParamTable; ++g_suppressed;
        return 1;
    }
    const uintptr_t gx = *(uintptr_t*)C5DF88;
    if (!gx) {
        ++g_noGxDevice; ++g_suppressed;
        return 1;
    }
    if (!IsDeviceReady(gx)) {
        ++g_deviceNotReady; ++g_suppressed;
        return 1;
    }

    return g_orig873060(a1, a2);
}

// ----------------------------------------------------------------------------
// AsyncTextureWait guard (sub_4B6550 @ 0x004B6550)
//
// In stock 3.3.5a build 12340, TextureGetGxTex (sub_4B6CB0) invokes
// AsyncTextureWait (sub_4B6550) when m_asyncObject (at offset +0x40) is non-zero.
// sub_4B6550 blindly loads edi = [esi+40h] and executes:
//     cmp dword ptr [edi+4], 0
//
// If an async operation failed, was aborted, or an external caller/addon
// left INVALID_HANDLE_VALUE ((void*)-1 / 0xFFFFFFFF) or a wild pointer in
// texture->m_asyncObject, the client dereferences [0xFFFFFFFF+4] = 0x00000003,
// crashing immediately with 0xC0000005 ACCESS_VIOLATION on the render thread.
//
// Guard:
// If esi is invalid (< 0x10000 or >= 0xFFFE0000), or if [esi+0x40] is invalid,
// we sanitize [esi+0x40] to nullptr, count the suppression, and return
// immediately. This avoids the fatal dereference and allows TextureGetGxTex
// to proceed safely to return the atlas/surface pointer.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget4B6550 = 0x004B6550;
static const unsigned char kExpectedPrologue4B6550[8] = {
    0x57, 0x8B, 0x7E, 0x40, 0x85, 0xFF, 0x74, 0x1C
};

static void* g_orig4B6550 = nullptr;
static volatile LONG g_asyncWaitSuppressed = 0;
static volatile LONG g_asyncReentrantWaitSuppressed = 0;
static bool g_guard4B6550Active = false;
static bool g_optHighPlacementClientActive = false;

static inline bool IsValidUserPointer(uintptr_t p) {
    if (p < 0x10000) return false;
    if (p < 0x7FFE0000) return true;
    if (!g_optHighPlacementClientActive) return false;
    if (p < 0x80010000 || p >= 0xFFFE0000) return false;
    return true;
}

__declspec(naked) static void Hooked_4B6550() {
    __asm {
        cmp esi, 0x10000
        jb skip_wait
        cmp esi, 0x7FFE0000
        jb check_esi_content
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je skip_wait
        cmp esi, 0x80010000
        jb skip_wait
        cmp esi, 0xFFFE0000
        jae skip_wait

    check_esi_content:
        cmp dword ptr [esi], 0xFFFFFFFF
        je skip_wait

        mov eax, [esi + 0x40]
        test eax, eax
        jz skip_wait

        cmp eax, 0x10000
        jb sanitize_and_skip
        cmp eax, 0x7FFE0000
        jb check_s_waiting
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je sanitize_and_skip
        cmp eax, 0x80010000
        jb sanitize_and_skip
        cmp eax, 0xFFFE0000
        jae sanitize_and_skip

    check_s_waiting:
        // Only skip if an active wait is ACTUALLY in its polling loop (s_asyncWaitObject at 0x00B4A204 != 0)
        // AND s_waiting != 0. If s_asyncWaitObject is null, no wait is polling!
        mov edx, 0x00B4A204
        cmp dword ptr [edx], 0
        jz proceed_to_orig
        mov edx, 0x00B4A26C
        cmp dword ptr [edx], 0
        jnz reentrant_skip

    proceed_to_orig:
        jmp dword ptr [g_orig4B6550]

    sanitize_and_skip:
        mov dword ptr [esi + 0x40], 0
        cmp dword ptr [esi + 0x5C], 0x10000
        jb check_gxtex
        cmp dword ptr [esi + 0x5C], 0x7FFE0000
        jb check_gxtex
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je zero_atlas
        cmp dword ptr [esi + 0x5C], 0x80010000
        jb zero_atlas
        cmp dword ptr [esi + 0x5C], 0xFFFE0000
        jb check_gxtex
    zero_atlas:
        mov dword ptr [esi + 0x5C], 0
    check_gxtex:
        cmp dword ptr [esi + 0x44], 0x10000
        jb finish_sanitize
        cmp dword ptr [esi + 0x44], 0x7FFE0000
        jb finish_sanitize
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je zero_gxtex
        cmp dword ptr [esi + 0x44], 0x80010000
        jb zero_gxtex
        cmp dword ptr [esi + 0x44], 0xFFFE0000
        jb finish_sanitize
    zero_gxtex:
        mov dword ptr [esi + 0x44], 0
    finish_sanitize:
        inc dword ptr [g_asyncWaitSuppressed]
        ret

    reentrant_skip:
        inc dword ptr [g_asyncReentrantWaitSuppressed]

    skip_wait:
        ret
    }
}

// ----------------------------------------------------------------------------
// TextureGetGxTex guard (sub_4B6CB0 @ 0x004B6CB0)
//
// In stock 3.3.5a build 12340, TextureGetGxTex (sub_4B6CB0) is called from M2
// rendering, terrain rendering, spell effects, and UI batch rendering (sub_484B00)
// to obtain the active CGxTex surface pointer.
//
// When a texture object has been freed, uninitialized, or filled with 0xFFFFFFFF,
// or when an asynchronous texture load failed leaving wild pointers in m_atlas
// (+0x5C) or m_gxTex (+0x44), the stock client executes:
//     0x004B6D1F: mov ecx, [esi+5Ch]
//     0x004B6D22: test ecx, ecx
//     0x004B6D24: jz short loc_4B6D3C
//     0x004B6D26: test byte ptr [ecx+2], 1  ; CRASH if ecx is 0xFFFFFFFF!
//
// If ecx is 0xFFFFFFFF, reading [ecx+2] accesses address 0x00000001, crashing
// with ERROR #132 (0x85100084) Fatal Exception (ACCESS_VIOLATION at 0023:004B6D26).
//
// Guard:
// - If texture is null or outside valid address space [0x10000, 0xFFFE0000), return nullptr.
// - If texture is marked freed/dead (*(const uintptr_t*)texture == 0xFFFFFFFF), return nullptr.
// - Sanitize wild/0xFFFFFFFF fields in m_asyncObject (+0x40), m_atlas (+0x5C), and
//   m_gxTex (+0x44) to nullptr.
// - Forwards safely to original sub_4B6CB0 (which gracefully returns nullptr to
//   the caller without crashing).
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget4B6CB0 = 0x004B6CB0;
static const unsigned char kExpectedPrologue4B6CB0[9] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x85, 0xF6
};

typedef void* (__cdecl* TextureGetGxTex_fn)(void* texture, int wait, void* status);
static TextureGetGxTex_fn g_orig4B6CB0 = nullptr;
static volatile LONG g_textureWildSanitized = 0;
static bool g_guard4B6CB0Active = false;

__declspec(safebuffers) static void* __cdecl Hooked_4B6CB0(void* texture, int wait, void* status) {
    if (!texture || !IsValidUserPointer((uintptr_t)texture)) {
        return nullptr;
    }

    if (*(const uintptr_t*)texture == 0xFFFFFFFF) {
        InterlockedIncrement(&g_textureWildSanitized);
        return nullptr;
    }

    uintptr_t* pAsync = (uintptr_t*)((uintptr_t)texture + 0x40);
    uintptr_t* pGxTex = (uintptr_t*)((uintptr_t)texture + 0x44);
    uintptr_t* pAtlas = (uintptr_t*)((uintptr_t)texture + 0x5C);

    bool sanitized = false;
    if (*pAsync && !IsValidUserPointer(*pAsync)) {
        *pAsync = 0;
        sanitized = true;
    }
    if (*pAtlas && !IsValidUserPointer(*pAtlas)) {
        *pAtlas = 0;
        sanitized = true;
    }
    if (*pGxTex && !IsValidUserPointer(*pGxTex)) {
        *pGxTex = 0;
        sanitized = true;
    }
    if (sanitized) {
        InterlockedIncrement(&g_textureWildSanitized);
    }

    // In stock sub_4B6CB0 (TextureGetGxTex), when wait == 2 and m_gxTex (+0x44) is 0:
    // the client executes:
    //     0x004B6D5E: mov eax, [esi+40h]
    //     0x004B6D61: cmp byte ptr [eax+24h], 0
    // If m_atlas (+0x5C) is non-null, the texture is an atlas/sub-texture (e.g. action bar / spell icons)
    // and the client retrieves the surface from m_atlas.
    // Only when m_atlas, m_gxTex, AND m_asyncObject are all null does the client crash.
    if (*pAtlas == 0 && *pGxTex == 0 && *pAsync == 0) {
        return nullptr;
    }

    return g_orig4B6CB0(texture, wait, status);
}

// ----------------------------------------------------------------------------
// TextureAsyncCallback guard (sub_4B7E80 @ 0x004B7E80)
//
// In stock 3.3.5a build 12340, sub_4B7E80 is invoked by AsyncFileReadThread
// (sub_4B9B20) upon async texture file decompression. It loads:
//     0x004B7E87: mov eax, [esi+40h]   ; m_asyncObject
//     0x004B7E8A: mov ecx, [eax+4]     ; buffer pointer
// If m_asyncObject is null or wild, reading [eax+4] crashes with ACCESS_VIOLATION
// at 0x004B7E8A reading from 0x00000004.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget4B7E80 = 0x004B7E80;
static const unsigned char kExpectedPrologue4B7E80[8] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x8B
};

typedef int (__cdecl* TextureAsyncCallback_fn)(void* texture);
static TextureAsyncCallback_fn g_orig4B7E80 = nullptr;
static volatile LONG g_textureAsyncNullSuppressed = 0;
static bool g_guard4B7E80Active = false;

__declspec(safebuffers) static int __cdecl Hooked_4B7E80(void* texture) {
    if (!texture || !IsValidUserPointer((uintptr_t)texture)) {
        return 0;
    }

    void* asyncObj = *(void**)((char*)texture + 0x40);
    if (!asyncObj || !IsValidUserPointer((uintptr_t)asyncObj)) {
        InterlockedIncrement(&g_textureAsyncNullSuppressed);
        return 0;
    }

    void* buffer = *(void**)((char*)asyncObj + 0x04);
    if (!buffer || !IsValidUserPointer((uintptr_t)buffer)) {
        InterlockedIncrement(&g_textureAsyncNullSuppressed);
        return 0;
    }

    return g_orig4B7E80(texture);
}

// ----------------------------------------------------------------------------
// CGxDeviceD3d::ITexCreate guard (sub_6A2C00 @ 0x006A2C00)
//
// In stock 3.3.5a build 12340, CGxDeviceD3d::ITexCreate indexes s_GxTexFormatToUse
// (dword_AD8EF0) with texId->m_format (offset +0x24):
//     0x006A2C26: mov ecx, [esi+24h]
//     0x006A2C29: mov eax, dword_AD8EF0[ecx*4]
//
// s_GxTexFormatToUse has exactly 13 entries [0..12]. If texId is not a valid
// pointer (e.g. an IEEE-754 float coordinate leaked during UI batch rendering
// or modified by third-party DLLs like AwesomeWotlkLib.dll), or if m_format >= 13,
// the client calculates an out-of-bounds address (e.g. 0xB8E298C4) and crashes
// with ERROR #132 (ACCESS_VIOLATION).
//
// Guard validates this_device, validates texId as a valid user-space pointer,
// checks for 0xFFFFFFFF freed marker, and verifies m_format < 13.
// If any check fails, increments g_texCreateWildSanitized and safely returns 0.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget6A2C00 = 0x006A2C00;
static const unsigned char kExpectedPrologue6A2C00[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x53, 0x56
};

typedef int (__fastcall* ITexCreate_fn)(void* this_device, void* dummy_edx, void* texId);
static ITexCreate_fn g_orig6A2C00 = nullptr;
static volatile LONG g_texCreateWildSanitized = 0;
static bool g_guard6A2C00Active = false;

__declspec(safebuffers) static int __fastcall Hooked_6A2C00(void* this_device, void* dummy_edx, void* texId) {
    if (!this_device || !IsValidUserPointer((uintptr_t)this_device)) {
        return 0;
    }
    if (!texId || !IsValidUserPointer((uintptr_t)texId)) {
        InterlockedIncrement(&g_texCreateWildSanitized);
        return 0;
    }
    if (*(const uintptr_t*)texId == 0xFFFFFFFF) {
        InterlockedIncrement(&g_texCreateWildSanitized);
        return 0;
    }
    uint32_t format = *(const uint32_t*)((uintptr_t)texId + 0x24);
    if (format >= 13) {
        InterlockedIncrement(&g_texCreateWildSanitized);
        return 0;
    }
    return g_orig6A2C00(this_device, dummy_edx, texId);
}

// ----------------------------------------------------------------------------
// CGxDeviceD3d::ITexUpdate guard (sub_6A31E0 @ 0x006A31E0)
//
// In stock 3.3.5a build 12340, CGxDeviceD3d::ITexUpdate dereferences texId at
// offsets +91 (+0x5B) and +56 (+0x38) before invoking ITexCreate:
//     0x006A3206: cmp byte ptr [esi+5Bh], 0
// If texId is not a valid user pointer or points to freed/float garbage,
// accessing offsets +0x5B or +0x38 faults immediately.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget6A31E0 = 0x006A31E0;
static const unsigned char kExpectedPrologue6A31E0[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x53, 0x33,
    0xDB, 0x39, 0x99, 0x58, 0x0F, 0x00, 0x00, 0x89
};

typedef BOOL (__fastcall* ITexUpdate_fn)(void* this_device, void* dummy_edx, void* texId, const void* a3, const void* a4, int a5, int a6);
static ITexUpdate_fn g_orig6A31E0 = nullptr;
static volatile LONG g_texUpdateWildSanitized = 0;
static bool g_guard6A31E0Active = false;

__declspec(safebuffers) static BOOL __fastcall Hooked_6A31E0(void* this_device, void* dummy_edx, void* texId, const void* a3, const void* a4, int a5, int a6) {
    if (!this_device || !IsValidUserPointer((uintptr_t)this_device)) {
        return 0;
    }
    if (!texId || !IsValidUserPointer((uintptr_t)texId)) {
        InterlockedIncrement(&g_texUpdateWildSanitized);
        return 0;
    }
    if (*(const uintptr_t*)texId == 0xFFFFFFFF) {
        InterlockedIncrement(&g_texUpdateWildSanitized);
        return 0;
    }
    uint32_t format = *(const uint32_t*)((uintptr_t)texId + 0x24);
    if (format >= 13) {
        InterlockedIncrement(&g_texUpdateWildSanitized);
        return 0;
    }
    return g_orig6A31E0(this_device, dummy_edx, texId, a3, a4, a5, a6);
}


// ----------------------------------------------------------------------------
// AsyncFileReadWait guard (sub_4BA060 @ 0x004BA060)
//
// In stock 3.3.5a build 12340, AsyncFileReadWait (sub_4BA060) waits for pending
// asynchronous file reads (textures, models, WMOs, sounds, DBCs).
// It maintains a global re-entrancy flag s_waiting (at 0x00B4A26C) and a single
// global request pointer dword_B4A204.
// If sub_4BA060 is called while s_waiting is already non-zero, it executes:
//     sub_772AA0("AsyncFileReadWait(): s_waiting != FALSE", ...);
// immediately terminating the process with ERROR #134 (0x85100086) Fatal Condition.
//
// Guard:
// - If req is null or outside valid address space [0x10000, 0xFFFE0000), return 0.
// - If req is already completed (*((const uint8_t*)req + 0x21) != 0), return 1.
// - If s_waiting != 0 (*(const uint32_t*)0x00B4A26C != 0), a wait is already active!
//   Calling the client's sub_4BA060 would trigger the fatal Error #134 condition.
//   We suppress the recursive wait, increment g_fileWaitReentrantSuppressed, and
//   return immediately.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget4BA060 = 0x004BA060;
static const unsigned char kExpectedPrologue4BA060[9] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00
};

typedef int (__cdecl* AsyncFileReadWait_fn)(void* req);
static AsyncFileReadWait_fn g_orig4BA060 = nullptr;
static volatile LONG g_fileWaitReentrantSuppressed = 0;
static volatile LONG g_fileWaitCorruptedSuppressed = 0;
static bool g_guard4BA060Active = false;

__declspec(safebuffers) static int __cdecl Hooked_4BA060(void* req) {
    if (!req || !IsValidUserPointer((uintptr_t)req)) {
        return 0;
    }

    typedef int (__cdecl* PollHandler_fn)(int, int);
    const PollHandler_fn PollHandler = (PollHandler_fn)0x004B9B20;

    // If s_waiting (0x00B4A26C) is non-zero BUT s_asyncWaitObject (0x00B4A204) is NULL,
    // then s_waiting was leaked by Blizzard's early-return bug at 0x004BA097! Heal it immediately.
    if (*(const uint32_t*)0x00B4A204 == 0 && *(uint32_t*)0x00B4A26C != 0) {
        *(uint32_t*)0x00B4A26C = 0;
    }

    // Offset +0x21 is isComplete / m_isProcessed in CAsyncObject
    if (*((const uint8_t*)req + 0x21) != 0) {
        // Even if the I/O marked this request complete (+0x21 != 0), its completion callback
        // (such as CMapObj's sub_7D8050) may not have been dispatched yet. Callers like sub_7AE1C0
        // loop on `for (e = mapObj[119]; e; e = mapObj[119]) sub_4BA060(e);` waiting for the
        // completion callback to unlink e and zero mapObj[119]. Pump completions so the callback
        // runs and unlinks the request.
        PollHandler(0, 0);
        return 1;
    }

    // If s_asyncWaitObject != 0 (genuinely re-entrant wait during another wait's callback):
    if (*(const uint32_t*)0x00B4A26C != 0 && *(const uint32_t*)0x00B4A204 != 0) {
        InterlockedIncrement(&g_fileWaitReentrantSuppressed);

        uint32_t spinCount = 0;
        while (*((const uint8_t*)req + 0x21) == 0 && spinCount < 10000) {
            PollHandler(0, 0);
            if (*((const uint8_t*)req + 0x21) != 0) {
                break;
            }
            Sleep(1);
            ++spinCount;
        }

        // Run poll handler once more to ensure completion callback was dispatched
        PollHandler(0, 0);
        return *((const uint8_t*)req + 0x21) != 0 ? 1 : 0;
    }

    // Normal path: not re-entrant. Run original function.
    int result = g_orig4BA060(req);

    // On exit from client sub_4BA060:
    // If client exited through 0x004BA09D without decrementing s_waiting, heal it!
    if (*(const uint32_t*)0x00B4A204 == 0 && *(uint32_t*)0x00B4A26C != 0) {
        *(uint32_t*)0x00B4A26C = 0;
    }

    // Ensure any completed callbacks are pumped so owner lists are updated immediately
    PollHandler(0, 0);

    return result;
}

// ----------------------------------------------------------------------------
// TSList::PushFront / Link guard (sub_7B5020 @ 0x007B5020)
//
// In stock 3.3.5a build 12340, sub_7B5020 links a node into a circular intrusive
// doubly-linked list (TSList / TSLink). ECX contains 'this' (the list head).
// At 0x007B502D, it executes:
//     mov eax, [ecx]
// If ECX is wild (< 0x10000 or >= 0xFFFE0000, such as 0x51 when a corrupted or
// recycled object's [obj+0x18] (0x49) + 8 is passed), it immediately crashes with
// ACCESS_VIOLATION reading from 0x00000051.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget7B5020 = 0x007B5020;
static const unsigned char kExpectedPrologue7B5020[8] = {
    0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x85
};

static void* g_orig7B5020 = nullptr;
static volatile LONG g_listLinkNullSuppressed = 0;
static bool g_guard7B5020Active = false;

__declspec(naked) static void Hooked_7B5020() {
    __asm {
        cmp ecx, 0x10000
        jb invalid_list
        cmp ecx, 0x7FFE0000
        jb valid_list
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je invalid_list
        cmp ecx, 0x80010000
        jb invalid_list
        cmp ecx, 0xFFFE0000
        jae invalid_list

    valid_list:
        jmp dword ptr [g_orig7B5020]

    invalid_list:
        inc dword ptr [g_listLinkNullSuppressed]
        xor eax, eax
        ret 4
    }
}

// ----------------------------------------------------------------------------
// WorldFrame HandleSpriteTrack guard (sub_4F5980 @ 0x004F5980)
//
// In stock 3.3.5a build 12340, CGWorldFrame::HandleSpriteTrack is a __thiscall
// method invoked by sub_4F66C0, sub_4F8190, and sub_4FA040 during frame rendering
// and scene target/sprite updates.
//
// When third-party libraries (such as AwesomeWotlkLib around sub_4F66C0) detour
// frame tracking, ECX can be clobbered or passed an uninitialized / invalid pointer
// (e.g. ECX = 8 [typeMask] or ECX = 0x10787 [player GUID low-part]).
// sub_4F5980 blindly loads ESI = ECX and executes:
//     mov eax, [esi+2C8h]  (crash 0x004F598D if read access violation)
//     ...
//     mov [esi+2C8h], edi  (crash 0x004F59C8 if write access violation)
//
// Guard:
// CGWorldFrame is a singleton instance whose canonical address is stored at
// s_worldFrame (0x00B7436C). If ECX is wild (< 0x10000 or >= 0xFFFE0000) or does
// not match s_worldFrame, we restore ECX to s_worldFrame. If s_worldFrame is null,
// we safely return 0 with ret 8. Pure naked ASM, zero /GS cookies, zero SEH.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget4F5980 = 0x004F5980;
static const unsigned char kExpectedPrologue4F5980[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x53, 0x8B
};

static void* g_orig4F5980 = nullptr;
static volatile LONG g_worldFrameRecovered  = 0;
static volatile LONG g_worldFrameSuppressed = 0;
static bool g_guard4F5980Active = false;

__declspec(naked) static void Hooked_4F5980() {
    __asm {
        // Validate ecx (this pointer to CGWorldFrame)
        cmp ecx, 0x10000
        jb recover_frame
        cmp ecx, 0xFFFE0000
        jae recover_frame

        // Check if ecx is already the canonical singleton s_worldFrame (0x00B7436C)
        mov edx, 0x00B7436C
        mov eax, dword ptr [edx]
        test eax, eax
        jz call_original
        cmp ecx, eax
        je call_original

    recover_frame:
        mov edx, 0x00B7436C
        mov eax, dword ptr [edx]
        test eax, eax
        jz suppress_call
        mov ecx, eax
        inc dword ptr [g_worldFrameRecovered]
        jmp call_original

    suppress_call:
        inc dword ptr [g_worldFrameSuppressed]
        xor eax, eax
        ret 8

    call_original:
        jmp dword ptr [g_orig4F5980]
    }
}

// ----------------------------------------------------------------------------
// FreeType Glyph Rasterizer guard (sub_992DA0 @ 0x00992DA0)
//
// In stock 3.3.5a build 12340, sub_992DA0 dispatches TrueType/bitmap glyph rendering.
// When rendering font glyphs (such as Cyrillic / custom UI fonts loaded when
// entering a Battleground or displaying addon scoreboard frames), the client
// evaluates whether an outline rasterizer exists or falls back to the default
// rasterizer callback at:
//     0x00992E49: mov ecx, [ecx+14h]   ; rasterizer driver/class table
//     0x00992E4C: mov ecx, [ecx+50h]   ; raster_render callback function pointer
//     0x00992E4F: push ebx             ; load flags (e.g. 0x208A)
//     0x00992E50: push edx             ; glyph index (e.g. 0x6C)
//     0x00992E51: push eax             ; face metrics/size pointer
//     0x00992E52: push esi             ; glyph slot pointer
//     0x00992E53: call ecx             ; UNGUARDED call! If ecx is null -> crash at 0x0
//
// If a font format does not provide a registered raster_render callback or the
// driver table has a null callback pointer, ecx is 0x00000000. Stock WoW executes
// "call ecx" unconditionally without checking "test ecx, ecx", immediately crashing
// the process with ERROR #132 (0x85100084) Fatal Exception (ACCESS_VIOLATION at 0023:00000000).
//
// Guard:
// - Check if face object, size, and slot pointers are valid; if not, return 35 (0x23).
// - Inspect the driver table at [face+0x60].
// - If the outline renderer path will be taken, verify the outline callback [altTable+4].
// - If the rasterizer path will be taken, verify the raster_render callback [rTable+0x50].
// - If the selected callback is null or outside valid address space [0x10000, 0xFFFE0000),
//   increment g_nullGlyphRasterizerSuppressed and return 35 (FT_Err_Cannot_Render_Glyph /
//   unsupported format error code in FreeType).
// - Callers (sub_6C8C10, GxuLoadGlyph, etc.) receive non-zero error code and return
//   false to the UI/rendering subsystem, safely rendering the fallback glyph instead
//   of crashing the game client.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget992DA0 = 0x00992DA0;
static const unsigned char kExpectedPrologue992DA0[6] = {
    0x55, 0x8B, 0xEC, 0x51, 0x56, 0x57
};

typedef int (__cdecl* GlyphRasterize_fn)(int a1, unsigned int a2, unsigned int a3);
static GlyphRasterize_fn g_orig992DA0 = nullptr;
static volatile LONG g_nullGlyphRasterizerSuppressed = 0;
static bool g_guard992DA0Active = false;

static inline bool IsValidCodePointer(const void* ptr) {
    if (!ptr) return false;
    uintptr_t fnAddr = (uintptr_t)ptr;
    if (fnAddr < 0x10000 || fnAddr >= 0xFFFE0000) return false;
    if (!g_optHighPlacementClientActive && fnAddr >= 0x7FFE0000) return false;

    // Fast path: wow.exe code section (.text) is always committed executable code:
    if (fnAddr >= 0x00401000 && fnAddr < 0x009DF000) {
        return true;
    }

    // Fast path: wow_optimize.dll module code:
    static uintptr_t s_dllBase = 0;
    static uintptr_t s_dllEnd = 0;
    if (s_dllBase == 0) {
        HMODULE hSelf = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&IsValidCodePointer, &hSelf);
        if (hSelf) {
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)hSelf;
            const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)hSelf + dos->e_lfanew);
            s_dllBase = (uintptr_t)hSelf;
            s_dllEnd = s_dllBase + nt->OptionalHeader.SizeOfImage;
        }
    }
    if (fnAddr >= s_dllBase && fnAddr < s_dllEnd) {
        return true;
    }

    struct CodeRegionCache {
        uintptr_t start;
        uintptr_t end;
    };
    static thread_local CodeRegionCache s_codeCache[4] = {};
    static thread_local size_t s_codeIndex = 0;

    for (size_t i = 0; i < 4; ++i) {
        if (fnAddr >= s_codeCache[i].start && fnAddr < s_codeCache[i].end) {
            return true;
        }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD kExecFlags = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kExecFlags) == 0) {
        return false;
    }

    s_codeCache[s_codeIndex & 3] = { (uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.BaseAddress + mbi.RegionSize };
    ++s_codeIndex;
    return true;
}

static inline bool IsValidReadablePointer(const void* ptr, size_t size) {
    if (!ptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 0x10000 || addr >= 0xFFFE0000) return false;
    if (addr + size < addr || addr + size >= 0xFFFE0000) return false;
    if (!g_optHighPlacementClientActive && (addr + size) > 0x7FFE0000) return false;

    // Fast path: wow.exe static data / bss
    if (addr >= 0x00401000 && (addr + size) <= 0x00E00000) {
        return true;
    }

    struct RegionCache {
        uintptr_t start;
        uintptr_t end;
    };
    static thread_local RegionCache s_regionCache[4] = {};
    static thread_local size_t s_cacheIndex = 0;

    for (size_t i = 0; i < 4; ++i) {
        if (addr >= s_regionCache[i].start && (addr + size) <= s_regionCache[i].end) {
            return true;
        }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < addr + size) {
        return false;
    }

    s_regionCache[s_cacheIndex & 3] = { (uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.BaseAddress + mbi.RegionSize };
    ++s_cacheIndex;
    return true;
}

__declspec(safebuffers) static int __cdecl Hooked_992DA0(int a1, unsigned int a2, unsigned int a3) {
    // Fast path: MRU glyph rasterizer structure cache
    static thread_local int s_lastA1 = 0;
    static thread_local uintptr_t s_lastSizeObj = 0;
    static thread_local uintptr_t s_lastSlotObj = 0;
    static thread_local uintptr_t s_lastDriver = 0;
    static thread_local uintptr_t s_lastTable = 0;
    static thread_local void* s_lastRenderFn = nullptr;

    if (a1 == s_lastA1 && a1 != 0) {
        uintptr_t sizeObj = *(uintptr_t*)(a1 + 0x58);
        uintptr_t slotObj = *(uintptr_t*)(a1 + 0x54);
        uintptr_t driver  = *(uintptr_t*)(a1 + 0x60);
        if (sizeObj == s_lastSizeObj && slotObj == s_lastSlotObj && driver == s_lastDriver && driver != 0) {
            uintptr_t rTable = *(uintptr_t*)(driver + 0x14);
            if (rTable == s_lastTable && rTable != 0) {
                void* renderFn = *(void**)(rTable + 0x50);
                if (renderFn == s_lastRenderFn && renderFn != nullptr) {
                    return g_orig992DA0(a1, a2, a3);
                }
            }
        }
    }

    if (!IsValidReadablePointer((const void*)a1, 0x64)) {
        return 35;
    }

    uintptr_t sizeObj = *(uintptr_t*)(a1 + 0x58);
    uintptr_t slotObj = *(uintptr_t*)(a1 + 0x54);
    if (!IsValidReadablePointer((const void*)sizeObj, 16) ||
        !IsValidReadablePointer((const void*)slotObj, 16)) {
        return 35;
    }

    uintptr_t driver = *(uintptr_t*)(a1 + 0x60);
    if (!IsValidReadablePointer((const void*)driver, 0x18)) {
        InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
        return 35;
    }

    // Determine whether Path A (outline renderer) or Path B (raster_render) is selected
    bool pathA = false;
    uintptr_t pClazz = *(uintptr_t*)(driver + 4);
    if (IsValidReadablePointer((const void*)pClazz, 0x60)) {
        uintptr_t v8 = *(uintptr_t*)(pClazz + 0x5C);
        if (v8 != 0 && (a3 & 3) == 0) {
            uintptr_t pV7_0 = *(uintptr_t*)driver;
            if (IsValidReadablePointer((const void*)pV7_0, 4)) {
                uintptr_t v9 = *(uintptr_t*)pV7_0;
                if ((v9 & 0x100) != 0 && (v9 & 0x200) == 0 &&
                    (((v9 & 0x400) == 0) || ((a3 & 0x20) != 0))) {
                    pathA = true;
                    if (!IsValidReadablePointer((const void*)v8, 4)) {
                        InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
                        return 35;
                    }
                    uintptr_t altEcX = *(uintptr_t*)v8;
                    if (!IsValidReadablePointer((const void*)altEcX, 0x18)) {
                        InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
                        return 35;
                    }
                    uintptr_t altTable = *(uintptr_t*)(altEcX + 0x14);
                    if (!IsValidReadablePointer((const void*)altTable, 8)) {
                        InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
                        return 35;
                    }
                    void* altRenderFn = *(void**)(altTable + 4);
                    if (!IsValidCodePointer(altRenderFn)) {
                        InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
                        return 35;
                    }
                }
            }
        }
    }

    if (!pathA) {
        uintptr_t rTable = *(uintptr_t*)(driver + 0x14);
        if (!IsValidReadablePointer((const void*)rTable, 0x54)) {
            InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
            return 35;
        }
        void* renderFn = *(void**)(rTable + 0x50);
        if (!IsValidCodePointer(renderFn)) {
            InterlockedIncrement(&g_nullGlyphRasterizerSuppressed);
            return 35;
        }

        s_lastA1 = a1;
        s_lastSizeObj = sizeObj;
        s_lastSlotObj = slotObj;
        s_lastDriver = driver;
        s_lastTable = rTable;
        s_lastRenderFn = renderFn;
    }

    return g_orig992DA0(a1, a2, a3);
}

// ----------------------------------------------------------------------------
// M2 GetSequenceDuration / SequenceData guard (sub_8267E0 @ 0x008267E0)
//
// In stock 3.3.5a build 12340, sub_8267E0 is a __thiscall method called from
// CSimpleModel::UpdateAnimation (sub_5971B0), character model updates, and UI model
// rendering to retrieve the duration and animation record for a sequence index.
// ECX contains the model instance pointer ('this').
// At 0x00826828:
//     mov edx, [esi+2Ch]   ; CM2Shared / model definition
//     mov ecx, [edx+150h]  ; M2Header sequence chunk
// At 0x00826856:
//     mov ecx, [esi+94h]   ; m_animData array (+148)
//     imul eax, 0ACh       ; 172 bytes per sequence record
//     mov eax, [eax+ecx+90h] ; CRASH at 0x00826862 reading 0x00000090 if ecx is null!
//
// Root Cause:
// In stock 3.3.5a, when a model is uninitialized (bit 0 of [this+16] is 0), sub_8267E0
// first checks and waits for pending file reads (sub_4BA060) and invokes sub_832EA0(this)
// to allocate and load m_animData. A prior guard returned 0 before sub_832EA0 could run,
// preventing uninitialized models (such as 3D portrait frames) from ever initializing.
// Furthermore, if a model has no animData or async loading has not populated it yet,
// dereferencing [0 + 0x90] crashed with Error #132 ACCESS_VIOLATION at 0x00826862.
//
// Guard:
// If uninitialized, executes the engine initialization path (sub_4BA060 and sub_832EA0).
// Then safely resolves the sequence record, verifying that animData [this+0x94] is non-null
// before reading sequence duration at +0x90. If null, increments g_modelAnimNullSuppressed
// and returns 0 without crashing. Zero /GS cookies, zero SEH frames.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget8267E0 = 0x008267E0;
static const unsigned char kExpectedPrologue8267E0[6] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1
};

typedef int (__fastcall *ModelGetAnimDuration_fn)(void* this_ptr, void* dummy_edx, unsigned int a2);
static ModelGetAnimDuration_fn g_orig8267E0 = nullptr;
static volatile LONG g_modelAnimNullSuppressed = 0;
static bool g_guard8267E0Active = false;

__declspec(safebuffers)
static int __fastcall Hooked_8267E0(void* this_ptr, void* /*dummy_edx*/, unsigned int a2) {
    if (!this_ptr || !IsValidUserPointer(reinterpret_cast<uintptr_t>(this_ptr))) {
        return 0;
    }

    const uintptr_t pModel = reinterpret_cast<uintptr_t>(this_ptr);

    // If model is uninitialized (bit 0 of +0x10 is clear), run engine initialization:
    if ((*reinterpret_cast<const uint8_t*>(pModel + 0x10) & 1) == 0) {
        const uintptr_t shared = *reinterpret_cast<const uintptr_t*>(pModel + 0x2C);
        if (shared && IsValidUserPointer(shared)) {
            typedef int (__cdecl *AsyncWaitFn)(void*);
            const AsyncWaitFn asyncWait = reinterpret_cast<AsyncWaitFn>(0x004BA060);

            void* req1 = *reinterpret_cast<void**>(shared + 0x0C);
            if (req1 && IsValidUserPointer(reinterpret_cast<uintptr_t>(req1))) {
                asyncWait(req1);
            }

            const uintptr_t shared2 = *reinterpret_cast<const uintptr_t*>(pModel + 0x2C);
            if (shared2 && IsValidUserPointer(shared2)) {
                void* req2 = *reinterpret_cast<void**>(shared2 + 0x0C);
                if (req2 && IsValidUserPointer(reinterpret_cast<uintptr_t>(req2))) {
                    asyncWait(req2);
                }
            }
        }

        if (*reinterpret_cast<const uint8_t*>(pModel + 4) & 0x20) {
            typedef int (__thiscall *ModelInitFn)(void*);
            reinterpret_cast<ModelInitFn>(0x00832EA0)(this_ptr);
        }
    }

    const uintptr_t shared = *reinterpret_cast<const uintptr_t*>(pModel + 0x2C);
    if (!shared || !IsValidUserPointer(shared)) {
        return 0;
    }

    const uintptr_t seqChunk = *reinterpret_cast<const uintptr_t*>(shared + 0x150);
    if (!seqChunk || !IsValidUserPointer(seqChunk)) {
        return 0;
    }

    uint16_t seqIndex = 0;
    if (a2 == 0xFFFFFFFF) {
        seqIndex = 0;
    } else {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(seqChunk + 0x34);
        if (a2 < count) {
            const uintptr_t indices = *reinterpret_cast<const uintptr_t*>(seqChunk + 0x38);
            if (!indices || !IsValidUserPointer(indices)) {
                return 0;
            }
            seqIndex = *reinterpret_cast<const uint16_t*>(indices + 2 * a2);
        } else {
            seqIndex = 0xFFFF;
        }
    }

    const uint32_t maxSeq = *reinterpret_cast<const uint32_t*>(seqChunk + 0x2C);
    if (static_cast<uint32_t>(seqIndex) < maxSeq) {
        const uintptr_t animData = *reinterpret_cast<const uintptr_t*>(pModel + 0x94);
        if (!animData || !IsValidUserPointer(animData)) {
            InterlockedIncrement(&g_modelAnimNullSuppressed);
            return 0;
        }
        return *reinterpret_cast<const int*>(animData + 172 * static_cast<uint32_t>(seqIndex) + 0x90);
    }

    return 0;
}


// ----------------------------------------------------------------------------
// DeviceSetViewport guard (sub_6848A0 @ 0x006848A0)
//
// In stock 3.3.5a build 12340:
// sub_6848A0 is a __thiscall method on CGxDevice (this in ECX, retn 0Ch):
//     int __thiscall sub_6848A0(CGxDevice* this, void* target, const int* rect, int flush)
//
// On function entry:
//     0x006848A0: push ebp
//     0x006848A1: mov ebp, esp
//     0x006848A3: mov eax, [ebp+8]    ; target
//     0x006848A6: mov edx, [ebp+0Ch]  ; rect
//     0x006848A9: mov byte ptr [eax+5Ah], 1  ; <-- CRASH if target is null!
//
// Root Cause:
// When entering the world, UI frame layouts are refreshed. The ColorSelect /
// ValueTexture widget updates via sub_96E030. It queries the texture surface via
// sub_481720 -> sub_4B6CB0(wait=1). When the texture is still loading asynchronously
// or missing, sub_4B6CB0 returns 0. sub_96E030 does not check for NULL and passes
// 0 as target to sub_681F20, which passes 0 to sub_6848A0. sub_6848A0 blindly
// dereferences [0 + 0x5A] = 1, crashing with ACCESS_VIOLATION (0xC0000005) at
// 0x006848A9 writing to 0x0000005A on the main render thread.
//
// Furthermore, sub_6848A0 unconditionally reads [rect] and [rect+8], and if flush is
// non-zero, dereferences [this] (ECX) to invoke vtable[0].
//
// Guard:
// Naked ASM hook on sub_6848A0 that validates target ([esp+4]), this (ECX), and
// rect ([esp+8]). If any pointer is null or invalid, increment
// g_nullViewportTargetSuppressed, set eax = 0, and return with ret 0x0C.
// Zero /GS cookies, zero SEH frames.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget6848A0 = 0x006848A0;
static const unsigned char kExpectedPrologue6848A0[9] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x55, 0x0C
};

static void* g_orig6848A0 = nullptr;
static volatile LONG g_nullViewportTargetSuppressed = 0;
static bool g_guard6848A0Active = false;

__declspec(naked) static void Hooked_6848A0() {
    __asm {
        // [esp+4] = target
        mov eax, [esp+4]
        cmp eax, 0x10000
        jb invalid_viewport
        cmp eax, 0x7FFE0000
        jb check_device
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je invalid_viewport
        cmp eax, 0x80010000
        jb invalid_viewport
        cmp eax, 0xFFFE0000
        jae invalid_viewport

    check_device:
        // ecx = this (CGxDevice)
        cmp ecx, 0x10000
        jb invalid_viewport
        cmp ecx, 0x7FFE0000
        jb check_rect
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je invalid_viewport
        cmp ecx, 0x80010000
        jb invalid_viewport
        cmp ecx, 0xFFFE0000
        jae invalid_viewport

    check_rect:
        // [esp+8] = rect
        mov edx, [esp+8]
        cmp edx, 0x10000
        jb invalid_viewport
        cmp edx, 0x7FFE0000
        jb valid_viewport
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je invalid_viewport
        cmp edx, 0x80010000
        jb invalid_viewport
        cmp edx, 0xFFFE0000
        jae invalid_viewport

    valid_viewport:
        jmp dword ptr [g_orig6848A0]

    invalid_viewport:
        inc dword ptr [g_nullViewportTargetSuppressed]
        xor eax, eax
        ret 0x0C
    }
}

// ----------------------------------------------------------------------------
// DeviceClearTarget guard (sub_683320 @ 0x00683320)
//
// In stock 3.3.5a build 12340:
// sub_683320 is a __thiscall method on CGxDevice (this in ECX, retn 4):
//     void __thiscall sub_683320(CGxDevice* this, void* target)
//
// On function entry:
//     0x00683320: push ebp
//     0x00683321: mov ebp, esp
//     0x00683323: mov eax, [ebp+8]   ; target
//     0x00683326: xor ecx, ecx
//     0x00683328: mov [eax+34h], ecx ; <-- CRASH if target is null!
//     0x0068332B: mov [eax+30h], ecx
//     0x0068332E: pop ebp
//     0x0068332F: retn 4
//
// Root Cause:
// During /reload or UI frame teardown, sub_4EFCA0 iterates and detaches textures
// from their render targets. It queries the surface via TextureGetGxTex:
//     sub_4B6CB0(texture, 1, 0)
// If the texture was already unloaded or unbacked, sub_4B6CB0 returns null (0).
// sub_4EFCA0 passes this 0 without checking to sub_6813F0, which calls
// sub_683320(0). sub_683320 unconditionally writes to [0 + 34h], crashing with
// ACCESS_VIOLATION (0xC0000005) at 0x00683328 writing to 0x00000034.
// This crash during /reload aborts UI reinitialization, causing broken action bar
// panels ("умирают панели") and getting stuck on a black loading screen.
//
// Guard:
// Naked ASM hook on sub_683320 that validates target ([esp+4]). If target is null
// or outside valid address space, increment g_nullClearTargetSuppressed, set eax = 0,
// and return immediately with ret 4.
// Zero /GS cookies, zero SEH frames.
// ----------------------------------------------------------------------------

static constexpr uintptr_t kTarget683320 = 0x00683320;
static const unsigned char kExpectedPrologue683320[8] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x33, 0xC9
};

static void* g_orig683320 = nullptr;
static volatile LONG g_nullClearTargetSuppressed = 0;
static bool g_guard683320Active = false;

__declspec(naked) static void Hooked_683320() {
    __asm {
        // [esp+4] = target
        mov eax, [esp+4]
        cmp eax, 0x10000
        jb invalid_target
        cmp eax, 0x7FFE0000
        jb valid_target
        cmp byte ptr [g_optHighPlacementClientActive], 0
        je invalid_target
        cmp eax, 0x80010000
        jb invalid_target
        cmp eax, 0xFFFE0000
        jae invalid_target

    valid_target:
        jmp dword ptr [g_orig683320]

    invalid_target:
        inc dword ptr [g_nullClearTargetSuppressed]
        xor eax, eax
        ret 4
    }
}

bool InstallRenderNullGuard()
{
    if (!Config::g_settings.OptRenderNullGuard) {
        Log("[RenderGuard] DISABLED via configuration - sub_873060, sub_4B6550, sub_4B6CB0, sub_4BA060, sub_4F5980, sub_6848A0, sub_683320 and sub_992DA0 run unguarded");
        return true;
    }

    g_optHighPlacementClientActive = Config::g_settings.OptHighPlacementClient;

    void* target = (void*)0x00873060;
    unsigned char* p = (unsigned char*)target;

    // Verify prologue: push ebp; mov ebp, esp
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[RenderGuard] BAD PROLOGUE at 0x%08X (got %02X %02X %02X)",
            (uintptr_t)target, p[0], p[1], p[2]);
        return false;
    }

    if (MH_CreateHook(target, (void*)Hooked_873060, (void**)&g_orig873060) != MH_OK) {
        Log("[RenderGuard] MH_CreateHook FAILED for 0x00873060");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[RenderGuard] MH_EnableHook FAILED for 0x00873060");
        return false;
    }

    g_active = true;
    Log("[RenderGuard] ACTIVE v3 -- guards sub_873060, suppressions are counted");

    void* target4B6550 = (void*)kTarget4B6550;
    if (WowOpt_ClientPatchAllowed(target4B6550)) {
        if (memcmp(target4B6550, kExpectedPrologue4B6550, sizeof(kExpectedPrologue4B6550)) == 0) {
            if (MH_CreateHook(target4B6550, (void*)Hooked_4B6550, (void**)&g_orig4B6550) == MH_OK &&
                MH_EnableHook(target4B6550) == MH_OK) {
                g_guard4B6550Active = true;
                Log("[RenderGuard] AsyncTextureWait guard ACTIVE on sub_4B6550 @ 0x%08X", (unsigned)kTarget4B6550);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget4B6550);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget4B6550);
        }
    }

    void* target4B6CB0 = (void*)kTarget4B6CB0;
    if (!Config::g_settings.OptTextureGetGxTexFast && WowOpt_ClientPatchAllowed(target4B6CB0)) {
        if (memcmp(target4B6CB0, kExpectedPrologue4B6CB0, sizeof(kExpectedPrologue4B6CB0)) == 0) {
            if (MH_CreateHook(target4B6CB0, (void*)Hooked_4B6CB0, (void**)&g_orig4B6CB0) == MH_OK &&
                MH_EnableHook(target4B6CB0) == MH_OK) {
                g_guard4B6CB0Active = true;
                Log("[RenderGuard] TextureGetGxTex guard ACTIVE on sub_4B6CB0 @ 0x%08X", (unsigned)kTarget4B6CB0);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget4B6CB0);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget4B6CB0);
        }
    }

    void* target4F5980 = (void*)kTarget4F5980;
    if (WowOpt_ClientPatchAllowed(target4F5980)) {
        if (memcmp(target4F5980, kExpectedPrologue4F5980, sizeof(kExpectedPrologue4F5980)) == 0) {
            if (MH_CreateHook(target4F5980, (void*)Hooked_4F5980, (void**)&g_orig4F5980) == MH_OK &&
                MH_EnableHook(target4F5980) == MH_OK) {
                g_guard4F5980Active = true;
                Log("[RenderGuard] WorldFrame HandleSpriteTrack guard ACTIVE on sub_4F5980 @ 0x%08X", (unsigned)kTarget4F5980);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget4F5980);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget4F5980);
        }
    }

    void* target4BA060 = (void*)kTarget4BA060;
    if (WowOpt_ClientPatchAllowed(target4BA060)) {
        if (memcmp(target4BA060, kExpectedPrologue4BA060, sizeof(kExpectedPrologue4BA060)) == 0) {
            if (MH_CreateHook(target4BA060, (void*)Hooked_4BA060, (void**)&g_orig4BA060) == MH_OK &&
                MH_EnableHook(target4BA060) == MH_OK) {
                g_guard4BA060Active = true;
                Log("[RenderGuard] AsyncFileReadWait guard ACTIVE on sub_4BA060 @ 0x%08X", (unsigned)kTarget4BA060);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget4BA060);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget4BA060);
        }
    }

    void* target992DA0 = (void*)kTarget992DA0;
    if (WowOpt_ClientPatchAllowed(target992DA0)) {
        if (memcmp(target992DA0, kExpectedPrologue992DA0, sizeof(kExpectedPrologue992DA0)) == 0) {
            if (MH_CreateHook(target992DA0, (void*)Hooked_992DA0, (void**)&g_orig992DA0) == MH_OK &&
                MH_EnableHook(target992DA0) == MH_OK) {
                g_guard992DA0Active = true;
                Log("[RenderGuard] GlyphRasterizer guard ACTIVE on sub_992DA0 @ 0x%08X", (unsigned)kTarget992DA0);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget992DA0);
            }
        } else {
            const unsigned char* p = (const unsigned char*)target992DA0;
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X (got %02X %02X %02X %02X %02X %02X)",
                (unsigned)kTarget992DA0, p[0], p[1], p[2], p[3], p[4], p[5]);
        }
    }


    void* target7B5020 = (void*)kTarget7B5020;
    if (WowOpt_ClientPatchAllowed(target7B5020)) {
        if (memcmp(target7B5020, kExpectedPrologue7B5020, sizeof(kExpectedPrologue7B5020)) == 0) {
            if (MH_CreateHook(target7B5020, (void*)Hooked_7B5020, (void**)&g_orig7B5020) == MH_OK &&
                MH_EnableHook(target7B5020) == MH_OK) {
                g_guard7B5020Active = true;
                Log("[RenderGuard] TSList link guard ACTIVE on sub_7B5020 @ 0x%08X", (unsigned)kTarget7B5020);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget7B5020);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget7B5020);
        }
    }

    void* target4B7E80 = (void*)kTarget4B7E80;
    if (WowOpt_ClientPatchAllowed(target4B7E80)) {
        if (memcmp(target4B7E80, kExpectedPrologue4B7E80, sizeof(kExpectedPrologue4B7E80)) == 0) {
            if (MH_CreateHook(target4B7E80, (void*)Hooked_4B7E80, (void**)&g_orig4B7E80) == MH_OK &&
                MH_EnableHook(target4B7E80) == MH_OK) {
                g_guard4B7E80Active = true;
                Log("[RenderGuard] TextureAsyncCallback guard ACTIVE on sub_4B7E80 @ 0x%08X", (unsigned)kTarget4B7E80);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget4B7E80);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget4B7E80);
        }
    }

    void* target6848A0 = (void*)kTarget6848A0;
    if (WowOpt_ClientPatchAllowed(target6848A0)) {
        if (memcmp(target6848A0, kExpectedPrologue6848A0, sizeof(kExpectedPrologue6848A0)) == 0) {
            if (MH_CreateHook(target6848A0, (void*)Hooked_6848A0, (void**)&g_orig6848A0) == MH_OK &&
                MH_EnableHook(target6848A0) == MH_OK) {
                g_guard6848A0Active = true;
                Log("[RenderGuard] DeviceSetViewport guard ACTIVE on sub_6848A0 @ 0x%08X", (unsigned)kTarget6848A0);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget6848A0);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget6848A0);
        }
    }

    void* target683320 = (void*)kTarget683320;
    if (WowOpt_ClientPatchAllowed(target683320)) {
        if (memcmp(target683320, kExpectedPrologue683320, sizeof(kExpectedPrologue683320)) == 0) {
            if (MH_CreateHook(target683320, (void*)Hooked_683320, (void**)&g_orig683320) == MH_OK &&
                MH_EnableHook(target683320) == MH_OK) {
                g_guard683320Active = true;
                Log("[RenderGuard] DeviceClearTarget guard ACTIVE on sub_683320 @ 0x%08X", (unsigned)kTarget683320);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget683320);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget683320);
        }
    }

    void* target6A2C00 = (void*)kTarget6A2C00;
    if (WowOpt_ClientPatchAllowed(target6A2C00)) {
        if (memcmp(target6A2C00, kExpectedPrologue6A2C00, sizeof(kExpectedPrologue6A2C00)) == 0) {
            if (MH_CreateHook(target6A2C00, (void*)Hooked_6A2C00, (void**)&g_orig6A2C00) == MH_OK &&
                MH_EnableHook(target6A2C00) == MH_OK) {
                g_guard6A2C00Active = true;
                Log("[RenderGuard] ITexCreate guard ACTIVE on sub_6A2C00 @ 0x%08X", (unsigned)kTarget6A2C00);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget6A2C00);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget6A2C00);
        }
    }

    void* target6A31E0 = (void*)kTarget6A31E0;
    if (WowOpt_ClientPatchAllowed(target6A31E0)) {
        if (memcmp(target6A31E0, kExpectedPrologue6A31E0, sizeof(kExpectedPrologue6A31E0)) == 0) {
            if (MH_CreateHook(target6A31E0, (void*)Hooked_6A31E0, (void**)&g_orig6A31E0) == MH_OK &&
                MH_EnableHook(target6A31E0) == MH_OK) {
                g_guard6A31E0Active = true;
                Log("[RenderGuard] ITexUpdate guard ACTIVE on sub_6A31E0 @ 0x%08X", (unsigned)kTarget6A31E0);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget6A31E0);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget6A31E0);
        }
    }


    void* target8267E0 = (void*)kTarget8267E0;
    if (WowOpt_ClientPatchAllowed(target8267E0)) {
        if (memcmp(target8267E0, kExpectedPrologue8267E0, sizeof(kExpectedPrologue8267E0)) == 0) {
            if (MH_CreateHook(target8267E0, (void*)Hooked_8267E0, (void**)&g_orig8267E0) == MH_OK &&
                MH_EnableHook(target8267E0) == MH_OK) {
                g_guard8267E0Active = true;
                Log("[RenderGuard] Model AnimSequence guard ACTIVE on sub_8267E0 @ 0x%08X", (unsigned)kTarget8267E0);
            } else {
                Log("[RenderGuard] MH_CreateHook FAILED for 0x%08X", (unsigned)kTarget8267E0);
            }
        } else {
            Log("[RenderGuard] BAD PROLOGUE at 0x%08X", (unsigned)kTarget8267E0);
        }
    }

    return true;
}

void RenderNullGuard_LogStats()
{
    // Two different silences used to look the same: a guard that is not in,
    // and one that is in and has never been reached. This is a crash guard on
    // by default, so both are worth a line.
    if (!g_active) {
        Log("[RenderGuard] not measured: the guard is not installed.");
        return;
    }
    if (g_calls == 0) {
        Log("[RenderGuard] measured and zero: the guard is in and no draw-path "
            "call reached it.");
        return;
    }

    if (g_suppressed == 0) {
        Log("[RenderGuard] %lld draw-path calls, none suppressed - the three "
            "null conditions it exists for did not occur. Counts are lower "
            "bounds.", (long long)g_calls);
    } else {
        // Worth saying loudly. Each suppressed call is a model drawn with whatever
        // parameters the previous one left behind.
        Log("[RenderGuard] %lld of %lld draw-path calls SUPPRESSED (%.3f%%) - each one "
            "is a model drawn with the previous model's parameters",
            g_suppressed, g_calls, (double)g_suppressed * 100.0 / (double)g_calls);
        Log("[RenderGuard]   parameter table null: %lld   CGxDevice null: %lld   "
            "D3D9 device not ready: %lld",
            g_noParamTable, g_noGxDevice, g_deviceNotReady);
    }

    if (g_asyncWaitSuppressed > 0) {
        Log("[RenderGuard] %ld invalid AsyncTextureWait calls SUPPRESSED (m_asyncObject wild/0xFFFFFFFF sanitized to null)",
            (long)g_asyncWaitSuppressed);
    }

    if (g_asyncReentrantWaitSuppressed > 0) {
        Log("[RenderGuard] %ld re-entrant AsyncTextureWait calls avoided while AsyncFileReadWait active (s_waiting != FALSE)",
            (long)g_asyncReentrantWaitSuppressed);
    }

    if (g_fileWaitReentrantSuppressed > 0) {
        Log("[RenderGuard] %ld re-entrant AsyncFileReadWait calls avoided while s_waiting active (Error #134 prevented)",
            (long)g_fileWaitReentrantSuppressed);
    }

    if (g_worldFrameRecovered > 0 || g_worldFrameSuppressed > 0) {
        Log("[RenderGuard] %ld WorldFrame HandleSpriteTrack calls recovered (valid s_worldFrame restored), %ld suppressed",
            (long)g_worldFrameRecovered, (long)g_worldFrameSuppressed);
    }

    if (g_nullGlyphRasterizerSuppressed > 0) {
        Log("[RenderGuard] %ld invalid/null FreeType glyph rasterizer calls SUPPRESSED on sub_992DA0 (Error #132 prevented)",
            (long)g_nullGlyphRasterizerSuppressed);
    }


    if (g_listLinkNullSuppressed > 0) {
        Log("[RenderGuard] %ld invalid TSList link calls SUPPRESSED on sub_7B5020 (Crash at 0x007B502D prevented)",
            (long)g_listLinkNullSuppressed);
    }

    if (g_textureAsyncNullSuppressed > 0) {
        Log("[RenderGuard] %ld invalid TextureAsyncCallback calls SUPPRESSED on sub_4B7E80 (Null m_asyncObject crash prevented)",
            (long)g_textureAsyncNullSuppressed);
    }

    if (g_textureWildSanitized > 0) {
        Log("[RenderGuard] %ld corrupted/freed texture lookups SANITIZED in TextureGetGxTex (sub_4B6CB0 @ 0x004B6D26 prevented)",
            (long)g_textureWildSanitized);
    }

    if (g_nullViewportTargetSuppressed > 0) {
        Log("[RenderGuard] %ld invalid DeviceSetViewport calls SUPPRESSED on sub_6848A0 (Null target crash at 0x006848A9 prevented)",
            (long)g_nullViewportTargetSuppressed);
    }

    if (g_nullClearTargetSuppressed > 0) {
        Log("[RenderGuard] %ld invalid DeviceClearTarget calls SUPPRESSED on sub_683320 (Null target crash at 0x00683328 prevented)",
            (long)g_nullClearTargetSuppressed);
    }

    if (g_texCreateWildSanitized > 0) {
        Log("[RenderGuard] %ld corrupted/out-of-bounds ITexCreate calls SANITIZED on sub_6A2C00 (Error #132 at 0x006A2C29 prevented)",
            (long)g_texCreateWildSanitized);
    }

    if (g_texUpdateWildSanitized > 0) {
        Log("[RenderGuard] %ld corrupted ITexUpdate calls SANITIZED on sub_6A31E0 (Error #132 at 0x006A320B prevented)",
            (long)g_texUpdateWildSanitized);
    }

    if (g_modelAnimNullSuppressed > 0) {
        Log("[RenderGuard] %ld uninitialized Model AnimSequence calls SUPPRESSED on sub_8267E0 (Error #132 at 0x00826862 prevented)",
            (long)g_modelAnimNullSuppressed);
    }

}



