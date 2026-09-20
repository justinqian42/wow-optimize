// ============================================================================
// Module: m2_anim_stride
//
// The model animation family around sub_82F0F0 is about a fifth of main-thread
// execution, the largest single target in the profile. 24.8 million calls in one
// measured session at roughly 5900 cycles each. Nothing that makes the inside of
// it cheaper reaches that number; the only lever big enough is doing it less
// often for models the player cannot see the difference on.
// ---------------------------------------------------------------------------
// Why the previous attempt could not work, and why this one can
//
// AnimLod skips the whole call. Its own guard refuses 94.2% of the time and it
// skipped 0.0% of 24.8 million calls, because the function does two unrelated
// things and only the first is safe to drop:
//
//     +0x336 .. +0x119C   the bone loop - track interpolation, quaternion to
//                         matrix, the per-bone matrix product. Its callees are
//                         sub_828680 (3.00% of the profile), sub_82B0A0 (2.50%)
//                         and the matrix helpers.
//     +0x11B3 .. end      the tail - attachments, particle emitters, ribbon
//                         emitters, lights, materials. sub_82AF40, sub_82D6F0,
//                         sub_82B270, sub_82B340, sub_82B460, sub_82B8A0,
//                         sub_82BB50, sub_82D2F0, sub_82E550.
//
// Every tail callee is after the loop and every bone callee is inside it. Skip
// the call and the tail goes with it, which is the glowing shoulder pads bug
// that guard exists to prevent. Skip only the loop and the model keeps its pose
// while its materials, particles and attachments carry on animating normally.
// ---------------------------------------------------------------------------
// The cut, and why it needs no new control flow at all
//
//     0x0082F418  mov  edx, [ebx+2Ch]      ; the bone count
//     0x0082F41B  cmp  edx, edi            ; edi is the starting index, zero
//     0x0082F41D  mov  [ebp+arg_10], edi
//     0x0082F420  jbe  loc_8302A3          ; no bones -> straight to the tail
//     0x0082F426  ...                      ; the loop
//
// The client already has the branch. `jbe loc_8302A3` is its own "this model has
// no bones, skip the loop" exit, and it lands exactly where the tail begins.
// Taking it early is a path the client takes itself, so the x87 stack depth, the
// frame and every register the tail reads are whatever they already were - there
// is nothing to reconstruct and nothing to get wrong about the state at the
// bottom of a 3686-byte loop.
//
// The tail confirms it: 0x008302A3 is `mov ecx, [ebp+var_4]`, `xor eax, eax`,
// `cmp [ecx+48h], eax`, `mov [ebp+arg_10], eax`. It reloads what it needs from
// the frame and wants nothing the loop would have left.
//
// The five bytes at 0x0082F418 are one `mov` and one `cmp` exactly, so the jump
// fits without splitting an instruction, and nothing in the client jumps into
// them: the only cross references are to 0x0082F418 itself, two jumps and a
// fall-through.
// ---------------------------------------------------------------------------
// What a held frame looks like
//
// The bone matrices live in the model's own array at [ESI+0x98] and are simply
// not rewritten, so the skeleton holds the pose it had. It is not a frozen
// model: the tail still runs, so its particles, ribbons, attached items and
// material animation continue. A model at 90 yards updating its skeleton every
// third frame is 15 Hz of skeletal animation on something a few dozen pixels
// tall.
//
// Which models, and when:
//
//   - crowd threshold: if distinct models evaluated per frame is under
//     kQuietModelThreshold (24), nothing is held. Sparse scenes run at 100%
//     animation quality.
//   - bounding radius and extents guard: EBX at the cut point points directly
//     to M2Data/M2Header. +0x0A4..0x0B8 are bounding box floats (minX, minY,
//     minZ, maxX, maxY, maxZ) and +0x0BC is the bounding sphere radius. Any
//     model with radius <= 0.0f, radius > 6.0f, or extents > 10.0f (or Z > 12.0f)
//     is never held. This completely eliminates the previous stutter on Ironforge
//     lava, Deeprun Tram tunnels, elevators, and large raid boss geometry.
//   - apparent screen footprint guard: (radius * radius) > 0.0015f * distSq
//     rejects holding. Only models whose projected angular size on screen is small
//     are eligible for striding.
//   - inside kNearYd (45 yards), never. The player, whatever they are fighting
//     and everything in melee range animate every frame, always.
//   - the phase comes from the model's own address, so models in the same band
//     do not all update on the same frame and produce a sawtooth.
//   - the first kWarmupFrames after install hold nothing, so a session that
//     crashes early cannot blame this.
//
// The distance is the model's world translation at +228 - the fourth row of the
// 4x4 at +180, which AnimLod's learning phase already confirmed is world space -
// against the camera at 0x00CD7778 that WowWorld::StreamCentre reads.
//
// Off by default and an A/B subject. A held frame is a visible change, not an
// invisible one, and the harness is what says whether it is worth the change.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "m2_anim_stride.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "world_position.h"

extern "C" void Log(const char* fmt, ...);

namespace M2AnimStride {

namespace {

// The five bytes the jump replaces: mov edx,[ebx+2Ch] ; cmp edx,edi
const unsigned char kHead[5] = { 0x8B, 0x53, 0x2C, 0x3B, 0xD7 };
// The client's own no-bones branch, immediately after them.
const unsigned char kTail[6] = { 0x0F, 0x86, 0x7D, 0x0E, 0x00, 0x00 };

constexpr uintptr_t kHeadAddr    = 0x0082F418;
constexpr uintptr_t kTailAddr    = 0x0082F420;   // the jbe, for the signature
constexpr uintptr_t kRunAddr     = 0x0082F41D;   // mov [ebp+arg_10],edi ; jbe
constexpr uintptr_t kHoldAddr    = 0x008302A3;   // where the client's own jbe goes

// The x87 TOP field on entry to the decision, and how often it came back
// different.
//
// This module shares its five bytes and its destination with m2_anim_reuse, so
// it shares the trap. Three instructions before the cut the client runs
// `fst [ebp+var_98]` - fst, not fstp - leaving one value live on the x87 stack,
// and the tail at 0x008302B4 pops it with `fstp st`. Jumping there with an empty
// stack is a masked underflow: no fault, an indefinite value written, the tag
// word left wrong, and every float in the rest of the frame quietly wrong.
//
// Nothing between the cut and the jump may touch the x87 stack. Nothing does
// today - the built object contains zero x87 instructions - but that holds only
// until a float appears somewhere in the decision, which the compiler will
// answer with the x87 stack and no warning. So the thunk checks instead of
// trusting, and a decision that moved the stack refuses to hold.
unsigned short g_topBefore = 0;
unsigned long  g_fpuMoved  = 0;

// Read by the naked thunk, so plain addresses rather than a struct.
void* g_retRun  = (void*)kRunAddr;
void* g_retHold = (void*)kHoldAddr;

unsigned char g_saved[5] = {};
bool g_patched = false;

// The model's world matrix and the row that carries its translation.
constexpr unsigned kM_localMatrix = 180;
constexpr unsigned kM_translation = kM_localMatrix + 48;

// Bands, in yards. Nothing inside the first is ever held.
constexpr float kNearYd = 45.0f;
constexpr float kMidYd  = 90.0f;
constexpr float kFarYd  = 160.0f;

// Size limits: normal player character is radius ~1.5 yd, tauren ~2.0 yd.
constexpr float kMaxHoldRadius = 6.0f;
constexpr float kMaxExtentXY   = 10.0f;
constexpr float kMaxExtentZ    = 12.0f;

// Screen footprint limit: (r*r)/d^2. 0.0015f corresponds to ~40 yd for radius 1.5,
// ~77 yd for radius 3.0, and ~130 yd for radius 5.0.
constexpr float kMaxFootprintFactor = 0.0015f;

// Crowd threshold: below 24 models in the frame, do not stride at all.
constexpr unsigned long kQuietModelThreshold = 24;

// One frame in N gets the bones, per band.
constexpr unsigned kStrideMid = 2;
constexpr unsigned kStrideFar = 3;
constexpr unsigned kStrideVeryFar = 4;

// Nothing is held until the camera has been readable for this many frames, so a
// session that never gets a world position never strides anything.
constexpr unsigned kWarmupFrames = 120;

unsigned long g_frame = 0;
float g_camera[3] = {};
bool  g_cameraOk = false;
unsigned long g_cameraFrames = 0;
// Every frame boundary, whether or not the camera was readable on it. Without
// this, a clock that never ticks and a camera that never reads produce the same
// report line - "the camera was readable for only 0 frames" - and the first of
// those two is not the camera's fault. The sibling module spent six field
// sessions being read the wrong way for exactly this reason.
unsigned long g_frames       = 0;

// Crowd tracking: models evaluated per frame.
unsigned long g_modelsThisFrame = 0;
unsigned long g_modelsPrevFrame = 0;

// Plain, main thread only. Lower bounds if that ever stops being true.
unsigned long g_calls = 0;
unsigned long g_held  = 0;
unsigned long g_nearKept = 0;
unsigned long g_largeModelKept = 0;
unsigned long g_screenKept = 0;
unsigned long g_quietKept = 0;
unsigned long g_noPos  = 0;
unsigned long g_bandHeld[3] = {};

// The A/B flag: true means this module is the subject being measured.
bool g_abSubject = false;

bool Readable(uintptr_t p) {
    if (p < 0x10000 || p > 0xFFE00000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, int len) {
    if (!Readable(addr) || !Readable(addr + (uintptr_t)len - 1)) return false;
    return memcmp((const void*)addr, want, (size_t)len) == 0;
}

}  // namespace

// Called from the thunk with model in ESI and m2data in EBX. Returns nothing; the
// answer goes into the byte the thunk tests, because a naked thunk cannot read EAX
// across the popad that protects the client's registers.
extern "C" unsigned char g_m2StrideHold = 0;

extern "C" void __cdecl M2AnimStride_Decide(void* model, const void* m2data) {
    g_m2StrideHold = 0;
    ++g_calls;
    ++g_modelsThisFrame;

    if (!g_cameraOk || g_cameraFrames < kWarmupFrames) return;
    if (g_abSubject && AbTest::StandAside()) return;

    // Crowd guard: in sparse scenes, run at 100% animation fidelity.
    if (g_modelsPrevFrame < kQuietModelThreshold) {
        ++g_quietKept;
        return;
    }

    const uintptr_t m = (uintptr_t)model;
    const uintptr_t d = (uintptr_t)m2data;

    // Fast pointer sanitization (already dereferenced by client at 0x82F415/0x82F418).
    // Avoids costly VirtualQuery syscalls on the per-call hot path.
    if (m < 0x10000 || m > 0xFFE00000 || d < 0x10000 || d > 0xFFE00000) {
        ++g_noPos;
        return;
    }

    // Bounding radius & extents guard: M2Header offsets:
    // +0x0A4..0x0B8: minX, minY, minZ, maxX, maxY, maxZ (6 floats)
    // +0x0BC: bounding sphere radius (1 float)
    const float radius = *(const float*)(d + 0xBC);
    if (radius <= 0.0f || radius > kMaxHoldRadius) {
        ++g_largeModelKept;
        return;
    }

    const float minX = *(const float*)(d + 0xA4);
    const float minY = *(const float*)(d + 0xA8);
    const float minZ = *(const float*)(d + 0xAC);
    const float maxX = *(const float*)(d + 0xB0);
    const float maxY = *(const float*)(d + 0xB4);
    const float maxZ = *(const float*)(d + 0xB8);

    const float extX = maxX - minX;
    const float extY = maxY - minY;
    const float extZ = maxZ - minZ;

    if (extX <= 0.0f || extX > kMaxExtentXY ||
        extY <= 0.0f || extY > kMaxExtentXY ||
        extZ <= 0.0f || extZ > kMaxExtentZ) {
        ++g_largeModelKept;
        return;
    }

    const float* t = (const float*)(m + kM_translation);
    const float dx = t[0] - g_camera[0];
    const float dy = t[1] - g_camera[1];
    const float dz = t[2] - g_camera[2];
    const float d2 = dx * dx + dy * dy + dz * dz;

    if (d2 <= kNearYd * kNearYd) {
        ++g_nearKept;
        return;
    }

    // Apparent screen footprint guard: (r*r) > factor * d2
    if ((radius * radius) > (kMaxFootprintFactor * d2)) {
        ++g_screenKept;
        return;
    }

    unsigned stride;
    int band;
    if (d2 <= kMidYd * kMidYd)       { stride = kStrideMid;     band = 0; }
    else if (d2 <= kFarYd * kFarYd)  { stride = kStrideFar;     band = 1; }
    else                             { stride = kStrideVeryFar; band = 2; }

    // The phase comes from the model's own address so a band's models do not all
    // land on the same frame, which would be a sawtooth rather than a saving.
    const unsigned long phase = (unsigned long)((m >> 4) * 2654435761u);
    if (((g_frame + phase) % stride) == 0) return;   // this is its frame

    g_m2StrideHold = 1;
    ++g_held;
    ++g_bandHeld[band];
}

namespace {

// Entered by a jump, not a call. Every exit is a jump to an address the client
// would have reached anyway.
__declspec(naked) void Thunk() {
    __asm {
        pushad
        // TOP, bits 11..13 of the status word. Inside pushad, so ax is free.
        fnstsw ax
        and  ax, 3800h
        mov  word ptr [g_topBefore], ax

        push ebx
        push esi
        call M2AnimStride_Decide
        add  esp, 8

        // If the decision moved the x87 stack, the tail's fstp would pop the
        // wrong thing. Refuse to hold rather than hand back a frame of
        // indefinite floats.
        fnstsw ax
        and  ax, 3800h
        cmp  ax, word ptr [g_topBefore]
        je   top_unchanged
        mov  byte ptr [g_m2StrideHold], 0
        inc  dword ptr [g_fpuMoved]
    top_unchanged:
        popad

        cmp  byte ptr [g_m2StrideHold], 0
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

bool Install() {
    if (!Config::g_settings.OptM2AnimStride) {
        Log("[M2Stride] not installed: switched off.");
        return false;
    }
    if (!BytesMatch(kHeadAddr, kHead, 5)) {
        Log("[M2Stride] NOT installed: the bytes at 0x%08X are not the bone count "
            "test this was read from.", (unsigned)kHeadAddr);
        return false;
    }
    if (!BytesMatch(kTailAddr, kTail, 6)) {
        Log("[M2Stride] NOT installed: the bone count test matches but the branch "
            "after it at 0x%08X is not the client's own no-bones exit, so the "
            "address this holds a model by is not confirmed.", (unsigned)kTailAddr);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kHeadAddr)) {
        Log("[M2Stride] NOT installed: No Client Patches is on, and this writes "
            "five bytes into wow.exe like every hook here does.");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)kHeadAddr, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[M2Stride] NOT installed: could not make 0x%08X writable",
            (unsigned)kHeadAddr);
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

    g_abSubject = AbTest::IsSubject("M2AnimStride", &g_abSubject);
    Log("[M2Stride] ACTIVE. A model past %.0f yards holds its skeleton for a "
        "frame instead of re-solving every bone, by taking the client's own "
        "no-bones branch out of the loop at 0x%08X. Its materials, particles and "
        "attachments still animate, because the tail is not skipped. Nothing is "
        "held for the first %u frames.",
        kNearYd, (unsigned)kHeadAddr, kWarmupFrames);
    return true;
}

void Shutdown() {
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

void OnPresent() {
    if (g_patched) {
        ++g_frame;
        g_modelsPrevFrame = g_modelsThisFrame;
        g_modelsThisFrame = 0;
    }
}

void OnFrame() {
    if (!g_patched) return;
    ++g_frames;
    float c[3];
    if (WowWorld::StreamCentre(c)) {
        g_camera[0] = c[0];
        g_camera[1] = c[1];
        g_camera[2] = c[2];
        g_cameraOk = true;
        ++g_cameraFrames;
    } else {
        // No camera means no distance, and no distance means nothing is held.
        g_cameraOk = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptM2AnimStride) {
        Log("[M2Stride] not measured: switched off.");
        return;
    }
    if (!g_patched) {
        Log("[M2Stride] not measured: not installed.");
        return;
    }
    if (g_calls == 0) {
        Log("[M2Stride] measured and zero: patched, and the bone loop was never "
            "reached.");
        return;
    }
    // The frame clock, printed before anything that depends on it. The sibling
    // module reported 2 repeats in 454 million calls purely because this counter
    // was being advanced by something that is not a frame, and no line in its
    // report could say so.
    if (g_frames == 0) {
        Log("[Wrong] [M2Stride] the bone loop ran %lu time(s) and the frame "
            "counter is still zero. The stride bands are counted in frames, so "
            "nothing below can be right. OnFrame is not being called.", g_calls);
    } else {
        Log("[M2Stride] %lu frame(s) seen, %.1f call(s) per frame.",
            g_frames, (double)g_calls / (double)g_frames);
    }
    Log("[M2Stride] %lu of %lu bone loops held (%.1f%%). %lu were inside %.0f "
        "yards and never eligible, %lu had no readable world position.",
        g_held, g_calls, 100.0 * (double)g_held / (double)g_calls,
        g_nearKept, kNearYd, g_noPos);
    Log("[M2Stride]   guards: %lu kept for size (r > %.1f yd or ext > %.1f), %lu "
        "kept for screen footprint ((r^2)/d2 > 0.0015), %lu kept for quiet scene (< %lu models).",
        g_largeModelKept, kMaxHoldRadius, kMaxExtentXY,
        g_screenKept, g_quietKept, kQuietModelThreshold);
    // Printed whether or not it fired, because zero is the answer that says the
    // decision path is still free of x87 and the hold is safe to take.
    Log("[M2Stride]   the x87 stack depth was unchanged across the decision on "
        "every call but %lu. The tail pops a value the client left on the stack, "
        "so a non-zero figure here means something in the decision now uses the "
        "FPU and those calls refused to hold rather than corrupt the frame.",
        g_fpuMoved);
    Log("[M2Stride]   held by band: %lu at %.0f-%.0f yd (1 frame in %u), %lu at "
        "%.0f-%.0f (1 in %u), %lu past %.0f (1 in %u).",
        g_bandHeld[0], kNearYd, kMidYd, kStrideMid,
        g_bandHeld[1], kMidYd, kFarYd, kStrideFar,
        g_bandHeld[2], kFarYd, kStrideVeryFar);
    if (g_frames > 0 && g_cameraFrames == 0) {
        Log("[M2Stride]   the frame boundary was reached %lu time(s) and the "
            "camera was readable on none of them, so no distance was ever "
            "available. That is the camera, not the clock.", g_frames);
    }
    if (g_cameraFrames < kWarmupFrames) {
        Log("[M2Stride]   the camera was readable for only %lu frames, under the "
            "%u this waits for, so most of the session held nothing whatever the "
            "distances were.", g_cameraFrames, kWarmupFrames);
    }
    if (g_held == 0) {
        Log("[M2Stride]   measured and zero: every loop ran. Either everything "
            "was inside %.0f yards or the world position never read.", kNearYd);
    }
}

}  // namespace M2AnimStride
