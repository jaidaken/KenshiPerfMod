#pragma once

#include <string>

namespace PerfSettings
{
    void Init();

    // Phase 0: Infrastructure
    bool GetEnableProfiling();
    void SetEnableProfiling(bool value);
    std::string GetProfileOutputPath();
    int GetWorkerThreadCount();

    // Phase 1: Spatial Grid
    bool GetEnableSpatialGrid();
    float GetGridCellSize();

    // Phase 2: Simulation LOD
    bool GetEnableSimulationLOD();
    float GetNearDistance();
    float GetMediumDistance();
    int GetFarUpdateInterval();
    int GetPathRequestsPerFrame();

    // Phase 3: Parallel Threaded Updates
    bool GetEnableParallelThreadedUpdates();

    // Phase 4: Parallel Full Updates
    bool GetEnableParallelCharUpdate();
    bool GetEnablePrefetch();

    // Phase 5: Daily Update Spreading
    bool GetEnableDailyUpdateSpreading();
    int GetDailyUpdateBatchSize();

    // Phase 6: Parallel Buildings
    bool GetEnableParallelBuildings();

    // Phase 7: Mod Load Optimizer
    bool GetEnableModLoadOptimizer();
    bool GetEnablePathInterning();

    // Phase 8: OGRE Animation Threading
    bool GetEnableOgreAnimThreading();
}
