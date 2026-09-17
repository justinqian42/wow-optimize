// ============================================================================
// Description: Counts and times the client's archive file-open path, and says
//              how much of it is asking for the same name twice.
// Safety & Threading: Counting hook. Calls through on every path.
// ============================================================================

#pragma once

namespace MpqOpenCensus {

bool Init();
void Shutdown();
void OnLoadBegin();
void ReportLoad(double loadMs);
void LogStats();

} // namespace MpqOpenCensus
