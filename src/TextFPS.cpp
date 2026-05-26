#include "TextFPS.h"
#include "Urho3D/Graphics/Font.h"
#include "Urho3D/Graphics/Text.h"
TextFPS::TextFPS(Context* context) : Component(context)
{
    font_ = GetSubsystem<Font>();
    text_ = CreateChild<Text>();
    text_->SetFont(font_, 14);
    text_->SetAlignment(HA_CENTER, VA_CENTER);
    text_->SetPosition(0.0f, 0.0f);
}
void TextFPS::Update(float timeStep)
{
    text_->SetText("FPS: " + String::ToString((int)GetSubsystem<Window>()->GetFrameRate()));
}