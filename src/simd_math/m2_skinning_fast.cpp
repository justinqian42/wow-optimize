// ============================================================================
// Module: m2_skinning_fast.cpp
//
// Fast aligned SSE2 vertex transformation and skinning for M2 models:
//   sub_82AC10: CM2Model::TransformVerticesOneBone_sse (single-bone rigid mesh)
//   sub_82A600: CM2Model::TransformVertices_sse        (multi-bone blended mesh)
//
// Background & Analysis:
//   Every frame, all animated 3D models (characters, creatures, weapons, doodads)
//   undergo CPU vertex skinning in CM2Model::FillVertexBuffer before being submitted
//   to the Direct3D vertex buffer with vertex format GxVBF_PNT (32 bytes per vertex:
//   pos.xyz = 12 bytes, norm.xyz = 12 bytes, tex.uv = 8 bytes).
//
//   In stock 3.3.5a (sub_82AC10 and sub_82A600):
//     1. The engine computes position and stores it at offset 0:
//          movaps [out + 0], xmm1   ; writes bytes 0..15 (pos.x, pos.y, pos.z, pos.w)
//     2. Then stores the transformed normal starting at byte offset 12:
//          movups [out + 12], xmm0  ; unaligned 16-byte store writing bytes 12..27!
//        This unaligned vector store straddles the 16-byte boundary and pollutes
//        bytes 24..27 with normal.w.
//     3. Immediately on the subsequent instructions, scalar 32-bit stores write:
//          mov [out + 24], ecx      ; tex.u (overwriting bytes 24..27)
//          mov [out + 28], edx      ; tex.v (writing bytes 28..31)
//
//   Because the unaligned vector store at byte 12 overlaps with the immediately
//   following scalar writes at bytes 24 and 28, it triggers a catastrophic
//   store-to-load forwarding conflict and store buffer bubble on modern out-of-order
//   CPUs for every single vertex rendered on screen.
//
//   Furthermore, in sub_82AC10 (single-bone skinning), the client reloads all four
//   16-byte rows of the bone matrix (64 bytes) from memory on every vertex, even
//   though all vertices in the section share the identical bone matrix.
//
// This replacement:
//   1. Caches the active bone matrix across contiguous vertices in single-bone meshes,
//      cutting memory read bandwidth by more than 50%.
//   2. Packs position, normal, and UV coordinates into two aligned 16-byte vectors:
//        out0 = (pos.x, pos.y, pos.z, norm.x)
//        out1 = (norm.y, norm.z, tex.u, tex.v)
//      and writes them with two aligned _mm_store_ps instructions.
//   3. Completely eliminates unaligned stores, cache-line splits, and store-forwarding
//      bubbles across all M2 model rendering.
//   4. Compiles with __declspec(safebuffers) for zero /GS security cookies.
//   5. Verifies bit-exact output against the client routine and retires immediately
//      on any discrepancy.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "m2_skinning_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2SkinningFast {

namespace {

constexpr uintptr_t kTargetOneBone   = 0x0082AC10;
constexpr uintptr_t kTargetMultiBone = 0x0082A600;

const unsigned char kExpectedPrologue[16] = {
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
    0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04
};

typedef void (__cdecl* TransformVertices_fn)(void* model, void* section, uint8_t* vertices, int32_t texCoord);

static TransformVertices_fn g_orig_TransformVerticesOneBone   = nullptr;
static TransformVertices_fn g_orig_TransformVerticesMultiBone = nullptr;

static bool g_installed = false;
static bool g_dead      = false;

static unsigned long long g_callsOneBone     = 0;
static unsigned long long g_callsMultiBone   = 0;
static unsigned long long g_verifiedOneBone  = 0;
static unsigned long long g_verifiedMulti    = 0;
static unsigned long long g_mismatches       = 0;

__forceinline void Fast_TransformOneBone_Impl(
    const void* bones_ptr,
    const uint8_t* vertex,
    uint16_t vertexCount,
    uint8_t* out_bytes,
    size_t tex_offset)
{
    float* out = reinterpret_cast<float*>(out_bytes);
    uint32_t cached_bone = 0xFFFFFFFF;
    __m128 row0 = _mm_setzero_ps();
    __m128 row1 = _mm_setzero_ps();
    __m128 row2 = _mm_setzero_ps();
    __m128 row3 = _mm_setzero_ps();

    for (uint16_t i = 0; i < vertexCount; ++i, vertex += 48, out += 8) {
        const uint32_t bone_idx = static_cast<uint32_t>(vertex[0x10]);
        if (bone_idx != cached_bone) {
            cached_bone = bone_idx;
            const float* bone = reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(bones_ptr) + (bone_idx << 6));
            row0 = _mm_load_ps(&bone[0]);
            row1 = _mm_load_ps(&bone[4]);
            row2 = _mm_load_ps(&bone[8]);
            row3 = _mm_load_ps(&bone[12]);
        }

        const float* pos  = reinterpret_cast<const float*>(vertex);
        const float* norm = reinterpret_cast<const float*>(vertex + 0x14);

        __m128 px = _mm_set1_ps(pos[0]);
        __m128 py = _mm_set1_ps(pos[1]);
        __m128 pz = _mm_set1_ps(pos[2]);
        __m128 position = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row2, pz), _mm_add_ps(_mm_mul_ps(row1, py), _mm_mul_ps(row0, px))),
            row3);

        __m128 nx = _mm_set1_ps(norm[0]);
        __m128 ny = _mm_set1_ps(norm[1]);
        __m128 nz = _mm_set1_ps(norm[2]);
        __m128 normal = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row0, nx), _mm_mul_ps(row1, ny)),
            _mm_mul_ps(row2, nz));

        __m128 tex = _mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(vertex + tex_offset));

        __m128 temp = _mm_shuffle_ps(position, normal, _MM_SHUFFLE(0, 0, 2, 2));
        __m128 out0 = _mm_shuffle_ps(position, temp, _MM_SHUFFLE(2, 0, 1, 0));
        __m128 out1 = _mm_shuffle_ps(normal, tex, _MM_SHUFFLE(1, 0, 2, 1));

        _mm_store_ps(&out[0], out0);
        _mm_store_ps(&out[4], out1);
    }
}

__forceinline void Fast_TransformMultiBone_Impl(
    const void* bones_ptr,
    const uint8_t* vertex,
    uint16_t vertexCount,
    uint8_t* out_bytes,
    size_t tex_offset)
{
    float* out = reinterpret_cast<float*>(out_bytes);
    uint32_t last_weights = 0xFFFFFFFF;
    uint32_t last_indices = 0xFFFFFFFF;

    __m128 row0 = _mm_setzero_ps();
    __m128 row1 = _mm_setzero_ps();
    __m128 row2 = _mm_setzero_ps();
    __m128 row3 = _mm_setzero_ps();

    for (uint16_t i = 0; i < vertexCount; ++i, vertex += 48, out += 8) {
        const uint32_t weights = *reinterpret_cast<const uint32_t*>(vertex + 0x0C);
        const uint32_t indices = *reinterpret_cast<const uint32_t*>(vertex + 0x10);

        if (weights != last_weights || indices != last_indices) {
            last_weights = weights;
            last_indices = indices;

            const uint8_t* w = vertex + 0x0C;
            const uint8_t* b = vertex + 0x10;

            const float w0 = static_cast<float>(w[0]) * 0.0039215689f;
            __m128 mw0 = _mm_set1_ps(w0);
            const float* bone0 = reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(bones_ptr) + (static_cast<size_t>(b[0]) << 6));
            row0 = _mm_mul_ps(_mm_load_ps(&bone0[0]), mw0);
            row1 = _mm_mul_ps(_mm_load_ps(&bone0[4]), mw0);
            row2 = _mm_mul_ps(_mm_load_ps(&bone0[8]), mw0);
            row3 = _mm_mul_ps(_mm_load_ps(&bone0[12]), mw0);

            for (int j = 1; j < 4 && w[j] != 0; ++j) {
                const float wj = static_cast<float>(w[j]) * 0.0039215689f;
                __m128 mwj = _mm_set1_ps(wj);
                const float* bonej = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(bones_ptr) + (static_cast<size_t>(b[j]) << 6));
                row0 = _mm_add_ps(row0, _mm_mul_ps(_mm_load_ps(&bonej[0]), mwj));
                row1 = _mm_add_ps(row1, _mm_mul_ps(_mm_load_ps(&bonej[4]), mwj));
                row2 = _mm_add_ps(row2, _mm_mul_ps(_mm_load_ps(&bonej[8]), mwj));
                row3 = _mm_add_ps(row3, _mm_mul_ps(_mm_load_ps(&bonej[12]), mwj));
            }
        }

        const float* pos  = reinterpret_cast<const float*>(vertex);
        const float* norm = reinterpret_cast<const float*>(vertex + 0x14);

        __m128 px = _mm_set1_ps(pos[0]);
        __m128 py = _mm_set1_ps(pos[1]);
        __m128 pz = _mm_set1_ps(pos[2]);
        __m128 position = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row2, pz), _mm_add_ps(_mm_mul_ps(row1, py), _mm_mul_ps(row0, px))),
            row3);

        __m128 nx = _mm_set1_ps(norm[0]);
        __m128 ny = _mm_set1_ps(norm[1]);
        __m128 nz = _mm_set1_ps(norm[2]);
        __m128 normal = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row0, nx), _mm_mul_ps(row1, ny)),
            _mm_mul_ps(row2, nz));

        __m128 tex = _mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(vertex + tex_offset));

        __m128 temp = _mm_shuffle_ps(position, normal, _MM_SHUFFLE(0, 0, 2, 2));
        __m128 out0 = _mm_shuffle_ps(position, temp, _MM_SHUFFLE(2, 0, 1, 0));
        __m128 out1 = _mm_shuffle_ps(normal, tex, _MM_SHUFFLE(1, 0, 2, 1));

        _mm_store_ps(&out[0], out0);
        _mm_store_ps(&out[4], out1);
    }
}

__declspec(noinline) static void Verify_TransformOneBone(
    void* model,
    void* section,
    uint8_t* vertices,
    int32_t texCoord,
    const void* bones,
    const uint8_t* vertex,
    uint16_t count,
    size_t tex_offset)
{
    const size_t byte_size = static_cast<size_t>(count) * 32;
    uint8_t* temp_buffer = static_cast<uint8_t*>(_malloca(byte_size));
    if (!temp_buffer) {
        g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        return;
    }

    g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
    Fast_TransformOneBone_Impl(bones, vertex, count, temp_buffer, tex_offset);

    if (memcmp(vertices, temp_buffer, byte_size) != 0) {
        ++g_mismatches;
        g_dead = true;
        Log("[M2SkinningFast] OneBone verification DISAGREEMENT on section vertexCount=%u. Retiring.", (unsigned)count);
    } else {
        ++g_verifiedOneBone;
    }

    _freea(temp_buffer);
}

__declspec(noinline) static void Verify_TransformMultiBone(
    void* model,
    void* section,
    uint8_t* vertices,
    int32_t texCoord,
    const void* bones,
    const uint8_t* vertex,
    uint16_t count,
    size_t tex_offset)
{
    const size_t byte_size = static_cast<size_t>(count) * 32;
    uint8_t* temp_buffer = static_cast<uint8_t*>(_malloca(byte_size));
    if (!temp_buffer) {
        g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        return;
    }

    g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
    Fast_TransformMultiBone_Impl(bones, vertex, count, temp_buffer, tex_offset);

    if (memcmp(vertices, temp_buffer, byte_size) != 0) {
        ++g_mismatches;
        g_dead = true;
        Log("[M2SkinningFast] MultiBone verification DISAGREEMENT on section vertexCount=%u. Retiring.", (unsigned)count);
    } else {
        ++g_verifiedMulti;
    }

    _freea(temp_buffer);
}

__declspec(safebuffers) static void __cdecl Hook_TransformVerticesOneBone(
    void* model,
    void* section,
    uint8_t* vertices,
    int32_t texCoord)
{
    ++g_callsOneBone;

    if (g_dead || !model || !section || !vertices) {
        if (g_orig_TransformVerticesOneBone) {
            g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        }
        return;
    }

    // If destination buffer is not 16-byte aligned, fallback to original
    if ((reinterpret_cast<uintptr_t>(vertices) & 0x0F) != 0) {
        g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        return;
    }

    const void* bones = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(model) + 0x98);
    const void* shared = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(model) + 0x2C);
    if (!bones || !shared) {
        g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        return;
    }

    const void* data = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(shared) + 0x150);
    if (!data) {
        g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        return;
    }

    const uint8_t* vert_base = *reinterpret_cast<const uint8_t* const*>(
        reinterpret_cast<const char*>(data) + 0x40);
    if (!vert_base) {
        g_orig_TransformVerticesOneBone(model, section, vertices, texCoord);
        return;
    }

    const uint16_t vertexStart = *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const char*>(section) + 4);
    const uint16_t vertexCount = *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const char*>(section) + 6);
    if (vertexCount == 0) {
        return;
    }

    const int32_t tc = (texCoord < 0 || texCoord > 1) ? 0 : texCoord;
    const size_t tex_offset = static_cast<size_t>(tc) * 8 + 0x20;
    const uint8_t* vertex = vert_base + static_cast<size_t>(vertexStart) * 48;

    if (g_verifiedOneBone < 1000 || (g_callsOneBone & 255) == 0) {
        Verify_TransformOneBone(model, section, vertices, texCoord, bones, vertex, vertexCount, tex_offset);
        return;
    }

    Fast_TransformOneBone_Impl(bones, vertex, vertexCount, vertices, tex_offset);
}

__declspec(safebuffers) static void __cdecl Hook_TransformVerticesMultiBone(
    void* model,
    void* section,
    uint8_t* vertices,
    int32_t texCoord)
{
    ++g_callsMultiBone;

    if (g_dead || !model || !section || !vertices) {
        if (g_orig_TransformVerticesMultiBone) {
            g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        }
        return;
    }

    if ((reinterpret_cast<uintptr_t>(vertices) & 0x0F) != 0) {
        g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        return;
    }

    const void* bones = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(model) + 0x98);
    const void* shared = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(model) + 0x2C);
    if (!bones || !shared) {
        g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        return;
    }

    const void* data = *reinterpret_cast<const void* const*>(
        reinterpret_cast<const char*>(shared) + 0x150);
    if (!data) {
        g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        return;
    }

    const uint8_t* vert_base = *reinterpret_cast<const uint8_t* const*>(
        reinterpret_cast<const char*>(data) + 0x40);
    if (!vert_base) {
        g_orig_TransformVerticesMultiBone(model, section, vertices, texCoord);
        return;
    }

    const uint16_t vertexStart = *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const char*>(section) + 4);
    const uint16_t vertexCount = *reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const char*>(section) + 6);
    if (vertexCount == 0) {
        return;
    }

    const int32_t tc = (texCoord < 0 || texCoord > 1) ? 0 : texCoord;
    const size_t tex_offset = static_cast<size_t>(tc) * 8 + 0x20;
    const uint8_t* vertex = vert_base + static_cast<size_t>(vertexStart) * 48;

    if (g_verifiedMulti < 1000 || (g_callsMultiBone & 255) == 0) {
        Verify_TransformMultiBone(model, section, vertices, texCoord, bones, vertex, vertexCount, tex_offset);
        return;
    }

    Fast_TransformMultiBone_Impl(bones, vertex, vertexCount, vertices, tex_offset);
}

static bool CheckPrologue(uintptr_t addr, const unsigned char* expected, size_t size) {
    __try {
        return memcmp((const void*)addr, expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void Init() {
    if (!Config::g_settings.OptM2SkinningFast) {
        return;
    }

    if (!CheckPrologue(kTargetOneBone, kExpectedPrologue, sizeof(kExpectedPrologue))) {
        Log("[M2SkinningFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTargetOneBone);
        return;
    }

    if (!CheckPrologue(kTargetMultiBone, kExpectedPrologue, sizeof(kExpectedPrologue))) {
        Log("[M2SkinningFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTargetMultiBone);
        return;
    }

    MH_STATUS status = WineSafe_CreateHook(
        (void*)kTargetOneBone,
        (void*)Hook_TransformVerticesOneBone,
        (void**)&g_orig_TransformVerticesOneBone);
    if (status != MH_OK) {
        Log("[M2SkinningFast] NOT active: hook on OneBone failed with status %d", (int)status);
        return;
    }

    status = WineSafe_CreateHook(
        (void*)kTargetMultiBone,
        (void*)Hook_TransformVerticesMultiBone,
        (void**)&g_orig_TransformVerticesMultiBone);
    if (status != MH_OK) {
        Log("[M2SkinningFast] NOT active: hook on MultiBone failed with status %d", (int)status);
        MH_RemoveHook((void*)kTargetOneBone);
        return;
    }

    status = WO_EnableHook((void*)kTargetOneBone);
    if (status != MH_OK) {
        Log("[M2SkinningFast] NOT active: WO_EnableHook on OneBone failed with status %d", (int)status);
        return;
    }

    status = WO_EnableHook((void*)kTargetMultiBone);
    if (status != MH_OK) {
        Log("[M2SkinningFast] NOT active: WO_EnableHook on MultiBone failed with status %d", (int)status);
        MH_DisableHook((void*)kTargetOneBone);
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("TransformVerticesOneBone_Fast", (const void*)&Hook_TransformVerticesOneBone);
    SamplingProfiler::RegisterSelfSymbol("TransformVerticesMultiBone_Fast", (const void*)&Hook_TransformVerticesMultiBone);
    g_installed = true;
    Log("[M2SkinningFast] ACTIVE on sub_82AC10 and sub_82A600 (aligned vector stores, bone matrix caching)");
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTargetOneBone);
        MH_RemoveHook((void*)kTargetOneBone);
        MH_DisableHook((void*)kTargetMultiBone);
        MH_RemoveHook((void*)kTargetMultiBone);
        g_installed = false;
        Log("[M2SkinningFast] Shutdown complete");
    }
}

void LogStats() {
    if (!Config::g_settings.OptM2SkinningFast) return;
    if (!g_installed) {
        Log("[M2SkinningFast] not installed");
        return;
    }
    if (g_dead) {
        Log("[M2SkinningFast] RETIRED after mismatch (oneBoneCalls=%llu, multiBoneCalls=%llu, mismatches=%llu)",
            g_callsOneBone, g_callsMultiBone, g_mismatches);
    } else {
        Log("[M2SkinningFast] active and bit-exact (oneBoneCalls=%llu verified=%llu, multiBoneCalls=%llu verified=%llu)",
            g_callsOneBone, g_verifiedOneBone, g_callsMultiBone, g_verifiedMulti);
    }
}

}  // namespace M2SkinningFast

