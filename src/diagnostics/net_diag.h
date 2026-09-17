#pragma once

// ============================================================================
// Passive observer on the receive path, so that when a player is disconnected
// the log says something about it. Changes no behaviour.
// ============================================================================

#ifndef NET_DIAG_H
#define NET_DIAG_H

namespace NetDiag {

bool Init();
void Shutdown();
void LogStats();

} // namespace NetDiag

#endif // NET_DIAG_H
