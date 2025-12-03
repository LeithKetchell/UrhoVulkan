// Copyright (c) 2008-2025 the Urho3D project
// License: MIT

#include "Precompiled.h"
#include "ProfilerUI.h"
#include "../UI/UI.h"
#include "../UI/UIEvents.h"

namespace Urho3D
{

ProfilerUI::ProfilerUI(Context* context) : Object(context), profiler_(nullptr)
{
}

void ProfilerUI::Initialize(UI* ui, VulkanProfiler* profiler)
{
    profiler_ = profiler;

    if (!ui)
        return;

    // Create root window - larger to fit all metrics
    window_ = ui->GetRoot()->CreateChild<Window>();
    window_->SetStyleAuto();
    window_->SetSize(250, 130);
    window_->SetPosition(10, 10);
    window_->SetOpacity(0.9f);
    window_->SetMovable(true);
    window_->SetResizable(false);
    window_->SetEnabled(true);

    // Create title
    auto title = window_->CreateChild<Text>();
    title->SetStyleAuto();
    title->SetText("Performance");
    title->SetFontSize(14);
    title->SetPosition(5, 5);

    // FPS text
    fpsText_ = window_->CreateChild<Text>();
    fpsText_->SetStyleAuto();
    fpsText_->SetFontSize(12);
    fpsText_->SetPosition(5, 25);
    fpsText_->SetText("FPS: 0");

    // Frame time text
    frameTimeText_ = window_->CreateChild<Text>();
    frameTimeText_->SetStyleAuto();
    frameTimeText_->SetFontSize(12);
    frameTimeText_->SetPosition(5, 45);
    frameTimeText_->SetText("Frame: 0.00ms");

    // Average frame time text
    avgFrameTimeText_ = window_->CreateChild<Text>();
    avgFrameTimeText_->SetStyleAuto();
    avgFrameTimeText_->SetFontSize(12);
    avgFrameTimeText_->SetPosition(5, 65);
    avgFrameTimeText_->SetText("Avg: 0.00ms");
}

void ProfilerUI::Update()
{
    if (!profiler_ || !window_ || !window_->IsVisible())
        return;

    float fps = profiler_->GetFPS();
    float frameTime = profiler_->GetFrameTime();
    float avgFrameTime = profiler_->GetAverageFrameTime();

    if (fpsText_)
        fpsText_->SetText(String("FPS: ") + String((int)fps));

    if (frameTimeText_)
        frameTimeText_->SetText(String("Frame: ") + String(frameTime, 2) + "ms");

    if (avgFrameTimeText_)
        avgFrameTimeText_->SetText(String("Avg: ") + String(avgFrameTime, 2) + "ms");
}

void ProfilerUI::SetVisible(bool visible)
{
    if (window_)
        window_->SetVisible(visible);
}

} // namespace Urho3D
