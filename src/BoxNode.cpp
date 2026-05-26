#include "BoxNode.h"
#include "PhysicsWorld.h"
void BoxNode::CreateBoxNode(PhysicsWorld* physicsWorld)
{
    auto node = GetScene()->CreateChild("BoxNode");
    auto model = node->CreateComponent<StaticModel>();
    auto rigidBody = node->CreateComponent<RigidBody>();
    auto collisionShape = node->CreateComponent<CollisionShape>();
    model->SetModel(GetAssetManager()->GetAsset<StaticModel>("Models/Box.mdl"));
    rigidBody->SetMass(1.0f);
    collisionShape->SetShapeType(CollisionShape::Box);
}