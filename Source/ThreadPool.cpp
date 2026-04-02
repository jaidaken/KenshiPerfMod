#include "ThreadPool.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <Debug.h>
#include <algorithm>

// Worker thread state
struct WorkerState
{
    boost::thread* thread;
    unsigned int threadId;
};

// Shared dispatch state for a single DispatchAndWait call
struct DispatchBatch
{
    PerfThreadPool::Task* tasks;
    int taskCount;
    boost::atomic_int nextTask;
    boost::atomic_int completedCount;
};

static std::vector<WorkerState> s_workers;
static int s_workerCount = 0;
static boost::atomic_bool s_running(false);
static boost::atomic_bool s_paused(false);

// Signaled when new work is available
static boost::mutex s_workMutex;
static boost::condition_variable s_workAvailable;

// Signaled when all tasks in a batch are complete
static boost::mutex s_completeMutex;
static boost::condition_variable s_batchComplete;

// Current batch being processed (null when idle)
static DispatchBatch* s_currentBatch = NULL;

// Track which threads are workers for IsWorkerThread()
static DWORD s_workerThreadIds[16] = {0};
static int s_workerThreadIdCount = 0;

static void WorkerProc(int workerIndex)
{
    // Register this thread's ID
    s_workerThreadIds[workerIndex] = GetCurrentThreadId();

    while (s_running.load())
    {
        // Wait for work
        {
            boost::unique_lock<boost::mutex> lock(s_workMutex);
            while (s_running.load() && (s_currentBatch == NULL || s_paused.load()))
            {
                s_workAvailable.wait(lock);
            }
        }

        if (!s_running.load())
            break;

        if (s_paused.load())
            continue;

        // Grab and execute tasks from the current batch
        DispatchBatch* batch = s_currentBatch;
        if (batch != NULL)
        {
            while (true)
            {
                int taskIdx = batch->nextTask.fetch_add(1);
                if (taskIdx >= batch->taskCount)
                    break;

                batch->tasks[taskIdx]();

                int completed = batch->completedCount.fetch_add(1) + 1;
                if (completed >= batch->taskCount)
                {
                    // Last task completed - signal the dispatcher
                    boost::lock_guard<boost::mutex> lock(s_completeMutex);
                    s_batchComplete.notify_all();
                }
            }
        }
    }
}

int PerfThreadPool::Init(int workerCount)
{
    if (s_running.load())
    {
        ErrorLog("[KenshiPerfMod] ThreadPool already initialized");
        return s_workerCount;
    }

    if (workerCount <= 0)
    {
        workerCount = (int)boost::thread::hardware_concurrency();
        if (workerCount < 2) workerCount = 2;
        if (workerCount > 8) workerCount = 8;
        // Reserve one core for the main thread
        workerCount -= 1;
    }

    s_workerCount = workerCount;
    s_running.store(true);
    s_paused.store(false);
    s_currentBatch = NULL;
    s_workerThreadIdCount = workerCount;

    s_workers.resize(workerCount);
    for (int i = 0; i < workerCount; ++i)
    {
        s_workers[i].thread = new boost::thread(WorkerProc, i);
    }

    DebugLog("[KenshiPerfMod] ThreadPool started with " + std::to_string((long long)workerCount) + " workers");
    return workerCount;
}

void PerfThreadPool::Shutdown()
{
    if (!s_running.load())
        return;

    s_running.store(false);

    // Wake all workers so they can exit
    {
        boost::lock_guard<boost::mutex> lock(s_workMutex);
        s_workAvailable.notify_all();
    }

    for (int i = 0; i < (int)s_workers.size(); ++i)
    {
        s_workers[i].thread->join();
        delete s_workers[i].thread;
    }
    s_workers.clear();
    s_workerCount = 0;

    DebugLog("[KenshiPerfMod] ThreadPool shut down");
}

int PerfThreadPool::GetWorkerCount()
{
    return s_workerCount;
}

void PerfThreadPool::DispatchAndWait(Task* tasks, int taskCount)
{
    if (taskCount <= 0)
        return;

    // Single task: just run it on the calling thread
    if (taskCount == 1 || s_workerCount == 0)
    {
        for (int i = 0; i < taskCount; ++i)
            tasks[i]();
        return;
    }

    DispatchBatch batch;
    batch.tasks = tasks;
    batch.taskCount = taskCount;
    batch.nextTask.store(0);
    batch.completedCount.store(0);

    // Publish the batch and wake workers
    {
        boost::lock_guard<boost::mutex> lock(s_workMutex);
        s_currentBatch = &batch;
        s_workAvailable.notify_all();
    }

    // Calling thread also grabs tasks
    while (true)
    {
        int taskIdx = batch.nextTask.fetch_add(1);
        if (taskIdx >= taskCount)
            break;

        batch.tasks[taskIdx]();

        int completed = batch.completedCount.fetch_add(1) + 1;
        if (completed >= taskCount)
        {
            // We completed the last task
            boost::lock_guard<boost::mutex> lock(s_completeMutex);
            s_batchComplete.notify_all();
            break;
        }
    }

    // Wait for all tasks to complete
    {
        boost::unique_lock<boost::mutex> lock(s_completeMutex);
        while (batch.completedCount.load() < taskCount)
        {
            s_batchComplete.wait(lock);
        }
    }

    // Clear the batch so workers go back to sleep
    {
        boost::lock_guard<boost::mutex> lock(s_workMutex);
        s_currentBatch = NULL;
    }
}

void PerfThreadPool::ParallelFor(int count, boost::function<void(int, int)> fn)
{
    if (count <= 0)
        return;

    int totalThreads = s_workerCount + 1; // workers + calling thread
    int perThread = count / totalThreads;
    int remainder = count % totalThreads;

    std::vector<Task> tasks(totalThreads);
    int offset = 0;
    for (int i = 0; i < totalThreads; ++i)
    {
        int start = offset;
        int chunkSize = perThread + (i < remainder ? 1 : 0);
        int end = start + chunkSize;
        offset = end;

        tasks[i] = boost::bind(fn, start, end);
    }

    DispatchAndWait(&tasks[0], totalThreads);
}

void PerfThreadPool::Pause()
{
    s_paused.store(true);
}

void PerfThreadPool::Resume()
{
    s_paused.store(false);
    boost::lock_guard<boost::mutex> lock(s_workMutex);
    s_workAvailable.notify_all();
}

bool PerfThreadPool::IsWorkerThread()
{
    DWORD id = GetCurrentThreadId();
    for (int i = 0; i < s_workerThreadIdCount; ++i)
    {
        if (s_workerThreadIds[i] == id)
            return true;
    }
    return false;
}
