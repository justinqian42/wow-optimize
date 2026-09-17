#pragma once

// ============================================================================
// Description: SSE2 fast paths for the engine's matrix-vector multiply and
//              vector normalize.
// ============================================================================

namespace SimdMathFast {

bool Init();
void Shutdown();

} // namespace SimdMathFast
