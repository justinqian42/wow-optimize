#pragma once

// Combat log optimizer.
// 1. Retention time increase (300 -> 1800 sec)
// 2. Periodic CombatLogClearEntries from C level

#ifndef COMBATLOG_OPTIMIZE_H
#define COMBATLOG_OPTIMIZE_H

#include <windows.h>
#include <cstdint>

namespace CombatLogOpt {

bool Init();
void OnFrame(DWORD mainThreadId);
void Shutdown();
void SetCombat(bool active);
void ProcessUnifiedFrameTicks(int luaState, DWORD mainThreadId);

struct PoolStats {
    int  poolSize;
    int  poolUsed;
    bool poolActive;
};

PoolStats GetPoolStats();

} // namespace CombatLogOpt

#endif