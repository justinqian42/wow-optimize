#pragma once

// SavedVariables Async Writer
// Offloads SV serialization to background thread so logout/reload
// doesn't block the main thread on large addon data writes.
bool InstallSavedVarsAsync();
void ShutdownSavedVarsAsync();
void FlushSavedVarsAsyncSynchronously();

