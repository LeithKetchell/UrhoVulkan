// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "../Precompiled.h"

#include "../Core/CoreEvents.h"
#include "../Core/ProcessUtils.h"
#include "../Core/Profiler.h"
#include "../Core/WorkQueue.h"
#include "../IO/Log.h"

namespace Urho3D
{

/// Worker thread managed by the work queue.
class WorkerThread : public Thread, public RefCounted
{
public:
    /// Construct.
    WorkerThread(WorkQueue* owner, i32 index) :
        owner_(owner),
        index_(index)
    {
        assert(index >= 0);
    }

    /// Process work items until stopped.
    void ThreadFunction() override
    {
#ifdef URHO3D_TRACY_PROFILING
        String name;
        name.AppendWithFormat("WorkerThread #%d", index_);
        URHO3D_PROFILE_THREAD(name.CString());
#endif
        // Init FPU state first
        InitFPU();
        owner_->ProcessItems(index_);
    }

    /// Return thread index.
    i32 GetIndex() const { return index_; }

private:
    /// Work queue.
    WorkQueue* owner_;
    /// Thread index.
    i32 index_;
};

WorkQueue::WorkQueue(Context* context) :
    Object(context),
    shutDown_(false),
    pausing_(false),
    paused_(false),
    completing_(false),
    tolerance_(10),
    lastSize_(0),
    maxNonThreadedWorkMs_(5)
{
    SubscribeToEvent(E_BEGINFRAME, URHO3D_HANDLER(WorkQueue, HandleBeginFrame));
}

WorkQueue::~WorkQueue()
{
    // Stop the worker threads. First make sure they are not waiting for work items
    shutDown_ = true;
    Resume();

    for (const SharedPtr<WorkerThread>& thread : threads_)
        thread->Stop();
}

void WorkQueue::CreateThreads(i32 numThreads)
{
#ifdef URHO3D_THREADING
    assert(numThreads >= 0);

    // Other subsystems may initialize themselves according to the number of threads.
    // Therefore allow creating the threads only once, after which the amount is fixed
    if (!threads_.Empty())
        return;

    // Start threads in paused mode
    Pause();

    for (i32 i = 0; i < numThreads; ++i)
    {
        SharedPtr<WorkerThread> thread(new WorkerThread(this, i + 1));
        thread->Run();
        threads_.Push(thread);

        // Pin thread to CPU core for better cache locality
        thread->SetAffinity(i);
    }

    // Initialize per-thread work-stealing deques for lock-free work distribution
    workerDeques_.Resize(numThreads);
    for (i32 i = 0; i < numThreads; ++i)
    {
        workerDeques_[i] = new WorkStealingDeque(256);
    }
#else
    URHO3D_LOGERROR("Can not create worker threads as threading is disabled");
#endif
}

SharedPtr<WorkItem> WorkQueue::GetFreeItem()
{
    if (poolItems_.Size() > 0)
    {
        SharedPtr<WorkItem> item = poolItems_.Front();
        poolItems_.PopFront();

        // BUGFIX: Reset claimed_ flag when reusing item from pool
        // This was deferred from ReturnToPool() to prevent recycled items
        // still in work-stealing deques from being re-executed
        item->claimed_ = false;
        item->completed_ = false;

        return item;
    }
    else
    {
        // No usable items found, create a new one set it as pooled and return it.
        SharedPtr<WorkItem> item(new WorkItem());
        item->pooled_ = true;
        return item;
    }
}

void WorkQueue::AddWorkItem(const SharedPtr<WorkItem>& item)
{
    if (!item)
    {
        URHO3D_LOGERROR("Null work item submitted to the work queue");
        return;
    }

    // BUGFIX: Validate that work function is set
    if (!item->workFunction_)
    {
        URHO3D_LOGERROR("Work item submitted with null work function");
        return;
    }

    // Check for duplicate items.
    assert(!workItems_.Contains(item));

    // Push to the main thread list to keep item alive
    // Clear completed flag in case item is reused
    workItems_.Push(item);
    item->completed_ = false;

    // Make sure worker threads' list is safe to modify
    if (threads_.Size() && !paused_)
        queueMutex_.Acquire();

    // Find position for new item
    if (queue_.Empty())
        queue_.Push(item);
    else
    {
        bool inserted = false;

        for (List<WorkItem*>::Iterator i = queue_.Begin(); i != queue_.End(); ++i)
        {
            if ((*i)->priority_ <= item->priority_)
            {
                queue_.Insert(i, item);
                inserted = true;
                break;
            }
        }

        if (!inserted)
            queue_.Push(item);
    }

    // Also add to work-stealing deques for lock-free distribution
    // Start with deque 0, worker threads will use work-stealing to balance load
    if (!workerDeques_.Empty() && workerDeques_[0])
    {
        workerDeques_[0]->Push(item.Get());
    }

    if (threads_.Size())
    {
        queueMutex_.Release();
        paused_ = false;
    }
}

bool WorkQueue::RemoveWorkItem(SharedPtr<WorkItem> item)
{
    if (!item)
        return false;

    MutexLock lock(queueMutex_);

    // Can only remove successfully if the item was not yet taken by threads for execution
    List<WorkItem*>::Iterator i = queue_.Find(item.Get());
    if (i != queue_.End())
    {
        List<SharedPtr<WorkItem>>::Iterator j = workItems_.Find(item);
        if (j != workItems_.End())
        {
            queue_.Erase(i);
            ReturnToPool(item);
            workItems_.Erase(j);
            return true;
        }
    }

    return false;
}

i32 WorkQueue::RemoveWorkItems(const Vector<SharedPtr<WorkItem>>& items)
{
    MutexLock lock(queueMutex_);
    i32 removed = 0;

    for (Vector<SharedPtr<WorkItem>>::ConstIterator i = items.Begin(); i != items.End(); ++i)
    {
        List<WorkItem*>::Iterator j = queue_.Find(i->Get());
        if (j != queue_.End())
        {
            List<SharedPtr<WorkItem>>::Iterator k = workItems_.Find(*i);
            if (k != workItems_.End())
            {
                queue_.Erase(j);
                ReturnToPool(*k);
                workItems_.Erase(k);
                ++removed;
            }
        }
    }

    return removed;
}

void WorkQueue::Pause()
{
    if (!paused_)
    {
        pausing_ = true;

        queueMutex_.Acquire();
        paused_ = true;

        pausing_ = false;
    }
}

void WorkQueue::Resume()
{
    if (paused_)
    {
        queueMutex_.Release();
        paused_ = false;
    }
}


void WorkQueue::Complete(i32 priority)
{
    assert(priority >= 0);
    completing_ = true;

    if (threads_.Size())
    {
        Resume();

        // Take work items also in the main thread until queue empty or no high-priority items anymore
        i32 maxSkips = queue_.Size() + 10;  // Prevent infinite loop if all items are claimed
        i32 skips = 0;
        while (!queue_.Empty() && skips < maxSkips)
        {
            queueMutex_.Acquire();
            if (!queue_.Empty() && queue_.Front()->priority_ >= priority)
            {
                WorkItem* item = queue_.Front();

                // Try to claim the item BEFORE removing from queue
                bool expected = false;
                bool claimed = item->claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);

                if (claimed)
                {
                    // We claimed it - now remove from queue and execute
                    queue_.PopFront();
                    queueMutex_.Release();
                    item->workFunction_(item, 0);
                    item->completed_ = true;
                    skips = 0;  // Reset skip counter since we made progress
                }
                else
                {
                    // Already claimed by worker thread - remove from queue since it's being handled elsewhere
                    queue_.PopFront();
                    queueMutex_.Release();
                    skips++;
                    Time::Sleep(0);  // Yield to let worker threads make progress
                }
            }
            else
            {
                queueMutex_.Release();
                break;
            }
        }

        // Wait for threaded work to complete
        while (!IsCompleted(priority))
        {
            Time::Sleep(0);  // Yield CPU to worker threads
        }

        // If no work at all remaining, pause worker threads by leaving the mutex locked
        if (queue_.Empty())
            Pause();
    }
    else
    {
        // No worker threads: ensure all high-priority items are completed in the main thread
        while (!queue_.Empty() && queue_.Front()->priority_ >= priority)
        {
            WorkItem* item = queue_.Front();
            queue_.PopFront();

            // Atomically claim the item (shouldn't be necessary without threads, but safe)
            bool alreadyClaimed = item->claimed_.exchange(true, std::memory_order_acq_rel);
            if (!alreadyClaimed)
            {
                item->workFunction_(item, 0);
                item->completed_ = true;
            }
        }
    }

    PurgeCompleted(priority);
    completing_ = false;
}

bool WorkQueue::IsCompleted(i32 priority) const
{
    assert(priority >= 0);
    i32 incompleteCount = 0;
    for (List<SharedPtr<WorkItem>>::ConstIterator i = workItems_.Begin(); i != workItems_.End(); ++i)
    {
        if ((*i)->priority_ >= priority && !(*i)->completed_)
        {
            incompleteCount++;
        }
    }

    if (incompleteCount > 0)
        return false;

    return true;
}

void WorkQueue::ProcessItems(i32 threadIndex)
{
    assert(threadIndex >= 0);

    bool wasActive = false;

    for (;;)
    {
        if (shutDown_)
        {
            return;
        }

        if (pausing_ && !wasActive)
            Time::Sleep(0);
        else
        {
            WorkItem* item = nullptr;
            bool itemAlreadyClaimed = false;  // Track if we pre-claimed this item

            // Try work-stealing first (lock-free)
            if (threadIndex > 0 && threadIndex <= (i32)workerDeques_.Size())
            {
                i32 dequeIndex = threadIndex - 1;

                // Try own deque first
                if (workerDeques_[dequeIndex])
                {
                    item = (WorkItem*)workerDeques_[dequeIndex]->Pop();
                    if (item)
                        wasActive = true;  // BUGFIX: Mark as active when work found
                }

                // Try stealing from neighbors if own deque empty
                if (!item)
                {
                    for (i32 i = 1; i < (i32)workerDeques_.Size(); ++i)
                    {
                        i32 neighbor = (dequeIndex + i) % workerDeques_.Size();
                        if (workerDeques_[neighbor])
                        {
                            item = (WorkItem*)workerDeques_[neighbor]->Steal();
                            if (item)
                            {
                                wasActive = true;  // BUGFIX: Mark as active when work stolen
                                break;
                            }
                        }
                    }
                }
            }

            // Fall back to mutex-based queue if work-stealing didn't find anything
            if (!item)
            {
                queueMutex_.Acquire();
                if (!queue_.Empty())
                {
                    WorkItem* candidateItem = queue_.Front();

                    // Try to claim BEFORE removing from queue
                    bool expected = false;
                    bool claimed = candidateItem->claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);

                    if (claimed)
                    {
                        // Successfully claimed - remove from queue
                        item = candidateItem;
                        itemAlreadyClaimed = true;  // Mark as already claimed
                        queue_.PopFront();
                        wasActive = true;
                        queueMutex_.Release();

                        // DEBUG: Log successful claim from mutex queue (DISABLED - too verbose)
                        // char itemBuf[32];
                        // sprintf(itemBuf, "%p", (void*)item);
                        // URHO3D_LOGDEBUG("Thread " + String(threadIndex) + " CLAIMED item " +
                        //                String(itemBuf) + " from mutex queue (priority=" + String(item->priority_) + ")");
                    }
                    else
                    {
                        // Already claimed - leave in queue, continue searching
                        queueMutex_.Release();
                        Time::Sleep(0);
                        continue;
                    }
                }
                else
                {
                    wasActive = false;

                    queueMutex_.Release();
                    Time::Sleep(0);
                    continue;
                }
            }
            else
            {
                wasActive = true;
            }

            // Execute the work item
            if (item)
            {
                bool shouldExecute = false;

                if (itemAlreadyClaimed)
                {
                    // Item was pre-claimed from mutex queue - execute directly
                    shouldExecute = true;
                }
                else
                {
                    // Item from work-stealing deque - need to claim it first
                    // BUGFIX: Atomically claim to prevent double-execution (item might be in both queue_ and workerDeques_)
                    bool alreadyClaimed = item->claimed_.exchange(true, std::memory_order_acq_rel);
                    shouldExecute = !alreadyClaimed;

                    // DEBUG: Logging disabled - too verbose
                    // char itemBuf[32];
                    // sprintf(itemBuf, "%p", (void*)item);
                    // if (shouldExecute)
                    // {
                    //     URHO3D_LOGDEBUG("Thread " + String(threadIndex) + " CLAIMED item " +
                    //                    String(itemBuf) + " from deque (priority=" + String(item->priority_) + ")");
                    // }
                    // else
                    // {
                    //     URHO3D_LOGDEBUG("Thread " + String(threadIndex) + " SKIPPED item " +
                    //                    String(itemBuf) + " from deque (already claimed)");
                    // }
                }

                if (shouldExecute)
                {
                    // DEBUG: Execution logging disabled - too verbose
                    // char itemBuf[32], funcBuf[32];
                    // sprintf(itemBuf, "%p", (void*)item);
                    // sprintf(funcBuf, "%p", (void*)item->workFunction_);
                    // URHO3D_LOGDEBUG("Thread " + String(threadIndex) + " EXECUTING item " +
                    //                String(itemBuf) + " (priority=" + String(item->priority_) +
                    //                ", workFunction=" + String(funcBuf) + ")");

                    // Execute the work item
                    // BUGFIX: Defensive check for null work function
                    if (item->workFunction_)
                    {
                        item->workFunction_(item, threadIndex);
                    }
                    else
                    {
                        // Null work function - this shouldn't happen but defensive check prevents crash
                        URHO3D_LOGWARNING("Work item has null work function - marking completed to prevent leak");
                    }
                    // Always mark as completed to ensure proper cleanup
                    item->completed_ = true;

                    // DEBUG: Completion logging disabled - too verbose
                    // URHO3D_LOGDEBUG("Thread " + String(threadIndex) + " COMPLETED item " +
                    //                String(itemBuf) + " (priority=" + String(item->priority_) + ")");
                }
                // else: Another thread already claimed and is executing this item, skip it
            }
        }
    }
}

void WorkQueue::PurgeCompleted(i32 priority)
{
    assert(priority >= 0);

    // Purge completed work items and send completion events. Do not signal items lower than priority threshold,
    // as those may be user submitted and lead to eg. scene manipulation that could happen in the middle of the
    // render update, which is not allowed
    for (List<SharedPtr<WorkItem>>::Iterator i = workItems_.Begin(); i != workItems_.End();)
    {
        if ((*i)->completed_ && (*i)->priority_ >= priority)
        {
            if ((*i)->sendEvent_)
            {
                using namespace WorkItemCompleted;

                VariantMap& eventData = GetEventDataMap();
                eventData[P_ITEM] = i->Get();
                SendEvent(E_WORKITEMCOMPLETED, eventData);
            }

            // BUGFIX: Remove from queue_ before returning to pool
            // Items were being returned to pool (workFunction_=nullptr, claimed_=false)
            // but staying in queue_, causing worker threads to try to execute them again
            WorkItem* itemPtr = i->Get();
            if (threads_.Size())
            {
                queueMutex_.Acquire();
                List<WorkItem*>::Iterator queueIter = queue_.Find(itemPtr);
                if (queueIter != queue_.End())
                    queue_.Erase(queueIter);
                queueMutex_.Release();
            }
            else
            {
                List<WorkItem*>::Iterator queueIter = queue_.Find(itemPtr);
                if (queueIter != queue_.End())
                    queue_.Erase(queueIter);
            }

            ReturnToPool(*i);
            i = workItems_.Erase(i);
        }
        else
            ++i;
    }
}

void WorkQueue::PurgePool()
{
    i32 currentSize = poolItems_.Size();
    i32 difference = lastSize_ - currentSize;

    // Difference tolerance, should be fairly significant to reduce the pool size.
    for (i32 i = 0; poolItems_.Size() > 0 && difference > tolerance_ && i < difference; i++)
        poolItems_.PopFront();

    lastSize_ = currentSize;
}

void WorkQueue::ReturnToPool(SharedPtr<WorkItem>& item)
{
    // Check if this was a pooled item and set it to usable
    if (item->pooled_)
    {
        // Reset the values to their defaults. This should
        // be safe to do here as the completed event has
        // already been handled and this is part of the
        // internal pool.
        item->start_ = nullptr;
        item->end_ = nullptr;
        item->aux_ = nullptr;
        item->workFunction_ = nullptr;
        item->priority_ = WI_MAX_PRIORITY;
        item->sendEvent_ = false;
        item->completed_ = false;
        // BUGFIX: Don't reset claimed_ here! Item pointer may still be in work-stealing deques.
        // If we reset claimed_, worker threads could retrieve and try to execute it again.
        // claimed_ will be reset when item is actually reused from pool in GetFreeItem().
        // item->claimed_ = false;

        poolItems_.Push(item);
    }
}

void WorkQueue::HandleBeginFrame(StringHash eventType, VariantMap& eventData)
{
    // If no worker threads, complete low-priority work here
    if (threads_.Empty() && !queue_.Empty())
    {
        URHO3D_PROFILE(CompleteWorkNonthreaded);

        HiresTimer timer;

        while (!queue_.Empty() && timer.GetUSec(false) < maxNonThreadedWorkMs_ * 1000LL)
        {
            WorkItem* item = queue_.Front();
            queue_.PopFront();

            // Atomically claim the item (shouldn't be necessary without threads, but safe)
            bool alreadyClaimed = item->claimed_.exchange(true, std::memory_order_acq_rel);
            if (!alreadyClaimed)
            {
                item->workFunction_(item, 0);
                item->completed_ = true;
            }
        }
    }

    // Complete and signal items down to the lowest priority
    PurgeCompleted(0);
    PurgePool();
}

}
