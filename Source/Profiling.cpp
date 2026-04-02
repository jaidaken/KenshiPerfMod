#include "Profiling.h"
#include "Settings.h"
#include "Overlay.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>
#include <Debug.h>

#include <fstream>
#include <string>

static bool s_enabled = false;
static std::ofstream s_csvFile;
static LARGE_INTEGER s_freq;
static int s_frameCount = 0;

// Running stats for summary report
static double s_sumTotal = 0, s_sumChars = 0, s_sumCharsUT = 0, s_sumSysMsg = 0, s_sumKillList = 0;
static float s_maxTotal = 0, s_maxChars = 0;
static int s_maxCharCount = 0;
static int s_spikeCount = 0; // frames > 16.6ms (below 60fps)

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

// Alternative: hook individual Character::update to measure total char update time
static void (*charUpdate_orig)(Character*) = NULL;
static __int64 s_charUpdateAccum = 0;
static int s_charUpdateCount = 0;
static void charUpdate_hook(Character* self)
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    charUpdate_orig(self);
    QueryPerformanceCounter(&end);
    s_charUpdateAccum += (end.QuadPart - start.QuadPart);
    ++s_charUpdateCount;
}
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
    // Reset sub-timings so stale values are obvious (show as 0)
    s_charUpdateAccum = 0;
    s_charUpdateCount = 0;
    s_current.charsUpdate_start = s_current.charsUpdate_end = start.QuadPart;
    s_current.charsUpdateUT_start = s_current.charsUpdateUT_end = start.QuadPart;
    s_current.processSysMessages_start = s_current.processSysMessages_end = start.QuadPart;
    s_current.processKillList_start = s_current.processKillList_end = start.QuadPart;
    s_current.dailyUpdates_start = s_current.dailyUpdates_end = start.QuadPart;

    mainLoop_orig(self, time);

    QueryPerformanceCounter(&end);
    s_current.frameEnd = end.QuadPart;

    // Compute frame timings
    __int64 frameTotal = s_current.frameEnd - s_current.frameStart;
    // Use accumulated Character::update() time (charsUpdate hook doesn't fire - likely inlined)
    __int64 charsTotal = s_charUpdateAccum;
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

    // Character count from our hook (more accurate than the update list size)
    int charCount = s_charUpdateCount;

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

    // Update running stats for summary report
    s_sumTotal += totalMs;
    s_sumChars += charsMs;
    s_sumCharsUT += charsUTMs;
    s_sumSysMsg += sysMsgMs;
    s_sumKillList += killListMs;
    if (totalMs > s_maxTotal) s_maxTotal = totalMs;
    if (charsMs > s_maxChars) s_maxChars = charsMs;
    if (charCount > s_maxCharCount) s_maxCharCount = charCount;
    if (totalMs > 16.6f) ++s_spikeCount;

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

static void WriteSummaryReport()
{
    if (s_frameCount == 0)
        return;

    std::string path = PerfSettings::GetProfileOutputPath();
    // Replace .csv with _summary.txt
    size_t dot = path.rfind('.');
    std::string summaryPath = (dot != std::string::npos)
        ? path.substr(0, dot) + "_summary.txt"
        : path + "_summary.txt";

    std::ofstream f(summaryPath);
    if (!f.is_open())
        return;

    double avgTotal = s_sumTotal / s_frameCount;
    double avgChars = s_sumChars / s_frameCount;
    double avgCharsUT = s_sumCharsUT / s_frameCount;
    double avgSysMsg = s_sumSysMsg / s_frameCount;
    double avgKillList = s_sumKillList / s_frameCount;
    double avgFps = (avgTotal > 0.001) ? (1000.0 / avgTotal) : 0;
    double charsPct = (avgTotal > 0.001) ? (avgChars / avgTotal * 100.0) : 0;

    f << "=== KenshiPerfMod Profiling Summary ===" << "\n";
    f << "\n";
    f << "Total frames recorded: " << s_frameCount << "\n";
    f << "Max characters loaded: " << s_maxCharCount << "\n";
    f << "\n";
    f << "--- Average Frame Breakdown ---" << "\n";
    f << "  Total frame:       " << avgTotal << " ms (" << avgFps << " fps)" << "\n";
    f << "  charsUpdate:       " << avgChars << " ms (" << charsPct << "% of frame)" << "\n";
    f << "  charsUpdateUT:     " << avgCharsUT << " ms" << "\n";
    f << "  processSysMessages:" << avgSysMsg << " ms" << "\n";
    f << "  processKillList:   " << avgKillList << " ms" << "\n";
    f << "\n";
    f << "--- Worst Frames ---" << "\n";
    f << "  Worst total frame: " << s_maxTotal << " ms" << "\n";
    f << "  Worst charsUpdate: " << s_maxChars << " ms" << "\n";
    f << "  Frames below 60fps:" << s_spikeCount << " (" << ((float)s_spikeCount / s_frameCount * 100.0f) << "%)" << "\n";
    f << "\n";
    f << "--- Analysis ---" << "\n";
    if (charsPct > 50.0)
        f << "  ** charsUpdate dominates frame time (" << charsPct << "%). Phases 2-4 (simulation LOD, parallel updates) will have high impact." << "\n";
    else if (charsPct > 20.0)
        f << "  charsUpdate is significant (" << charsPct << "%). Phases 2-4 will help." << "\n";
    else
        f << "  charsUpdate is small (" << charsPct << "%). Bottleneck may be elsewhere (rendering, physics, etc)." << "\n";

    if (s_spikeCount > s_frameCount * 0.05)
        f << "  ** High spike rate (" << s_spikeCount << " frames). Daily update spreading (Phase 5) and simulation LOD (Phase 2) will help." << "\n";

    f << "\n";
    f << "Raw data: " << PerfSettings::GetProfileOutputPath() << "\n";

    f.close();
    PerfLog::InfoF("Summary report written to %s", summaryPath.c_str());
}

void Profiling::Shutdown()
{
    WriteSummaryReport();

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

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        (void*)mainLoop_hook, (void**)&mainLoop_orig);
    PerfLog::InfoF("Hook mainLoop_GPUSensitiveStuff: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff));

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::charsUpdate),
        (void*)charsUpdate_hook, (void**)&charsUpdate_orig);
    PerfLog::InfoF("Hook charsUpdate: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::charsUpdate));

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::charsUpdateUT),
        (void*)charsUpdateUT_hook, (void**)&charsUpdateUT_orig);
    PerfLog::InfoF("Hook charsUpdateUT: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::charsUpdateUT));

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processSysMessages),
        (void*)processSysMessages_hook, (void**)&processSysMessages_orig);
    PerfLog::InfoF("Hook processSysMessages: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::processSysMessages));

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::processKillList),
        (void*)processKillList_hook, (void**)&processKillList_orig);
    PerfLog::InfoF("Hook processKillList: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::processKillList));

    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&GameWorld::dailyUpdates),
        (void*)dailyUpdates_hook, (void**)&dailyUpdates_orig);
    PerfLog::InfoF("Hook dailyUpdates: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&GameWorld::dailyUpdates));

    // Hook individual Character::update() - charsUpdate() appears to be inlined
    status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&Character::_NV_update),
        (void*)charUpdate_hook, (void**)&charUpdate_orig);
    PerfLog::InfoF("Hook Character::update: %s (addr=0x%llX)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        KenshiLib::GetRealAddress(&Character::_NV_update));

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
