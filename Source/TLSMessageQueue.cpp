#include "TLSMessageQueue.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

// We use raw Windows TLS instead of RE_Kenshi's TLS::TLSSlot because
// we need multiple slots (one per queue type) and the GCTLSObj overhead
// isn't worth it for simple POD data.

// TLS slot for the parallel phase flag
static DWORD s_parallelFlagSlot = TLS_OUT_OF_INDEXES;

// Per-thread queue storage. We collect all queues from all threads after the barrier.
// Since we know worker count at init time, we pre-allocate per-worker-index arrays.

struct ThreadQueues
{
    std::vector<GameWorld::SysMessage> sysMessages;
    std::vector<Character*> deathParadeAdd;
    std::vector<Character*> deathParadeRemove;
    std::vector<Character*> updateListRemovals;

    void clear()
    {
        sysMessages.clear();
        deathParadeAdd.clear();
        deathParadeRemove.clear();
        updateListRemovals.clear();
    }
};

// Index 0 = main thread, 1..N = worker threads
// Max 16 threads (8 workers + main + headroom)
static const int MAX_THREADS = 16;
static ThreadQueues s_queues[MAX_THREADS];
static boost::mutex s_queueMutex;

// Map thread ID to queue index
static DWORD s_threadIdToIndex[MAX_THREADS] = {0};
static int s_threadCount = 0;

static int GetOrCreateThreadIndex()
{
    DWORD tid = GetCurrentThreadId();

    // Fast path: check existing mappings
    for (int i = 0; i < s_threadCount; ++i)
    {
        if (s_threadIdToIndex[i] == tid)
            return i;
    }

    // Slow path: register new thread
    boost::lock_guard<boost::mutex> lock(s_queueMutex);
    // Double-check after taking lock
    for (int i = 0; i < s_threadCount; ++i)
    {
        if (s_threadIdToIndex[i] == tid)
            return i;
    }

    int idx = s_threadCount;
    if (idx >= MAX_THREADS)
        return 0; // fallback to main thread queue

    s_threadIdToIndex[idx] = tid;
    ++s_threadCount;
    return idx;
}

void TLSQueues::Init()
{
    s_parallelFlagSlot = TlsAlloc();
}

void TLSQueues::BeginParallelPhase()
{
    TlsSetValue(s_parallelFlagSlot, (LPVOID)1);
}

void TLSQueues::EndParallelPhase(GameWorld* world)
{
    TlsSetValue(s_parallelFlagSlot, (LPVOID)0);

    // Flush all per-thread queues to real GameWorld structures
    for (int i = 0; i < s_threadCount; ++i)
    {
        ThreadQueues& q = s_queues[i];

        for (size_t j = 0; j < q.sysMessages.size(); ++j)
            world->sysMessage(q.sysMessages[j]);

        for (size_t j = 0; j < q.deathParadeAdd.size(); ++j)
            world->addToDeathParade(q.deathParadeAdd[j]);

        for (size_t j = 0; j < q.deathParadeRemove.size(); ++j)
            world->removeFromDeathParade(q.deathParadeRemove[j]);

        // mainUpdateListRemovalQueue is a lektor at offset 0x888
        // We can't call a method on it - it's just a raw data member.
        // For now, we access it directly after the barrier on the main thread.
        // TODO: hook the actual removal function if needed.

        q.clear();
    }
}

bool TLSQueues::IsInParallelPhase()
{
    if (s_parallelFlagSlot == TLS_OUT_OF_INDEXES)
        return false;
    return TlsGetValue(s_parallelFlagSlot) != NULL;
}

void TLSQueues::PushSysMessage(const GameWorld::SysMessage& msg)
{
    int idx = GetOrCreateThreadIndex();
    s_queues[idx].sysMessages.push_back(msg);
}

void TLSQueues::PushDeathParadeAdd(Character* who)
{
    int idx = GetOrCreateThreadIndex();
    s_queues[idx].deathParadeAdd.push_back(who);
}

void TLSQueues::PushDeathParadeRemove(Character* who)
{
    int idx = GetOrCreateThreadIndex();
    s_queues[idx].deathParadeRemove.push_back(who);
}

void TLSQueues::PushUpdateListRemoval(Character* who)
{
    int idx = GetOrCreateThreadIndex();
    s_queues[idx].updateListRemovals.push_back(who);
}
