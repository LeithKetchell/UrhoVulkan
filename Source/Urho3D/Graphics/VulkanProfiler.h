// Copyright (c) 2008-2024 the Urho3D project
// License: MIT

#pragma once

#ifdef URHO3D_VULKAN

#include "../Container/HashMap.h"
#include "../Core/Object.h"
#include "../Core/Profiler.h"
#include "../IO/Log.h"
#include <chrono>

namespace Urho3D
{

/// High-resolution timer for Vulkan graphics profiling.
/// Measures per-phase timing in command buffer recording, state management, and submission.
class VulkanProfiler : public RefCounted
{
public:
    /// Constructor
    VulkanProfiler() = default;

    /// Start timing a phase
    void StartPhase(const String& phaseName)
    {
        phaseStart_ = std::chrono::high_resolution_clock::now();
        currentPhase_ = phaseName;
    }

    /// End timing current phase and record measurement
    void EndPhase()
    {
        if (currentPhase_.Empty())
            return;

        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - phaseStart_);

        // Record timing in microseconds for precision
        phaseTimings_[currentPhase_].Push(duration.count());

        // Keep rolling average of last 60 frames
        auto& timings = phaseTimings_[currentPhase_];
        if (timings.Size() > 60)
        {
            timings.Erase(timings.Begin());
        }

        currentPhase_.Clear();
    }

    /// Record frame time and update FPS
    void RecordFrame(float deltaTime)
    {
        frameTime_ = deltaTime;
        frameCount_++;
        totalTime_ += deltaTime;

        // Keep rolling buffer of frame times (last 60 frames)
        frameTimes_.Push(deltaTime * 1000.0f); // Convert to milliseconds
        if (frameTimes_.Size() > 60)
        {
            frameTimes_.Erase(frameTimes_.Begin());
        }

        // Calculate FPS from average frame time in the rolling buffer
        // FPS = 1000 ms / average_frame_time_in_ms
        if (!frameTimes_.Empty())
        {
            float avgFrameTime = GetAverageFrameTime();
            if (avgFrameTime > 0.0f)
            {
                fps_ = 1000.0f / avgFrameTime;
            }
        }
    }

    /// Get current FPS
    float GetFPS() const { return fps_; }

    /// Get current frame time in milliseconds
    float GetFrameTime() const { return frameTime_ * 1000.0f; }

    /// Get average frame time from rolling buffer in milliseconds
    float GetAverageFrameTime() const
    {
        if (frameTimes_.Empty())
            return 0.0f;

        float sum = 0.0f;
        for (float time : frameTimes_)
        {
            sum += time;
        }
        return sum / frameTimes_.Size();
    }

    /// Get average time for a phase in milliseconds
    float GetAveragePhaseTime(const String& phaseName) const
    {
        auto it = phaseTimings_.Find(phaseName);
        if (it == phaseTimings_.End() || it->second_.Empty())
            return 0.0f;

        double sum = 0.0;
        for (double timing : it->second_)
        {
            sum += timing;
        }
        return (float)(sum / it->second_.Size() / 1000.0); // Convert microseconds to milliseconds
    }

    /// Get all phase timing statistics for debug output
    void PrintStats() const
    {
        for (const auto& pair : phaseTimings_)
        {
            if (pair.second_.Empty())
                continue;

            double sum = 0.0;
            double minTime = 1e9;
            double maxTime = 0.0;

            for (double timing : pair.second_)
            {
                sum += timing;
                minTime = std::min(minTime, timing);
                maxTime = std::max(maxTime, timing);
            }

            double avg = sum / pair.second_.Size();

            URHO3D_LOGINFO(String("Vulkan Phase: ") + pair.first_ +
                           " | Avg: " + String(avg / 1000.0f, 2) + "ms" +
                           " | Min: " + String(minTime / 1000.0f, 2) + "ms" +
                           " | Max: " + String(maxTime / 1000.0f, 2) + "ms");
        }
    }

    /// Clear all recorded timings
    void Clear()
    {
        phaseTimings_.Clear();
        currentPhase_.Clear();
        frameTimes_.Clear();
        fps_ = 0.0f;
        frameTime_ = 0.0f;
        frameCount_ = 0;
        totalTime_ = 0.0f;
    }

private:
    /// Start time of current phase
    std::chrono::high_resolution_clock::time_point phaseStart_;

    /// Current phase being measured
    String currentPhase_;

    /// Recorded timings per phase (in microseconds, rolling buffer of 60 frames)
    HashMap<String, Vector<double>> phaseTimings_;

    /// FPS tracking
    float fps_ = 0.0f;
    float frameTime_ = 0.0f;
    unsigned frameCount_ = 0;
    float totalTime_ = 0.0f;
    Vector<float> frameTimes_;
};

} // namespace Urho3D

#endif // URHO3D_VULKAN
