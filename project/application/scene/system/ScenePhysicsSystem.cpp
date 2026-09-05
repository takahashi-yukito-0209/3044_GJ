// 役割: SceneDocumentの物理設定とRuntimeオブジェクトの運動を同期する。
#include "ScenePhysicsSystem.h"

#include "../../../engine/3d/Object3d.h"
#include "../../../engine/collision/Collider.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/collision/SphereCollider.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "../../player/Player.h"

#include <algorithm>
#include <cstdint>
#include <cfloat>
#include <cmath>
#include <string>

namespace {
	bool RayIntersectObb(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		const OBBCollider& collider,
		float& outDistance
	) {
		const OBBCollider::OBB obb = collider.GetOBB();
		const Vector3 delta = Math::Subtract(obb.center, origin);
		const float halfSizes[3] = {
			obb.halfSize.x, obb.halfSize.y, obb.halfSize.z
		};
		float minDistance = 0.0f;
		float maxHitDistance = maxDistance;
		for (uint32_t index = 0; index < 3; ++index) {
			const Vector3& axis = obb.axis[index];
			const float projectedOrigin = Math::Dot(axis, delta);
			const float projectedDirection = Math::Dot(axis, direction);
			if (std::abs(projectedDirection) > 0.000001f) {
				float first = (projectedOrigin + halfSizes[index]) / projectedDirection;
				float second = (projectedOrigin - halfSizes[index]) / projectedDirection;
				if (first > second) { std::swap(first, second); }
				minDistance = (std::max)(minDistance, first);
				maxHitDistance = (std::min)(maxHitDistance, second);
				if (minDistance > maxHitDistance) { return false; }
			} else if (
				-projectedOrigin - halfSizes[index] > 0.0f ||
				-projectedOrigin + halfSizes[index] < 0.0f
			) {
				return false;
			}
		}
		outDistance = minDistance;
		return outDistance >= 0.0f && outDistance <= maxDistance;
	}

	Vector3 GetObbSurfaceNormal(const OBBCollider& collider, const Vector3& position) {
		const OBBCollider::OBB obb = collider.GetOBB();
		const Vector3 delta = Math::Subtract(position, obb.center);
		const float halfSizes[3] = { obb.halfSize.x, obb.halfSize.y, obb.halfSize.z };
		uint32_t closestAxis = 0;
		float closestDifference = FLT_MAX;
		float projected[3]{};
		for (uint32_t index = 0; index < 3; ++index) {
			projected[index] = Math::Dot(delta, obb.axis[index]);
			const float difference = std::abs(std::abs(projected[index]) - halfSizes[index]);
			if (difference < closestDifference) {
				closestDifference = difference;
				closestAxis = index;
			}
		}
		return Math::Multiply(obb.axis[closestAxis], projected[closestAxis] >= 0.0f ? 1.0f : -1.0f);
	}

	bool RayIntersectSphere(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		const SphereCollider& collider,
		float& outDistance
	) {
		const Vector3 toCenter = Math::Subtract(collider.GetWorldCenter(), origin);
		const float projectedDistance = Math::Dot(toCenter, direction);
		const float centerDistanceSq = Math::Dot(toCenter, toCenter);
		const float radius = collider.GetRadius();
		const float perpendicularDistanceSq = centerDistanceSq -
			projectedDistance * projectedDistance;
		if (perpendicularDistanceSq > radius * radius) { return false; }
		const float offset = std::sqrt((std::max)(radius * radius - perpendicularDistanceSq, 0.0f));
		outDistance = projectedDistance - offset;
		if (outDistance < 0.0f) { outDistance = projectedDistance + offset; }
		return outDistance >= 0.0f && outDistance <= maxDistance;
	}

	PhysicsBodyType ToPhysicsBodyType(const std::string& bodyType) {
		if (bodyType == "Dynamic") {
			return PhysicsBodyType::Dynamic;
		}
		if (bodyType == "Kinematic") {
			return PhysicsBodyType::Kinematic;
		}
		return PhysicsBodyType::Static;
	}

	bool IsPointInsideWaterVolume(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& waterVolume,
		const Vector3& point
	) {
		const Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const Vector3 center = {
			transform.translate.x + waterVolume.waterOffset.x,
			transform.translate.y + waterVolume.waterOffset.y,
			transform.translate.z + waterVolume.waterOffset.z
		};
		const Vector3 halfSize = {
			(std::max)(waterVolume.waterHalfSize.x, 0.001f),
			(std::max)(waterVolume.waterHalfSize.y, 0.001f),
			(std::max)(waterVolume.waterHalfSize.z, 0.001f)
		};

		return
			std::abs(point.x - center.x) <= halfSize.x &&
			std::abs(point.y - center.y) <= halfSize.y &&
			std::abs(point.z - center.z) <= halfSize.z;
	}

	const SceneRuntimeObjectBinding* FindBinding(
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		uint64_t entityId
	) {
		for (const SceneRuntimeObjectBinding& binding : bindings) {
			if (binding.entity && binding.entity->id == entityId) {
				return &binding;
			}
		}
		return nullptr;
	}
}

void ScenePhysicsSystem::ApplyBodyComponent(
	PhysicsBody& body,
	const SceneComponent& component,
	Object3d* object,
	Collider* collider,
	bool resetVelocity
) const {
	const Vector3 runtimeVelocity = body.velocity;
	body.type = ToPhysicsBodyType(component.physicsBodyType);
	body.transform = object ? &object->GetTransform() : nullptr;
	body.syncTransform = [object]() {
		if (object) {
			object->Update();
		}
	};
	body.collider = collider;
	body.mass = (std::max)(component.physicsMass, 0.001f);
	body.useGravity = component.physicsUseGravity;
	body.gravityScale = component.physicsGravityScale;
	body.drag = (std::max)(component.physicsDrag, 0.0f);
	body.restitution = std::clamp(component.physicsRestitution, 0.0f, 1.0f);
	body.friction = std::clamp(component.physicsFriction, 0.0f, 1.0f);
	body.maxFallSpeed = (std::max)(component.physicsMaxFallSpeed, 0.0f);
	body.freezePositionX = component.physicsFreezePositionX;
	body.freezePositionY = component.physicsFreezePositionY;
	body.freezePositionZ = component.physicsFreezePositionZ;
	body.velocity = resetVelocity ? component.physicsVelocity : runtimeVelocity;
}

void ScenePhysicsSystem::SyncSceneSettings(
	const SceneDocument& document,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	bool editing
) {
	ApplyPlayerBehavior(document, player);
	ApplyPlayerPhysics(document, player, bindings, editing);
	ApplyWaterVolumes(document, player);
	RebuildStaticColliders(document, bindings);
}

void ScenePhysicsSystem::Step(
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	bool playing
) {
	if (!playing) {
		return;
	}

	// BodyとColliderは非所有なので、現在のbindingsからWorld登録を毎Step再構築する。
	physicsWorld_.Clear();
	for (Collider* collider : staticColliders_) {
		physicsWorld_.AddStaticCollider(collider);
	}
	if (player && player->GetObject()) {
		physicsWorld_.AddBody(&player->GetPhysicsBody());
		for (Collider* collider : formationObstacleColliders_) {
			physicsWorld_.AddIgnoredCollision(
				&player->GetPhysicsBody(),
				collider
			);
		}
	}
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!binding.body ||
			SceneEntityQuery::HasComponent(*binding.entity, "PlayerBehavior")
		) {
			continue;
		}
		physicsWorld_.AddBody(binding.body);
	}

	physicsWorld_.Step(deltaTime);

	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!binding.body ||
			SceneEntityQuery::HasComponent(*binding.entity, "PlayerBehavior")
		) {
			continue;
		}
		binding.object->Update();
		const Transform& runtimeTransform = binding.object->GetTransform();
		binding.entity->transform.scale = runtimeTransform.scale;
		binding.entity->transform.rotate = runtimeTransform.useQuaternionRotation
			? runtimeTransform.quaternionRotate
			: MakeQuaternionFromEuler(runtimeTransform.rotate);
		binding.entity->transform.translate = runtimeTransform.translate;
	}
}

void ScenePhysicsSystem::ResetBodies(
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	const std::vector<uint64_t>& entityIds
) const {
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.body ||
			std::find(entityIds.begin(), entityIds.end(), binding.entity->id) ==
				entityIds.end()) {
			continue;
		}
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "PhysicsBody");
		if (component) {
			binding.body->velocity = component->physicsVelocity;
		}
	}
}

bool ScenePhysicsSystem::RaycastStatic(
	const Vector3& origin,
	const Vector3& direction,
	float maxDistance,
	SceneStaticRaycastHit& outHit
) const {
	const float safeDistance = (std::max)(maxDistance, 0.0f);
	if (safeDistance <= 0.0f || Math::Length(direction) <= 0.000001f) {
		return false;
	}
	const Vector3 normalizedDirection = Math::Normalize(direction);
	float closestDistance = safeDistance;
	const Collider* closestCollider = nullptr;
	bool found = false;
	for (const Collider* collider : staticColliders_) {
		if (!collider || !collider->IsActive() || collider->IsTrigger()) {
			continue;
		}
		float distance = 0.0f;
		const bool hit = collider->GetType() == Collider::Type::OBB
			? RayIntersectObb(
				origin, normalizedDirection, closestDistance,
				static_cast<const OBBCollider&>(*collider), distance
			)
			: RayIntersectSphere(
				origin, normalizedDirection, closestDistance,
				static_cast<const SphereCollider&>(*collider), distance
			);
		if (!hit) { continue; }
		closestDistance = distance;
		closestCollider = collider;
		found = true;
	}
	if (found) {
		outHit.distance = closestDistance;
		outHit.position = Math::Add(
			origin, Math::Multiply(normalizedDirection, closestDistance)
		);
		outHit.normal = closestCollider->GetType() == Collider::Type::OBB
			? GetObbSurfaceNormal(static_cast<const OBBCollider&>(*closestCollider), outHit.position)
			: Math::Normalize(Math::Subtract(
				outHit.position,
				static_cast<const SphereCollider&>(*closestCollider).GetWorldCenter()
			));
	}
	return found;
}

void ScenePhysicsSystem::Clear() {
	physicsWorld_.Clear();
	staticColliders_.clear();
	formationObstacleColliders_.clear();
}

void ScenePhysicsSystem::ApplyPlayerBehavior(
	const SceneDocument& document,
	Player* player
) const {
	if (!player) {
		return;
	}
	const SceneEntity* playerEntity = document.FindEntityByName("Player");
	const SceneComponent* behavior = playerEntity
		? SceneEntityQuery::FindEnabledComponent(*playerEntity, "PlayerBehavior")
		: nullptr;
	if (!behavior) {
		return;
	}

	player->SetBehaviorSettings(
		behavior->playerMoveSpeed,
		behavior->playerJumpVelocity,
		behavior->playerTurnResponsiveness,
		behavior->playerDashMultiplier,
		behavior->playerCameraRelativeMove,
		behavior->playerAllowJump,
		behavior->playerAutoForward
	);
	player->SetInputDeviceSettings(
		behavior->playerInputMode,
		behavior->playerGamepadDeadzone
	);
}

void ScenePhysicsSystem::ApplyPlayerPhysics(
	const SceneDocument& document,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	bool editing
) const {
	if (!player || !player->GetObject()) {
		return;
	}
	const SceneEntity* playerEntity = document.FindEntityByName("Player");
	if (
		!playerEntity ||
		!SceneEntityQuery::HasComponent(*playerEntity, "PlayerBehavior")
	) {
		return;
	}

	const SceneRuntimeObjectBinding* binding =
		FindBinding(bindings, playerEntity->id);
	player->SetCollider(binding ? binding->collider : nullptr);
	const SceneComponent* component =
		SceneEntityQuery::FindEnabledComponent(*playerEntity, "PhysicsBody");
	if (!component) {
		return;
	}

	PhysicsBody& body = player->GetPhysicsBody();
	ApplyBodyComponent(
		body,
		*component,
		player->GetObject(),
		player->GetCollider(),
		editing
	);
	if (body.type == PhysicsBodyType::Static) {
		body.type = PhysicsBodyType::Dynamic;
	}
}

void ScenePhysicsSystem::ApplyWaterVolumes(
	const SceneDocument& document,
	Player* player
) const {
	if (!player || !player->GetObject()) {
		return;
	}
	player->SetWaterState(false, 1.0f, 0.0f);

	const Vector3 playerPosition =
		player->GetObject()->GetTransform().translate;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* waterVolume =
			SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume");
		if (
			!waterVolume ||
			!IsPointInsideWaterVolume(
				document,
				entity,
				*waterVolume,
				playerPosition
			)
		) {
			continue;
		}

		player->SetWaterState(
			true,
			waterVolume->waterMoveSpeedMultiplier,
			waterVolume->waterSwimUpSpeed
		);
		PhysicsBody& body = player->GetPhysicsBody();
		body.gravityScale = waterVolume->waterGravityScale;
		body.drag = (std::max)(body.drag, waterVolume->waterDrag);
		body.maxFallSpeed =
			(std::max)(waterVolume->waterMaxFallSpeed, 0.0f);
		return;
	}
}

void ScenePhysicsSystem::RebuildStaticColliders(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	staticColliders_.clear();
	formationObstacleColliders_.clear();
	bool formationCollisionDelegated = false;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}
		const SceneComponent* director =
			SceneEntityQuery::FindEnabledComponent(
				entity,
				"FishingScoreAttackDirector"
			);
		if (director && director->fishingUseFormationCapsuleCollision) {
			formationCollisionDelegated = true;
			break;
		}
	}
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.collider ||
			SceneEntityQuery::HasComponent(
				*binding.entity,
				"PlayerBehavior"
			) ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(
				document,
				*binding.entity
			)
		) {
			continue;
		}
		const SceneComponent* fishingObstacle =
			SceneEntityQuery::FindEnabledComponent(
				*binding.entity,
				"FishingObstacle"
			);
		if (
			formationCollisionDelegated &&
			fishingObstacle &&
			binding.collider->GetType() == Collider::Type::OBB &&
			binding.collider->IsActive() &&
			!binding.collider->IsTrigger()
		) {
			formationObstacleColliders_.push_back(binding.collider);
		}
		if (binding.body) {
			continue;
		}
		staticColliders_.push_back(binding.collider);
	}
}
