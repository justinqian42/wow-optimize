#pragma once

// ============================================================================
// Module: ui_anim_update_fast.h
//
// Fastpath for CSimpleFrame::OnAnimUpdate (sub_488890, 90 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_488890 was the
// #35 overall hotspot (6,573 samples at 0x0048889C, 0.65% of executing time),
// invoked through 33 UI vtables across all buttons, textures, fontstrings, and
// frames on every frame.
// ============================================================================

namespace UIAnimUpdateFast {

bool Init();
void Shutdown();
void LogStats();

} // namespace UIAnimUpdateFast
