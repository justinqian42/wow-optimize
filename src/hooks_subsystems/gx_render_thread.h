// ============================================================================
// Module: gx_render_thread
//
// Runs the client's IDirect3DDevice9 calls on a thread of our own. The main
// thread writes each state change and draw into a lock-free ring and carries on
// with the next frame's work; the render thread executes them, in order, against
// the real device. See gx_ring.h for the ring and gx_render_thread.cpp for the
// rules about which calls are queued and which wait for the ring to drain.
//
// Off by default (D3d9RenderThread in [Graphics_Sound]). Forced off under
// Wine/Rosetta, with NoClientPatches, and with the draw merger.
// ============================================================================

#pragma once

namespace GxRT {

// Call once, early: hooks the client's D3D loader so the device is created with
// D3DCREATE_MULTITHREADED and handed to us. Returns false when the feature is off
// or cannot run; the reason is logged.
bool Init();

bool IsActive();
void LogStats();
void Shutdown();

}  // namespace GxRT
