#include "Profiling.h"
#include "Settings.h"
#include "Overlay.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Platoon.h>
#include <kenshi/ResourceLoader.h>
#include <kenshi/PhysicsActual.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>
#include "SpawnThrottle.h"

#include <fstream>
#include <string>

static bool s_enabled = false;
static std::ofstream s_csvFile;
static LARGE_INTEGER s_freq;
static int s_frameCount = 0;

static double TicksToMs(__int64 ticks)
{
    return (double)ticks * 1000.0 / (double)s_freq.QuadPart;
}

// ============================================================
// Per-frame accumulators (reset at frame start)
// ============================================================

// Spatial queries (Phase 1)
static __int64 s_spatialQueryAccum = 0;
static int s_spatialQueryCount = 0;

// Kill list
static __int64 s_killListAccum = 0;

// Zone loading / spawning
static int s_charsSpawnedThisFrame = 0;
static int s_squadsSpawnedThisFrame = 0;
static int s_platoonsActivatedThisFrame = 0;
static int s_meshLoadsThisFrame = 0;
static int s_terrainLoadsThisFrame = 0;
static int s_charsAddedToUpdateList = 0;

// ============================================================
// Running stats for summary
// ============================================================
static double s_sumTotal = 0;
static float s_maxTotal = 0;
static int s_maxCharCount = 0;
static int s_spikeCount = 0;
static int s_bigSpikeCount = 0;
static double s_sumSpatialQuery = 0;
static int s_totalSpatialQueries = 0;
static float s_maxSpatialQueryFrame = 0;
static int s_activeFrameCount = 0;
static float s_maxGameSpeed = 0;
static int s_totalCharsSpawned = 0;
static int s_totalSquadsSpawned = 0;
static int s_totalPlatoonsActivated = 0;
static int s_totalMeshLoads = 0;
static int s_totalTerrainLoads = 0;

// ============================================================
// Hook originals
// ============================================================
static void (*mainLoop_orig)(GameWorld*, float) = NULL;
static void (*processKillList_orig)(GameWorld*, bool) = NULL;

// Spatial queries
static void (*getObjectsWithinSphere_orig)(GameWorld*, lektor<RootObject*>&,
    const Ogre::Vector3&, float, itemType, int, RootObject*) = NULL;
static void (*getCharactersWithinSphere_orig)(GameWorld*, lektor<RootObject*>&,
    const Ogre::Vector3&, float, float, float, int, int, RootObject*) = NULL;

// Zone loading hooks
static RootObject* (*createRandomCharacter_orig)(RootObjectFactory*, Faction*,
    Ogre::Vector3, RootObjectContainer*, GameData*, Building*, float) = NULL;
static Platoon* (*createRandomSquad_orig)(RootObjectFactory*, Faction*, Ogre::Vector3,
    TownBase*, int, Building*, GameData*, RootObjectContainer*, AreaBiomeGroup*,
    Platoon*, bool, const hand&, TownBase*, float, SquadType, bool) = NULL;
static void (*platoonActivate_orig)(Platoon*) = NULL;
static void (*addToUpdateListMain_orig)(GameWorld*, Character*) = NULL;
static void (*loadTerrain_orig)(PhysicsInterface*, TerrainSector*) = NULL;

// ============================================================
// Hook implementations
// ============================================================

// --- Spatial queries ---
static void getObjectsWithinSphere_hook(GameWorld* self, lektor<RootObject*>& results,
    const Ogre::Vector3& spherePos, float radius, itemType type, int maxNumber, RootObject* skip)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    getObjectsWithinSphere_orig(self, results, spherePos, radius, type, maxNumber, skip);
    QueryPerformanceCounter(&end);
    s_spatialQueryAccum += (end.QuadPart - start.QuadPart);
    ++s_spatialQueryCount;
}

static void getCharactersWithinSphere_hook(GameWorld* self, lektor<RootObject*>& results,
    const Ogre::Vector3& spherePos, float farRadius, float nearRadius,
    float always, int maxFar, int maxNear, RootObject* skip)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    getCharactersWithinSphere_orig(self, results, spherePos, farRadius, nearRadius, always, maxFar, maxNear, skip);
    QueryPerformanceCounter(&end);
    s_spatialQueryAccum += (end.QuadPart - start.QuadPart);
    ++s_spatialQueryCount;
}

// --- Kill list ---
static void processKillList_hook(GameWorld* self, bool forceImmediate)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    processKillList_orig(self, forceImmediate);
    QueryPerformanceCounter(&end);
    s_killListAccum += (end.QuadPart - start.QuadPart);
}

// --- Zone loading counters ---
static RootObject* createRandomCharacter_hook(RootObjectFactory* self, Faction* faction,
    Ogre::Vector3 position, RootObjectContainer* owner, GameData* tmpl, Building* home, float age)
{
    ++s_charsSpawnedThisFrame;
    return createRandomCharacter_orig(self, faction, position, owner, tmpl, home, age);
}

static Platoon* createRandomSquad_hook(RootObjectFactory* self, Faction* faction,
    Ogre::Vector3 position, TownBase* homeTown, int maxnum, Building* home, GameData* squad,
    RootObjectContainer* ownr, AreaBiomeGroup* maparea, Platoon* activePlatoon,
    bool permanentsquad, const hand& AItarget, TownBase* targetTown, float sizeMultiplier,
    SquadType squadType, bool isJustARefresh)
{
    ++s_squadsSpawnedThisFrame;
    return createRandomSquad_orig(self, faction, position, homeTown, maxnum, home, squad,
        ownr, maparea, activePlatoon, permanentsquad, AItarget, targetTown, sizeMultiplier,
        squadType, isJustARefresh);
}

static void platoonActivate_hook(Platoon* self)
{
    ++s_platoonsActivatedThisFrame;
    PerfLog::InfoF("Platoon activated (frame %d)", s_frameCount);
    platoonActivate_orig(self);
}

static void addToUpdateListMain_hook(GameWorld* self, Character* character)
{
    ++s_charsAddedToUpdateList;
    addToUpdateListMain_orig(self, character);
}

static void loadTerrain_hook(PhysicsInterface* self, TerrainSector* t)
{
    ++s_terrainLoadsThisFrame;
    loadTerrain_orig(self, t);
}

// --- Main loop ---
static void mainLoop_hook(GameWorld* self, float time)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    // Reset per-frame accumulators
    s_spatialQueryAccum = 0;
    s_spatialQueryCount = 0;
    s_killListAccum = 0;
    s_charsSpawnedThisFrame = 0;
    s_squadsSpawnedThisFrame = 0;
    s_platoonsActivatedThisFrame = 0;
    s_meshLoadsThisFrame = 0;
    s_terrainLoadsThisFrame = 0;
    s_charsAddedToUpdateList = 0;

    mainLoop_orig(self, time);

    QueryPerformanceCounter(&end);
    __int64 frameTotal = end.QuadPart - start.QuadPart;

    float totalMs = (float)TicksToMs(frameTotal);
    float spatialMs = (float)TicksToMs(s_spatialQueryAccum);
    float killListMs = (float)TicksToMs(s_killListAccum);
    float gameLogicMs = totalMs - killListMs;

    // Read game state
    int charCount = 0;
    float gameSpeed = 0;
    bool paused = false;
    float camX = 0, camY = 0, camZ = 0;

    if (ou && ou->initialized)
    {
        charCount = (int)ou->getCharacterUpdateList().size();
        gameSpeed = ou->frameSpeedMult;
        paused = ou->paused;
        Ogre::Vector3 camPos = ou->getCameraPos();
        camX = camPos.x;
        camY = camPos.y;
        camZ = camPos.z;
    }

    // Write CSV
    if (s_csvFile.is_open())
    {
        s_csvFile
            << s_frameCount << ","
            << totalMs << ","
            << gameLogicMs << ","
            << spatialMs << ","
            << s_spatialQueryCount << ","
            << killListMs << ","
            << charCount << ","
            << gameSpeed << ","
            << (paused ? 1 : 0) << ","
            << s_charsSpawnedThisFrame << ","
            << s_squadsSpawnedThisFrame << ","
            << s_platoonsActivatedThisFrame << ","
            << s_terrainLoadsThisFrame << ","
            << s_charsAddedToUpdateList << ","
            << camX << "," << camY << "," << camZ << "\n";

        if (s_frameCount % 300 == 0)
            s_csvFile.flush();
    }

    // Update running stats (skip paused frames for averages)
    if (!paused)
    {
        s_sumTotal += totalMs;
        s_sumSpatialQuery += spatialMs;
        s_totalSpatialQueries += s_spatialQueryCount;
        ++s_activeFrameCount;
    }
    if (totalMs > s_maxTotal) s_maxTotal = totalMs;
    if (spatialMs > s_maxSpatialQueryFrame) s_maxSpatialQueryFrame = spatialMs;
    if (charCount > s_maxCharCount) s_maxCharCount = charCount;
    if (gameSpeed > s_maxGameSpeed) s_maxGameSpeed = gameSpeed;
    if (!paused && totalMs > 16.6f) ++s_spikeCount;
    if (!paused && totalMs > 33.3f) ++s_bigSpikeCount;
    s_totalCharsSpawned += s_charsSpawnedThisFrame;
    s_totalSquadsSpawned += s_squadsSpawnedThisFrame;
    s_totalPlatoonsActivated += s_platoonsActivatedThisFrame;
    s_totalTerrainLoads += s_terrainLoadsThisFrame;

    // Feed frame time to spawn throttle for dynamic budget
    SpawnThrottle_UpdateFrameTime(totalMs);

    // Update overlay with all profiler data
    PerfOverlay::Update(totalMs, gameLogicMs, spatialMs, (float)s_spatialQueryCount, killListMs,
        (float)s_charsSpawnedThisFrame, charCount, gameSpeed,
        s_platoonsActivatedThisFrame, s_terrainLoadsThisFrame);
    PerfOverlay::CheckToggleKey();

    // Periodic summary every 1000 frames
    if (s_frameCount > 0 && s_frameCount % 1000 == 0)
        Profiling::WriteSummary();

    ++s_frameCount;
}

// ============================================================
// Init / Shutdown / Summary
// ============================================================

void Profiling::Init()
{
    QueryPerformanceFrequency(&s_freq);
    s_enabled = PerfSettings::GetEnableProfiling();

    if (s_enabled)
    {
        std::string path = PerfSettings::GetProfileOutputPath();
        s_csvFile.open(path);
        if (s_csvFile.is_open())
        {
            s_csvFile << "frame,total_ms,gameLogic_ms,spatialQuery_ms,spatialQuery_count,"
                      << "killList_ms,char_count,game_speed,paused,"
                      << "chars_spawned,squads_spawned,platoons_activated,terrain_loads,chars_added,"
                      << "cam_x,cam_y,cam_z\n";
            DebugLog("[KenshiPerfMod] Profiling enabled, writing to " + path);
        }
        else
        {
            ErrorLog("[KenshiPerfMod] Could not open profile output: " + path);
            s_enabled = false;
        }
    }
}

void Profiling::WriteSummary()
{
    if (s_frameCount == 0)
        return;

    std::string path = PerfSettings::GetProfileOutputPath();
    size_t dot = path.rfind('.');
    std::string summaryPath = (dot != std::string::npos)
        ? path.substr(0, dot) + "_summary.txt"
        : path + "_summary.txt";

    std::ofstream f(summaryPath);
    if (!f.is_open())
        return;

    int frames = s_activeFrameCount > 0 ? s_activeFrameCount : 1;
    double avgTotal = s_sumTotal / frames;
    double avgFps = (avgTotal > 0.001) ? (1000.0 / avgTotal) : 0;
    double avgSpatial = s_sumSpatialQuery / frames;
    double avgSpatialCount = (double)s_totalSpatialQueries / frames;
    double spatialPct = (s_sumTotal > 0.001) ? (s_sumSpatialQuery / s_sumTotal * 100.0) : 0;

    f << "=== KenshiPerfMod Profiling Summary ===\n";
    f << "Total frames:    " << s_frameCount << "\n";
    f << "Active frames:   " << s_activeFrameCount << " (paused excluded)\n";
    f << "Max characters:  " << s_maxCharCount << "\n";
    f << "Max game speed:  " << s_maxGameSpeed << "x\n";
    f << "\n";

    f << "--- Frame Time ---\n";
    f << "  Average: " << avgTotal << " ms (" << avgFps << " fps)\n";
    f << "  Worst:   " << s_maxTotal << " ms\n";
    f << "  Below 60fps: " << s_spikeCount << " (" << ((float)s_spikeCount / frames * 100.0f) << "%)\n";
    f << "  Below 30fps: " << s_bigSpikeCount << " (" << ((float)s_bigSpikeCount / frames * 100.0f) << "%)\n";
    f << "\n";

    f << "--- Spatial Queries (Phase 1) ---\n";
    f << "  Total calls:     " << s_totalSpatialQueries << "\n";
    f << "  Avg calls/frame: " << avgSpatialCount << "\n";
    f << "  Avg time/frame:  " << avgSpatial << " ms (" << spatialPct << "%)\n";
    f << "  Worst frame:     " << s_maxSpatialQueryFrame << " ms\n";
    if (spatialPct > 10.0)
        f << "  >> HIGH: Spatial grid will significantly help.\n";
    else if (spatialPct > 2.0)
        f << "  >> Moderate: Spatial grid will help.\n";
    else
        f << "  >> Low: Not a major bottleneck.\n";
    f << "\n";

    f << "--- Character Updates (Phase 2-4) ---\n";
    f << "  Max characters: " << s_maxCharCount << "\n";
    if (s_maxCharCount > 0)
        f << "  Per-char estimate: " << (avgTotal / s_maxCharCount * 1000.0) << " us/char\n";
    f << "\n";

    f << "--- Zone Loading ---\n";
    f << "  Platoons activated: " << s_totalPlatoonsActivated << "\n";
    f << "  Characters spawned: " << s_totalCharsSpawned << "\n";
    f << "  Squads spawned:     " << s_totalSquadsSpawned << "\n";
    f << "  Terrain loads:      " << s_totalTerrainLoads << "\n";
    if (s_totalPlatoonsActivated > 0)
        f << "  Avg chars/platoon:  " << ((float)s_totalCharsSpawned / s_totalPlatoonsActivated) << "\n";
    if (s_bigSpikeCount > 0 && s_totalPlatoonsActivated > 0)
        f << "  >> Zone loading causes " << s_bigSpikeCount << " severe spikes. Spreading entity creation will help.\n";
    f << "\n";

    f << "--- Spikes (Phase 5) ---\n";
    if (s_bigSpikeCount > 0)
        f << "  " << s_bigSpikeCount << " frames exceeded 33ms.\n";
    else
        f << "  No severe spikes.\n";
    f << "\n";

    f << "Raw data: " << PerfSettings::GetProfileOutputPath() << "\n";
    f.close();

    PerfLog::InfoF("Summary written (%d frames, %d chars, %.1f avg ms)",
        s_frameCount, s_maxCharCount, avgTotal);
}

void Profiling::Shutdown()
{
    WriteSummary();
    if (s_csvFile.is_open())
    {
        s_csvFile.flush();
        s_csvFile.close();
    }
    s_enabled = false;
}

void Profiling::InstallHooks()
{
    if (!s_enabled)
        return;

    KenshiLib::HookStatus status;

    // Main frame loop
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        (void*)mainLoop_hook, (void**)&mainLoop_orig);
    PerfLog::InfoF("Hook mainLoop: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Spatial queries
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::getObjectsWithinSphere),
        (void*)getObjectsWithinSphere_hook, (void**)&getObjectsWithinSphere_orig);
    PerfLog::InfoF("Hook getObjectsWithinSphere: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::getCharactersWithinSphere),
        (void*)getCharactersWithinSphere_hook, (void**)&getCharactersWithinSphere_orig);
    PerfLog::InfoF("Hook getCharactersWithinSphere: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Kill list
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processKillList),
        (void*)processKillList_hook, (void**)&processKillList_orig);
    PerfLog::InfoF("Hook processKillList: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Zone loading: character spawning
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomCharacter),
        (void*)createRandomCharacter_hook, (void**)&createRandomCharacter_orig);
    PerfLog::InfoF("Hook createRandomCharacter: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomSquad),
        (void*)createRandomSquad_hook, (void**)&createRandomSquad_orig);
    PerfLog::InfoF("Hook createRandomSquad: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Zone loading: platoon activation
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&Platoon::activate),
        (void*)platoonActivate_hook, (void**)&platoonActivate_orig);
    PerfLog::InfoF("Hook Platoon::activate: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Zone loading: character registration
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::addToUpdateListMain),
        (void*)addToUpdateListMain_hook, (void**)&addToUpdateListMain_orig);
    PerfLog::InfoF("Hook addToUpdateListMain: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Zone loading: terrain physics
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&PhysicsInterface::loadTerrain),
        (void*)loadTerrain_hook, (void**)&loadTerrain_orig);
    PerfLog::InfoF("Hook loadTerrain: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    DebugLog("[KenshiPerfMod] Profiling hooks installed");
}

bool Profiling::IsEnabled()
{
    return s_enabled;
}

__int64 Profiling::GetTimestamp()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double Profiling::TimestampToMs(__int64 start, __int64 end)
{
    return (double)(end - start) * 1000.0 / (double)s_freq.QuadPart;
}
