#pragma once

bool InstallD3D9StateManager(void);
void ShutdownD3D9StateManager(void);

// Per-hook call and skip counts. Printed from the periodic report rather than
// from shutdown, which this process does not reach.
void D3D9StateManager_LogStats(void);

// Restores the device vtable on the process-exit teardown path, where blocking
// on the vtable lock could hang the exiting process. Gives up rather than waits.
void ShutdownD3D9StateManagerAtProcessExit(void);
void OnFrameD3D9StateManager(DWORD mainThreadId);
bool IsD3D9DeviceHooked(void);
// How many merge barriers went in, of how many, and how many state blocks the
// client created. The draw-merge census needs all three to say whether its own
// answer can be trusted.
// Whether the surface and query barriers, which are not device methods,
// both went in. The merger will not run without them.
bool D3D9StateManager_DerivedBarriersOk(void);
void D3D9StateManager_GetBarrierState(int* installed, int* total,
                                      unsigned long* stateBlocks);

extern volatile LONG g_deviceResetCounter;
