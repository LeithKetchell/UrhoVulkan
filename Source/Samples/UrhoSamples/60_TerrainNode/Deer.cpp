// Deer — medium herbivore.

#include "Deer.h"

#include <Urho3D/Core/Context.h>

Deer::Deer(Context* context) :
    Animal(context)
{
}

void Deer::RegisterObject(Context* context)
{
    context->RegisterFactory<Deer>();
}
