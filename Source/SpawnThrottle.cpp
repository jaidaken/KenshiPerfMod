#include "SpawnThrottle.h"
#include "Settings.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/RootObjectFactory.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>

#include <boost/thread/lock_guard.hpp>

// The idea: RootObjectFactory::mainThreadUpdate() processes its ENTIRE todoList
// deque in one frame. When a zone loads, 200+ characters get queued and all
// processed at once, causing 280-560ms hitches.
//
// We hook mainThreadUpdate() and replace it with a throttled version that
// processes at most N items per frame. The rest stay in the deque for next frame.
//
// Dynamic budget: if the frame is fast, process more. If slow, process fewer.
// This adapts to the player's hardware.

static void (*mainThreadUpdate_orig)(RootObjectFactory*) = NULL;

// We need access to the private process() method. Since we can't call it through
// the class (it's private), we store a function pointer resolved from the RVA.
typedef RootObjectBase* (*ProcessFn)(RootObjectFactory*, RootObjectFactory::CreatelistItem*);
static ProcessFn s_processFunc = NULL;

// Frame time tracking for dynamic budget
static LARGE_INTEGER s_freq;
static float s_lastFrameMs = 0;

static void mainThreadUpdate_hook(RootObjectFactory* self)
{
    if (!PerfSettings::GetEnableSpawnThrottling())
    {
        mainThreadUpdate_orig(self);
        return;
    }

    // Access todoList via known offsets
    // todoMutex at offset 0x0, todoList at offset 0x20
    boost::shared_mutex& mutex = self->todoMutex;
    std::deque<RootObjectFactory::CreatelistItem*>& todoList = self->todoList;

    // Take the lock
    boost::lock_guard<boost::shared_mutex> lock(mutex);

    if (todoList.empty())
        return;

    int queueSize = (int)todoList.size();

    // Dynamic budget: target 8ms max for spawning per frame
    // At ~1.3ms per character (from profiling), 8ms = ~6 characters
    // But if last frame was fast, allow more
    int maxPerFrame = PerfSettings::GetMaxSpawnsPerFrame();

    // Dynamic: if frame was under 8ms, allow full budget. If over 16ms, reduce.
    if (s_lastFrameMs < 8.0f)
        maxPerFrame = maxPerFrame * 2;  // lots of headroom, process faster
    else if (s_lastFrameMs > 16.0f)
        maxPerFrame = maxPerFrame / 2;  // already slow, be conservative

    if (maxPerFrame < 5)
        maxPerFrame = 5;  // always process at least 5

    int toProcess = queueSize;
    if (toProcess > maxPerFrame)
        toProcess = maxPerFrame;

    // Process N items from the front of the deque
    LARGE_INTEGER batchStart, batchEnd;
    QueryPerformanceCounter(&batchStart);

    for (int i = 0; i < toProcess; ++i)
    {
        if (todoList.empty())
            break;

        RootObjectFactory::CreatelistItem* item = todoList.front();
        todoList.pop_front();

        // Call the private process() method via resolved function pointer
        if (s_processFunc)
            s_processFunc(self, item);
    }

    QueryPerformanceCounter(&batchEnd);
    float batchMs = (float)((double)(batchEnd.QuadPart - batchStart.QuadPart) * 1000.0 / (double)s_freq.QuadPart);

    int remaining = (int)todoList.size();
    if (remaining > 0)
    {
        PerfLog::InfoF("SpawnThrottle: processed %d/%d items (%.1fms), %d remaining",
            toProcess, queueSize, batchMs, remaining);
    }
}

// Called from the profiler's main loop hook to track frame time for dynamic budget
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

    // Resolve the private process() function via game base + RVA
    HMODULE gameModule = GetModuleHandleA("kenshi_x64.exe");
    if (!gameModule)
        gameModule = GetModuleHandleA("kenshi_GOG_x64.exe");

    if (gameModule)
    {
        s_processFunc = (ProcessFn)((intptr_t)gameModule + 0x580FF0);
        PerfLog::InfoF("SpawnThrottle: process() resolved to 0x%llX", (intptr_t)s_processFunc);
    }
    else
    {
        PerfLog::Error("SpawnThrottle: could not find game module");
        return;
    }

    // Hook mainThreadUpdate
    KenshiLib::HookStatus status = KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&RootObjectFactory::mainThreadUpdate),
        (void*)mainThreadUpdate_hook,
        (void**)&mainThreadUpdate_orig);

    PerfLog::InfoF("SpawnThrottle: hook mainThreadUpdate %s (max %d/frame)",
        status == KenshiLib::SUCCESS ? "OK" : "FAIL",
        PerfSettings::GetMaxSpawnsPerFrame());
}
