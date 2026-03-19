// Rabbit — small herbivore.

#include "Rabbit.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Graphics/AnimationController.h>
#include <Urho3D/Math/MathDefs.h>

Rabbit::Rabbit(Context* context) :
    Animal(context)
{
}

void Rabbit::RegisterObject(Context* context)
{
    context->RegisterFactory<Rabbit>();
}

void Rabbit::OnStateEnter(AnimalState newState)
{
    auto* animCtrl = node_->GetComponent<AnimationController>(true);
    if (!animCtrl)
    {
        Animal::OnStateEnter(newState);
        return;
    }

    switch (newState)
    {
    case ANIMAL_IDLE:
        // Randomly choose between sitting and basic idle
        if (Random(1.0f) < 0.6f)
            animCtrl->PlayExclusive("Models/Animals/Rabbit_ArmatureSitting.000.ani", 0, true, 0.3f);
        else
            animCtrl->PlayExclusive("Models/Animals/Rabbit_ArmatureBasic.ani", 0, true, 0.3f);
        break;

    case ANIMAL_WANDER:
        animCtrl->PlayExclusive("Models/Animals/Rabbit_ArmatureRunning.ani", 0, true, 0.3f);
        animCtrl->SetSpeed("Models/Animals/Rabbit_ArmatureRunning.ani", 0.4f);
        break;

    case ANIMAL_FLEE:
        animCtrl->PlayExclusive("Models/Animals/Rabbit_ArmatureRunning.ani", 0, true, 0.2f);
        animCtrl->SetSpeed("Models/Animals/Rabbit_ArmatureRunning.ani", 2.0f);
        break;

    case ANIMAL_DIE:
        animCtrl->PlayExclusive("Models/Animals/Rabbit_ArmatureDying.000.ani", 0, false, 0.2f);
        break;
    }
}
