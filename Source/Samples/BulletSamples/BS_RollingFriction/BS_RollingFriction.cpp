// Honest port of Bullet SDK RollingFrictionDemo to Urho3D.
// Original: bulletphysics/bullet3/examples/RollingFrictionDemo/RollingFrictionDemo.cpp
// License: zlib (Erwin Coumans, Bullet Physics)
//
// 5x5x5 grid of shapes (spheres, capsules, cones, cylinders) on a slightly
// sloped plane. Rolling friction prevents indefinite rolling. Spinning friction
// was added to Urho3D's RigidBody for this port.

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
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>

#include "BS_RollingFriction.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_RollingFriction)

// Original Bullet defines
#define ARRAY_SIZE_X 5
#define ARRAY_SIZE_Y 5
#define ARRAY_SIZE_Z 5

void BS_RollingFriction::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_RollingFriction::CreateScene()
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
    zone->SetAmbientColor(Color(0.2f, 0.2f, 0.2f));
    zone->SetFogColor(Color(0.5f, 0.5f, 0.7f));
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

    // === Sloped ground (original: box(10,5,25) at z=-28, rotated 0.03*PI around Y) ===
    // Mapped to Urho3D Y-up: slope tilts around Z axis
    {
        Node* slopeNode = scene_->CreateChild("Slope");
        slopeNode->SetPosition(Vector3(0.0f, -5.0f, 0.0f));
        slopeNode->SetScale(Vector3(20.0f, 10.0f, 50.0f));
        slopeNode->SetRotation(Quaternion(5.4f, Vector3::FORWARD));  // ~0.03*PI radians ≈ 5.4 degrees
        auto* slopeModel = slopeNode->CreateComponent<StaticModel>();
        slopeModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        slopeModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* slopeBody = slopeNode->CreateComponent<RigidBody>();
        slopeBody->SetBoxShape(Vector3::ONE);
        slopeBody->SetFriction(0.5f);
    }

    // === Flat floor underneath (original: box(100,100,50) at z=-54) ===
    {
        Node* floorNode = scene_->CreateChild("Floor");
        floorNode->SetPosition(Vector3(0.0f, -60.0f, 0.0f));
        floorNode->SetScale(Vector3(200.0f, 200.0f, 100.0f));
        auto* floorModel = floorNode->CreateComponent<StaticModel>();
        floorModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        floorModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* floorBody = floorNode->CreateComponent<RigidBody>();
        floorBody->SetBoxShape(Vector3::ONE);
        floorBody->SetFriction(0.1f);
    }

    // === 5x5x5 array of shapes with rolling/spinning friction ===
    // Original cycles through 10 shapes: sphere, 3 capsule orientations,
    // 3 cone orientations, 3 cylinder orientations.
    // Urho3D RigidBody supports sphere, capsule, cone, cylinder directly.
    // We cycle through 4 shape types for variety.
    {
        int shapeIndex = 0;

        float start_x = -5.0f - ARRAY_SIZE_X / 2.0f;
        float start_z = -3.0f - ARRAY_SIZE_Z / 2.0f;

        for (int k = 0; k < ARRAY_SIZE_Y; k++)
        {
            for (int i = 0; i < ARRAY_SIZE_X; i++)
            {
                for (int j = 0; j < ARRAY_SIZE_Z; j++)
                {
                    float x = 2.0f * i + start_x;
                    float y = 20.0f + 2.0f * k + (-5.0f);
                    float z = 2.0f * j + start_z;

                    Node* node = scene_->CreateChild("Shape");
                    node->SetPosition(Vector3(x, y, z));

                    // Visual — all use box model since Urho3D doesn't ship sphere/capsule models
                    // Physics shape is the real one; debug draw (Space) shows actual collision shapes
                    auto* model = node->CreateComponent<StaticModel>();
                    model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
                    model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
                    model->SetCastShadows(true);

                    auto* body = node->CreateComponent<RigidBody>();
                    body->SetMass(1.0f);
                    body->SetFriction(1.0f);
                    body->SetRollingFriction(0.1f);
                    body->SetSpinningFriction(0.1f);

                    // Cycle through shape types
                    switch (shapeIndex % 4)
                    {
                    case 0: // Sphere (r=0.5)
                        body->SetSphereShape(0.5f);
                        break;
                    case 1: // Capsule (r=0.25, h=0.5)
                        body->SetCapsuleShape(0.25f, 0.5f);
                        break;
                    case 2: // Cone (r=0.25, h=0.5)
                        body->SetConeShape(0.25f, 0.5f);
                        break;
                    case 3: // Cylinder (r=0.25, h=0.5)
                        body->SetCylinderShape(0.25f, 0.5f);
                        break;
                    }

                    shapeIndex++;
                }
            }
        }
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Bullet RollingFrictionDemo\n"
        "125 shapes (sphere/capsule/cone/cylinder) on sloped plane\n"
        "Rolling + spinning friction bring objects to rest\n"
        "WASD+mouse | Space=debug draw (shows actual collision shapes)"
    );
    instructionText->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 12);
    instructionText->SetTextAlignment(HA_CENTER);
    instructionText->SetHorizontalAlignment(HA_CENTER);
    instructionText->SetVerticalAlignment(VA_CENTER);
    instructionText->SetPosition(0, ui->GetRoot()->GetHeight() / 4);

    // Camera — pulled back to see the whole slope
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(300.0f);
    cameraNode_->SetPosition(Vector3(0.0f, 15.0f, -35.0f));
    cameraNode_->LookAt(Vector3(0.0f, 0.0f, 0.0f));
}

void BS_RollingFriction::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_RollingFriction::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_RollingFriction, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_RollingFriction, HandlePostRenderUpdate));
}

void BS_RollingFriction::HandleUpdate(StringHash eventType, VariantMap& eventData)
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

void BS_RollingFriction::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
