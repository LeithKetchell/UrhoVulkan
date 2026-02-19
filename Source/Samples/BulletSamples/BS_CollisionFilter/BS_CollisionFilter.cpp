// Collision filtering — demonstrates layer/mask collision groups.
// Red, green, and blue objects each collide with the ground but
// only selectively with each other.

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

#include "BS_CollisionFilter.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_CollisionFilter)

// Collision layers (bit masks)
static const unsigned LAYER_GROUND = 1;
static const unsigned LAYER_RED    = 2;
static const unsigned LAYER_GREEN  = 4;
static const unsigned LAYER_BLUE   = 8;

void BS_CollisionFilter::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_CollisionFilter::CreateScene()
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

    // Ground — collides with ALL groups
    {
        Node* groundNode = scene_->CreateChild("Ground");
        groundNode->SetPosition(Vector3(0.0f, -0.5f, 0.0f));
        groundNode->SetScale(Vector3(40.0f, 1.0f, 40.0f));
        auto* groundModel = groundNode->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* groundBody = groundNode->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
        groundBody->SetCollisionLayer(LAYER_GROUND);
        groundBody->SetCollisionMask(LAYER_RED | LAYER_GREEN | LAYER_BLUE);
    }

    // Red column — collides with ground + green (NOT blue)
    // Blue column — collides with ground + green (NOT red)
    // Green column — collides with ground + red + blue (everything)
    // Result: red and blue fall through each other, green blocks both

    struct GroupDef {
        const char* name;
        unsigned layer;
        unsigned mask;
        float x;
        String material;
    };

    GroupDef groups[] = {
        {"Red",   LAYER_RED,   LAYER_GROUND | LAYER_GREEN,                      -3.0f, "Materials/StoneEnvMapSmall.xml"},
        {"Green", LAYER_GREEN, LAYER_GROUND | LAYER_RED | LAYER_BLUE,            0.0f, "Materials/StoneTiled.xml"},
        {"Blue",  LAYER_BLUE,  LAYER_GROUND | LAYER_GREEN,                       3.0f, "Materials/StoneEnvMapSmall.xml"},
    };

    for (int g = 0; g < 3; g++)
    {
        for (int i = 0; i < 5; i++)
        {
            Node* node = scene_->CreateChild(groups[g].name);
            node->SetPosition(Vector3(groups[g].x, 1.0f + i * 2.0f, 0.0f));
            node->SetScale(Vector3(1.0f, 1.0f, 1.0f));

            auto* model = node->CreateComponent<StaticModel>();
            model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
            model->SetMaterial(cache->GetResource<Material>(groups[g].material));
            model->SetCastShadows(true);

            auto* body = node->CreateComponent<RigidBody>();
            body->SetMass(1.0f);
            body->SetBoxShape(Vector3::ONE);
            body->SetFriction(0.5f);
            body->SetCollisionLayer(groups[g].layer);
            body->SetCollisionMask(groups[g].mask);
        }
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Collision Filtering — layer/mask groups\n"
        "Red & Blue pass through each other\n"
        "Green collides with everything\n"
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
    cameraNode_->SetPosition(Vector3(0.0f, 8.0f, -15.0f));
    cameraNode_->LookAt(Vector3(0.0f, 3.0f, 0.0f));
}

void BS_CollisionFilter::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_CollisionFilter::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_CollisionFilter, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_CollisionFilter, HandlePostRenderUpdate));
}

void BS_CollisionFilter::HandleUpdate(StringHash eventType, VariantMap& eventData)
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

void BS_CollisionFilter::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
