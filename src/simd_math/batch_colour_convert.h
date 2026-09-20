#pragma once

// The colour block inside the M2 batch state setup, sub_81FB10.
// See batch_colour_convert.cpp.
namespace BatchColourConvert {
bool Init();
void Shutdown();
void LogStats();
}
