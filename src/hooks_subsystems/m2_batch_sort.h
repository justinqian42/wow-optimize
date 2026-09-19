#pragma once

// The opaque M2 batch sort in sub_821A20. See m2_batch_sort.cpp.
namespace M2BatchSort {
bool Init();
void Shutdown();
void LogStats();
}
