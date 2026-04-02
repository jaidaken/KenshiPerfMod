#include "Log.h"
#include "Settings.h"
#include "ThreadPool.h"
#include "TLSMessageQueue.h"
#include "Profiling.h"
#include "Overlay.h"
#include "SpawnThrottle.h"

#include <kenshi/Kenshi.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// Plugin entry point - called by RE_Kenshi's plugin loader.
// RE_Kenshi looks up the C++ mangled name ?startPlugin@@YAXXZ via GetProcAddress.
__declspec(dllexport) void startPlugin()
{
    // Start our own log first
    PerfLog::Init();
    PerfLog::Info("KenshiPerfMod starting");
    DebugLog("[KenshiPerfMod] Starting KenshiPerfMod");

    // Load configuration
    PerfSettings::Init();

    // Log system info
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    PerfLog::InfoF("System: %d logical processors", sysInfo.dwNumberOfProcessors);
    PerfLog::InfoF("KenshiLib version: %s", KenshiLib::GetKenshiVersion().ToString().c_str());

    // Initialize TLS slots for message queues (needed before any hooks run)
    TLSQueues::Init();
    PerfLog::Info("TLS message queues initialized");

    // Start thread pool
    int workers = PerfThreadPool::Init(PerfSettings::GetWorkerThreadCount());
    PerfLog::InfoF("Thread pool started: %d workers", workers);
    DebugLog("[KenshiPerfMod] Thread pool: " + std::to_string((long long)workers) + " workers");

    // Initialize and install profiler hooks
    Profiling::Init();
    if (Profiling::IsEnabled())
    {
        Profiling::InstallHooks();
        PerfLog::Info("Profiling hooks installed");
    }
    else
    {
        PerfLog::Info("Profiling disabled");
    }

    // Log enabled features
    PerfLog::InfoF("Features: SpawnThrottle=%s(%d) SimLOD=%s ParallelThreaded=%s ParallelChar=%s DailySpreading=%s",
        PerfSettings::GetEnableSpawnThrottling() ? "on" : "off",
        PerfSettings::GetMaxSpawnsPerFrame(),
        PerfSettings::GetEnableSimulationLOD() ? "on" : "off",
        PerfSettings::GetEnableParallelThreadedUpdates() ? "on" : "off",
        PerfSettings::GetEnableParallelCharUpdate() ? "on" : "off",
        PerfSettings::GetEnableDailyUpdateSpreading() ? "on" : "off");
    PerfLog::InfoF("Features: ParallelBuildings=%s ModLoadOpt=%s PathIntern=%s OgreAnimThread=%s",
        PerfSettings::GetEnableParallelBuildings() ? "on" : "off",
        PerfSettings::GetEnableModLoadOptimizer() ? "on" : "off",
        PerfSettings::GetEnablePathInterning() ? "on" : "off",
        PerfSettings::GetEnableOgreAnimThreading() ? "on" : "off");

    // Initialize overlay (hooks MainBarGUI constructor, creates widget when in-game)
    PerfOverlay::Init();

    // Phase 1: Spawn Throttling
    SpawnThrottle::Init();
    // TODO Phase 2: SimulationLOD::Init()
    // TODO Phase 3: ParallelThreadedUpdates::Init()
    // TODO Phase 4: ParallelCharsUpdate::Init()
    // TODO Phase 5: DeferredUpdates::Init()
    // TODO Phase 6: ParallelBuildings::Init()
    // TODO Phase 7: ModLoadOptimizer::Init()
    // TODO Phase 8: OgreAnimThreading::Init()

    PerfLog::Info("Initialization complete");
    DebugLog("[KenshiPerfMod] Initialization complete");
}
