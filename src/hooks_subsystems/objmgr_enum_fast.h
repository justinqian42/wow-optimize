// ============================================================================
// Module: objmgr_enum_fast.h
// Description: Hoists a loop-invariant out of the object manager's enumerator,
//              and measures what the enumerator actually costs.
// Safety & Threading: Main thread, same as the function it replaces.
// ============================================================================

#pragma once

namespace ObjMgrEnumFast {

bool Init();
void Shutdown();
void OnFrame();
void LogStats();

} // namespace ObjMgrEnumFast
