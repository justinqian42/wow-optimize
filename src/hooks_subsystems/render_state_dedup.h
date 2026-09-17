#pragma once

bool InstallRenderStateDedup(void);
// Printed from the periodic report. This used to be logged only from shutdown,
// which this DLL never reaches, so the dedup rate has never been seen.
void RenderStateDedup_LogStats(void);
void ShutdownRenderStateDedup(void);
void RenderStateDedup_ClearCache(void);