#pragma once

// ============================================================================
// Module: fog_state_dedup.h
//
// Fastpath and deduplication for scene fog shader and device setup (sub_873210, 379 bytes).
// In field profiles, sub_873210 is repeatedly called across every terrain chunk
// (sub_7A8440, sub_7A9380) and every M2 model batch (sub_81FB10) to update scene
// atmospheric fog. In >99% of calls, the fog distances, density, and color are
// identical to the previous call. This fastpaths redundant state changes in ~6 instructions,
// bypassing redundant float conversions, reciprocal divisions, and device shader calls.
// ============================================================================

namespace FogStateDedup {

bool Init();
void Install();
void Shutdown();
void LogStats();

} // namespace FogStateDedup
