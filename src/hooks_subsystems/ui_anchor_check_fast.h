#pragma once

// ============================================================================
// Module: ui_anchor_check_fast.h
//
// Fastpath for CSimpleRegion::DependsOn (sub_4891D0, 95 bytes).
// Accounting for 25,627 samples at 0x004891E2 (#9 hotspot in field profiles),
// invoked via SetPoint (sub_49DB10) and SetAllPoints (sub_49DE40) to validate
// directed acyclic graph reachability before linking UI frame anchors.
// ============================================================================

namespace UIAnchorCheckFast {

bool Init();
void Install();
void Shutdown();
void LogStats();

} // namespace UIAnchorCheckFast
