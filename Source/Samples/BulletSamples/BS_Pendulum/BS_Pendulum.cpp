// Newton's cradle — point-to-point constrained pendulums.
// Demonstrates energy transfer through a chain of rigid bodies.

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
#include <Urho3D/Physics/Constraint.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>

#include "BS_Pendulum.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_Pendulum)

static const int NUM_BALLS = 5;
static const float BALL_RADIUS = 0.5f;
static const float CHAIN_LENGTH = 5.0f;
static const float BALL_MASS = 1.0f;

void BS_Pendulum::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_Pendulum::CreateScene()
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

    // Support beam (static, above the pendulums)
    float beamY = CHAIN_LENGTH + 2.0f;
    {
        Node* beamNode = scene_->CreateChild("Beam");
        beamNode->SetPosition(Vector3(0.0f, beamY, 0.0f));
        beamNode->SetScale(Vector3((NUM_BALLS + 1) * BALL_RADIUS * 2.0f, 0.3f, 0.3f));
        auto* beamModel = beamNode->CreateComponent<StaticModel>();
        beamModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        beamModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* beamBody = beamNode->CreateComponent<RigidBody>();
        beamBody->SetBoxShape(Vector3::ONE);
    }

    // Pendulum balls — each hung from the beam by a point-to-point constraint
    float startX = -(NUM_BALLS - 1) * BALL_RADIUS;

    for (int i = 0; i < NUM_BALLS; i++)
    {
        float x = startX + i * BALL_RADIUS * 2.0f;
        float y = beamY - CHAIN_LENGTH;

        Node* ballNode = scene_->CreateChild("Ball");
        ballNode->SetPosition(Vector3(x, y, 0.0f));

        auto* model = ballNode->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Sphere.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* body = ballNode->CreateComponent<RigidBody>();
        body->SetMass(BALL_MASS);
        body->SetSphereShape(BALL_RADIUS * 2.0f);
        body->SetFriction(0.0f);
        body->SetRestitution(1.0f);
        body->SetLinearDamping(0.0f);
        body->SetAngularDamping(0.0f);

        // Constrain to XY plane to keep it a clean 2D swing
        body->SetLinearFactor(Vector3(1.0f, 1.0f, 0.0f));
        body->SetAngularFactor(Vector3(0.0f, 0.0f, 1.0f));

        // Point-to-point constraint to the beam
        auto* constraint = ballNode->CreateComponent<Constraint>();
        constraint->SetConstraintType(CONSTRAINT_POINT);
        constraint->SetOtherBody(scene_->GetChild("Beam")->GetComponent<RigidBody>());
        constraint->SetPosition(Vector3(0.0f, CHAIN_LENGTH, 0.0f));
        constraint->SetOtherPosition(Vector3(
            x / ((NUM_BALLS + 1) * BALL_RADIUS * 2.0f),
            0.0f, 0.0f
        ));

        // Pull the first ball to the side to start the cradle
        if (i == 0)
        {
            ballNode->SetPosition(Vector3(x - 3.0f, y + 1.0f, 0.0f));
        }
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Newton's Cradle — 5 pendulum balls\n"
        "Restitution=1.0, Friction=0.0\n"
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
    cameraNode_->SetPosition(Vector3(0.0f, 5.0f, -12.0f));
    cameraNode_->LookAt(Vector3(0.0f, 4.0f, 0.0f));
}

void BS_Pendulum::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_Pendulum::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_Pendulum, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_Pendulum, HandlePostRenderUpdate));
}

void BS_Pendulum::HandleUpdate(StringHash eventType, VariantMap& eventData)
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

void BS_Pendulum::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
