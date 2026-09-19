#pragma once

// ============================================================================
// Module: anim_spline_track_sse2
//
// Hardware double-precision SSE2 rewrite of the M2 cubic spline animation track
// evaluators:
//   sub_82B460: 3D vector spline tracks (cubic Hermite / Bezier)
//   sub_82B8A0: scalar spline tracks (cubic Hermite / Bezier)
//
// Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

namespace AnimSplineTrack {

bool Init();
void Shutdown();
void LogStats();

}  // namespace AnimSplineTrack
