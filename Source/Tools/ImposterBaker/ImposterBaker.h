#pragma once

#include <Urho3D/Engine/Application.h>
#include <Urho3D/Resource/Image.h>

using namespace Urho3D;

class ImposterBaker : public Application
{
    URHO3D_OBJECT(ImposterBaker, Application);

public:
    explicit ImposterBaker(Context* context);

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    /// Parse command-line arguments. Returns false on error.
    bool ParseArgs();
    /// Take an RGBA screenshot (4-component, preserving alpha).
    bool TakeScreenShotRGBA(Image& destImage);

    String modelPath_;
    String materialPath_;
    String outputPrefix_{"imposter_"};
    int numViews_{8};
    int outputWidth_{512};
    int outputHeight_{1024};
};
