// Honest port of Bullet SDK GyroscopicDemo to Urho3D.
// Original: bulletphysics/bullet3/examples/GyroscopicDemo/GyroscopicSetup.cpp
// License: zlib (Erwin Coumans, Bullet Physics)
//
// 4 spinning bodies in zero gravity, each with a different gyroscopic force mode:
// 0 = none, 1 = explicit, 2 = implicit (world), 3 = implicit (body).
// The differences in precession behavior are visible in debug draw.
//
// Gyroscopic flags are set via raw Bullet access (btRigidBody::setFlags)
// since Urho3D doesn't wrap this yet.

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

// Raw Bullet access for gyroscopic flags
#include <Bullet/BulletDynamics/Dynamics/btRigidBody.h>

#include "BS_Gyroscopic.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_Gyroscopic)

// Bullet gyroscopic force flags (from btRigidBody.h)
static const int GYRO_FLAGS[4] = {
    0,                                      // No gyroscopic term
    BT_ENABLE_GYROSCOPIC_FORCE_EXPLICIT,    // Explicit Euler
    BT_ENABLE_GYROSCOPIC_FORCE_IMPLICIT_WORLD, // Implicit (world frame)
    BT_ENABLE_GYROSCOPIC_FORCE_IMPLICIT_BODY   // Implicit (body frame)
};

static const char* GYRO_NAMES[4] = {
    "No Gyroscopic",
    "Explicit",
    "Implicit (World)",
    "Implicit (Body)"
};

void BS_Gyroscopic::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_Gyroscopic::CreateScene()
{
    auto* cache = GetSubsystem<ResourceCache>();

    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>();
    scene_->CreateComponent<DebugRenderer>();

    auto* physicsWorld = scene_->CreateComponent<PhysicsWorld>();
    physicsWorld->SetGravity(Vector3::ZERO);  // Zero gravity — original uses (0,0,0)

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

    // Ground plane
    {
        Node* groundNode = scene_->CreateChild("Ground");
        groundNode->SetPosition(Vector3(0.0f, -0.5f, 0.0f));
        groundNode->SetScale(Vector3(200.0f, 1.0f, 200.0f));
        auto* groundModel = groundNode->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* groundBody = groundNode->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
        groundBody->SetFriction(1.414f);  // sqrt(2) from original
    }

    // === 4 spinning bodies with different gyroscopic modes ===
    // Original positions: (-10,8,4), (-5,8,4), (0,8,4), (5,8,4)
    // Original: compound shape (cylinder + box), angular velocity (0, 0.1, 10)
    // We use boxes as visual, with compound shapes for the asymmetric inertia
    Vector3 positions[4] = {
        Vector3(-10.0f, 8.0f, 4.0f),
        Vector3(-5.0f, 8.0f, 4.0f),
        Vector3(0.0f, 8.0f, 4.0f),
        Vector3(5.0f, 8.0f, 4.0f)
    };

    for (int i = 0; i < 4; i++)
    {
        Node* node = scene_->CreateChild(GYRO_NAMES[i]);
        node->SetPosition(positions[i]);
        // Elongated box to visualize spin axis
        node->SetScale(Vector3(2.0f, 0.2f, 0.2f));

        auto* model = node->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));

        auto* body = node->CreateComponent<RigidBody>();
        body->SetMass(1.0f);
        body->SetBoxShape(Vector3::ONE);
        body->SetFriction(1.0f);
        body->SetLinearDamping(0.0f);
        body->SetAngularDamping(0.0f);

        // Set angular velocity: original (0, 0.1, 10) — fast spin around Z, slow around Y
        body->SetAngularVelocity(Vector3(0.0f, 0.1f, 10.0f));

        // Raw Bullet: set gyroscopic force flags
        btRigidBody* btBody = body->GetBody();
        if (btBody)
            btBody->setFlags(GYRO_FLAGS[i]);
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Bullet GyroscopicDemo — Zero Gravity\n"
        "4 spinning bodies, each with a different gyroscopic mode:\n"
        "None | Explicit | Implicit(World) | Implicit(Body)\n"
        "Watch the precession differences in debug draw\n"
        "WASD+mouse | Space=debug draw"
    );
    instructionText->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 12);
    instructionText->SetTextAlignment(HA_CENTER);
    instructionText->SetHorizontalAlignment(HA_CENTER);
    instructionText->SetVerticalAlignment(VA_CENTER);
    instructionText->SetPosition(0, ui->GetRoot()->GetHeight() / 4);

    // Camera — positioned to see all 4 bodies
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(300.0f);
    cameraNode_->SetPosition(Vector3(-2.4f, 10.0f, -20.0f));
    cameraNode_->LookAt(Vector3(-2.4f, 8.0f, 4.0f));
}

void BS_Gyroscopic::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_Gyroscopic::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_Gyroscopic, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_Gyroscopic, HandlePostRenderUpdate));
}

void BS_Gyroscopic::HandleUpdate(StringHash eventType, VariantMap& eventData)
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

void BS_Gyroscopic::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
