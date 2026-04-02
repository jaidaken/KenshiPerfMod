#pragma once

#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/atomic.hpp>
#include <boost/function.hpp>
#include <vector>
#include <deque>

namespace PerfThreadPool
{
    typedef boost::function<void()> Task;

    // Initialize the thread pool with workerCount threads.
    // If workerCount is 0, auto-detects based on hardware_concurrency (clamped 2-8).
    // Returns the actual number of workers created (not counting the calling thread).
    int Init(int workerCount = 0);

    // Shut down the pool. Waits for all workers to finish.
    void Shutdown();

    // Returns the number of worker threads (not counting the caller).
    int GetWorkerCount();

    // Submit a batch of tasks and wait for all of them to complete.
    // The calling thread participates: it runs tasks[0], workers run the rest.
    // Blocks until all tasks are done.
    void DispatchAndWait(Task* tasks, int taskCount);

    // Submit a batch of work defined by a function and a range [0, count).
    // Partitions range across workers + calling thread, runs fn(startIdx, endIdx) on each.
    // Blocks until all partitions are done.
    void ParallelFor(int count, boost::function<void(int start, int end)> fn);

    // Pause all workers (for save synchronization).
    void Pause();

    // Resume all workers after pause.
    void Resume();

    // Returns true if the calling thread is a worker thread.
    bool IsWorkerThread();
}
