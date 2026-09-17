#pragma once

#ifndef WOW_MEMORY_OPT_H
#define WOW_MEMORY_OPT_H

namespace WowMemoryOpt {
    // Call early in DllMain or MainThread before any allocations
    bool EnableLargeAddressAware();
    
    // Initialize async worker thread pool for MPQ/texture/model loading
    bool InitAsyncWorkerPool();
    void ShutdownAsyncWorkerPool();
    
    // Apply aggressive memory optimizations within 32-bit limits
    bool ApplyMemoryOptimizations();
    
    // Stats
    void DumpStats();
}

#endif
