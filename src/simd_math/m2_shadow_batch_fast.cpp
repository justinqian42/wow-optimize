#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "MinHook.h"
#include "config.h"
#include "ab_test.h"
#include "m2_shadow_batch_fast.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace M2ShadowBatchFast {

namespace {

constexpr uintptr_t kTarget = 0x00834660; // CM2Model::GatherModelBatchesShadowMap (0x1AE bytes)

// Verified 16-byte prologue:
// 55                push ebp
// 8B EC             mov  ebp, esp
// 83 EC 14          sub  esp, 14h
// 56                push esi
// 8B F1             mov  esi, ecx
// F6 46 10 01       test byte ptr [esi+10h], 1
// 75 25             jnz  loc_834694
// F6                (head of test byte ptr [esi+4], 20h)
static const uint8_t kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x56, 0x8B,
    0xF1, 0xF6, 0x46, 0x10, 0x01, 0x75, 0x25, 0xF6
};

#pragma pack(push, 1)
struct M2Batch {
    uint16_t m_flags;
    uint16_t m_shader;
    uint16_t m_skinSectionIndex;
    uint16_t m_geosetIndex;
    uint16_t m_colorIndex;
    uint16_t m_materialIndex;
    uint16_t m_materialLayer;
    uint16_t m_textureCount;
    uint16_t m_pad[2];
    uint16_t m_textureWeightComboIndex;
    uint16_t m_pad2;
};
static_assert(sizeof(M2Batch) == 24, "M2Batch size mismatch");

struct M2Material {
    uint16_t m_flags;
    uint16_t m_blendMode;
};
static_assert(sizeof(M2Material) == 4, "M2Material size mismatch");

struct M2BatchEntry {
    void* m_model;
    uint32_t m_batchIndex;
    uint32_t m_flag;
};
static_assert(sizeof(M2BatchEntry) == 12, "M2BatchEntry size mismatch");

struct CModelBatchList {
    M2BatchEntry* m_data;
    uint32_t m_count;
    uint32_t m_capacity;
};
#pragma pack(pop)

typedef void (__thiscall* ModelInit_fn)(void* model);
static const ModelInit_fn Call_InitializeLoaded = (ModelInit_fn)0x00832EA0;

typedef void (__thiscall* ModelAnim_fn)(void* model);
static const ModelAnim_fn Call_AnimateSM = (ModelAnim_fn)0x00831990;

typedef void (__fastcall* GatherBatches_fn)(
    void* this_ptr,
    void* dummy_edx,
    CModelBatchList* untextured,
    CModelBatchList* textured
);
static GatherBatches_fn g_orig = nullptr;

static bool g_installed = false;
static bool g_dead      = false;
static bool g_abSubject = false;

static unsigned long long g_calls       = 0;
static unsigned long long g_verified    = 0;
static unsigned long long g_mismatches  = 0;
static unsigned long long g_control     = 0;
static unsigned long long g_batchesEval = 0;
static unsigned long long g_batchesPass = 0;

static const unsigned long kVerifyCount      = 2000;
static const unsigned long kVerifySampleMask = 0x7F; // 1 in 128 thereafter

static thread_local int t_verifyDepth = 0;

#define SCRATCH_CAPACITY 1024
static M2BatchEntry s_scratchOrigU[SCRATCH_CAPACITY];
static M2BatchEntry s_scratchOrigT[SCRATCH_CAPACITY];
static M2BatchEntry s_scratchFastU[SCRATCH_CAPACITY];
static M2BatchEntry s_scratchFastT[SCRATCH_CAPACITY];

static inline void AppendEntries(CModelBatchList* dst, const M2BatchEntry* src, uint32_t count) {
    if (!dst || !src) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (dst->m_count < dst->m_capacity) {
            dst->m_data[dst->m_count++] = src[i];
        }
    }
}

#pragma check_stack(off)

__declspec(safebuffers) static void FastGatherModelBatches(
    uintptr_t model,
    CModelBatchList* untextured,
    CModelBatchList* textured,
    bool runAnimate,
    int depth = 0
) {
    if (!model || depth > 8) return;

    if (runAnimate) {
        // 1. Initialized check
        uint8_t fFlags = *(const uint8_t*)(model + 0x10);
        if (!(fFlags & 1)) {
            uint8_t flags = *(const uint8_t*)(model + 0x04);
            if (!(flags & 0x20)) {
                return;
            }

            uintptr_t shared = *(const uintptr_t*)(model + 0x2C);
            if (!shared) return;
            uint32_t sharedFlags = *(const uint32_t*)(shared + 0x08);
            if (!(sharedFlags & 1) || !(sharedFlags & 2)) {
                return;
            }

            Call_InitializeLoaded((void*)model);
        }

        // 2. Animate shadow map
        Call_AnimateSM((void*)model);
    }

    // 3. Batch traversal
    uintptr_t shared = *(const uintptr_t*)(model + 0x2C);
    if (!shared) return;
    uintptr_t optGeo = *(const uintptr_t*)(model + 0x2D0);
    uintptr_t skin = *(const uintptr_t*)(shared + 0x170);

    const M2Batch* batches;
    uint32_t count;
    bool optimized = (optGeo != 0);

    if (optimized) {
        count = *(const uint32_t*)(optGeo + 0x04);
        batches = *(const M2Batch* const*)(optGeo + 0x00);
    } else {
        if (!skin) return;
        count = *(const uint32_t*)(skin + 0x24);
        batches = *(const M2Batch* const*)(skin + 0x28);
    }

    // Client line 0x8346CF: if count == 0, returns immediately without visiting children!
    if (count == 0 || !batches) {
        return;
    }

    uintptr_t data = *(const uintptr_t*)(shared + 0x150);
    if (!data) return;

    const uintptr_t* skinSections = *(const uintptr_t* const*)(model + 0x9C);
    const M2Material* materials = *(const M2Material* const*)(data + 0x74);
    uint32_t colorCount = *(const uint32_t*)(data + 0x48);
    const uint16_t* weightCombos = *(const uint16_t* const*)(data + 0x94);
    const uint8_t* colors = *(const uint8_t* const*)(model + 0xA0);
    const uint8_t* weights = *(const uint8_t* const*)(model + 0xA8);
    float baseAlpha = *(const float*)(model + 0x19C);

    g_batchesEval += count;

    for (uint32_t i = 0; i < count; ++i) {
        const M2Batch* b = &batches[i];

        // Filter 1: unoptimized skin section null check
        if (!optimized) {
            if (!skinSections || !skinSections[b->m_skinSectionIndex]) {
                continue;
            }
        }

        // Filter 2: shader 0x8000, flag 4, secondary material layers
        if (b->m_shader == 0x8000 || (b->m_flags & 4) || b->m_materialLayer != 0) {
            continue;
        }

        // Filter 3: material flags and blend mode
        if (!materials) continue;
        const M2Material* mat = &materials[b->m_materialIndex];
        uint16_t matFlags = mat->m_flags;
        if (matFlags & 0x40) {
            continue;
        }
        uint16_t blendMode = mat->m_blendMode;
        if (!(matFlags & 0x80) && blendMode > 1) {
            continue;
        }

        // Filter 4: alpha calculation in IEEE double precision matching client operation order
        double alpha = (double)baseAlpha;
        uint32_t colorIdx = b->m_colorIndex;
        if (colorIdx < colorCount && colors) {
            float colorAlpha = *(const float*)(colors + colorIdx * 32 + 0x1C);
            alpha *= (double)colorAlpha;
        }

        if (b->m_textureCount != 0 && weightCombos && weights) {
            uint16_t weightIdx = weightCombos[b->m_textureWeightComboIndex];
            float weightAlpha = *(const float*)(weights + weightIdx * 12 + 0x08);
            alpha *= (double)weightAlpha;
        }

        if (alpha >= 0.550000011920928955078125) {
            CModelBatchList* targetList = (blendMode != 0) ? textured : untextured;
            if (targetList && targetList->m_count < targetList->m_capacity) {
                M2BatchEntry* entry = &targetList->m_data[targetList->m_count++];
                entry->m_model = (void*)model;
                entry->m_batchIndex = i;
                entry->m_flag = 1;
                ++g_batchesPass;
            }
        }
    }

    // 4. Attached child models
    for (uintptr_t child = *(const uintptr_t*)(model + 0x58); child; child = *(const uintptr_t*)(child + 0x60)) {
        if (child == model) break;
        FastGatherModelBatches(child, untextured, textured, runAnimate, depth + 1);
    }
}

__declspec(safebuffers) void __fastcall Hooked_GatherModelBatches(
    void* this_ptr,
    void* dummy_edx,
    CModelBatchList* untextured,
    CModelBatchList* textured
) {
    if (g_dead || !Config::g_settings.OptM2ShadowBatchFast) {
        g_orig(this_ptr, dummy_edx, untextured, textured);
        return;
    }

    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        g_orig(this_ptr, dummy_edx, untextured, textured);
        return;
    }

    if (t_verifyDepth > 0) {
        g_orig(this_ptr, dummy_edx, untextured, textured);
        return;
    }

    ++g_calls;

    bool doVerify = (g_verified < kVerifyCount) || ((g_calls & kVerifySampleMask) == 0);
    if (!doVerify) {
        DWORD start = GetTickCount();
        FastGatherModelBatches((uintptr_t)this_ptr, untextured, textured, true);
        DWORD end = GetTickCount();
        if (end - start > 100) {
            Log("[M2ShadowBatchFast] WARNING: FastGatherModelBatches took %lu ms", end - start);
        }
        return;
    }

    CModelBatchList dummyOrigU = { s_scratchOrigU, 0, SCRATCH_CAPACITY };
    CModelBatchList dummyOrigT = { s_scratchOrigT, 0, SCRATCH_CAPACITY };
    CModelBatchList dummyFastU = { s_scratchFastU, 0, SCRATCH_CAPACITY };
    CModelBatchList dummyFastT = { s_scratchFastT, 0, SCRATCH_CAPACITY };

    t_verifyDepth++;
    g_orig(this_ptr, dummy_edx, &dummyOrigU, &dummyOrigT);
    t_verifyDepth--;

    // Run fast path (AnimateSM already ran inside g_orig for this frame)
    FastGatherModelBatches((uintptr_t)this_ptr, &dummyFastU, &dummyFastT, false);

    if (dummyOrigU.m_count >= SCRATCH_CAPACITY || dummyOrigT.m_count >= SCRATCH_CAPACITY) {
        AppendEntries(untextured, s_scratchOrigU, dummyOrigU.m_count);
        AppendEntries(textured, s_scratchOrigT, dummyOrigT.m_count);
        return;
    }

    bool match = (dummyOrigU.m_count == dummyFastU.m_count) &&
                 (dummyOrigT.m_count == dummyFastT.m_count) &&
                 (dummyOrigU.m_count == 0 || memcmp(s_scratchOrigU, s_scratchFastU, dummyOrigU.m_count * sizeof(M2BatchEntry)) == 0) &&
                 (dummyOrigT.m_count == 0 || memcmp(s_scratchOrigT, s_scratchFastT, dummyOrigT.m_count * sizeof(M2BatchEntry)) == 0);

    if (!match) {
        ++g_mismatches;
        g_dead = true;
        Log("[M2ShadowBatchFast] MISMATCH at call %llu (model=%p): origU=%u fastU=%u origT=%u fastT=%u - retiring hook for session",
            g_calls, this_ptr, dummyOrigU.m_count, dummyFastU.m_count, dummyOrigT.m_count, dummyFastT.m_count);
        AppendEntries(untextured, s_scratchOrigU, dummyOrigU.m_count);
        AppendEntries(textured, s_scratchOrigT, dummyOrigT.m_count);
        return;
    }

    ++g_verified;
    AppendEntries(untextured, s_scratchFastU, dummyFastU.m_count);
    AppendEntries(textured, s_scratchFastT, dummyFastT.m_count);
}

#pragma check_stack()

bool BytesMatch(uintptr_t addr, const uint8_t* want, int n) {
    __try {
        return memcmp((const void*)addr, want, (size_t)n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ----------------------------------------------------------------------------
// Startup Self-Test
// ----------------------------------------------------------------------------
bool SelfTestPasses() {
    alignas(16) static uint8_t s_mockModel[1024];
    alignas(16) static uint8_t s_mockShared[512];
    alignas(16) static uint8_t s_mockData[512];
    alignas(16) static uint8_t s_mockSkin[512];
    alignas(16) static uintptr_t s_mockSkinSections[8];
    alignas(16) static M2Batch s_mockBatches[8];
    alignas(16) static M2Material s_mockMaterials[4];
    alignas(16) static uint16_t s_mockCombos[4];
    alignas(16) static uint8_t s_mockColors[64];
    alignas(16) static uint8_t s_mockWeights[32];

    memset(s_mockModel, 0, sizeof(s_mockModel));
    memset(s_mockShared, 0, sizeof(s_mockShared));
    memset(s_mockData, 0, sizeof(s_mockData));
    memset(s_mockSkin, 0, sizeof(s_mockSkin));
    memset(s_mockSkinSections, 0, sizeof(s_mockSkinSections));
    memset(s_mockBatches, 0, sizeof(s_mockBatches));
    memset(s_mockMaterials, 0, sizeof(s_mockMaterials));
    memset(s_mockCombos, 0, sizeof(s_mockCombos));
    memset(s_mockColors, 0, sizeof(s_mockColors));
    memset(s_mockWeights, 0, sizeof(s_mockWeights));

    // Configure model pointers
    *(uint8_t*)(s_mockModel + 0x10) = 1; // m_fFlags: initialized
    *(uintptr_t*)(s_mockModel + 0x2C) = (uintptr_t)s_mockShared;
    *(uintptr_t*)(s_mockModel + 0x9C) = (uintptr_t)s_mockSkinSections;
    *(uintptr_t*)(s_mockModel + 0xA0) = (uintptr_t)s_mockColors;
    *(uintptr_t*)(s_mockModel + 0xA8) = (uintptr_t)s_mockWeights;
    *(float*)(s_mockModel + 0x19C) = 1.0f; // m_alpha

    // Configure shared pointers
    *(uintptr_t*)(s_mockShared + 0x150) = (uintptr_t)s_mockData;
    *(uintptr_t*)(s_mockShared + 0x170) = (uintptr_t)s_mockSkin;

    // Configure data
    *(uint32_t*)(s_mockData + 0x48) = 1; // m_colors.m_count = 1
    *(uintptr_t*)(s_mockData + 0x74) = (uintptr_t)s_mockMaterials;
    *(uintptr_t*)(s_mockData + 0x94) = (uintptr_t)s_mockCombos;

    // Materials:
    // mat 0: flags=0, blendMode=0 (opaque)
    s_mockMaterials[0].m_flags = 0;
    s_mockMaterials[0].m_blendMode = 0;
    // mat 1: flags=0, blendMode=1 (alpha-test)
    s_mockMaterials[1].m_flags = 0;
    s_mockMaterials[1].m_blendMode = 1;
    // mat 2: flags=0, blendMode=2 (transparent blend -> rejected)
    s_mockMaterials[2].m_flags = 0;
    s_mockMaterials[2].m_blendMode = 2;
    // mat 3: flags=0x40, blendMode=0 (no-shadow flag -> rejected)
    s_mockMaterials[3].m_flags = 0x40;
    s_mockMaterials[3].m_blendMode = 0;

    // Skin sections: section 0 is valid, section 1 is null
    s_mockSkinSections[0] = (uintptr_t)0x12345678;
    s_mockSkinSections[1] = 0;

    // Color 0: alpha = 1.0f (offset 0x1C in 32-byte stride)
    *(float*)(s_mockColors + 0x1C) = 1.0f;

    // Batches:
    // Batch 0: layer 0, shader 0, flags 0, mat 0 (opaque) -> EXPECT untextured list
    s_mockBatches[0].m_materialLayer = 0;
    s_mockBatches[0].m_materialIndex = 0;
    s_mockBatches[0].m_skinSectionIndex = 0;

    // Batch 1: layer 0, shader 0, flags 0, mat 1 (alpha-test) -> EXPECT textured list
    s_mockBatches[1].m_materialLayer = 0;
    s_mockBatches[1].m_materialIndex = 1;
    s_mockBatches[1].m_skinSectionIndex = 0;

    // Batch 2: layer 1 -> reject (secondary layer)
    s_mockBatches[2].m_materialLayer = 1;
    s_mockBatches[2].m_materialIndex = 0;
    s_mockBatches[2].m_skinSectionIndex = 0;

    // Batch 3: shader 0x8000 -> reject
    s_mockBatches[3].m_materialLayer = 0;
    s_mockBatches[3].m_materialIndex = 0;
    s_mockBatches[3].m_shader = 0x8000;
    s_mockBatches[3].m_skinSectionIndex = 0;

    // Batch 4: flags 4 -> reject
    s_mockBatches[4].m_materialLayer = 0;
    s_mockBatches[4].m_materialIndex = 0;
    s_mockBatches[4].m_flags = 4;
    s_mockBatches[4].m_skinSectionIndex = 0;

    // Batch 5: mat 2 (blendMode 2) -> reject
    s_mockBatches[5].m_materialLayer = 0;
    s_mockBatches[5].m_materialIndex = 2;
    s_mockBatches[5].m_skinSectionIndex = 0;

    // Batch 6: mat 3 (flags 0x40) -> reject
    s_mockBatches[6].m_materialLayer = 0;
    s_mockBatches[6].m_materialIndex = 3;
    s_mockBatches[6].m_skinSectionIndex = 0;

    // Batch 7: skinSectionIndex 1 (null section) -> reject
    s_mockBatches[7].m_materialLayer = 0;
    s_mockBatches[7].m_materialIndex = 0;
    s_mockBatches[7].m_skinSectionIndex = 1;

    // Configure skin profile
    *(uint32_t*)(s_mockSkin + 0x24) = 8; // batch count
    *(uintptr_t*)(s_mockSkin + 0x28) = (uintptr_t)s_mockBatches;

    M2BatchEntry testU[16];
    M2BatchEntry testT[16];
    CModelBatchList listU = { testU, 0, 16 };
    CModelBatchList listT = { testT, 0, 16 };

    FastGatherModelBatches((uintptr_t)s_mockModel, &listU, &listT, false);

    if (listU.m_count != 1 || listT.m_count != 1) {
        Log("[M2ShadowBatchFast] Self-test FAILED: count mismatch (U=%u want 1, T=%u want 1)",
            listU.m_count, listT.m_count);
        return false;
    }

    if (testU[0].m_model != (void*)s_mockModel || testU[0].m_batchIndex != 0 || testU[0].m_flag != 1) {
        Log("[M2ShadowBatchFast] Self-test FAILED: untextured entry 0 mismatch");
        return false;
    }

    if (testT[0].m_model != (void*)s_mockModel || testT[0].m_batchIndex != 1 || testT[0].m_flag != 1) {
        Log("[M2ShadowBatchFast] Self-test FAILED: textured entry 0 mismatch");
        return false;
    }

    return true;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptM2ShadowBatchFast) return;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[M2ShadowBatchFast] NOT active: prologue bytes at 0x%08X do not match "
            "CM2Model::GatherModelBatchesShadowMap, hook refused.", (unsigned)kTarget);
        return;
    }

    if (!SelfTestPasses()) {
        Log("[M2ShadowBatchFast] NOT active: startup self-test failed.");
        return;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2ShadowBatchFast] NOT active: No Client Patches is on, and this hooks "
            "a function inside wow.exe.");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hooked_GatherModelBatches, (void**)&g_orig) != MH_OK) {
        Log("[M2ShadowBatchFast] NOT active: hook on 0x%08X could not be created.", (unsigned)kTarget);
        return;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[M2ShadowBatchFast] NOT active: hook on 0x%08X could not be enabled.", (unsigned)kTarget);
        return;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2ShadowBatchFast", &g_abSubject);

    Log("[M2ShadowBatchFast] ACTIVE on CM2Model::GatherModelBatchesShadowMap at 0x%08X "
        "(0.65%% CPU in profile, 3,715 samples). Replaces serialized x87 fcomp/fnstsw stalls "
        "with streamlined batch filtering and IEEE double precision alpha test.", (unsigned)kTarget);

    if (g_abSubject) {
        Log("[M2ShadowBatchFast]   under A/B test: control half calls original client code.");
    }
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptM2ShadowBatchFast) return;
    if (!g_installed) {
        Log("[M2ShadowBatchFast] not installed - check initialization messages");
        return;
    }
    if (g_calls == 0) {
        Log("[M2ShadowBatchFast] hooked; no calls to GatherModelBatchesShadowMap recorded yet.");
        return;
    }

    Log("[M2ShadowBatchFast] %llu calls, %llu verified (0 tolerance, %llu mismatches); "
        "%llu batches evaluated, %llu passed to shadow map (%.1f%% rejection rate).",
        g_calls, g_verified, g_mismatches, g_batchesEval, g_batchesPass,
        g_batchesEval ? (100.0 * (1.0 - (double)g_batchesPass / (double)g_batchesEval)) : 0.0);

    if (g_abSubject) {
        Log("[M2ShadowBatchFast]   %llu call(s) ran through client code as A/B control.", g_control);
    }
}

} // namespace M2ShadowBatchFast
