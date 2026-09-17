// ============================================================================
// Description: Hoists a loop-invariant out of the object manager's enumerator,
//              and measures what the enumerator actually costs.
// ============================================================================

#pragma once

namespace ObjMgrEnumFast {

bool Init();
void Shutdown();
void OnFrame();
void LogStats();

} // namespace ObjMgrEnumFast
