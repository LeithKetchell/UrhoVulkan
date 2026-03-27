// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "HUD.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Core/Timer.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/UI.h>

HUD::HUD(Context* context)
    : LogicComponent(context)
{
    SetUpdateEventMask(LogicComponentEvents::Update);
}

void HUD::RegisterObject(Context* context)
{
    context->RegisterFactory<HUD>();
}

void HUD::Start()
{
    // Set up bar parameters per the plan
    hpBar_.barColor = Color(0.8f, 0.2f, 0.2f);       // #CC3333 deep red
    hpBar_.criticalColor = Color(1.0f, 0.0f, 0.0f);
    hpBar_.showThreshold = 1.01f;  // visible whenever HP < 1.0 (always show when damaged)
    hpBar_.hideDelay = 5.0f;
    hpBar_.labelText = "HP";
    hpBar_.criticalText = "";

    hungerBar_.barColor = Color(0.8f, 0.53f, 0.2f);   // #CC8833 amber
    hungerBar_.criticalColor = Color(0.9f, 0.3f, 0.1f);
    hungerBar_.showThreshold = 0.75f;
    hungerBar_.hideDelay = 5.0f;
    hungerBar_.labelText = "Hunger";
    hungerBar_.criticalText = "STARVING";

    thirstBar_.barColor = Color(0.2f, 0.4f, 0.67f);   // #3366AA muted blue
    thirstBar_.criticalColor = Color(0.5f, 0.2f, 0.2f);
    thirstBar_.showThreshold = 0.75f;
    thirstBar_.hideDelay = 5.0f;
    thirstBar_.labelText = "Thirst";
    thirstBar_.criticalText = "DEHYDRATED";

    staminaBar_.barColor = Color(0.8f, 0.67f, 0.2f);  // #CCAA33 gold
    staminaBar_.criticalColor = Color(0.4f, 0.4f, 0.4f);
    staminaBar_.showThreshold = 1.01f;  // caller controls visibility via value changes
    staminaBar_.hideDelay = 3.0f;
    staminaBar_.labelText = "Stamina";
    staminaBar_.criticalText = "";

    // Load default font if none set
    if (!font_)
    {
        auto* cache = GetSubsystem<ResourceCache>();
        font_ = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
        if (!font_)
            font_ = cache->GetResource<Font>("Fonts/BlueHighway.sdf");
    }

    CreateBars();
}

void HUD::SetFont(Font* font, int size)
{
    font_ = font;
    fontSize_ = size;
}

void HUD::CreateBars()
{
    auto* ui = GetSubsystem<UI>();
    auto* root = ui->GetRoot();

    // Bottom-center container
    barContainer_ = root->CreateChild<UIElement>("HUDVitals");
    barContainer_->SetAlignment(HA_CENTER, VA_BOTTOM);
    barContainer_->SetPosition(0, -BAR_MARGIN_BOTTOM);
    barContainer_->SetLayout(LM_VERTICAL, BAR_GAP, IntRect(0, 0, 0, 0));

    // Top row: HP | Hunger | Thirst side by side
    topRow_ = barContainer_->CreateChild<UIElement>("HUDTopRow");
    topRow_->SetLayout(LM_HORIZONTAL, BAR_GAP, IntRect(0, 0, 0, 0));
    topRow_->SetHorizontalAlignment(HA_CENTER);

    // Bottom row: Stamina centered
    bottomRow_ = barContainer_->CreateChild<UIElement>("HUDBottomRow");
    bottomRow_->SetLayout(LM_HORIZONTAL, 0, IntRect(0, 0, 0, 0));
    bottomRow_->SetHorizontalAlignment(HA_CENTER);

    CreateBar(hpBar_, "HP");
    CreateBar(hungerBar_, "Hunger");
    CreateBar(thirstBar_, "Thirst");
    CreateBar(staminaBar_, "Stamina");

    // Temperature indicator — text below the bars, hidden when comfortable
    tempIndicator_ = barContainer_->CreateChild<Text>("TempIndicator");
    tempIndicator_->SetFont(font_, fontSize_);
    tempIndicator_->SetHorizontalAlignment(HA_CENTER);
    tempIndicator_->SetColor(Color(1.0f, 1.0f, 1.0f, 0.0f));
    tempIndicator_->SetVisible(false);
}

void HUD::CreateBar(VitalBar& bar, const String& name)
{
    // HP/Hunger/Thirst go in top row, Stamina in bottom row
    UIElement* parent = (name == "Stamina") ? bottomRow_ : topRow_;

    auto* wrapper = parent->CreateChild<UIElement>(name + "Wrapper");
    wrapper->SetFixedSize(BAR_WIDTH, BAR_HEIGHT + 14);  // room for label above
    wrapper->SetLayoutMode(LM_FREE);

    // Label above bar
    bar.label = wrapper->CreateChild<Text>(name + "Label");
    bar.label->SetFont(font_, fontSize_);
    bar.label->SetColor(Color(0.87f, 0.8f, 0.73f));  // #DDCCBB warm off-white
    bar.label->SetText(bar.labelText);
    bar.label->SetHorizontalAlignment(HA_CENTER);
    bar.label->SetPosition(0, 0);

    // Background (dark)
    bar.background = wrapper->CreateChild<BorderImage>(name + "Bg");
    bar.background->SetColor(Color(0.0f, 0.0f, 0.0f, 0.5f));
    bar.background->SetFixedSize(BAR_WIDTH, BAR_HEIGHT);
    bar.background->SetPosition(0, 14);

    // Fill (colored)
    bar.fill = bar.background->CreateChild<BorderImage>(name + "Fill");
    bar.fill->SetColor(bar.barColor);
    bar.fill->SetFixedSize(BAR_WIDTH, BAR_HEIGHT);
    bar.fill->SetPosition(0, 0);

    // Critical text (hidden by default)
    bar.criticalLabel = wrapper->CreateChild<Text>(name + "Critical");
    bar.criticalLabel->SetFont(font_, fontSize_);
    bar.criticalLabel->SetColor(Color(1.0f, 0.3f, 0.2f, 0.0f));
    bar.criticalLabel->SetText(bar.criticalText);
    bar.criticalLabel->SetHorizontalAlignment(HA_CENTER);
    bar.criticalLabel->SetPosition(0, BAR_HEIGHT + 16);
    bar.criticalLabel->SetVisible(false);

    // Start fully transparent
    bar.currentAlpha = 0.0f;
    wrapper->SetOpacity(0.0f);
}

void HUD::Update(float timeStep)
{
    UpdateBar(hpBar_, timeStep);
    UpdateBar(hungerBar_, timeStep);
    UpdateBar(thirstBar_, timeStep);
    UpdateBar(staminaBar_, timeStep);

    // Temperature indicator — only visible when uncomfortable
    if (tempIndicator_)
    {
        // Thresholds from warmth_rules defaults
        bool freezing = temperature_ < -10.0f;
        bool cold = temperature_ < 0.0f;
        bool shivering = temperature_ < 5.0f;
        bool hot = temperature_ > 40.0f;
        bool uncomfortable = shivering || hot;

        float targetAlpha = uncomfortable ? 1.0f : 0.0f;
        if (tempIndicatorAlpha_ < targetAlpha)
            tempIndicatorAlpha_ = Min(tempIndicatorAlpha_ + FADE_SPEED * timeStep, targetAlpha);
        else if (tempIndicatorAlpha_ > targetAlpha)
            tempIndicatorAlpha_ = Max(tempIndicatorAlpha_ - FADE_SPEED * timeStep, targetAlpha);

        tempIndicator_->SetVisible(tempIndicatorAlpha_ > 0.01f);

        if (tempIndicatorAlpha_ > 0.01f)
        {
            Color c;
            String text;
            if (freezing)
            {
                c = Color(0.4f, 0.6f, 1.0f, tempIndicatorAlpha_);  // bright blue
                text = "FREEZING";
            }
            else if (cold)
            {
                c = Color(0.5f, 0.7f, 0.95f, tempIndicatorAlpha_); // light blue
                text = "COLD";
            }
            else if (shivering)
            {
                c = Color(0.7f, 0.8f, 0.9f, tempIndicatorAlpha_);  // pale blue
                text = "Chilly";
            }
            else if (hot)
            {
                c = Color(1.0f, 0.4f, 0.15f, tempIndicatorAlpha_); // orange-red
                text = "OVERHEATING";
            }
            tempIndicator_->SetText(text);
            tempIndicator_->SetColor(c);

            // Pulse when freezing or overheating
            if (freezing || hot)
            {
                float pulse = 0.7f + 0.3f * sinf(GetSubsystem<Time>()->GetElapsedTime() * 3.0f);
                c.a_ = tempIndicatorAlpha_ * pulse;
                tempIndicator_->SetColor(c);
            }
        }
    }
}

void HUD::UpdateBar(VitalBar& bar, float timeStep)
{
    // Determine target alpha
    float targetAlpha = 0.0f;
    if (bar.value < bar.showThreshold)
    {
        targetAlpha = 1.0f;
        bar.hideTimer = 0.0f;
    }
    else
    {
        bar.hideTimer += timeStep;
        if (bar.hideTimer < bar.hideDelay)
            targetAlpha = 1.0f;  // still showing during delay
    }

    // Smooth fade
    if (bar.currentAlpha < targetAlpha)
        bar.currentAlpha = Min(bar.currentAlpha + FADE_SPEED * timeStep, targetAlpha);
    else if (bar.currentAlpha > targetAlpha)
        bar.currentAlpha = Max(bar.currentAlpha - FADE_SPEED * timeStep, targetAlpha);

    // Update fill width
    int fillW = (int)(Clamp(bar.value, 0.0f, 1.0f) * BAR_WIDTH);
    if (bar.fill)
        bar.fill->SetFixedWidth(Max(0, fillW));

    // Critical color swap below 25%
    bool critical = bar.value < CRITICAL_THRESHOLD;
    if (bar.fill)
    {
        Color c = critical ? bar.criticalColor : bar.barColor;
        c.a_ = bar.currentAlpha;
        bar.fill->SetColor(c);
    }

    // Background alpha
    if (bar.background)
    {
        Color bg(0.0f, 0.0f, 0.0f, bar.currentAlpha * 0.5f);
        bar.background->SetColor(bg);
    }

    // Label alpha
    if (bar.label)
    {
        Color lc = bar.label->GetColor(C_TOPLEFT);
        lc.a_ = bar.currentAlpha;
        bar.label->SetColor(lc);
    }

    // Critical text — show at 0% with pulse
    if (bar.criticalLabel && bar.criticalText.Length() > 0)
    {
        bool showCrit = critical && bar.value < 0.01f && bar.currentAlpha > 0.1f;
        bar.criticalLabel->SetVisible(showCrit);
        if (showCrit)
        {
            // Subtle pulse: oscillate alpha between 0.5 and 1.0
            float pulse = 0.75f + 0.25f * sinf(GetSubsystem<Time>()->GetElapsedTime() * 4.0f);
            Color cc(1.0f, 0.3f, 0.2f, pulse * bar.currentAlpha);
            bar.criticalLabel->SetColor(cc);
        }
    }

    // Set parent wrapper opacity for the whole bar group
    UIElement* wrapper = bar.background ? bar.background->GetParent() : nullptr;
    if (wrapper)
        wrapper->SetOpacity(bar.currentAlpha);
}

void HUD::SetHP(float v)
{
    hpBar_.value = Clamp(v, 0.0f, 1.0f);
}

void HUD::SetHunger(float v)
{
    hungerBar_.value = Clamp(v, 0.0f, 1.0f);
}

void HUD::SetThirst(float v)
{
    thirstBar_.value = Clamp(v, 0.0f, 1.0f);
}

void HUD::SetStamina(float v)
{
    staminaBar_.value = Clamp(v, 0.0f, 1.0f);
}

void HUD::SetTemperature(float celsius)
{
    temperature_ = celsius;
}
