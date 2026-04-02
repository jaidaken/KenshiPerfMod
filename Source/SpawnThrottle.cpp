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

// INVESTIGATION RESULTS:
// Two spawn sources cause frame spikes:
//   1. Platoon::activate() - called from loadAllPlatoons() in a loop (8-19 platoons = 200+ chars)
//   2. populateMapArea_nonPermanent() - spawns ambient NPCs (up to 68 chars, no platoon)
// Both call createRandomCharacter() synchronously. All chars added to update list same frame.
//
// APPROACH: Throttle at TWO levels:
//   A. Hook Platoon::activate() - defer excess platoon activations
//   B. Hook mainThreadUpdate() - process deferred platoons + track per-frame budget
//
// We can't safely defer individual createRandomCharacter() calls (callers expect
// a valid pointer back). But we CAN defer entire platoon activations and the
// populateMapArea_nonPermanent calls since those are fire-and-forget loops.

static std::deque<Platoon*> s_deferredPlatoons;
static boost::mutex s_deferredMutex;
static int s_activatedThisFrame = 0;
static int s_charsCreatedThisFrame = 0;
static float s_lastFrameMs = 0;

// Originals
static void (*platoonActivate_orig)(Platoon*) = NULL;
static void (*mainThreadUpdate_orig)(RootObjectFactory*) = NULL;
static RootObject* (*createRandomCharacter_orig)(RootObjectFactory*, Faction*,
    Ogre::Vector3, RootObjectContainer*, GameData*, Building*, float) = NULL;
static void (*populateMapArea_orig)(GameWorld*, ZoneMap*, int, bool) = NULL;

// Forward declare ZoneMap (not fully defined in our headers)
class ZoneMap;

static int GetPlatoonBudget()
{
    int max = PerfSettings::GetMaxSpawnsPerFrame();

    // Dynamic: fast frames allow more, slow frames less
    if (s_lastFrameMs < 8.0f)
        max = max * 2;
    else if (s_lastFrameMs > 16.0f)
        max = max / 2;

    if (max < 2)
        max = 2;

    return max;
}

// Count characters as they're created (for budget tracking, not throttling)
static RootObject* createRandomCharacter_hook(RootObjectFactory* self, Faction* faction,
    Ogre::Vector3 position, RootObjectContainer* owner, GameData* tmpl, Building* home, float age)
{
    ++s_charsCreatedThisFrame;
    return createRandomCharacter_orig(self, faction, position, owner, tmpl, home, age);
}

// Throttle platoon activation
static void platoonActivate_hook(Platoon* self)
{
    int budget = GetPlatoonBudget();

    if (s_activatedThisFrame < budget)
    {
        ++s_activatedThisFrame;
        platoonActivate_orig(self);
        return;
    }

    // Over budget - defer
    {
        boost::lock_guard<boost::mutex> lock(s_deferredMutex);
        s_deferredPlatoons.push_back(self);
    }
}

// Throttle ambient NPC population - defer if we've already spawned a lot this frame
static void populateMapArea_hook(GameWorld* self, ZoneMap* map, int howMany, bool rePopulationMode)
{
    // If we've already created many chars this frame, skip and retry next frame.
    // populateMapArea is called periodically so skipping one call is safe.
    if (s_charsCreatedThisFrame > PerfSettings::GetMaxSpawnsPerFrame())
    {
        PerfLog::InfoF("SpawnThrottle: deferred populateMapArea (%d chars already this frame)", s_charsCreatedThisFrame);
        return; // skip this frame, zone manager will retry
    }

    populateMapArea_orig(self, map, howMany, rePopulationMode);
}

// Process deferred platoons at frame start
static void mainThreadUpdate_hook(RootObjectFactory* self)
{
    // Reset per-frame counters
    s_activatedThisFrame = 0;
    s_charsCreatedThisFrame = 0;

    // Process deferred platoons
    int budget = GetPlatoonBudget();
    int processed = 0;

    {
        boost::lock_guard<boost::mutex> lock(s_deferredMutex);

        while (!s_deferredPlatoons.empty() && processed < budget)
        {
            Platoon* p = s_deferredPlatoons.front();
            s_deferredPlatoons.pop_front();

            platoonActivate_orig(p);
            ++processed;
            ++s_activatedThisFrame;
        }
    }

    if (processed > 0)
    {
        int remaining = 0;
        {
            boost::lock_guard<boost::mutex> lock(s_deferredMutex);
            remaining = (int)s_deferredPlatoons.size();
        }
        PerfLog::InfoF("SpawnThrottle: activated %d deferred platoons, %d remaining", processed, remaining);
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

    KenshiLib::HookStatus status;

    // Hook Platoon::activate - throttle platoon activation rate
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&Platoon::activate),
        (void*)platoonActivate_hook,
        (void**)&platoonActivate_orig);
    PerfLog::InfoF("SpawnThrottle: hook Platoon::activate %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Hook createRandomCharacter - count chars per frame for budget tracking
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomCharacter),
        (void*)createRandomCharacter_hook,
        (void**)&createRandomCharacter_orig);
    PerfLog::InfoF("SpawnThrottle: hook createRandomCharacter %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Hook populateMapArea_nonPermanent - defer ambient spawns when busy
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::populateMapArea_nonPermanent),
        (void*)populateMapArea_hook,
        (void**)&populateMapArea_orig);
    PerfLog::InfoF("SpawnThrottle: hook populateMapArea %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Hook mainThreadUpdate - process deferred platoons + reset counters
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::mainThreadUpdate),
        (void*)mainThreadUpdate_hook,
        (void**)&mainThreadUpdate_orig);
    PerfLog::InfoF("SpawnThrottle: hook mainThreadUpdate %s",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    PerfLog::InfoF("SpawnThrottle: enabled (max %d platoons/frame, dynamic)",
        PerfSettings::GetMaxSpawnsPerFrame());
}
