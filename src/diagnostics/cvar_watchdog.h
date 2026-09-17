#pragma once

bool InitCvarWatchdog();

// Call every frame. No-ops until the client has a Lua state, scans once, then
// no-ops forever after.
void CvarWatchdog_Check();
