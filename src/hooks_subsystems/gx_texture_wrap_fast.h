#pragma once

// ============================================================================
// Module: gx_texture_wrap_fast.h
//
// Fastpath for CGxDevice::SetTextureWrap (sub_685D40, 127 bytes).
// In field profiles (wow_optimize_2026-10-22_11-55-53.log), sub_685D40 was a
// top rendering hotspot (569 samples at 0x00685D51, 0.65% of executing CPU time),
// invoked via GxSetTextureWrap across M2 models, terrain, and WMO geometry.
// ============================================================================

namespace GxTextureWrapFast {

bool Init();
void Shutdown();
void LogStats();

} // namespace GxTextureWrapFast
