#pragma once

// ============================================================================
// Module: m2_skinning_fast.h
//
// Fast aligned SSE2 vertex transformation and skinning for M2 models:
//   sub_82AC10: CM2Model::TransformVerticesOneBone_sse (single-bone rigid mesh)
//   sub_82A600: CM2Model::TransformVertices_sse        (multi-bone blended mesh)
//
// Eliminates unaligned 16-byte stores crossing cache lines and overlapping
// scalar texture coordinate writes that trigger severe store-to-load forwarding
// pipeline stalls on modern out-of-order x86 execution cores.
// ============================================================================

namespace M2SkinningFast {

void Init();
void Shutdown();
void LogStats();

}  // namespace M2SkinningFast

