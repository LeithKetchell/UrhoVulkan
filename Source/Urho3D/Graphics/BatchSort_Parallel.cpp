// Copyright (c) 2008-2025 the Urho3D project
// License: MIT
//
// Phase 1.5: Parallel Merge-Sort Implementation for Batches

#include "../Precompiled.h"
#include "BatchSort_Parallel.h"
#include "../Core/Thread.h"
#include <algorithm>

namespace Urho3D
{

Vector<SortedBatch> BatchSort_Parallel::SortBatches(
    const Vector<SortedBatch>& input,
    uint32_t numThreads)
{
    if (input.Empty())
        return Vector<SortedBatch>();

    // Auto-detect threads if not specified
    if (numThreads == 0)
        numThreads = Thread::GetNumCores();

    // Single-threaded path for small inputs
    if (input.Size() < MIN_PARTITION_SIZE * 2 || numThreads <= 1)
    {
        Vector<SortedBatch> output = input;
        MergeSortSequential(output);
        return output;
    }

    // Multi-threaded path
    Vector<SortedBatch> output = input;
    MergeSortParallel(output, numThreads);
    return output;
}

void BatchSort_Parallel::MergeSortSequential(Vector<SortedBatch>& batches)
{
    // Use standard library sort for sequential (typically introsort)
    std::sort(batches.Begin(), batches.End(), BatchComparator);
}

void BatchSort_Parallel::MergeSortParallel(
    Vector<SortedBatch>& batches,
    uint32_t numThreads)
{
    // For now, use sequential sort
    // True parallel implementation would use:
    // - Thread pool from WorkQueue
    // - Task-based divide-and-conquer
    // - Synchronization points between phases
    // Placeholder: sequential sort with framework
    std::sort(batches.Begin(), batches.End(), BatchComparator);
}

void BatchSort_Parallel::Merge(
    Vector<SortedBatch>& data,
    uint32_t left,
    uint32_t mid,
    uint32_t right,
    Vector<SortedBatch>& temp)
{
    uint32_t i = left;
    uint32_t j = mid + 1;
    uint32_t k = left;

    while (i <= mid && j <= right)
    {
        if (BatchComparator(data[i], data[j]))
        {
            temp[k++] = data[i++];
        }
        else
        {
            temp[k++] = data[j++];
        }
    }

    while (i <= mid)
        temp[k++] = data[i++];

    while (j <= right)
        temp[k++] = data[j++];

    for (i = left; i <= right; ++i)
        data[i] = temp[i];
}

void BatchSort_Parallel::MergeSortKernel(
    Vector<SortedBatch>& data,
    uint32_t left,
    uint32_t right,
    Vector<SortedBatch>& temp,
    uint32_t depth,
    uint32_t maxDepth)
{
    if (left < right)
    {
        uint32_t mid = left + (right - left) / 2;

        MergeSortKernel(data, left, mid, temp, depth + 1, maxDepth);
        MergeSortKernel(data, mid + 1, right, temp, depth + 1, maxDepth);

        Merge(data, left, mid, right, temp);
    }
}

} // namespace Urho3D
