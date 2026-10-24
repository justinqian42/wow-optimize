#pragma once

// ============================================================================
// Module: gx_device_texture_fast.h
//
// Fastpath for CGxDevice::SetTexture (sub_685F50, 94 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_685F50 was a
// top rendering hotspot (5,558 samples at 0x00685F98, 0.55% of executing CPU time),
// invoked on every texture bind across UI batch rendering, M2 models, terrain,
// and particle systems.
// ============================================================================

namespace GxDeviceTextureFast {

bool Init();
void Shutdown();
void LogStats();

} // namespace GxDeviceTextureFast
