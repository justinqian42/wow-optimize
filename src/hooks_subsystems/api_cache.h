#pragma once

#ifndef API_CACHE_H
#define API_CACHE_H

#include <windows.h>

struct lua_State;

namespace ApiCache {

bool Init();
void Shutdown();
void LogStats();
void ClearCache();

struct Stats {
    long itemHits;
    long itemMisses;
    long itemBypassed;   // argument too long to key on, or result too large to store
    bool active;
};

Stats GetStats();

} // namespace ApiCache

#endif