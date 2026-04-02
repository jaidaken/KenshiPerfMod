#pragma once

#include <string>

namespace Profiling
{
    // Initialize profiler. Call once at startup after settings are loaded.
    void Init();

    // Shut down profiler. Flushes remaining data to CSV.
    void Shutdown();

    // Install hooks on game functions to measure their execution time.
    // Call after KenshiLib is initialized (RVAs loaded).
    void InstallHooks();

    // Returns true if profiling is active.
    bool IsEnabled();

    // Manual timing API for custom measurements.
    // Returns a high-resolution timestamp.
    __int64 GetTimestamp();

    // Converts timestamp delta to milliseconds.
    double TimestampToMs(__int64 start, __int64 end);
}
