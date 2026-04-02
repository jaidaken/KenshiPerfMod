#include "SpawnThrottle.h"
#include "Settings.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/RootObjectFactory.h>
#include <kenshi/Platoon.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>

#include <deque>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

// Profiling showed Platoon::activate() triggers batch character creation:
// Frame 989: 8 platoons -> 201 chars -> 260ms
// Frame 2591: 17 platoons -> 90 chars -> 468ms
//
// Strategy: hook Platoon::activate(). Allow N platoons per frame.
// Defer the rest to subsequent frames via mainThreadUpdate hook.
// This keeps each platoon's internal state consistent (either fully
// activated or waiting) while spreading the load.

static std::deque<Platoon*> s_deferredPlatoons;
static boost::mutex s_deferredMutex;
static int s_activatedThisFrame = 0;
static float s_lastFrameMs = 0;

static LARGE_INTEGER s_freq;

// Originals
static void (*platoonActivate_orig)(Platoon*) = NULL;
static void (*mainThreadUpdate_orig)(RootObjectFactory*) = NULL;

static int GetBudget()
{
    int max = PerfSettings::GetMaxSpawnsPerFrame();

    // Dynamic budget based on last frame time
    // At ~1.3ms per character and ~6 chars per platoon, each platoon costs ~8ms
    if (s_lastFrameMs < 8.0f)
        max = max * 2;    // headroom, go faster
    else if (s_lastFrameMs > 16.0f)
        max = max / 2;    // slow frame, be conservative

    if (max < 2)
        max = 2;

    return max;
}

static void platoonActivate_hook(Platoon* self)
{
    int budget = GetBudget();

    if (s_activatedThisFrame < budget)
    {
        ++s_activatedThisFrame;
        platoonActivate_orig(self);
        return;
    }

    // Over budget - defer to next frame
    boost::lock_guard<boost::mutex> lock(s_deferredMutex);
    s_deferredPlatoons.push_back(self);

    PerfLog::InfoF("SpawnThrottle: deferred platoon (budget %d, queued %d)",
        budget, (int)s_deferredPlatoons.size());
}

// Process deferred platoons at the start of each frame
static void mainThreadUpdate_hook(RootObjectFactory* self)
{
    // Reset per-frame counter
    s_activatedThisFrame = 0;

    // Process deferred platoons
    if (!s_deferredPlatoons.empty())
    {
        int budget = GetBudget();
        int processed = 0;

        boost::lock_guard<boost::mutex> lock(s_deferredMutex);

        while (!s_deferredPlatoons.empty() && processed < budget)
        {
            Platoon* p = s_deferredPlatoons.front();
            s_deferredPlatoons.pop_front();

            platoonActivate_orig(p);
            ++processed;
            ++s_activatedThisFrame;
        }

        if (processed > 0)
        {
            int remaining = (int)s_deferredPlatoons.size();
            PerfLog::InfoF("SpawnThrottle: activated %d deferred platoons, %d remaining",
                processed, remaining);
        }
    }

    // Run original mainThreadUpdate
    mainThreadUpdate_orig(self);
}

void SpawnThrottle_UpdateFrameTime(float frameMs)
{
    s_lastFrameMs = frameMs;
}

void SpawnThrottle::Init()
{
    if (!PerfSettings::GetEnableSpawnThrottling())
    {
        PerfLog::Info("SpawnThrottle: disabled");
        return;
    }

    QueryPerformanceFrequency(&s_freq);

    KenshiLib::HookStatus status;

    // Hook Platoon::activate - throttle how many platoons activate per frame
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&Platoon::activate),
        (void*)platoonActivate_hook,
        (void**)&platoonActivate_orig);
    PerfLog::InfoF("SpawnThrottle: hook Platoon::activate %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Hook mainThreadUpdate to process deferred platoons each frame
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::mainThreadUpdate),
        (void*)mainThreadUpdate_hook,
        (void**)&mainThreadUpdate_orig);
    PerfLog::InfoF("SpawnThrottle: hook mainThreadUpdate %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    PerfLog::InfoF("SpawnThrottle: enabled (max %d platoons/frame, dynamic)",
        PerfSettings::GetMaxSpawnsPerFrame());
}
