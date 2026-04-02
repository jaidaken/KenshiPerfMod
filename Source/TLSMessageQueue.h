#pragma once

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <vector>

// Thread-local message queues for redirecting shared-state writes
// during parallel character updates.
//
// Pattern: during parallel phase, hooks on sysMessage/addToDeathParade/etc
// check a TLS flag. If set, they push to these per-thread queues instead of
// the real GameWorld lists. After the barrier, the main thread flushes all
// queues to the real lists.

namespace TLSQueues
{
    // Call before parallel dispatch. Sets the TLS guard flag on calling thread.
    void BeginParallelPhase();

    // Call after barrier. Flushes all per-thread queues to GameWorld.
    // Must be called from the main thread.
    void EndParallelPhase(GameWorld* world);

    // Returns true if the calling thread is in the parallel update phase.
    bool IsInParallelPhase();

    // Per-thread queue accessors (called from hooks)
    void PushSysMessage(const GameWorld::SysMessage& msg);
    void PushDeathParadeAdd(Character* who);
    void PushDeathParadeRemove(Character* who);
    void PushUpdateListRemoval(Character* who);

    // Initialize TLS slots. Call once at startup.
    void Init();
}
