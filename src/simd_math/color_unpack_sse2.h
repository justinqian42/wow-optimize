#pragma once

// ============================================================================
// Module: color_unpack_sse2
//
// Hardware SSE2 color packing/unpacking and vector math optimizations:
//   sub_984C90: Color_UnpackBGRA  (32-bit BGRA bytes -> 4x float RGBA in [0, 1])
//   sub_982970: Color_UnpackBGR   (24-bit BGR bytes  -> 3x float RGB  in [0, 1])
//   sub_48BD20: Color_PackBGRA    (4x float ARGB     -> 32-bit BGRA uint32)
//   sub_9851A0: Color_PackBGR     (3x float RGB      -> 32-bit BGRA uint32, A=0xFF)
//   sub_984F60: Color_RGBToHSV    (3x float RGB [0,1]-> 3x float HSV [0..360, 0..1, 0..1])
//   sub_985030: Color_HSVToRGB    (3x float HSV      -> 3x float RGB [0, 1])
//   sub_9829B0: Vec3_DominantAxis (C3Vector dominant axis index 0, 1, 2)
//   sub_9829F0: Vec3_RecessiveAxis (C3Vector recessive axis index 0, 1, 2)
//
// Replaces integer stack spills, serialized x87 fild/fmul/fstp sequences, and
// 8 pipeline-flushing fldcw control word manipulations with parallel SSE2
// integer unpack/pack, conversion, and vector multiply. Vectorizes RGB/HSV
// conversions with branchless coordinate extrema and parallel color sector math.
//
// 100% bit-exact with client x87 output across all test cases and color ranges.
// ============================================================================

namespace ColorUnpack {

bool Init();
void Shutdown();
void LogStats();

}  // namespace ColorUnpack
