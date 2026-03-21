// Fox — small predator.

#include "Fox.h"

#include <Urho3D/Core/Context.h>

Fox::Fox(Context* context) :
    Animal(context)
{
}

void Fox::RegisterObject(Context* context)
{
    context->RegisterFactory<Fox>();
}
