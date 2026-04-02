#include "Profiling.h"
#include "Settings.h"
#include "Overlay.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>

#include <fstream>
#include <string>

static bool s_enabled = false;
static std::ofstream s_csvFile;
static LARGE_INTEGER s_freq;
static int s_frameCount = 0;

// Per-frame timing data (in QPC ticks)
struct FrameTiming
{
    __int64 frameStart;
    __int64 frameEnd;
    __int64 charsUpdate_start;
    __int64 charsUpdate_end;
    __int64 charsUpdateUT_start;
    __int64 charsUpdateUT_end;
    __int64 processSysMessages_start;
    __int64 processSysMessages_end;
    __int64 processKillList_start;
    __int64 processKillList_end;
    __int64 dailyUpdates_start;
    __int64 dailyUpdates_end;
    bool dailyUpdatesRan;
};

static FrameTiming s_current;

static double TicksToMs(__int64 ticks)
{
    return (double)ticks * 1000.0 / (double)s_freq.QuadPart;
}

// Hook originals
static void (*mainLoop_orig)(GameWorld*, float) = NULL;
static void (*charsUpdate_orig)(GameWorld*) = NULL;
static void (*charsUpdateUT_orig)(GameWorld*) = NULL;
static void (*processSysMessages_orig)(GameWorld*) = NULL;
static void (*processKillList_orig)(GameWorld*, bool) = NULL;
static void (*dailyUpdates_orig)(GameWorld*) = NULL;

// Hook implementations
static void mainLoop_hook(GameWorld* self, float time)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.frameStart = start.QuadPart;
    s_current.dailyUpdatesRan = false;

    mainLoop_orig(self, time);

    QueryPerformanceCounter(&end);
    s_current.frameEnd = end.QuadPart;

    // Compute frame timings
    __int64 frameTotal = s_current.frameEnd - s_current.frameStart;
    __int64 charsTotal = s_current.charsUpdate_end - s_current.charsUpdate_start;
    __int64 charsUTTotal = s_current.charsUpdateUT_end - s_current.charsUpdateUT_start;
    __int64 sysMsg = s_current.processSysMessages_end - s_current.processSysMessages_start;
    __int64 killList = s_current.processKillList_end - s_current.processKillList_start;
    __int64 daily = s_current.dailyUpdatesRan
        ? (s_current.dailyUpdates_end - s_current.dailyUpdates_start) : 0;

    float totalMs = (float)TicksToMs(frameTotal);
    float charsMs = (float)TicksToMs(charsTotal);
    float charsUTMs = (float)TicksToMs(charsUTTotal);
    float sysMsgMs = (float)TicksToMs(sysMsg);
    float killListMs = (float)TicksToMs(killList);
    float dailyMs = (float)TicksToMs(daily);

    // Get character count
    int charCount = 0;
    if (ou && ou->initialized)
        charCount = (int)ou->getCharacterUpdateList().size();

    // Write frame data to CSV
    if (s_csvFile.is_open())
    {
        s_csvFile
            << s_frameCount << ","
            << totalMs << ","
            << charsMs << ","
            << charsUTMs << ","
            << sysMsgMs << ","
            << killListMs << ","
            << dailyMs << ","
            << charCount << "\n";

        // Flush periodically to avoid data loss on crash
        if (s_frameCount % 300 == 0)
            s_csvFile.flush();
    }

    // Update overlay (main thread, safe for MyGUI)
    PerfOverlay::Update(totalMs, charsMs, charsUTMs, sysMsgMs, killListMs, dailyMs, charCount);
    PerfOverlay::CheckToggleKey();

    ++s_frameCount;
}

static void charsUpdate_hook(GameWorld* self)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.charsUpdate_start = start.QuadPart;

    charsUpdate_orig(self);

    QueryPerformanceCounter(&end);
    s_current.charsUpdate_end = end.QuadPart;
}

static void charsUpdateUT_hook(GameWorld* self)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.charsUpdateUT_start = start.QuadPart;

    charsUpdateUT_orig(self);

    QueryPerformanceCounter(&end);
    s_current.charsUpdateUT_end = end.QuadPart;
}

static void processSysMessages_hook(GameWorld* self)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.processSysMessages_start = start.QuadPart;

    processSysMessages_orig(self);

    QueryPerformanceCounter(&end);
    s_current.processSysMessages_end = end.QuadPart;
}

static void processKillList_hook(GameWorld* self, bool forceImmediate)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.processKillList_start = start.QuadPart;

    processKillList_orig(self, forceImmediate);

    QueryPerformanceCounter(&end);
    s_current.processKillList_end = end.QuadPart;
}

static void dailyUpdates_hook(GameWorld* self)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    s_current.dailyUpdates_start = start.QuadPart;

    dailyUpdates_orig(self);

    QueryPerformanceCounter(&end);
    s_current.dailyUpdates_end = end.QuadPart;
    s_current.dailyUpdatesRan = true;
}

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
            s_csvFile << "frame,total_ms,charsUpdate_ms,charsUpdateUT_ms,"
                      << "processSysMessages_ms,processKillList_ms,dailyUpdates_ms,char_count\n";
            DebugLog("[KenshiPerfMod] Profiling enabled, writing to " + path);
        }
        else
        {
            ErrorLog("[KenshiPerfMod] Could not open profile output: " + path);
            s_enabled = false;
        }
    }
}

void Profiling::Shutdown()
{
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

    // mainLoop_GPUSensitiveStuff is virtual - GetRealAddress doesn't work for virtuals.
    // Use the non-virtual _NV_ variant which has the same RVA but is a regular function.
    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        (void*)mainLoop_hook, (void**)&mainLoop_orig);

    // These are all non-virtual, use GetRealAddress
    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::charsUpdate),
        (void*)charsUpdate_hook, (void**)&charsUpdate_orig);

    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::charsUpdateUT),
        (void*)charsUpdateUT_hook, (void**)&charsUpdateUT_orig);

    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processSysMessages),
        (void*)processSysMessages_hook, (void**)&processSysMessages_orig);

    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processKillList),
        (void*)processKillList_hook, (void**)&processKillList_orig);

    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::dailyUpdates),
        (void*)dailyUpdates_hook, (void**)&dailyUpdates_orig);

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
