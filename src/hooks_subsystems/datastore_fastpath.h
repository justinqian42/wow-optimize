#pragma once

// ============================================================================
// Module: datastore_fastpath.h
// ============================================================================









#ifndef DATASTORE_FASTPATH_H
#define DATASTORE_FASTPATH_H

// CDataStore Fast Path Optimizations
// Hooks sub_47B3C0/sub_47B0A0/sub_47B340/sub_47AFE0/sub_47B100/sub_47B400
// TLS-cached buffer pointer to eliminate repeated base arithmetic.
// Total: ~4179 xrefs across network packet processing hot paths.

bool InitDataStoreFastPath();
void ShutdownDataStoreFastPath();
// Called from the periodic report. Shutdown is not a place a counter can be
// printed from: this process leaves through TerminateProcess.
void LogDataStoreStats();
void DumpDataStoreStats();

#endif // DATASTORE_FASTPATH_H
