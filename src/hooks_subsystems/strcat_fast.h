#pragma once

#ifndef STRCAT_FAST_H
#define STRCAT_FAST_H

#include <cstdint>

bool InstallStrcatFast();
void StrcpyFast_GetStats(uint64_t* fast, uint64_t* fallback);

#endif // STRCAT_FAST_H
