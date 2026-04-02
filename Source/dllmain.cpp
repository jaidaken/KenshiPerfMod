#include "Settings.h"
#include "ThreadPool.h"
#include "TLSMessageQueue.h"
#include "Profiling.h"

#include <kenshi/Kenshi.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// Plugin entry point - called by RE_Kenshi's plugin loader.
// The mangled name ?startPlugin@@YAXXZ is resolved by GetProcAddress in Plugins.cpp.
void startPlugin()
{
    DebugLog("[KenshiPerfMod] Starting KenshiPerfMod");

    // Load configuration
    PerfSettings::Init();

    // Initialize TLS slots for message queues (needed before any hooks run)
    TLSQueues::Init();

    // Start thread pool
    int workers = PerfThreadPool::Init(PerfSettings::GetWorkerThreadCount());
    DebugLog("[KenshiPerfMod] Thread pool: " + std::to_string((long long)workers) + " workers");

    // Initialize and install profiler hooks
    Profiling::Init();
    if (Profiling::IsEnabled())
    {
        Profiling::InstallHooks();
    }

    // TODO Phase 1: SpatialGrid::Init()
    // TODO Phase 2: SimulationLOD::Init()
    // TODO Phase 3: ParallelThreadedUpdates::Init()
    // TODO Phase 4: ParallelCharsUpdate::Init()
    // TODO Phase 5: DeferredUpdates::Init()
    // TODO Phase 6: ParallelBuildings::Init()
    // TODO Phase 7: ModLoadOptimizer::Init()
    // TODO Phase 8: OgreAnimThreading::Init()

    DebugLog("[KenshiPerfMod] Initialization complete");
}
