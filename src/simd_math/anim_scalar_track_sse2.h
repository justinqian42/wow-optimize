#pragma once

// ============================================================================
// Module: anim_scalar_track_sse2
//
// Hardware double-precision SSE2 rewrite of the M2 scalar animation track
// evaluators:
//   sub_82AF40: packed int16 scalar tracks (transparency/alpha/FOV)
//   sub_82B340: float scalar tracks (colors/alpha channels)
//
// Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

namespace AnimScalarTrack {

bool Init();
void Shutdown();
void LogStats();

}  // namespace AnimScalarTrack
