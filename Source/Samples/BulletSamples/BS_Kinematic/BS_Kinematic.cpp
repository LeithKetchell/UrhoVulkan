// Port of bulletphysics/bullet3/examples/RigidBody/KinematicRigidBodyExample.cpp
// Copyright (c) Google Inc / Erwin Coumans, zlib license

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/DebugRenderer.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Light.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>

#include "BS_Kinematic.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_Kinematic)

// Original: 5x5x5 array, box size 0.1 half-extents (0.2 full), positions at 0.2*i spacing
#define ARRAY_SIZE_X 5
#define ARRAY_SIZE_Y 5
#define ARRAY_SIZE_Z 5

void BS_Kinematic::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_Kinematic::CreateScene()
{
    auto* cache = GetSubsystem<ResourceCache>();

    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>();
    scene_->CreateComponent<DebugRenderer>();

    auto* physicsWorld = scene_->CreateComponent<PhysicsWorld>();
    physicsWorld->SetGravity(Vector3(0.0f, -10.0f, 0.0f));

    // Zone
    Node* zoneNode = scene_->CreateChild("Zone");
    auto* zone = zoneNode->CreateComponent<Zone>();
    zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
    zone->SetAmbientColor(Color(0.3f, 0.3f, 0.3f));
    zone->SetFogColor(Color(0.2f, 0.2f, 0.3f));
    zone->SetFogStart(100.0f);
    zone->SetFogEnd(300.0f);

    // Light
    Node* lightNode = scene_->CreateChild("Light");
    lightNode->SetDirection(Vector3(0.6f, -1.0f, 0.8f));
    auto* light = lightNode->CreateComponent<Light>();
    light->SetLightType(LIGHT_DIRECTIONAL);
    light->SetCastShadows(true);
    light->SetShadowBias(BiasParameters(0.00025f, 0.5f));
    light->SetShadowCascade(CascadeParameters(10.0f, 50.0f, 200.0f, 0.0f, 0.8f));

    // === Kinematic ground platform ===
    // Original: btBoxShape(10, 0.1, 10) at y=-0.1, kinematic, rotates around Y
    // Scale up for visibility
    {
        groundNode_ = scene_->CreateChild("KinematicGround");
        groundNode_->SetPosition(Vector3(0.0f, -0.1f, 0.0f));
        groundNode_->SetScale(Vector3(20.0f, 0.2f, 20.0f));

        auto* groundModel = groundNode_->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));
        groundModel->SetCastShadows(true);

        auto* groundBody = groundNode_->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
        groundBody->SetKinematic(true);
        groundBody->SetFriction(0.5f);
    }

    // === 5x5x5 array of small dynamic boxes ===
    // Original: size 0.1 half-extents, positions at 0.2*i spacing, starting y=2
    // Scaled 10x for visibility: size 1, spacing 2, y=20
    {
        for (int k = 0; k < ARRAY_SIZE_Y; k++)
        {
            for (int i = 0; i < ARRAY_SIZE_X; i++)
            {
                for (int j = 0; j < ARRAY_SIZE_Z; j++)
                {
                    float x = 2.0f * (float)i;
                    float y = 2.0f + 2.0f * (float)k;
                    float z = 2.0f * (float)j;

                    Node* node = scene_->CreateChild("Box");
                    node->SetPosition(Vector3(x, y, z));

                    auto* model = node->CreateComponent<StaticModel>();
                    model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
                    model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
                    model->SetCastShadows(true);

                    auto* body = node->CreateComponent<RigidBody>();
                    body->SetMass(1.0f);
                    body->SetBoxShape(Vector3::ONE);
                    body->SetFriction(0.5f);
                }
            }
        }
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Bullet KinematicRigidBodyExample\n"
        "Kinematic platform rotates at angular velocity (0, 0.1, 0)\n"
        "125 small boxes fall and tumble on the rotating surface\n"
        "WASD+mouse | Space=debug draw"
    );
    instructionText->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 12);
    instructionText->SetTextAlignment(HA_CENTER);
    instructionText->SetHorizontalAlignment(HA_CENTER);
    instructionText->SetVerticalAlignment(VA_CENTER);
    instructionText->SetPosition(0, ui->GetRoot()->GetHeight() / 4);

    // Camera
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(300.0f);
    cameraNode_->SetPosition(Vector3(5.0f, 10.0f, -15.0f));
    cameraNode_->LookAt(Vector3(5.0f, 0.0f, 5.0f));
}

void BS_Kinematic::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_Kinematic::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_Kinematic, HandleUpdate));
    SubscribeToEvent(E_PHYSICSPRESTEP, URHO3D_HANDLER(BS_Kinematic, HandlePhysicsPreStep));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_Kinematic, HandlePostRenderUpdate));
}

void BS_Kinematic::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    // Original: kinematicPreTickCallback — rotates ground at angular velocity (0, 0.1, 0)
    // btTransformUtil::integrateTransform with angularVelocity(0, 0.1, 0)
    using namespace PhysicsPreStep;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    if (groundNode_)
    {
        // Integrate rotation: angular velocity (0, 0.1, 0) rad/s
        float angVel = 0.1f;  // original value
        Quaternion rot = groundNode_->GetRotation();
        // deltaAngle in degrees = angVel * timeStep * RAD_TO_DEG
        float deltaAngleDeg = angVel * timeStep * (180.0f / M_PI);
        groundNode_->SetRotation(rot * Quaternion(deltaAngleDeg, Vector3::UP));
    }
}

void BS_Kinematic::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    auto* input = GetSubsystem<Input>();
    const float MOVE_SPEED = 20.0f;
    const float MOUSE_SENSITIVITY = 0.1f;

    IntVector2 mouseMove = input->GetMouseMove();
    yaw_ += MOUSE_SENSITIVITY * mouseMove.x_;
    pitch_ += MOUSE_SENSITIVITY * mouseMove.y_;
    pitch_ = Clamp(pitch_, -90.0f, 90.0f);
    cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));

    if (input->GetKeyDown(KEY_W))
        cameraNode_->Translate(Vector3::FORWARD * MOVE_SPEED * timeStep);
    if (input->GetKeyDown(KEY_S))
        cameraNode_->Translate(Vector3::BACK * MOVE_SPEED * timeStep);
    if (input->GetKeyDown(KEY_A))
        cameraNode_->Translate(Vector3::LEFT * MOVE_SPEED * timeStep);
    if (input->GetKeyDown(KEY_D))
        cameraNode_->Translate(Vector3::RIGHT * MOVE_SPEED * timeStep);

    if (input->GetKeyPress(KEY_SPACE))
        drawDebug_ = !drawDebug_;
}

void BS_Kinematic::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
