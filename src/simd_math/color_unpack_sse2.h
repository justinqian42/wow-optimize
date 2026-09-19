#pragma once

// ============================================================================
// Module: color_unpack_sse2
//
// Hardware SSE2 color unpacking and vector math optimizations:
//   sub_984C90: Color_UnpackBGRA (32-bit BGRA bytes -> 4x float RGBA in [0, 1])
//   sub_982970: Color_UnpackBGR  (24-bit BGR bytes  -> 3x float RGB  in [0, 1])
//   sub_9829B0: Vec3_DominantAxis (C3Vector dominant axis index 0, 1, 2)
//
// Replaces integer stack spills and serialized x87 fild/fmul/fstp sequences
// with parallel SSE2 integer unpack/shuffle, conversion, and vector multiply.
// Vec3_DominantAxis evaluates coordinate magnitudes using bitwise IEEE fabs.
//
// 100% bit-exact with client x87 output across all 256 possible byte values.
// ============================================================================

namespace ColorUnpack {

bool Init();
void Shutdown();
void LogStats();

}  // namespace ColorUnpack
