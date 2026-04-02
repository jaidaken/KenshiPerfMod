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
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>

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

// processKillList (only sub-hook that actually fires)
static __int64 s_killListAccum = 0;

// ============================================================
// Running stats for summary report
// ============================================================
static double s_sumTotal = 0;
static float s_maxTotal = 0;
static int s_maxCharCount = 0;
static int s_spikeCount = 0;      // frames > 16.6ms
static int s_bigSpikeCount = 0;   // frames > 33.3ms
static double s_sumSpatialQuery = 0;
static int s_totalSpatialQueries = 0;
static float s_maxSpatialQueryFrame = 0;
static int s_activeFrameCount = 0; // non-paused frames
static float s_maxGameSpeed = 0;

// ============================================================
// Hook originals
// ============================================================
static void (*mainLoop_orig)(GameWorld*, float) = NULL;
static void (*processKillList_orig)(GameWorld*, bool) = NULL;

// Spatial query hooks (Phase 1 profiling)
static void (*getObjectsWithinSphere_orig)(GameWorld*, lektor<RootObject*>&,
    const Ogre::Vector3&, float, itemType, int, RootObject*) = NULL;
static void (*getCharactersWithinSphere_orig)(GameWorld*, lektor<RootObject*>&,
    const Ogre::Vector3&, float, float, float, int, int, RootObject*) = NULL;

// ============================================================
// Hook implementations
// ============================================================

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

static void processKillList_hook(GameWorld* self, bool forceImmediate)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    processKillList_orig(self, forceImmediate);
    QueryPerformanceCounter(&end);
    s_killListAccum += (end.QuadPart - start.QuadPart);
}

static void mainLoop_hook(GameWorld* self, float time)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    // Reset per-frame accumulators
    s_spatialQueryAccum = 0;
    s_spatialQueryCount = 0;
    s_killListAccum = 0;

    mainLoop_orig(self, time);

    QueryPerformanceCounter(&end);
    __int64 frameTotal = end.QuadPart - start.QuadPart;

    float totalMs = (float)TicksToMs(frameTotal);
    float spatialMs = (float)TicksToMs(s_spatialQueryAccum);
    float killListMs = (float)TicksToMs(s_killListAccum);
    float gameLogicMs = totalMs - killListMs;

    // Read game state (no hooks needed - just reading memory)
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
            << camX << "," << camY << "," << camZ << "\n";

        if (s_frameCount % 300 == 0)
            s_csvFile.flush();
    }

    // Update running stats (skip paused frames)
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

    // Update overlay
    PerfOverlay::Update(totalMs, gameLogicMs, spatialMs, (float)s_spatialQueryCount, killListMs, 0, charCount);
    PerfOverlay::CheckToggleKey();

    // Write periodic summary every 1000 frames (survives crashes)
    if (s_frameCount > 0 && s_frameCount % 1000 == 0)
    {
        Profiling::WriteSummary();
    }

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
            s_csvFile << "frame,total_ms,gameLogic_ms,spatialQuery_ms,spatialQuery_count,killList_ms,char_count,game_speed,paused,cam_x,cam_y,cam_z\n";
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
    f << "Active frames:   " << s_activeFrameCount << " (paused frames excluded from averages)\n";
    f << "Max characters:  " << s_maxCharCount << "\n";
    f << "Max game speed:  " << s_maxGameSpeed << "x\n";
    f << "\n";

    f << "--- Frame Time (active frames only) ---\n";
    f << "  Average: " << avgTotal << " ms (" << avgFps << " fps)\n";
    f << "  Worst:   " << s_maxTotal << " ms\n";
    f << "  Below 60fps: " << s_spikeCount << " frames (" << ((float)s_spikeCount / frames * 100.0f) << "%)\n";
    f << "  Below 30fps: " << s_bigSpikeCount << " frames (" << ((float)s_bigSpikeCount / frames * 100.0f) << "%)\n";
    f << "\n";

    f << "--- Spatial Queries (Phase 1 target) ---\n";
    f << "  Total calls:     " << s_totalSpatialQueries << "\n";
    f << "  Avg calls/frame: " << avgSpatialCount << "\n";
    f << "  Avg time/frame:  " << avgSpatial << " ms (" << spatialPct << "% of frame)\n";
    f << "  Worst frame:     " << s_maxSpatialQueryFrame << " ms\n";
    if (spatialPct > 10.0)
        f << "  >> HIGH IMPACT: Spatial grid (Phase 1) will significantly help.\n";
    else if (spatialPct > 2.0)
        f << "  >> Moderate: Spatial grid (Phase 1) will help.\n";
    else
        f << "  >> Low: Spatial queries are not a major bottleneck.\n";
    f << "\n";

    f << "--- Character Updates (Phase 2-4 target) ---\n";
    f << "  Characters loaded: " << s_maxCharCount << "\n";
    f << "  NOTE: Character update time cannot be isolated (function is inlined).\n";
    f << "  Total frame time scales with character count.\n";
    f << "  At " << s_maxCharCount << " characters, avg frame is " << avgTotal << " ms.\n";
    if (s_maxCharCount > 0)
        f << "  Per-character estimate: " << (avgTotal / s_maxCharCount * 1000.0) << " us/char\n";
    f << "\n";

    f << "--- Spikes (Phase 5 target) ---\n";
    if (s_bigSpikeCount > 0)
        f << "  " << s_bigSpikeCount << " frames exceeded 33ms. Daily update spreading will help.\n";
    else
        f << "  No severe spikes detected.\n";
    f << "\n";

    f << "--- What hooks are working ---\n";
    f << "  mainLoop_GPUSensitiveStuff: YES (total frame time)\n";
    f << "  getObjectsWithinSphere:     YES (spatial query timing)\n";
    f << "  getCharactersWithinSphere:  YES (spatial query timing)\n";
    f << "  processKillList:            YES\n";
    f << "  charsUpdate:                NO (inlined by compiler)\n";
    f << "  charsUpdateUT:              NO (inlined by compiler)\n";
    f << "  processSysMessages:         NO (inlined by compiler)\n";
    f << "  dailyUpdates:               NO (inlined by compiler)\n";
    f << "  Character::update:          UNSAFE (crashes on hook)\n";
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

    // Main frame loop (works - this is our primary timing source)
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        (void*)mainLoop_hook, (void**)&mainLoop_orig);
    PerfLog::InfoF("Hook mainLoop: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // Spatial queries - Phase 1 decision data
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::getObjectsWithinSphere),
        (void*)getObjectsWithinSphere_hook, (void**)&getObjectsWithinSphere_orig);
    PerfLog::InfoF("Hook getObjectsWithinSphere: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::getCharactersWithinSphere),
        (void*)getCharactersWithinSphere_hook, (void**)&getCharactersWithinSphere_orig);
    PerfLog::InfoF("Hook getCharactersWithinSphere: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // processKillList (confirmed working)
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processKillList),
        (void*)processKillList_hook, (void**)&processKillList_orig);
    PerfLog::InfoF("Hook processKillList: %s", status == KenshiLib::SUCCESS ? "OK" : "FAIL");

    // NOTE: The following cannot be hooked (inlined or unsafe):
    // - charsUpdate, charsUpdateUT, processSysMessages, dailyUpdates (inlined)
    // - Character::update (prologue too short, crashes MinHook)
    // - Town::update, Building::update (likely same issue)
    // We measure these indirectly through total frame time.
    PerfLog::Info("Inlined functions skipped. Using frame subtraction for estimates.");

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
