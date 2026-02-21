// Port of bulletphysics/bullet3/examples/TwoJoint/TwoJointMain.cpp
// Original: two-link robot loaded from URDF, sinusoidal torque on joint_2,
// logs positions and velocities. This port recreates the same two-link
// chain using Urho3D's Constraint system with applied torques.
// Copyright (c) Avik De / Bullet3, zlib license

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
#include <Urho3D/IO/Log.h>
#include <Urho3D/Math/MathDefs.h>
#include <Urho3D/Physics/Constraint.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>

#include "BS_TwoJoint.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_TwoJoint)

static const float LINK_LENGTH = 2.0f;
static const float LINK_WIDTH = 0.3f;
static const float LINK_MASS = 1.0f;
static const float TORQUE_AMPLITUDE = 5.0f;
static const float TORQUE_FREQUENCY = 1.0f; // Hz

void BS_TwoJoint::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_TwoJoint::CreateScene()
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

    // Ground
    {
        Node* groundNode = scene_->CreateChild("Ground");
        groundNode->SetPosition(Vector3(0.0f, -0.5f, 0.0f));
        groundNode->SetScale(Vector3(40.0f, 1.0f, 40.0f));
        auto* groundModel = groundNode->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* groundBody = groundNode->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
    }

    // === Two-link robot arm ===
    // Fixed base anchor point
    Node* baseNode = scene_->CreateChild("Base");
    baseNode->SetPosition(Vector3(0.0f, 6.0f, 0.0f));
    auto* baseModel = baseNode->CreateComponent<StaticModel>();
    baseModel->SetModel(cache->GetResource<Model>("Models/Sphere.mdl"));
    baseModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));
    baseNode->SetScale(0.5f);
    auto* baseBody = baseNode->CreateComponent<RigidBody>();
    baseBody->SetSphereShape(0.5f);

    // Link 1 — hangs from base
    Node* link1Node = scene_->CreateChild("Link1");
    link1Node->SetPosition(Vector3(0.0f, 6.0f - LINK_LENGTH / 2.0f, 0.0f));
    link1Node->SetScale(Vector3(LINK_WIDTH, LINK_LENGTH, LINK_WIDTH));
    auto* link1Model = link1Node->CreateComponent<StaticModel>();
    link1Model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    link1Model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
    link1Model->SetCastShadows(true);

    auto* link1Body = link1Node->CreateComponent<RigidBody>();
    link1Body->SetMass(LINK_MASS);
    link1Body->SetBoxShape(Vector3::ONE);
    link1Body->SetLinearDamping(0.05f);
    link1Body->SetAngularDamping(0.05f);

    // Joint 1: base → link1 (hinge, Z axis)
    auto* joint1 = link1Node->CreateComponent<Constraint>();
    joint1->SetConstraintType(CONSTRAINT_HINGE);
    joint1->SetOtherBody(baseBody);
    joint1->SetPosition(Vector3(0.0f, 0.5f, 0.0f));  // top of link1
    joint1->SetOtherPosition(Vector3::ZERO);            // center of base
    joint1->SetAxis(Vector3::FORWARD);
    joint1->SetOtherAxis(Vector3::FORWARD);

    link1_ = link1Node;

    // Link 2 — hangs from link1
    Node* link2Node = scene_->CreateChild("Link2");
    link2Node->SetPosition(Vector3(0.0f, 6.0f - LINK_LENGTH - LINK_LENGTH / 2.0f, 0.0f));
    link2Node->SetScale(Vector3(LINK_WIDTH, LINK_LENGTH, LINK_WIDTH));
    auto* link2Model = link2Node->CreateComponent<StaticModel>();
    link2Model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    link2Model->SetMaterial(cache->GetResource<Material>("Materials/StoneSmall.xml"));
    link2Model->SetCastShadows(true);

    auto* link2Body = link2Node->CreateComponent<RigidBody>();
    link2Body->SetMass(LINK_MASS);
    link2Body->SetBoxShape(Vector3::ONE);
    link2Body->SetLinearDamping(0.05f);
    link2Body->SetAngularDamping(0.05f);

    // Joint 2: link1 → link2 (hinge, Z axis) — this is the driven joint
    auto* joint2 = link2Node->CreateComponent<Constraint>();
    joint2->SetConstraintType(CONSTRAINT_HINGE);
    joint2->SetOtherBody(link1Body);
    joint2->SetPosition(Vector3(0.0f, 0.5f, 0.0f));   // top of link2
    joint2->SetOtherPosition(Vector3(0.0f, -0.5f, 0.0f)); // bottom of link1
    joint2->SetAxis(Vector3::FORWARD);
    joint2->SetOtherAxis(Vector3::FORWARD);

    link2_ = link2Node;

    // HUD
    auto* ui = GetSubsystem<UI>();
    auto* text = ui->GetRoot()->CreateChild<Text>();
    text->SetText(
        "Two-Joint Robot — sinusoidal torque on joint 2\n"
        "WASD+mouse | Space=debug draw"
    );
    text->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 12);
    text->SetHorizontalAlignment(HA_CENTER);
    text->SetVerticalAlignment(VA_TOP);
    text->SetPosition(0, 5);

    // Camera
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(300.0f);
    cameraNode_->SetPosition(Vector3(0.0f, 5.0f, -10.0f));
    cameraNode_->LookAt(Vector3(0.0f, 4.0f, 0.0f));
}

void BS_TwoJoint::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_TwoJoint::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_TwoJoint, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_TwoJoint, HandlePostRenderUpdate));
}

void BS_TwoJoint::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    simTime_ += timeStep;

    // Apply sinusoidal torque to joint 2 (link2), faithful to original
    if (link2_)
    {
        float torque = TORQUE_AMPLITUDE * Sin(simTime_ * TORQUE_FREQUENCY * 360.0f);
        auto* body = link2_->GetComponent<RigidBody>();
        if (body)
            body->ApplyTorque(Vector3(0.0f, 0.0f, torque));
    }

    // Camera
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

void BS_TwoJoint::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
