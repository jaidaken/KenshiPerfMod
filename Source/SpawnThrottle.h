#pragma once

namespace SpawnThrottle
{
    // Install hooks. Call after KenshiLib is initialized.
    void Init();
}

// Called from profiler to feed frame time for dynamic budget
void SpawnThrottle_UpdateFrameTime(float frameMs);
