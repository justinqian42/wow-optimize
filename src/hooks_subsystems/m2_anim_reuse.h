// ============================================================================
// Module: m2_anim_reuse.h
// Description: Holds the M2 bone loop when the animation state repeats.
// Safety & Threading: Main thread, same as the function it patches.
// ============================================================================

#pragma once

namespace M2AnimReuse {

bool Init();
void Shutdown();
void OnFrame();
void LogStats();

} // namespace M2AnimReuse
