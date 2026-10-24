#pragma once

// ============================================================================
// Module: nameplate_parallel
//
// Parallelizes CGUnit_C::ShouldShowNamePlate (0x0072B060) and 3D screen
// coordinate projection across worker threads during CGWorldFrame::OnWorldRender.
//
// Off by default (Graphics_Sound/NameplateParallel), experimental.
// ============================================================================

namespace NameplateParallel {

void Init();
void Shutdown();
void LogStats();

}  // namespace NameplateParallel
