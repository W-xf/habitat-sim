// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <Magnum/BulletIntegration/DebugDraw.h>
#include <Magnum/BulletIntegration/Integration.h>

#include "BulletCollision/CollisionShapes/btCompoundShape.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btConvexTriangleMeshShape.h"
#include "BulletCollision/Gimpact/btGImpactShape.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "BulletRigidScene.h"

namespace esp {
namespace physics {

BulletRigidScene::BulletRigidScene(
    scene::SceneNode* rigidBodyNode,
    std::shared_ptr<btMultiBodyDynamicsWorld> bWorld)
    : BulletBase(bWorld), RigidScene{rigidBodyNode} {}

BulletRigidScene::~BulletRigidScene() {
  // remove collision objects from the world
  for (auto& co : bStaticCollisionObjects_) {
    bWorld_->removeCollisionObject(co.get());
  }
}
bool BulletRigidScene::initialization_LibSpecific(
    const assets::ResourceManager& resMgr) {
  const auto collisionAssetHandle =
      initializationAttributes_->getCollisionAssetHandle();

  const std::vector<assets::CollisionMeshData>& meshGroup =
      resMgr.getCollisionMesh(collisionAssetHandle);

  const assets::MeshMetaData& metaData =
      resMgr.getMeshMetaData(collisionAssetHandle);

  constructBulletSceneFromMeshes(Magnum::Matrix4{}, meshGroup, metaData.root);
  for (auto& object : bStaticCollisionObjects_) {
    object->setFriction(initializationAttributes_->getFrictionCoefficient());
    object->setRestitution(
        initializationAttributes_->getRestitutionCoefficient());
    bWorld_->addCollisionObject(object.get());
  }

  return true;

}  // initialization_LibSpecific

void BulletRigidScene::constructBulletSceneFromMeshes(
    const Magnum::Matrix4& transformFromParentToWorld,
    const std::vector<assets::CollisionMeshData>& meshGroup,
    const assets::MeshTransformNode& node) {
  Magnum::Matrix4 transformFromLocalToWorld =
      transformFromParentToWorld * node.transformFromLocalToParent;
  if (node.meshIDLocal != ID_UNDEFINED) {
    const assets::CollisionMeshData& mesh = meshGroup[node.meshIDLocal];

    // SCENE: create a concave static mesh
    btIndexedMesh bulletMesh;

    Corrade::Containers::ArrayView<Magnum::Vector3> v_data = mesh.positions;
    Corrade::Containers::ArrayView<Magnum::UnsignedInt> ui_data = mesh.indices;

    //! Configure Bullet Mesh
    //! This part is very likely to cause segfault, if done incorrectly
    bulletMesh.m_numTriangles = ui_data.size() / 3;
    bulletMesh.m_triangleIndexBase =
        reinterpret_cast<const unsigned char*>(ui_data.data());
    bulletMesh.m_triangleIndexStride = 3 * sizeof(Magnum::UnsignedInt);
    bulletMesh.m_numVertices = v_data.size();
    bulletMesh.m_vertexBase =
        reinterpret_cast<const unsigned char*>(v_data.data());
    bulletMesh.m_vertexStride = sizeof(Magnum::Vector3);
    bulletMesh.m_indexType = PHY_INTEGER;
    bulletMesh.m_vertexType = PHY_FLOAT;
    std::unique_ptr<btTriangleIndexVertexArray> indexedVertexArray =
        std::make_unique<btTriangleIndexVertexArray>();
    indexedVertexArray->addIndexedMesh(bulletMesh, PHY_INTEGER);  // exact shape

    //! Embed 3D mesh into bullet shape
    //! btBvhTriangleMeshShape is the most generic/slow choice
    //! which allows concavity if the object is static
    std::unique_ptr<btBvhTriangleMeshShape> meshShape =
        std::make_unique<btBvhTriangleMeshShape>(indexedVertexArray.get(),
                                                 true);
    meshShape->setMargin(0.04);
    meshShape->setLocalScaling(
        btVector3{transformFromLocalToWorld
                      .scaling()});  // scale is a property of the shape

    // re-build the bvh after setting margin
    meshShape->buildOptimizedBvh();
    std::unique_ptr<btCollisionObject> sceneCollisionObject =
        std::make_unique<btCollisionObject>();
    sceneCollisionObject->setCollisionShape(meshShape.get());
    // rotation|translation are properties of the object
    sceneCollisionObject->setWorldTransform(
        btTransform{btMatrix3x3{transformFromLocalToWorld.rotation()},
                    btVector3{transformFromLocalToWorld.translation()}});

    bSceneArrays_.emplace_back(std::move(indexedVertexArray));
    bSceneShapes_.emplace_back(std::move(meshShape));
    bStaticCollisionObjects_.emplace_back(std::move(sceneCollisionObject));
  }

  for (auto& child : node.children) {
    constructBulletSceneFromMeshes(transformFromLocalToWorld, meshGroup, child);
  }
}  // constructBulletSceneFromMeshes

void BulletRigidScene::setFrictionCoefficient(
    const double frictionCoefficient) {
  for (std::size_t i = 0; i < bStaticCollisionObjects_.size(); i++) {
    bStaticCollisionObjects_[i]->setFriction(frictionCoefficient);
  }
}

void BulletRigidScene::setRestitutionCoefficient(
    const double restitutionCoefficient) {
  for (std::size_t i = 0; i < bStaticCollisionObjects_.size(); i++) {
    bStaticCollisionObjects_[i]->setRestitution(restitutionCoefficient);
  }
}

double BulletRigidScene::getFrictionCoefficient() const {
  if (bStaticCollisionObjects_.size() == 0) {
    return 0.0;
  } else {
    // Assume uniform friction in scene parts
    return bStaticCollisionObjects_.back()->getFriction();
  }
}

double BulletRigidScene::getRestitutionCoefficient() const {
  // Assume uniform restitution in scene parts
  if (bStaticCollisionObjects_.size() == 0) {
    return 0.0;
  } else {
    return bStaticCollisionObjects_.back()->getRestitution();
  }
}

const Magnum::Range3D BulletRigidScene::getCollisionShapeAabb() const {
  Magnum::Range3D combinedAABB;
  // concatenate all component AABBs
  for (auto& object : bStaticCollisionObjects_) {
    btVector3 localAabbMin, localAabbMax;
    object->getCollisionShape()->getAabb(object->getWorldTransform(),
                                         localAabbMin, localAabbMax);
    if (combinedAABB == Magnum::Range3D{}) {
      // override an empty range instead of combining it
      combinedAABB = Magnum::Range3D{Magnum::Vector3{localAabbMin},
                                     Magnum::Vector3{localAabbMax}};
    } else {
      combinedAABB = Magnum::Math::join(
          combinedAABB, Magnum::Range3D{Magnum::Vector3{localAabbMin},
                                        Magnum::Vector3{localAabbMax}});
    }
  }
  return combinedAABB;
}  // getCollisionShapeAabb

}  // namespace physics
}  // namespace esp