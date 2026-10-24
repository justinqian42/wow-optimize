#pragma once

// ============================================================================
// Module: ui_render_coord_fast.h
//
// Fastpath for CSimpleRender::UpdateCoordsAndFlags (sub_482DC0, 75 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_482DC0 was a
// significant UI rendering hotspot (3,827 samples at 0x00482E00, 0.36% of CPU time),
// invoked on every state sync and layout pass across all UI render nodes.
// ============================================================================

namespace UIRenderCoordFast {

bool Init();
void Shutdown();
void LogStats();

} // namespace UIRenderCoordFast
