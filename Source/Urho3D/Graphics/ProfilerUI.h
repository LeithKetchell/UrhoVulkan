// Copyright (c) 2008-2025 the Urho3D project
// License: MIT

#pragma once

#include "../Core/Object.h"
#include "../UI/Window.h"
#include "../UI/Text.h"
#include "../Graphics/VulkanProfiler.h"

namespace Urho3D
{

class UI;

/// Profiler UI overlay for displaying FPS and performance metrics
class ProfilerUI : public Object
{
    URHO3D_OBJECT(ProfilerUI, Object);

public:
    /// Constructor
    ProfilerUI(Context* context);

    /// Initialize profiler UI
    void Initialize(UI* ui, VulkanProfiler* profiler);

    /// Update profiler display
    void Update();

    /// Show/hide profiler UI
    void SetVisible(bool visible);

    /// Check if profiler UI is visible
    bool IsVisible() const { return window_ ? window_->IsVisible() : false; }

private:
    /// Root window for profiler display
    SharedPtr<Window> window_;

    /// Text element for FPS
    SharedPtr<Text> fpsText_;

    /// Text element for frame time
    SharedPtr<Text> frameTimeText_;

    /// Text element for average frame time
    SharedPtr<Text> avgFrameTimeText_;

    /// Profiler reference
    VulkanProfiler* profiler_;
};

} // namespace Urho3D
