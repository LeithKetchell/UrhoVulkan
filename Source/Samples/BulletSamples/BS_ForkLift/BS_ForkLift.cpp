// Port of bulletphysics/bullet3/examples/ForkLift/ForkLiftDemo.cpp
// Copyright (c) Erwin Coumans, zlib license

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
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>

// Raw Bullet for btRaycastVehicle
#include <Bullet/BulletDynamics/Vehicle/btRaycastVehicle.h>
#include <Bullet/BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h>
#include <Bullet/BulletDynamics/ConstraintSolver/btHingeConstraint.h>
#include <Bullet/BulletDynamics/ConstraintSolver/btSliderConstraint.h>

#include "BS_ForkLift.h"

#include <Urho3D/DebugNew.h>

URHO3D_DEFINE_APPLICATION_MAIN(BS_ForkLift)

// Original vehicle tuning parameters (exact values from ForkLiftDemo.cpp)
static const float maxMotorImpulse = 4000.0f;
static const float loadMass = 350.0f;
static const float maxEngineForce = 1000.0f;
static const float maxBreakingForce = 100.0f;
static const float defaultBreakingForce = 10.0f;
static const float steeringIncrement = 0.04f;
static const float steeringClamp = 0.3f;
static const float wheelRadius = 0.5f;
static const float wheelWidth = 0.4f;
static const float wheelFriction = 1000.0f;
static const float suspensionStiffness = 20.0f;
static const float suspensionDamping = 2.3f;
static const float suspensionCompression = 4.4f;
static const float rollInfluence = 0.1f;
static const float suspensionRestLength = 0.6f;
static const float CUBE_HALF_EXTENTS = 1.0f;

BS_ForkLift::~BS_ForkLift()
{
    // Vehicle is owned by the dynamics world after addVehicle, but raycaster is ours
    delete vehicleRayCaster_;
    delete vehicle_;
}

void BS_ForkLift::Start()
{
    Sample::Start();
    CreateScene();
    SetupViewport();
    SubscribeToEvents();
    Sample::InitMouseMode(MM_RELATIVE);
}

void BS_ForkLift::CreateScene()
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

    // === Ground ===
    // Original: btBoxShape(50,50,50) at y=-3
    {
        Node* groundNode = scene_->CreateChild("Ground");
        groundNode->SetPosition(Vector3(0.0f, -3.0f, 0.0f));
        groundNode->SetScale(Vector3(100.0f, 6.0f, 100.0f));
        auto* groundModel = groundNode->CreateComponent<StaticModel>();
        groundModel->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        groundModel->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

        auto* groundBody = groundNode->CreateComponent<RigidBody>();
        groundBody->SetBoxShape(Vector3::ONE);
        groundBody->SetFriction(1.414f);
    }

    // === Chassis ===
    // Original: compound shape — box(1,0.5,2) shifted up by 1 + support box(0.5,0.1,0.5) at (0,1,2.5)
    // Mass = 800
    {
        chassisNode_ = scene_->CreateChild("Chassis");
        chassisNode_->SetPosition(Vector3(0.0f, 1.0f, 0.0f));
        chassisNode_->SetScale(Vector3(2.0f, 1.0f, 4.0f));

        auto* model = chassisNode_->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* chassisBody = chassisNode_->CreateComponent<RigidBody>();
        chassisBody->SetMass(800.0f);
        chassisBody->SetBoxShape(Vector3::ONE);
        chassisBody->SetFriction(0.5f);
        chassisBody->SetLinearDamping(0.0f);
        chassisBody->SetAngularDamping(0.0f);

        // Never deactivate (original: DISABLE_DEACTIVATION)
        btRigidBody* btChassis = chassisBody->GetBody();
        btChassis->setActivationState(DISABLE_DEACTIVATION);

        // === Create btRaycastVehicle ===
        btDiscreteDynamicsWorld* btWorld = physicsWorld->GetWorld();
        vehicleRayCaster_ = new btDefaultVehicleRaycaster(btWorld);

        btRaycastVehicle::btVehicleTuning tuning;
        vehicle_ = new btRaycastVehicle(tuning, btChassis, vehicleRayCaster_);
        vehicle_->setCoordinateSystem(0, 1, 2);  // right=X, up=Y, forward=Z

        btWorld->addVehicle(vehicle_);

        // Add 4 wheels
        btVector3 wheelDirectionCS0(0, -1, 0);
        btVector3 wheelAxleCS(-1, 0, 0);
        float connectionHeight = 1.2f;

        // Front wheels
        btVector3 cp0(CUBE_HALF_EXTENTS - 0.3f * wheelWidth, connectionHeight, 2 * CUBE_HALF_EXTENTS - wheelRadius);
        vehicle_->addWheel(cp0, wheelDirectionCS0, wheelAxleCS, suspensionRestLength, wheelRadius, tuning, true);

        cp0 = btVector3(-CUBE_HALF_EXTENTS + 0.3f * wheelWidth, connectionHeight, 2 * CUBE_HALF_EXTENTS - wheelRadius);
        vehicle_->addWheel(cp0, wheelDirectionCS0, wheelAxleCS, suspensionRestLength, wheelRadius, tuning, true);

        // Rear wheels
        cp0 = btVector3(-CUBE_HALF_EXTENTS + 0.3f * wheelWidth, connectionHeight, -2 * CUBE_HALF_EXTENTS + wheelRadius);
        vehicle_->addWheel(cp0, wheelDirectionCS0, wheelAxleCS, suspensionRestLength, wheelRadius, tuning, false);

        cp0 = btVector3(CUBE_HALF_EXTENTS - 0.3f * wheelWidth, connectionHeight, -2 * CUBE_HALF_EXTENTS + wheelRadius);
        vehicle_->addWheel(cp0, wheelDirectionCS0, wheelAxleCS, suspensionRestLength, wheelRadius, tuning, false);

        // Configure wheel properties
        for (int i = 0; i < vehicle_->getNumWheels(); i++)
        {
            btWheelInfo& wheel = vehicle_->getWheelInfo(i);
            wheel.m_suspensionStiffness = suspensionStiffness;
            wheel.m_wheelsDampingRelaxation = suspensionDamping;
            wheel.m_wheelsDampingCompression = suspensionCompression;
            wheel.m_frictionSlip = wheelFriction;
            wheel.m_rollInfluence = rollInfluence;
        }
    }

    // === Lift mechanism ===
    // Original: box(0.5,2.0,0.05) at (0,2.5,3.05), mass=10
    // Hinged to chassis
    {
        liftNode_ = scene_->CreateChild("Lift");
        liftNode_->SetPosition(Vector3(0.0f, 2.5f, 3.05f));
        liftNode_->SetScale(Vector3(1.0f, 4.0f, 0.1f));

        auto* model = liftNode_->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* liftBody = liftNode_->CreateComponent<RigidBody>();
        liftBody->SetMass(10.0f);
        liftBody->SetBoxShape(Vector3::ONE);

        // Hinge constraint: lift to chassis
        auto* liftHinge = liftNode_->CreateComponent<Constraint>();
        liftHinge->SetConstraintType(CONSTRAINT_HINGE);
        liftHinge->SetOtherBody(chassisNode_->GetComponent<RigidBody>());
        liftHinge->SetWorldPosition(Vector3(0.0f, 1.5f, 3.05f));
        liftHinge->SetAxis(Vector3::RIGHT);
        liftHinge->SetHighLimit(Vector2(0.0f, 0.0f));
        liftHinge->SetLowLimit(Vector2(0.0f, 0.0f));
    }

    // === Fork mechanism ===
    // Original: compound (crossbar + 2 tines), mass=5, slider to lift
    {
        forkNode_ = scene_->CreateChild("Fork");
        forkNode_->SetPosition(Vector3(0.0f, 0.6f, 3.2f));
        forkNode_->SetScale(Vector3(2.0f, 0.2f, 0.2f));

        auto* model = forkNode_->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* forkBody = forkNode_->CreateComponent<RigidBody>();
        forkBody->SetMass(5.0f);
        forkBody->SetBoxShape(Vector3::ONE);

        // Slider constraint: fork slides on lift
        auto* forkSlider = forkNode_->CreateComponent<Constraint>();
        forkSlider->SetConstraintType(CONSTRAINT_SLIDER);
        forkSlider->SetOtherBody(liftNode_->GetComponent<RigidBody>());
        forkSlider->SetWorldPosition(Vector3(0.0f, 0.6f, 3.1f));
        forkSlider->SetAxis(Vector3::UP);
        forkSlider->SetHighLimit(Vector2(0.1f, 0.0f));
        forkSlider->SetLowLimit(Vector2(0.1f, 0.0f));
    }

    // === Load (pallet) ===
    // Original: compound (base + 2 sides), mass=350, at (0,3.5,7)
    {
        loadNode_ = scene_->CreateChild("Load");
        loadNode_->SetPosition(Vector3(0.0f, 3.5f, 7.0f));
        loadNode_->SetScale(Vector3(4.0f, 1.0f, 1.0f));

        auto* model = loadNode_->CreateComponent<StaticModel>();
        model->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        model->SetMaterial(cache->GetResource<Material>("Materials/StoneEnvMapSmall.xml"));
        model->SetCastShadows(true);

        auto* loadBody = loadNode_->CreateComponent<RigidBody>();
        loadBody->SetMass(loadMass);
        loadBody->SetBoxShape(Vector3::ONE);
        loadBody->SetFriction(1.0f);
    }

    // Labels
    auto* ui = GetSubsystem<UI>();
    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "Bullet ForkLiftDemo — Raycast Vehicle\n"
        "Arrow keys: drive/steer\n"
        "Shift+arrows: lift tilt / fork up-down\n"
        "Space=debug draw"
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
    cameraNode_->SetPosition(Vector3(0.0f, 5.0f, -15.0f));
    cameraNode_->LookAt(Vector3(0.0f, 0.0f, 0.0f));
}

void BS_ForkLift::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    renderer->SetViewport(0, viewport);
}

void BS_ForkLift::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(BS_ForkLift, HandleUpdate));
    SubscribeToEvent(E_PHYSICSPRESTEP, URHO3D_HANDLER(BS_ForkLift, HandlePhysicsPreStep));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(BS_ForkLift, HandlePostRenderUpdate));
}

void BS_ForkLift::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    if (!vehicle_)
        return;

    // Apply engine force to rear wheels (2,3), steering to front wheels (0,1)
    vehicle_->applyEngineForce(engineForce_, 2);
    vehicle_->setBrake(breakingForce_, 2);
    vehicle_->applyEngineForce(engineForce_, 3);
    vehicle_->setBrake(breakingForce_, 3);

    vehicle_->setSteeringValue(vehicleSteering_, 0);
    vehicle_->setSteeringValue(vehicleSteering_, 1);
}

void BS_ForkLift::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    auto* input = GetSubsystem<Input>();
    const float MOVE_SPEED = 20.0f;
    const float MOUSE_SENSITIVITY = 0.1f;

    // Camera
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

    // Vehicle controls (original key mapping)
    bool shiftDown = input->GetKeyDown(KEY_LSHIFT) || input->GetKeyDown(KEY_RSHIFT);

    if (!shiftDown)
    {
        // Drive controls
        if (input->GetKeyDown(KEY_UP))
        {
            engineForce_ = maxEngineForce;
            breakingForce_ = 0.0f;
        }
        else if (input->GetKeyDown(KEY_DOWN))
        {
            engineForce_ = -maxEngineForce;
            breakingForce_ = 0.0f;
        }
        else
        {
            engineForce_ = 0.0f;
            breakingForce_ = defaultBreakingForce;
        }

        if (input->GetKeyDown(KEY_LEFT))
        {
            vehicleSteering_ += steeringIncrement;
            if (vehicleSteering_ > steeringClamp)
                vehicleSteering_ = steeringClamp;
        }
        else if (input->GetKeyDown(KEY_RIGHT))
        {
            vehicleSteering_ -= steeringIncrement;
            if (vehicleSteering_ < -steeringClamp)
                vehicleSteering_ = -steeringClamp;
        }
    }
    else
    {
        // Lift/fork controls via raw Bullet constraints
        // Shift+Left/Right: tilt lift hinge
        // Shift+Up/Down: fork slider up/down
        if (liftNode_)
        {
            auto* liftConstraint = liftNode_->GetComponent<Constraint>();
            if (liftConstraint)
            {
                btTypedConstraint* btConstraint = liftConstraint->GetConstraint();
                btHingeConstraint* hinge = static_cast<btHingeConstraint*>(btConstraint);
                if (hinge)
                {
                    if (input->GetKeyDown(KEY_LEFT))
                    {
                        hinge->setLimit(-M_PI / 16.0f, M_PI / 8.0f);
                        hinge->enableAngularMotor(true, -0.1f, maxMotorImpulse);
                    }
                    else if (input->GetKeyDown(KEY_RIGHT))
                    {
                        hinge->setLimit(-M_PI / 16.0f, M_PI / 8.0f);
                        hinge->enableAngularMotor(true, 0.1f, maxMotorImpulse);
                    }
                    else
                    {
                        // Lock hinge at current angle
                        float angle = hinge->getHingeAngle();
                        hinge->enableAngularMotor(false, 0, 0);
                        hinge->setLimit(angle, angle);
                    }
                }
            }
        }

        if (forkNode_)
        {
            auto* forkConstraint = forkNode_->GetComponent<Constraint>();
            if (forkConstraint)
            {
                btTypedConstraint* btConstraint = forkConstraint->GetConstraint();
                btSliderConstraint* slider = static_cast<btSliderConstraint*>(btConstraint);
                if (slider)
                {
                    if (input->GetKeyDown(KEY_UP))
                    {
                        slider->setLowerLinLimit(0.1f);
                        slider->setUpperLinLimit(3.9f);
                        slider->setPoweredLinMotor(true);
                        slider->setMaxLinMotorForce(maxMotorImpulse);
                        slider->setTargetLinMotorVelocity(1.0f);
                    }
                    else if (input->GetKeyDown(KEY_DOWN))
                    {
                        slider->setLowerLinLimit(0.1f);
                        slider->setUpperLinLimit(3.9f);
                        slider->setPoweredLinMotor(true);
                        slider->setMaxLinMotorForce(maxMotorImpulse);
                        slider->setTargetLinMotorVelocity(-1.0f);
                    }
                    else
                    {
                        // Lock fork at current position
                        float pos = slider->getLinearPos();
                        slider->setPoweredLinMotor(false);
                        slider->setLowerLinLimit(pos);
                        slider->setUpperLinLimit(pos);
                    }
                }
            }
        }
    }

    // Reset vehicle
    if (input->GetKeyPress(KEY_R))
        ResetVehicle();
}

void BS_ForkLift::ResetVehicle()
{
    if (chassisNode_)
    {
        chassisNode_->SetPosition(Vector3(0.0f, 1.0f, 0.0f));
        chassisNode_->SetRotation(Quaternion::IDENTITY);
        auto* body = chassisNode_->GetComponent<RigidBody>();
        if (body)
        {
            body->SetLinearVelocity(Vector3::ZERO);
            body->SetAngularVelocity(Vector3::ZERO);
        }
    }
    vehicleSteering_ = 0.0f;
    engineForce_ = 0.0f;
    breakingForce_ = defaultBreakingForce;

    if (vehicle_)
    {
        vehicle_->resetSuspension();
        for (int i = 0; i < vehicle_->getNumWheels(); i++)
            vehicle_->updateWheelTransform(i, true);
    }
}

void BS_ForkLift::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (drawDebug_)
        scene_->GetComponent<PhysicsWorld>()->DrawDebugGeometry(true);
}
