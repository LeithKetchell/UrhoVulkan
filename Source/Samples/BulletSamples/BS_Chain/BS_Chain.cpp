// Chain bridge — hinge-connected rigid body chain between two fixed anchors.
// Demonstrates Bullet's constraint chain stability.

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

#include "BS_Chain.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_Chain)

static const int NUM_LINKS = 15;
static const float LINK_LENGTH = 1.2f;
static const float LINK_MASS = 1.0f;

void BS_Chain::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_Chain::CreateScene()
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
        groundNode->SetPosition(Vector3(0.0f, -5.0f, 0.0f));
        groundNode->SetScale(Vector3(100.0f, 1.0f, 100.0f));
        auto* groundModel = groundNode->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* groundBody = groundNode->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
    }

    // Chain span
    float totalSpan = NUM_LINKS * LINK_LENGTH;
    float startX = -totalSpan / 2.0f;
    float anchorY = 10.0f;

    // Left anchor (static)
    Node* leftAnchor = scene_->CreateChild("LeftAnchor");
    leftAnchor->SetPosition(Vector3(startX - LINK_LENGTH / 2.0f, anchorY, 0.0f));
    leftAnchor->SetScale(Vector3(1.0f, 1.0f, 1.0f));
    auto* leftModel = leftAnchor->CreateComponent<StaticModel>();
    leftModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    leftModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));
    auto* leftBody = leftAnchor->CreateComponent<RigidBody>();
    leftBody->SetBoxShape(Vector3::ONE);

    // Chain links
    Node* prevNode = leftAnchor;
    for (int i = 0; i < NUM_LINKS; i++)
    {
        float x = startX + LINK_LENGTH * (float)i;

        Node* linkNode = scene_->CreateChild("Link");
        linkNode->SetPosition(Vector3(x, anchorY, 0.0f));
        linkNode->SetScale(Vector3(LINK_LENGTH * 0.9f, 0.3f, 0.5f));

        auto* model = linkNode->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* body = linkNode->CreateComponent<RigidBody>();
        body->SetMass(LINK_MASS);
        body->SetBoxShape(Vector3::ONE);
        body->SetFriction(0.5f);
        body->SetLinearDamping(0.05f);
        body->SetAngularDamping(0.05f);

        // Hinge to previous link
        auto* constraint = linkNode->CreateComponent<Constraint>();
        constraint->SetConstraintType(CONSTRAINT_HINGE);
        constraint->SetOtherBody(prevNode->GetComponent<RigidBody>());
        // Connect at the left edge of this link, right edge of previous
        constraint->SetPosition(Vector3(-LINK_LENGTH * 0.45f / linkNode->GetScale().x_, 0.0f, 0.0f));
        constraint->SetOtherPosition(Vector3(LINK_LENGTH * 0.45f / prevNode->GetScale().x_, 0.0f, 0.0f));
        constraint->SetAxis(Vector3::FORWARD);
        constraint->SetOtherAxis(Vector3::FORWARD);

        prevNode = linkNode;
    }

    // Right anchor (static)
    Node* rightAnchor = scene_->CreateChild("RightAnchor");
    rightAnchor->SetPosition(Vector3(startX + NUM_LINKS * LINK_LENGTH + LINK_LENGTH / 2.0f, anchorY, 0.0f));
    rightAnchor->SetScale(Vector3(1.0f, 1.0f, 1.0f));
    auto* rightModel = rightAnchor->CreateComponent<StaticModel>();
    rightModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    rightModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));
    auto* rightBody = rightAnchor->CreateComponent<RigidBody>();
    rightBody->SetBoxShape(Vector3::ONE);

    // Connect last link to right anchor
    auto* endConstraint = rightAnchor->CreateComponent<Constraint>();
    endConstraint->SetConstraintType(CONSTRAINT_HINGE);
    endConstraint->SetOtherBody(prevNode->GetComponent<RigidBody>());
    endConstraint->SetPosition(Vector3(-0.5f, 0.0f, 0.0f));
    endConstraint->SetOtherPosition(Vector3(LINK_LENGTH * 0.45f / prevNode->GetScale().x_, 0.0f, 0.0f));
    endConstraint->SetAxis(Vector3::FORWARD);
    endConstraint->SetOtherAxis(Vector3::FORWARD);

    // Drop a heavy ball onto the middle of the chain
    {
        Node* ballNode = scene_->CreateChild("Ball");
        ballNode->SetPosition(Vector3(0.0f, anchorY + 8.0f, 0.0f));

        auto* model = ballNode->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* body = ballNode->CreateComponent<RigidBody>();
        body->SetMass(10.0f);
        body->SetSphereShape(1.0f);
        body->SetFriction(0.5f);
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Chain Bridge — 15 hinge-linked rigid bodies\n"
        "A heavy ball drops onto the chain\n"
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
    cameraNode_->SetPosition(Vector3(0.0f, 12.0f, -20.0f));
    cameraNode_->LookAt(Vector3(0.0f, 8.0f, 0.0f));
}

void BS_Chain::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_Chain::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_Chain, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_Chain, HandlePostRenderUpdate));
}

void BS_Chain::HandleUpdate(StringHash eventType, VariantMap& eventData)
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

void BS_Chain::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
