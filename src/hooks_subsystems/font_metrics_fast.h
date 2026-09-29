#pragma once

#include <cstdint>

bool InstallFontMetricsFast();
void ShutdownFontMetricsFast();
void FontMetrics_OnFrame();
void FontMetrics_LogStats();
extern "C" void FontMetrics_GetStats(long* widthCalls, long* heightCalls);
