#pragma once

// ============================================================================
// Module: m2_batch_matrix_sse2
//
// Replaces the serialized sixteen-float matrix setup copies in sub_823130
// (M2 model batch render pass setup) with four 128-bit SSE2 vector moves each.
// Site 1 (0x008236B3): matrix stack slot 8 push duplicate (64 bytes).
// Site 2 (0x00823762): matrix stack slot 8 identity matrix reset (64 bytes).
//
// Bit-exact: copies pure float values with no arithmetic.
// ============================================================================

namespace M2BatchMatrix {

bool Install();
void Shutdown();
void LogStats();

}  // namespace M2BatchMatrix
