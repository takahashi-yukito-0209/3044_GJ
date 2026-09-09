// 役割: Agent Componentから群れと個体の移動状態を更新する。
#include "SceneAgentSystem.h"

#include "../../../engine/3d/Object3d.h"
#include "../../../engine/agent/AgentSettingsResolver.h"
#include "../../../engine/agent/AgentSteering.h"
#include "../../../engine/collision/Collider.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/collision/SphereCollider.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/physics/PhysicsBody.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
	using AgentSettingsResolver::ResolveAgentBehaviorSettings;
	using AgentSettingsResolver::ResolveTeamLeaderSettings;
	using AgentSteering::AddScaled;
	using AgentSteering::BlendDirections;
	using AgentSteering::BuildAgentVelocityRotation;
	using AgentSteering::BuildFlockMemberJitterTarget;
	using AgentSteering::BuildFlockRuntimeSeed;
	using AgentSteering::BuildFlockWanderDirection;
	using AgentSteering::ClampVectorLength;
	using AgentSteering::FollowAmount;
	using AgentSteering::ForwardDirectionFromRotation;
	using AgentSteering::Hash01;
	using AgentSteering::LerpVector;
	using AgentSteering::MoveVectorToward;
	using AgentSteering::RotateDirection;
	using AgentSteering::RotateDirectionToward;
	using AgentSteering::SafeNormalize;
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;

	void ApplyAgentRotation(Transform& transform, const Vector3& rotate) {
		transform.rotate = rotate;
		transform.quaternionRotate = MakeQuaternionFromEuler(rotate);
		transform.useQuaternionRotation = true;
	}

	void SynchronizeSceneTransform(
		const SceneDocument& document,
		SceneEntity& entity,
		Object3d& object,
		const Transform& worldTransform
	) {
		Transform localTransform{};
		if (!SceneTransformResolver::TryConvertSceneWorldTransformToLocal(
			document,
			entity,
			worldTransform,
			localTransform
		)) {
			return;
		}
		entity.transform.scale = localTransform.scale;
		entity.transform.rotate = localTransform.quaternionRotate;
		entity.transform.translate = localTransform.translate;
		object.GetTransform() = localTransform;
		object.Update();
	}

	bool IsPointInsideWaterVolume(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& waterVolume,
		const Vector3& point
	) {
		const Transform transform = ResolveScene3DTransform(document, entity);
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

	struct AgentBounds {
		bool valid = false;
		Vector3 center{};
		Vector3 halfSize{};
	};

	struct AgentAttractorTarget {
		bool valid = false;
		Vector3 position{};
		float radius = 0.0f;
		float strength = 0.0f;
	};

	std::string ResolveAgentTeamRuntimeKey(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
		if (team && !team->name.empty()) {
			return "team:" + team->name;
		}
		return {};
	}

	std::string ResolveGroundCrowdKey(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& behavior
	) {
		const std::string teamKey = ResolveAgentTeamRuntimeKey(document, entity);
		if (!teamKey.empty()) {
			return teamKey;
		}
		if (!behavior.agentGroupName.empty()) {
			return "group:" + behavior.agentGroupName;
		}
		return "behavior:" + behavior.agentBehaviorName +
			"/profile:" + behavior.agentProfileName;
	}

	bool TryResolveWaterBounds(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& waterVolume,
		AgentBounds& bounds
	) {
		const Transform transform = ResolveScene3DTransform(document, entity);
		bounds.center = {
			transform.translate.x + waterVolume.waterOffset.x,
			transform.translate.y + waterVolume.waterOffset.y,
			transform.translate.z + waterVolume.waterOffset.z
		};
		bounds.halfSize = {
			(std::max)(waterVolume.waterHalfSize.x, 0.1f),
			(std::max)(waterVolume.waterHalfSize.y, 0.1f),
			(std::max)(waterVolume.waterHalfSize.z, 0.1f)
		};
		bounds.valid = true;
		return true;
	}

	bool TryResolveAgentBounds(
		const SceneDocument& document,
		const SceneComponent& behavior,
		const Vector3& position,
		AgentBounds& bounds
	) {
		const SceneEntity* boundsEntity = nullptr;
		if (behavior.agentBoundsEntityId != 0) {
			boundsEntity = document.FindEntity(behavior.agentBoundsEntityId);
		}
		if (!boundsEntity && !behavior.agentBoundsName.empty()) {
			boundsEntity = document.FindEntityByName(behavior.agentBoundsName);
		}
		if (boundsEntity && IsEntityActiveInHierarchy(document, *boundsEntity)) {
			if (const SceneComponent* waterVolume =
				FindEnabledComponent(*boundsEntity, "WaterVolume")) {
				return TryResolveWaterBounds(
					document,
					*boundsEntity,
					*waterVolume,
					bounds
				);
			}
			const Transform transform =
				ResolveScene3DTransform(document, *boundsEntity);
			bounds.center = transform.translate;
			bounds.halfSize = {
				(std::max)(std::abs(transform.scale.x), 0.1f),
				(std::max)(std::abs(transform.scale.y), 0.1f),
				(std::max)(std::abs(transform.scale.z), 0.1f)
			};
			bounds.valid = true;
			return true;
		}
		if (!behavior.agentUseWaterBounds) {
			return false;
		}

		const SceneEntity* fallback = nullptr;
		const SceneComponent* fallbackWater = nullptr;
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity)) {
				continue;
			}
			const SceneComponent* waterVolume =
				FindEnabledComponent(entity, "WaterVolume");
			if (!waterVolume) {
				continue;
			}
			if (IsPointInsideWaterVolume(
				document,
				entity,
				*waterVolume,
				position
			)) {
				return TryResolveWaterBounds(
					document,
					entity,
					*waterVolume,
					bounds
				);
			}
			if (!fallback) {
				fallback = &entity;
				fallbackWater = waterVolume;
			}
		}
		return fallback && fallbackWater
			? TryResolveWaterBounds(document, *fallback, *fallbackWater, bounds)
			: false;
	}

	bool AttractorMatchesAgent(
		const SceneComponent& attractor,
		const SceneComponent& behavior
	) {
		if (
			!attractor.attractorTargetBehaviorName.empty() &&
			attractor.attractorTargetBehaviorName != behavior.agentBehaviorName
		) {
			return false;
		}
		if (
			!attractor.attractorTargetProfileName.empty() &&
			attractor.attractorTargetProfileName != behavior.agentProfileName
		) {
			return false;
		}
		return true;
	}

	bool TryResolveAgentAttractor(
		const SceneDocument& document,
		const SceneComponent& behavior,
		AgentAttractorTarget& target
	) {
		const SceneEntity* attractorEntity = nullptr;
		const SceneComponent* attractor = nullptr;
		if (behavior.agentAttractorEntityId != 0) {
			attractorEntity = document.FindEntity(behavior.agentAttractorEntityId);
			attractor = attractorEntity
				? FindEnabledComponent(*attractorEntity, "AgentAttractor")
				: nullptr;
		}
		if (!attractorEntity && !behavior.agentAttractorTag.empty()) {
			for (const SceneEntity& entity : document.GetEntities()) {
				if (!IsEntityActiveInHierarchy(document, entity)) {
					continue;
				}
				const SceneComponent* candidate =
					FindEnabledComponent(entity, "AgentAttractor");
				if (
					!candidate ||
					candidate->attractorTag != behavior.agentAttractorTag ||
					!AttractorMatchesAgent(*candidate, behavior)
				) {
					continue;
				}
				attractorEntity = &entity;
				attractor = candidate;
				break;
			}
		}
		if (
			!attractorEntity ||
			!attractor ||
			!IsEntityActiveInHierarchy(document, *attractorEntity)
		) {
			return false;
		}
		const Transform transform =
			ResolveScene3DTransform(document, *attractorEntity);
		target.valid = true;
		target.position = transform.translate;
		target.radius = (std::max)(attractor->attractorRadius, 0.0f);
		target.strength = (std::max)(attractor->attractorStrength, 0.0f);
		return true;
	}

	Vector3 ComputeBoundsSteering(
		const Vector3& position,
		const AgentBounds& bounds
	) {
		if (!bounds.valid) {
			return {};
		}
		Vector3 steer{};
		const Vector3 min = {
			bounds.center.x - bounds.halfSize.x,
			bounds.center.y - bounds.halfSize.y,
			bounds.center.z - bounds.halfSize.z
		};
		const Vector3 max = {
			bounds.center.x + bounds.halfSize.x,
			bounds.center.y + bounds.halfSize.y,
			bounds.center.z + bounds.halfSize.z
		};
		const Vector3 margin = {
			(std::max)(bounds.halfSize.x * 0.18f, 1.0f),
			(std::max)(bounds.halfSize.y * 0.25f, 0.8f),
			(std::max)(bounds.halfSize.z * 0.18f, 1.0f)
		};
		if (position.x < min.x + margin.x) {
			steer.x += (min.x + margin.x - position.x) / margin.x;
		} else if (position.x > max.x - margin.x) {
			steer.x -= (position.x - (max.x - margin.x)) / margin.x;
		}
		if (position.y < min.y + margin.y) {
			steer.y += (min.y + margin.y - position.y) / margin.y;
		} else if (position.y > max.y - margin.y) {
			steer.y -= (position.y - (max.y - margin.y)) / margin.y;
		}
		if (position.z < min.z + margin.z) {
			steer.z += (min.z + margin.z - position.z) / margin.z;
		} else if (position.z > max.z - margin.z) {
			steer.z -= (position.z - (max.z - margin.z)) / margin.z;
		}
		return steer;
	}

	Vector3 ClampToBounds(const Vector3& position, const AgentBounds& bounds) {
		if (!bounds.valid) {
			return position;
		}
		return {
			std::clamp(
				position.x,
				bounds.center.x - bounds.halfSize.x,
				bounds.center.x + bounds.halfSize.x
			),
			std::clamp(
				position.y,
				bounds.center.y - bounds.halfSize.y,
				bounds.center.y + bounds.halfSize.y
			),
			std::clamp(
				position.z,
				bounds.center.z - bounds.halfSize.z,
				bounds.center.z + bounds.halfSize.z
			)
		};
	}

	Vector3 BuildDeterministicPairNormal(
		uint64_t firstId,
		uint64_t secondId
	) {
		const uint64_t lowId = (std::min)(firstId, secondId);
		const uint64_t highId = (std::max)(firstId, secondId);
		const uint64_t pairSeed = lowId ^ (
			highId +
			0x9E3779B97F4A7C15ull +
			(lowId << 6) +
			(lowId >> 2)
		);
		const Vector3 canonical = SafeNormalize(
			{
				Hash01(pairSeed, 719u) * 2.0f - 1.0f,
				(Hash01(pairSeed, 721u) * 2.0f - 1.0f) * 0.45f,
				Hash01(pairSeed, 723u) * 2.0f - 1.0f
			},
			{ 0.0f, 0.0f, 1.0f }
		);
		return firstId == lowId
			? canonical
			: Math::Multiply(canonical, -1.0f);
	}

	float ResolveAgentColliderRadius(const Collider* collider) {
		if (!collider || !collider->IsActive()) {
			return 0.0f;
		}
		if (collider->GetType() == Collider::Type::Sphere) {
			const float radius =
				static_cast<const SphereCollider*>(collider)->GetRadius();
			return std::isfinite(radius) && radius > 0.0f
				? radius
				: 0.0f;
		}
		const Vector3 halfSize =
			static_cast<const OBBCollider*>(collider)->GetOBB().halfSize;
		const float radius = std::sqrt(
			halfSize.x * halfSize.x +
			halfSize.y * halfSize.y +
			halfSize.z * halfSize.z
		);
		return std::isfinite(radius) && radius > 0.0f
			? radius
			: 0.0f;
	}

	float ResolveAgentColliderSupportXZ(const Collider* collider) {
		if (!collider || !collider->IsActive()) {
			return 0.0f;
		}
		if (collider->GetType() == Collider::Type::Sphere) {
			const float radius =
				static_cast<const SphereCollider*>(collider)->GetRadius();
			return std::isfinite(radius) && radius > 0.0f
				? radius
				: 0.0f;
		}
		if (collider->GetType() != Collider::Type::OBB) {
			return 0.0f;
		}
		const OBBCollider::OBB& obb =
			static_cast<const OBBCollider*>(collider)->GetOBB();
		const float supportX =
			std::abs(obb.axis[0].x) * obb.halfSize.x +
			std::abs(obb.axis[1].x) * obb.halfSize.y +
			std::abs(obb.axis[2].x) * obb.halfSize.z;
		const float supportZ =
			std::abs(obb.axis[0].z) * obb.halfSize.x +
			std::abs(obb.axis[1].z) * obb.halfSize.y +
			std::abs(obb.axis[2].z) * obb.halfSize.z;
		const float support = std::sqrt(
			supportX * supportX + supportZ * supportZ
		);
		return std::isfinite(support) && support > 0.0f
			? support
			: 0.0f;
	}

	Vector3 BuildFormationAnchor(
		uint64_t entityId,
		uint64_t teamSeedId,
		float radius,
		float halfSegmentLength
	) {
		if (
			!std::isfinite(radius) || radius <= 0.0f ||
			!std::isfinite(halfSegmentLength) || halfSegmentLength < 0.0f
		) {
			return {};
		}
		const uint64_t seed = entityId ^ teamSeedId;
		const float capsuleHalfLength = halfSegmentLength + radius;
		for (uint32_t attempt = 0; attempt < 64; ++attempt) {
			const uint32_t salt = 1001u + attempt * 2u;
			const float x = (Hash01(seed, salt) * 2.0f - 1.0f) * radius;
			const float z = (Hash01(seed, salt + 1u) * 2.0f - 1.0f) *
				capsuleHalfLength;
			const float centerlineZ = std::clamp(
				z,
				-halfSegmentLength,
				halfSegmentLength
			);
			const float distanceX = x;
			const float distanceZ = z - centerlineZ;
			if (distanceX * distanceX + distanceZ * distanceZ <= radius * radius) {
				return { x, 0.0f, z };
			}
		}
		return { 0.0f, 0.0f, 0.0f };
	}

	Vector3 ProjectToFormationCapsule(
		const Vector3& localPosition,
		float radius,
		float halfSegmentLength
	) {
		if (
			!std::isfinite(radius) ||
			!std::isfinite(halfSegmentLength) || halfSegmentLength < 0.0f
		) {
			return localPosition;
		}
		const float centerlineZ = std::clamp(
			localPosition.z,
			-halfSegmentLength,
			halfSegmentLength
		);
		if (radius <= 0.0f) {
			return { 0.0f, localPosition.y, centerlineZ };
		}
		const float distanceX = localPosition.x;
		const float distanceZ = localPosition.z - centerlineZ;
		const float distance = std::sqrt(
			distanceX * distanceX + distanceZ * distanceZ
		);
		if (distance <= radius || distance <= 0.0001f) {
			return localPosition;
		}
		const float scale = radius / distance;
		return {
			distanceX * scale,
			localPosition.y,
			centerlineZ + distanceZ * scale
		};
	}

	Vector3 ProjectWorldPositionToFormationCapsule(
		const Vector3& worldPosition,
		const Vector3& center,
		float yaw,
		float radius,
		float halfSegmentLength,
		float colliderSupportRadius
	) {
		const float usableRadius = (std::max)(
			radius - (std::max)(colliderSupportRadius, 0.0f),
			0.0f
		);
		const Vector3 worldDelta = {
			worldPosition.x - center.x,
			0.0f,
			worldPosition.z - center.z
		};
		const float cosine = std::cos(yaw);
		const float sine = std::sin(yaw);
		const Vector3 localPosition = {
			worldDelta.x * cosine - worldDelta.z * sine,
			worldPosition.y,
			worldDelta.x * sine + worldDelta.z * cosine
		};
		const Vector3 projected = ProjectToFormationCapsule(
			localPosition,
			usableRadius,
			halfSegmentLength
		);
		return {
			center.x + projected.x * cosine + projected.z * sine,
			projected.y,
			center.z - projected.x * sine + projected.z * cosine
		};
	}

}

void SceneAgentSystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime
) {
	struct AgentUpdateEntry {
		SceneEntity* entity = nullptr;
		SceneComponent behavior{};
		Object3d* object = nullptr;
		PhysicsBody* body = nullptr;
		Collider* collider = nullptr;
		AgentRuntime* runtime = nullptr;
		Transform transform{};
		std::string teamKey;
	};
	struct GroundAgentUpdateEntry {
		SceneEntity* entity = nullptr;
		SceneComponent behavior{};
		PhysicsBody* body = nullptr;
		Collider* collider = nullptr;
		Transform transform{};
		std::string crowdKey;
	};

	const float dt = std::clamp(deltaTime, 0.0f, 0.1f);
	if (dt <= 0.0f) {
		return;
	}

	// 実行対象だけを収集し、削除済みEntityのランタイム状態を後段で破棄する。
	std::vector<AgentUpdateEntry> agents;
	std::vector<GroundAgentUpdateEntry> groundAgents;
	std::unordered_set<uint64_t> requiredIds;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.object) {
			continue;
		}
		SceneEntity& entity = *binding.entity;
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* behavior =
			FindEnabledComponent(entity, "AgentBehavior");
		if (!behavior) {
			continue;
		}
		Object3d* object = binding.object;
		const SceneComponent resolvedBehavior =
			ResolveAgentBehaviorSettings(document, entity, *behavior);
		if (resolvedBehavior.agentMovementMode == "GroundXZ") {
			groundAgents.push_back({
				&entity,
				resolvedBehavior,
				binding.body,
				binding.collider,
				object->GetTransform(),
				ResolveGroundCrowdKey(document, entity, resolvedBehavior)
			});
			continue;
		}

		AgentRuntime& runtime = agentRuntimes_[entity.id];
		if (!runtime.initialized) {
			const Vector3 entityRotate =
				MakeEulerFromQuaternion(entity.transform.rotate);
			runtime.wanderSeedId = BuildFlockRuntimeSeed(
				entity.id,
				resolvedBehavior.agentRandomSeed,
				resolvedBehavior.agentRandomizeSeedOnPlay
			);
			const Vector3 initialDirection = BuildFlockWanderDirection(
				{ std::sin(entityRotate.y), 0.0f, std::cos(entityRotate.y) },
				runtime.wanderSeedId,
				runtime.wanderStep,
				3.14159265359f,
				resolvedBehavior.agentWanderVerticalRange
			);
			++runtime.wanderStep;
			const float speed =
				resolvedBehavior.agentMinSpeed +
				(resolvedBehavior.agentMaxSpeed -
					resolvedBehavior.agentMinSpeed) *
				Hash01(runtime.wanderSeedId, 29u);
			runtime.velocity = Math::Multiply(initialDirection, speed);
			runtime.rotation = entityRotate;
			runtime.wanderDirection = BuildFlockWanderDirection(
				initialDirection,
				runtime.wanderSeedId,
				runtime.wanderStep,
				resolvedBehavior.agentWanderDirectionRange,
				resolvedBehavior.agentWanderVerticalRange
			);
			runtime.wanderTimer =
				resolvedBehavior.agentWanderChangeInterval *
				(0.75f + Hash01(runtime.wanderSeedId, 401u) * 0.5f);
			runtime.phase =
				Hash01(runtime.wanderSeedId, 59u) * 6.28318530718f;
			runtime.initialized = true;
		}

		requiredIds.insert(entity.id);
		agents.push_back({
			&entity,
			resolvedBehavior,
			object,
			binding.body,
			binding.collider,
			&runtime,
			ResolveScene3DTransform(document, entity),
			ResolveAgentTeamRuntimeKey(document, entity)
		});
	}

	for (auto iterator = agentRuntimes_.begin();
		iterator != agentRuntimes_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = agentRuntimes_.erase(iterator);
		} else {
			++iterator;
		}
	}

	// GroundXZはEnemyBehaviorが設定した基準速度へ離隔分だけを加える。
	// Transform・Rotation・Y速度は更新しないため、敵AIとAgentで移動所有が競合しない。
	for (GroundAgentUpdateEntry& agent : groundAgents) {
		if (!agent.body || agent.crowdKey.empty()) {
			continue;
		}
		const float radius = (std::max)(
			agent.behavior.agentSeparationRadius,
			0.0f
		);
		const float weight = (std::max)(
			agent.behavior.agentSeparationWeight,
			0.0f
		);
		if (radius <= 0.0001f || weight <= 0.0f) {
			continue;
		}

		Vector3 separation{};
		int neighborCount = 0;
		for (const GroundAgentUpdateEntry& other : groundAgents) {
			if (
				other.entity == agent.entity ||
				other.crowdKey != agent.crowdKey
			) {
				continue;
			}
			if (
				agent.behavior.agentNeighborLimit > 0 &&
				neighborCount >= agent.behavior.agentNeighborLimit
			) {
				break;
			}
			const Vector3 offset = {
				agent.transform.translate.x - other.transform.translate.x,
				0.0f,
				agent.transform.translate.z - other.transform.translate.z
			};
			const float distance = Math::Length(offset);
			if (distance >= radius) {
				continue;
			}
			++neighborCount;
			Vector3 direction{};
			if (distance > 0.0001f) {
				direction = Math::Multiply(offset, 1.0f / distance);
			} else {
				const float angle = Hash01(
					agent.entity->id ^ other.entity->id,
					743u
				) * 6.28318530718f;
				direction = { std::cos(angle), 0.0f, std::sin(angle) };
			}
			separation = AddScaled(
				separation,
				direction,
				1.0f - distance / radius
			);
		}

		const Vector3 correction = ClampVectorLength(
			Math::Multiply(separation, weight),
			(std::max)(agent.behavior.agentMaxSpeed, 0.001f)
		);
		agent.body->velocity.x += correction.x;
		agent.body->velocity.z += correction.z;
	}

	// GroundXZの接触は、全Dynamic BodyをPhysicsWorldで衝突させず、
	// 通常移動の速度補正だけで解く。後段のHit/Dead停止とKnockback Overrideは優先する。
	struct GroundContactAgent {
		GroundAgentUpdateEntry* agent = nullptr;
		float radius = 0.0f;
		Vector3 predictedPosition{};
		Vector3 velocityCorrection{};
	};
	struct GroundContactNeighbor {
		size_t contactIndex = 0;
		float distance = 0.0f;
	};
	struct GroundContactPair {
		size_t first = 0;
		size_t second = 0;
		float initialDistance = 0.0f;
	};

	constexpr size_t kGroundContactMaxNeighbors = 12;
	constexpr size_t kGroundContactPairBudget = 300;
	constexpr uint32_t kGroundContactSolverPasses = 2;
	constexpr float kGroundContactMaxSpeed = 6.0f;
	constexpr float kGroundContactEpsilon = 0.0001f;

	auto resolveGroundContactRadius = [](const Collider* collider) {
		if (!collider || !collider->IsActive() || collider->IsTrigger()) {
			return 0.0f;
		}
		if (collider->GetType() == Collider::Type::Sphere) {
			return (std::max)(
				static_cast<const SphereCollider*>(collider)->GetRadius(),
				0.0f
			);
		}
		const OBBCollider::OBB obb =
			static_cast<const OBBCollider*>(collider)->GetOBB();
		return std::sqrt(
			obb.halfSize.x * obb.halfSize.x +
			obb.halfSize.z * obb.halfSize.z
		);
	};

	std::vector<GroundContactAgent> contactAgents;
	contactAgents.reserve(groundAgents.size());
	for (GroundAgentUpdateEntry& agent : groundAgents) {
		if (
			!agent.entity ||
			!agent.body ||
			agent.body->type != PhysicsBodyType::Dynamic ||
			agent.crowdKey.empty()
		) {
			continue;
		}
		const float radius = resolveGroundContactRadius(agent.collider);
		if (radius <= kGroundContactEpsilon) {
			continue;
		}
		contactAgents.push_back({ &agent, radius, agent.transform.translate, {} });
	}
	std::sort(
		contactAgents.begin(),
		contactAgents.end(),
		[](const GroundContactAgent& a, const GroundContactAgent& b) {
			return a.agent->entity->id < b.agent->entity->id;
		}
	);

	std::vector<GroundContactPair> contactPairs;
	for (size_t firstIndex = 0; firstIndex < contactAgents.size(); ++firstIndex) {
		const GroundContactAgent& first = contactAgents[firstIndex];
		std::vector<GroundContactNeighbor> neighbors;
		for (size_t secondIndex = 0; secondIndex < contactAgents.size(); ++secondIndex) {
			if (firstIndex == secondIndex) {
				continue;
			}
			const GroundContactAgent& second = contactAgents[secondIndex];
			if (first.agent->crowdKey != second.agent->crowdKey) {
				continue;
			}
			const Vector3 offset = {
				first.predictedPosition.x - second.predictedPosition.x,
				0.0f,
				first.predictedPosition.z - second.predictedPosition.z
			};
			const float distance = Math::Length(offset);
			if (distance >= first.radius + second.radius) {
				continue;
			}
			neighbors.push_back({ secondIndex, distance });
		}
		std::sort(
			neighbors.begin(),
			neighbors.end(),
			[&contactAgents](
				const GroundContactNeighbor& a,
				const GroundContactNeighbor& b
			) {
				if (a.distance != b.distance) {
					return a.distance < b.distance;
				}
				return contactAgents[a.contactIndex].agent->entity->id <
					contactAgents[b.contactIndex].agent->entity->id;
			}
		);
		const size_t neighborCount = (std::min)(
			neighbors.size(),
			kGroundContactMaxNeighbors
		);
		for (size_t neighborIndex = 0; neighborIndex < neighborCount; ++neighborIndex) {
			const size_t secondIndex = neighbors[neighborIndex].contactIndex;
			contactPairs.push_back({
				(std::min)(firstIndex, secondIndex),
				(std::max)(firstIndex, secondIndex),
				0.0f
			});
		}
	}
	std::sort(
		contactPairs.begin(),
		contactPairs.end(),
		[](const GroundContactPair& a, const GroundContactPair& b) {
			return a.first != b.first ? a.first < b.first : a.second < b.second;
		}
	);
	contactPairs.erase(
		std::unique(
			contactPairs.begin(),
			contactPairs.end(),
			[](const GroundContactPair& a, const GroundContactPair& b) {
				return a.first == b.first && a.second == b.second;
			}
		),
		contactPairs.end()
	);
	for (GroundContactPair& pair : contactPairs) {
		const Vector3 offset = {
			contactAgents[pair.first].predictedPosition.x -
				contactAgents[pair.second].predictedPosition.x,
			0.0f,
			contactAgents[pair.first].predictedPosition.z -
				contactAgents[pair.second].predictedPosition.z
		};
		pair.initialDistance = Math::Length(offset);
	}
	std::sort(
		contactPairs.begin(),
		contactPairs.end(),
		[&contactAgents](const GroundContactPair& a, const GroundContactPair& b) {
			if (a.initialDistance != b.initialDistance) {
				return a.initialDistance < b.initialDistance;
			}
			const uint64_t aFirstId = contactAgents[a.first].agent->entity->id;
			const uint64_t bFirstId = contactAgents[b.first].agent->entity->id;
			if (aFirstId != bFirstId) {
				return aFirstId < bFirstId;
			}
			return contactAgents[a.second].agent->entity->id <
				contactAgents[b.second].agent->entity->id;
		}
	);
	if (contactPairs.size() > kGroundContactPairBudget) {
		contactPairs.resize(kGroundContactPairBudget);
	}

	auto mobilityAlongNormal = [](const PhysicsBody& body, const Vector3& normal) {
		const float inverseMass = 1.0f / (std::max)(body.mass, 0.001f);
		const float xMobility = body.freezePositionX ? 0.0f : normal.x * normal.x;
		const float zMobility = body.freezePositionZ ? 0.0f : normal.z * normal.z;
		return (xMobility + zMobility) * inverseMass;
	};
	auto applyPositionConstraints = [](const PhysicsBody& body, Vector3& displacement) {
		if (body.freezePositionX) {
			displacement.x = 0.0f;
		}
		if (body.freezePositionZ) {
			displacement.z = 0.0f;
		}
	};

	for (uint32_t pass = 0; pass < kGroundContactSolverPasses; ++pass) {
		for (const GroundContactPair& pair : contactPairs) {
			GroundContactAgent& first = contactAgents[pair.first];
			GroundContactAgent& second = contactAgents[pair.second];
			const Vector3 offset = {
				first.predictedPosition.x - second.predictedPosition.x,
				0.0f,
				first.predictedPosition.z - second.predictedPosition.z
			};
			const float distance = Math::Length(offset);
			const float penetration = first.radius + second.radius - distance;
			if (penetration <= kGroundContactEpsilon) {
				continue;
			}
			Vector3 normal{};
			if (distance > kGroundContactEpsilon) {
				normal = Math::Multiply(offset, 1.0f / distance);
			} else {
				const float angle = Hash01(
					first.agent->entity->id ^ second.agent->entity->id,
					911u
				) * 6.28318530718f;
				normal = { std::cos(angle), 0.0f, std::sin(angle) };
			}
			const float firstMobility = mobilityAlongNormal(*first.agent->body, normal);
			const float secondMobility = mobilityAlongNormal(*second.agent->body, normal);
			const float totalMobility = firstMobility + secondMobility;
			if (totalMobility <= kGroundContactEpsilon) {
				continue;
			}
			Vector3 firstDisplacement = Math::Multiply(
				normal,
				penetration * (firstMobility / totalMobility)
			);
			Vector3 secondDisplacement = Math::Multiply(
				normal,
				-penetration * (secondMobility / totalMobility)
			);
			applyPositionConstraints(*first.agent->body, firstDisplacement);
			applyPositionConstraints(*second.agent->body, secondDisplacement);
			first.predictedPosition = Math::Add(first.predictedPosition, firstDisplacement);
			second.predictedPosition = Math::Add(second.predictedPosition, secondDisplacement);
			first.velocityCorrection = Math::Add(
				first.velocityCorrection,
				Math::Multiply(firstDisplacement, 1.0f / dt)
			);
			second.velocityCorrection = Math::Add(
				second.velocityCorrection,
				Math::Multiply(secondDisplacement, 1.0f / dt)
			);
		}
	}
	for (GroundContactAgent& contact : contactAgents) {
		const Vector3 correction = ClampVectorLength(
			contact.velocityCorrection,
			kGroundContactMaxSpeed
		);
		contact.agent->body->velocity.x += correction.x;
		contact.agent->body->velocity.z += correction.z;
	}

	struct TeamFrameState {
		Vector3 centerSum{};
		SceneComponent motionBehavior{};
		Vector3 leaderStartPosition{};
		uint64_t seedId = 0;
		uint32_t count = 0;
		uint32_t referenceMemberCount = 0;
		float maxColliderSupport = 0.0f;
		float formationCapsuleRadius = 0.0f;
		float formationCapsuleHalfSegmentLength = 0.0f;
		bool useLeaderStartPosition = false;
		bool formationCapsuleEnabled = false;
		bool formationCapsuleScaleWithActiveMembers = false;
		bool hasMotionBehavior = false;
	};
	struct TeamControllerState {
		Object3d* object = nullptr;
		PhysicsBody* body = nullptr;
		Transform transform{};
	};

	std::unordered_map<std::string, TeamFrameState> teamFrames;
	std::unordered_map<std::string, uint32_t> teamReferenceMemberCounts;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!FindEnabledComponent(entity, "AgentBehavior")) {
			continue;
		}
		const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
		if (!team || team->name.empty()) {
			continue;
		}
		++teamReferenceMemberCounts["team:" + team->name];
	}
	std::unordered_map<std::string, TeamControllerState> teamControllers;
	std::unordered_set<std::string> ambiguousTeamControllers;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!IsEntityActiveInHierarchy(document, *binding.entity) ||
			!FindEnabledComponent(
				*binding.entity,
				"AgentTeamLeaderController"
			)
		) {
			continue;
		}
		const SceneTeamSettings* team =
			document.ResolveEntityTeam(*binding.entity);
		if (!team || team->name.empty()) {
			continue;
		}
		const std::string teamKey = "team:" + team->name;
		if (teamControllers.contains(teamKey)) {
			ambiguousTeamControllers.insert(teamKey);
			continue;
		}
		teamControllers.emplace(
			teamKey,
			TeamControllerState{
				binding.object,
				binding.body,
				ResolveScene3DTransform(document, *binding.entity)
			}
		);
	}
	std::unordered_set<std::string> requiredTeamKeys;
	// 群れの仮想リーダーは個体更新より先に決定し、同じフレームの共通基準にする。
	for (const AgentUpdateEntry& agent : agents) {
		if (agent.teamKey.empty()) {
			continue;
		}

		TeamFrameState& frame = teamFrames[agent.teamKey];
		frame.referenceMemberCount = teamReferenceMemberCounts[agent.teamKey];
		frame.centerSum = Math::Add(
			frame.centerSum,
			agent.transform.translate
		);
		if (!frame.hasMotionBehavior) {
			const SceneComponent* sourceBehavior =
				FindEnabledComponent(*agent.entity, "AgentBehavior");
			frame.motionBehavior = sourceBehavior
				? ResolveTeamLeaderSettings(
					document,
					*agent.entity,
					*sourceBehavior
				)
				: agent.behavior;
			if (const SceneTeamSettings* team =
				document.ResolveEntityTeam(*agent.entity)) {
				frame.useLeaderStartPosition =
					team->agentUseLeaderStartPosition;
				frame.leaderStartPosition = team->agentLeaderStartPosition;
				frame.formationCapsuleEnabled =
					team->agentFormationCapsuleEnabled;
				frame.formationCapsuleScaleWithActiveMembers =
					team->agentFormationCapsuleScaleWithActiveMembers;
				frame.formationCapsuleRadius =
					team->agentFormationCapsuleRadius;
				frame.formationCapsuleHalfSegmentLength =
					team->agentFormationCapsuleHalfSegmentLength;
			}
			frame.seedId = agent.entity->id;
			frame.hasMotionBehavior = true;
		}
		frame.maxColliderSupport = (std::max)(
			frame.maxColliderSupport,
			ResolveAgentColliderSupportXZ(agent.collider)
		);
		++frame.count;
		requiredTeamKeys.insert(agent.teamKey);
	}

	for (auto iterator = teamRuntimes_.begin();
		iterator != teamRuntimes_.end();) {
		if (!requiredTeamKeys.contains(iterator->first)) {
			iterator = teamRuntimes_.erase(iterator);
		} else {
			++iterator;
		}
	}

	for (const auto& [teamKey, frame] : teamFrames) {
		if (frame.count == 0) {
			continue;
		}

		const float invCount = 1.0f / static_cast<float>(frame.count);
		const Vector3 center = Math::Multiply(frame.centerSum, invCount);
		TeamRuntime& runtime = teamRuntimes_[teamKey];
		const bool wasInitialized = runtime.initialized;
		const auto controllerIt = teamControllers.find(teamKey);
		const bool controlled =
			controllerIt != teamControllers.end() &&
			!ambiguousTeamControllers.contains(teamKey);
		if (!runtime.initialized) {
			runtime.center = frame.useLeaderStartPosition
				? frame.leaderStartPosition
				: center;
			runtime.phase = Hash01(frame.seedId, 211u) * 6.28318530718f;
			runtime.seedId = BuildFlockRuntimeSeed(
				frame.seedId,
				frame.motionBehavior.agentRandomSeed,
				frame.motionBehavior.agentRandomizeSeedOnPlay
			);
			const Vector3 configuredHeading = SafeNormalize(
				frame.motionBehavior.agentTeamHeadingDirection,
				{ 0.0f, 0.0f, 1.0f }
			);
			runtime.heading = frame.motionBehavior.agentUseTeamHeading
				? configuredHeading
				: BuildFlockWanderDirection(
					{ 0.0f, 0.0f, 1.0f },
					runtime.seedId,
					runtime.wanderStep,
					3.14159265359f,
					frame.motionBehavior.agentWanderVerticalRange
				);
			++runtime.wanderStep;
			runtime.wanderDirection =
				frame.motionBehavior.agentWanderChangeInterval > 0.0f
					? BuildFlockWanderDirection(
						runtime.heading,
						runtime.seedId,
						runtime.wanderStep,
						frame.motionBehavior.agentWanderDirectionRange,
						frame.motionBehavior.agentWanderVerticalRange
					)
					: runtime.heading;
			runtime.wanderTimer =
				frame.motionBehavior.agentWanderChangeInterval *
				(0.75f + Hash01(runtime.seedId, 317u) * 0.5f);
			const float initialSpeed =
				(frame.motionBehavior.agentMinSpeed +
					frame.motionBehavior.agentMaxSpeed) * 0.5f;
			runtime.velocity = Math::Multiply(runtime.heading, initialSpeed);
			runtime.desiredDirection = runtime.heading;
			runtime.desiredSpeed = initialSpeed;
			runtime.decisionValid = false;
			runtime.initialized = true;
		}
		float formationRadius = frame.formationCapsuleEnabled
			? frame.formationCapsuleRadius
			: 0.0f;
		float formationHalfSegmentLength = frame.formationCapsuleEnabled
			? frame.formationCapsuleHalfSegmentLength
			: 0.0f;
		if (
			frame.formationCapsuleEnabled &&
			frame.formationCapsuleScaleWithActiveMembers &&
			frame.referenceMemberCount > 0 &&
			std::isfinite(frame.formationCapsuleRadius) &&
			frame.formationCapsuleRadius > 0.0f
		) {
			const float countRatio = std::clamp(
				static_cast<float>(frame.count) /
					static_cast<float>(frame.referenceMemberCount),
				0.0f,
				1.0f
			);
			const float countScale = std::sqrt(countRatio);
			const float supportScale = std::clamp(
				(frame.maxColliderSupport + 0.001f) /
					frame.formationCapsuleRadius,
				0.0f,
				1.0f
			);
			const float scale = (std::max)(countScale, supportScale);
			formationRadius = frame.formationCapsuleRadius * scale;
			formationHalfSegmentLength =
				frame.formationCapsuleHalfSegmentLength * scale;
		}
		const bool formationDimensionsChanged =
			runtime.activeMemberCount != frame.count ||
			runtime.referenceMemberCount != frame.referenceMemberCount ||
			std::abs(runtime.formationRadius - formationRadius) > 0.0001f ||
			std::abs(
				runtime.formationHalfSegmentLength - formationHalfSegmentLength
			) > 0.0001f;
		runtime.activeMemberCount = frame.count;
		runtime.referenceMemberCount = frame.referenceMemberCount;
		runtime.formationRadius = formationRadius;
		runtime.formationHalfSegmentLength = formationHalfSegmentLength;
		if (formationDimensionsChanged || runtime.formationRevision == 0) {
			++runtime.formationRevision;
		}
		if (controlled) {
			const TeamControllerState& controller = controllerIt->second;
			const Vector3 previousCenter = runtime.center;
			runtime.center = controller.transform.translate;
			const Vector3 controllerRotation = controller.transform.useQuaternionRotation
				? MakeEulerFromQuaternion(controller.transform.quaternionRotate)
				: controller.transform.rotate;
			runtime.heading = ForwardDirectionFromRotation(
				controllerRotation,
				"+Z",
				runtime.heading
			);
			if (
				wasInitialized &&
				controller.body &&
				Math::Length(controller.body->velocity) > 0.0001f
			) {
				runtime.velocity = controller.body->velocity;
			} else if (wasInitialized && dt > 0.0f) {
				runtime.velocity = Math::Multiply(
					Math::Subtract(runtime.center, previousCenter),
					1.0f / dt
				);
			} else {
				runtime.velocity = {};
			}
			runtime.desiredDirection = runtime.heading;
			runtime.desiredSpeed = Math::Length(runtime.velocity);
			runtime.rotation = controllerRotation;
			runtime.forwardAxis = frame.motionBehavior.agentForwardAxis;
			runtime.decisionValid = true;
			continue;
		}

		const SceneComponent& behavior = frame.motionBehavior;
		const Vector3 velocityDirection = SafeNormalize(
			runtime.velocity,
			runtime.heading
		);
		AgentBounds bounds{};
		TryResolveAgentBounds(document, behavior, runtime.center, bounds);
		runtime.decisionTimer -= dt;
		if (!runtime.decisionValid || runtime.decisionTimer <= 0.0f) {
			Vector3 desired = AddScaled({}, velocityDirection, 0.65f);
			if (behavior.agentWanderChangeInterval > 0.0f) {
				runtime.wanderTimer -= behavior.agentFlockDecisionInterval > 0.0f
					? behavior.agentFlockDecisionInterval
					: dt;
				if (runtime.wanderTimer <= 0.0f) {
					++runtime.wanderStep;
					runtime.wanderDirection = BuildFlockWanderDirection(
						runtime.heading,
						runtime.seedId,
						runtime.wanderStep,
						behavior.agentWanderDirectionRange,
						behavior.agentWanderVerticalRange
					);
					runtime.wanderTimer = behavior.agentWanderChangeInterval *
						(0.75f + Hash01(
							runtime.seedId,
							331u + runtime.wanderStep * 3u
						) * 0.5f);
				}
			}
			desired = AddScaled(
				desired,
				runtime.wanderDirection,
				behavior.agentWanderStrength
			);
			if (behavior.agentUseTeamHeading) {
				desired = AddScaled(
					desired,
					SafeNormalize(
						behavior.agentTeamHeadingDirection,
						velocityDirection
					),
					behavior.agentTeamHeadingWeight
				);
			}
			if (bounds.valid) {
				desired = AddScaled(
					desired,
					ComputeBoundsSteering(runtime.center, bounds),
					behavior.agentBoundsWeight
				);
			}
			AgentAttractorTarget attractor{};
			float speedScale = 1.0f;
			if (
				behavior.agentAttractorWeight > 0.0f &&
				TryResolveAgentAttractor(document, behavior, attractor)
			) {
				const Vector3 toAttractor = Math::Subtract(
					attractor.position,
					runtime.center
				);
				const float distance = Math::Length(toAttractor);
				const float radius = (std::max)(attractor.radius, 0.001f);
				desired = AddScaled(
					desired,
					SafeNormalize(toAttractor, velocityDirection),
					behavior.agentAttractorWeight * attractor.strength *
						std::clamp(distance / radius, 0.0f, 2.0f)
				);
				speedScale = distance < radius ? 0.7f : 1.0f;
			}
			runtime.desiredDirection = SafeNormalize(desired, velocityDirection);
			runtime.desiredSpeed =
				(behavior.agentMinSpeed + behavior.agentMaxSpeed) * 0.5f * speedScale;
			runtime.decisionTimer = behavior.agentFlockDecisionInterval;
			runtime.decisionValid = true;
		}
		runtime.heading = RotateDirectionToward(
			runtime.heading,
			runtime.desiredDirection,
			behavior.agentFlockTurnRate * dt,
			velocityDirection
		);
		runtime.velocity = MoveVectorToward(
			runtime.velocity,
			Math::Multiply(runtime.heading, runtime.desiredSpeed),
			behavior.agentFlockAcceleration * dt
		);
		runtime.center = Math::Add(
			runtime.center,
			Math::Multiply(runtime.velocity, dt)
		);
		if (bounds.valid) {
			runtime.center = ClampToBounds(runtime.center, bounds);
		}
		runtime.forwardAxis = behavior.agentForwardAxis;
		runtime.rotation = BuildAgentVelocityRotation(
			behavior,
			runtime.rotation,
			runtime.velocity,
			runtime.desiredDirection,
			dt
		);
	}

	// 個体は毎フレーム追従と逸脱補正を行い、Jitterは更新間隔ごとに目標だけを変更する。
	for (AgentUpdateEntry& agent : agents) {
		const SceneComponent& behavior = agent.behavior;
		AgentRuntime& runtime = *agent.runtime;
		Transform transform = agent.transform;
		Vector3 position = transform.translate;
		const auto teamRuntimeIt =
			agent.teamKey.empty()
				? teamRuntimes_.end()
				: teamRuntimes_.find(agent.teamKey);
		const TeamRuntime* teamRuntime =
			teamRuntimeIt == teamRuntimes_.end()
				? nullptr
				: &teamRuntimeIt->second;
		const SceneTeamSettings* teamSettings =
			agent.teamKey.empty()
				? nullptr
				: document.ResolveEntityTeam(*agent.entity);
		const bool formationEnabled =
			teamSettings && teamSettings->agentFormationCapsuleEnabled;
		if (teamRuntime) {
			bool initializedFlockThisFrame = false;
			if (
				!runtime.flockInitialized ||
				runtime.flockSeedId != teamRuntime->seedId
			) {
				runtime.velocity = teamRuntime->velocity;
				runtime.phase = Hash01(
					agent.entity->id ^ teamRuntime->seedId,
					59u
				) * 6.28318530718f;
				runtime.flockSeedId = teamRuntime->seedId;
				runtime.flockInitialized = true;
				initializedFlockThisFrame = true;
			}
			if (!formationEnabled) {
				runtime.formationAnchorInitialized = false;
			} else if (
				!runtime.formationAnchorInitialized ||
				runtime.formationSeedId != teamRuntime->seedId ||
				runtime.formationRevision != teamRuntime->formationRevision
			) {
				runtime.formationAnchorLocal = BuildFormationAnchor(
					agent.entity->id,
					teamRuntime->seedId,
					teamRuntime->formationRadius,
					teamRuntime->formationHalfSegmentLength
				);
				runtime.formationSeedId = teamRuntime->seedId;
				runtime.formationRevision = teamRuntime->formationRevision;
				runtime.formationAnchorInitialized = true;
			}
			const Vector3 teamDirection = SafeNormalize(
				teamRuntime->velocity,
				teamRuntime->heading
			);
			const float teamHeadingYaw = std::atan2(
				teamRuntime->heading.x,
				teamRuntime->heading.z
			);
			const float jitterUpdateInterval = (std::max)(
				behavior.agentMemberJitterUpdateInterval,
				0.0f
			);
			if (initializedFlockThisFrame) {
				runtime.jitterStep = 0;
				runtime.jitterTargetLocal = BuildFlockMemberJitterTarget(
					agent.entity->id,
					teamRuntime->seedId,
					runtime.jitterStep,
					behavior.agentMemberJitterStrength
				);
				runtime.jitterTimer = jitterUpdateInterval * (
					0.2f + Hash01(agent.entity->id ^ teamRuntime->seedId, 617u) * 0.8f
				);
			} else {
				runtime.jitterTimer = (std::max)(
					runtime.jitterTimer - dt,
					0.0f
				);
				if (jitterUpdateInterval <= 0.0f || runtime.jitterTimer <= 0.0f) {
					++runtime.jitterStep;
					runtime.jitterTargetLocal = BuildFlockMemberJitterTarget(
						agent.entity->id,
						teamRuntime->seedId,
						runtime.jitterStep,
						behavior.agentMemberJitterStrength
					);
					runtime.jitterTimer = jitterUpdateInterval * (
						0.75f + Hash01(
							agent.entity->id ^ teamRuntime->seedId,
							631u + runtime.jitterStep
						) * 0.5f
					);
				}
			}
			runtime.phase += dt * behavior.agentMemberJitterFrequency;
			const Vector3 jitterDetail = {
				std::sin(runtime.phase * 1.37f + Hash01(agent.entity->id, 3u) * 8.0f) *
					behavior.agentMemberJitterStrength * 0.18f,
				std::sin(runtime.phase * 0.83f + Hash01(agent.entity->id, 5u) * 9.0f) *
					behavior.agentMemberJitterStrength * 0.08f,
				std::cos(runtime.phase * 1.11f + Hash01(agent.entity->id, 7u) * 7.0f) *
					behavior.agentMemberJitterStrength * 0.18f
			};
			Vector3 localJitterTarget = Math::Add(
				runtime.jitterTargetLocal,
				jitterDetail
			);
			if (formationEnabled) {
				localJitterTarget = ProjectToFormationCapsule(
					Math::Add(runtime.formationAnchorLocal, localJitterTarget),
					teamRuntime->formationRadius,
					teamRuntime->formationHalfSegmentLength
				);
			}
			const Vector3 jitterTarget = RotateDirection(
				localJitterTarget,
				{ 0.0f, teamHeadingYaw, 0.0f }
			);
			if (initializedFlockThisFrame) {
				runtime.jitterOffset = jitterTarget;
			} else {
				runtime.jitterOffset = LerpVector(
					runtime.jitterOffset,
					jitterTarget,
					FollowAmount(behavior.agentMemberJitterFollowSpeed, dt)
				);
			}
			const float memberMinimumDistance =
				std::isfinite(behavior.agentMemberMinimumDistance) &&
				behavior.agentMemberMinimumDistance > 0.0f
					? behavior.agentMemberMinimumDistance
					: 0.0f;
			if (memberMinimumDistance > 0.0001f) {
				runtime.separationTimer = (std::max)(
					runtime.separationTimer - dt,
					0.0f
				);
				const float separationUpdateInterval = (std::max)(
					behavior.agentMemberSeparationUpdateInterval,
					0.0f
				);
				if (
					separationUpdateInterval <= 0.0f ||
					!runtime.separationCacheValid ||
					runtime.separationTimer <= 0.0f
				) {
					const float separationRadius = (std::max)(
						behavior.agentSeparationRadius,
						memberMinimumDistance
					);
					Vector3 separation{};
					int neighborCount = 0;
					for (const AgentUpdateEntry& other : agents) {
						if (
							other.entity == agent.entity ||
							other.teamKey != agent.teamKey
						) {
							continue;
						}
						if (
							behavior.agentNeighborLimit > 0 &&
							neighborCount >= behavior.agentNeighborLimit
						) {
							break;
						}
						++neighborCount;

						const Vector3 offset = Math::Subtract(
							position,
							other.transform.translate
						);
						const float distance = Math::Length(offset);
						if (distance >= separationRadius) {
							continue;
						}
						const Vector3 direction = distance > 0.0001f
							? Math::Multiply(offset, 1.0f / distance)
							: BuildDeterministicPairNormal(
								agent.entity->id,
								other.entity->id
							);
						const float ratio = 1.0f -
							distance / (std::max)(separationRadius, 0.0001f);
						separation = AddScaled(
							separation,
							direction,
							ratio
						);
					}

					const Vector3 separationSteering =
						Math::Length(separation) > 0.0001f
							? SafeNormalize(separation, {})
							: Vector3{};
					const float separationBlend = std::clamp(
						behavior.agentMemberSeparationBlend,
						0.0f,
						1.0f
					);
					runtime.cachedSeparationSteering =
						runtime.separationCacheValid
							? LerpVector(
								runtime.cachedSeparationSteering,
								separationSteering,
								separationBlend
							)
							: separationSteering;
					runtime.separationCacheValid = true;
					runtime.separationTimer = separationUpdateInterval;
				}
			} else {
				runtime.separationCacheValid = false;
				runtime.cachedSeparationSteering = {};
				runtime.separationTimer = 0.0f;
			}
			const Vector3 memberTarget = Math::Add(
				teamRuntime->center,
				runtime.jitterOffset
			);
			const Vector3 toTarget = Math::Subtract(memberTarget, position);
			const float maxDistance = (std::max)(
				behavior.agentMemberLeashDistance,
				0.01f
			);
			const float correctionLimit = (std::max)(
				behavior.agentMemberCatchupSpeed *
					(1.0f + behavior.agentMemberLeashStrength),
				0.01f
			);
			Vector3 correctionVelocity = ClampVectorLength(
				Math::Multiply(toTarget, behavior.agentMemberCenterFollow),
				correctionLimit
			);
			correctionVelocity = ClampVectorLength(
				AddScaled(
					correctionVelocity,
					runtime.cachedSeparationSteering,
					(std::max)(behavior.agentSeparationWeight, 0.0f)
				),
				correctionLimit
			);
			runtime.velocity = Math::Add(
				teamRuntime->velocity,
				correctionVelocity
			);
			position = Math::Add(position, Math::Multiply(runtime.velocity, dt));
			const Vector3 leaderOffset = Math::Subtract(
				position,
				teamRuntime->center
			);
			const float leaderDistance = Math::Length(leaderOffset);
			if (leaderDistance > maxDistance) {
				position = Math::Add(
					teamRuntime->center,
					Math::Multiply(
						Math::Normalize(leaderOffset),
						maxDistance
					)
				);
			}
			runtime.velocity = Math::Multiply(
				Math::Subtract(position, transform.translate),
				1.0f / dt
			);
			const Vector3 desiredDirection = SafeNormalize(
				runtime.velocity,
				teamDirection
			);
			AgentBounds bounds{};
			if (TryResolveAgentBounds(document, behavior, position, bounds) && bounds.valid) {
				position = ClampToBounds(position, bounds);
			}
			transform.translate = position;
			runtime.rotation = BuildAgentVelocityRotation(
				behavior,
				runtime.rotation,
				runtime.velocity,
				desiredDirection,
				dt
			);
			ApplyAgentRotation(transform, runtime.rotation);
			SynchronizeSceneTransform(
				document,
				*agent.entity,
				*agent.object,
				transform
			);
			agent.transform = transform;
			continue;
		}
		runtime.flockInitialized = false;
		runtime.flockSeedId = 0;
		runtime.jitterOffset = {};
		runtime.jitterTargetLocal = {};
		runtime.jitterTimer = 0.0f;
		runtime.jitterStep = 0;
		runtime.separationCacheValid = false;
		runtime.cachedSeparationSteering = {};
		runtime.separationTimer = 0.0f;

		const Vector3 velocityDirection = SafeNormalize(
			runtime.velocity,
			{ 0.0f, 0.0f, 1.0f }
		);
		Vector3 desired = AddScaled({}, velocityDirection, 0.65f);

		runtime.phase += dt * (
			0.7f +
			Hash01(runtime.wanderSeedId, 71u) * 0.8f
		);
		if (behavior.agentWanderStrength > 0.0f) {
			const float wanderChangeInterval = (std::max)(
				behavior.agentWanderChangeInterval,
				0.0f
			);
			if (wanderChangeInterval > 0.0f) {
				runtime.wanderTimer = (std::max)(
					runtime.wanderTimer - dt,
					0.0f
				);
				if (runtime.wanderTimer <= 0.0f) {
					++runtime.wanderStep;
					runtime.wanderDirection = BuildFlockWanderDirection(
						velocityDirection,
						runtime.wanderSeedId,
						runtime.wanderStep,
						behavior.agentWanderDirectionRange,
						behavior.agentWanderVerticalRange
					);
					runtime.wanderTimer = wanderChangeInterval * (
						0.75f + Hash01(
							runtime.wanderSeedId,
							419u + runtime.wanderStep * 3u
						) * 0.5f
					);
				}
			}
			desired = AddScaled(
				desired,
				runtime.wanderDirection,
				behavior.agentWanderStrength
			);
		}

		AgentBounds bounds{};
		if (TryResolveAgentBounds(document, behavior, position, bounds)) {
			desired = AddScaled(
				desired,
				ComputeBoundsSteering(position, bounds),
				behavior.agentBoundsWeight
			);
		}

		AgentAttractorTarget attractor{};
		const bool hasAttractor =
			behavior.agentAttractorWeight > 0.0f &&
			TryResolveAgentAttractor(document, behavior, attractor);
		float attractorSpeedScale = 1.0f;
		if (hasAttractor) {
			const Vector3 toAttractor =
				Math::Subtract(attractor.position, position);
			const float distance = Math::Length(toAttractor);
			const Vector3 toAttractorDirection =
				SafeNormalize(toAttractor, velocityDirection);
			const float radius = (std::max)(attractor.radius, 0.001f);
			const float strength =
				behavior.agentAttractorWeight * attractor.strength;
			if (distance > radius * 0.45f) {
				const float ratio = std::clamp(distance / radius, 0.0f, 2.0f);
				desired = AddScaled(
					desired,
					toAttractorDirection,
					strength * ratio
				);
			} else {
				const Vector3 tangent = SafeNormalize(
					{
						-toAttractorDirection.z,
						std::sin(runtime.phase) * 0.18f,
						toAttractorDirection.x
					},
					velocityDirection
				);
				desired = AddScaled(desired, tangent, strength);
				desired = AddScaled(
					desired,
					toAttractorDirection,
					strength * 0.25f
				);
			}
			if (distance < radius) {
				attractorSpeedScale = 0.7f;
			}
		}

		runtime.schoolingTimer =
			(std::max)(runtime.schoolingTimer - dt, 0.0f);
		if (behavior.agentSchooling) {
			const bool shouldUpdateSchooling =
				behavior.agentSchoolingUpdateInterval <= 0.0f ||
				!runtime.schoolingCacheValid ||
				runtime.schoolingTimer <= 0.0f;
			if (shouldUpdateSchooling) {
				Vector3 separation{};
				Vector3 alignment{};
				Vector3 cohesion{};
				uint32_t alignmentCount = 0;
				uint32_t cohesionCount = 0;
				int neighborCount = 0;

				for (const AgentUpdateEntry& other : agents) {
					if (
						other.entity == agent.entity ||
						other.behavior.agentGroupName != behavior.agentGroupName ||
						other.behavior.agentBehaviorName != behavior.agentBehaviorName
					) {
						continue;
					}
					if (
						behavior.agentNeighborLimit > 0 &&
						neighborCount >= behavior.agentNeighborLimit
					) {
						break;
					}
					++neighborCount;

					const Vector3 offset =
						Math::Subtract(position, other.transform.translate);
					const float distance = Math::Length(offset);
					if (distance <= 0.0001f) {
						continue;
					}
					if (distance < behavior.agentSeparationRadius) {
						const float ratio = 1.0f -
							distance / (std::max)(
								behavior.agentSeparationRadius,
								0.0001f
							);
						separation = AddScaled(
							separation,
							Math::Normalize(offset),
							ratio
						);
					}
					if (distance < behavior.agentAlignmentRadius) {
						alignment = Math::Add(
							alignment,
							other.runtime->velocity
						);
						++alignmentCount;
					}
					if (distance < behavior.agentCohesionRadius) {
						cohesion = Math::Add(
							cohesion,
							other.transform.translate
						);
						++cohesionCount;
					}
				}

				Vector3 schoolingSteering{};
				if (Math::Length(separation) > 0.0001f) {
					schoolingSteering = AddScaled(
						schoolingSteering,
						Math::Normalize(separation),
						behavior.agentSeparationWeight
					);
				}
				if (alignmentCount > 0) {
					alignment = Math::Multiply(
						alignment,
						1.0f / static_cast<float>(alignmentCount)
					);
					schoolingSteering = AddScaled(
						schoolingSteering,
						SafeNormalize(alignment, velocityDirection),
						behavior.agentAlignmentWeight
					);
				}
				if (cohesionCount > 0) {
					cohesion = Math::Multiply(
						cohesion,
						1.0f / static_cast<float>(cohesionCount)
					);
					schoolingSteering = AddScaled(
						schoolingSteering,
						SafeNormalize(
							Math::Subtract(cohesion, position),
							velocityDirection
						),
						behavior.agentCohesionWeight
					);
				}

				const float schoolingBlend = std::clamp(
					behavior.agentSchoolingBlend,
					0.0f,
					1.0f
				);
				runtime.cachedSchoolingSteering =
					runtime.schoolingCacheValid
						? LerpVector(
							runtime.cachedSchoolingSteering,
							schoolingSteering,
							schoolingBlend
						)
						: schoolingSteering;
				runtime.schoolingCacheValid = true;
				if (behavior.agentSchoolingUpdateInterval > 0.0f) {
					const float jitter =
						(Hash01(agent.entity->id, 193u) * 2.0f - 1.0f) *
						behavior.agentSchoolingUpdateJitter;
					runtime.schoolingTimer = (std::max)(
						behavior.agentSchoolingUpdateInterval *
							(1.0f + jitter),
						0.0f
					);
				} else {
					runtime.schoolingTimer = 0.0f;
				}
			}
			desired = Math::Add(desired, runtime.cachedSchoolingSteering);
		} else {
			runtime.schoolingCacheValid = false;
			runtime.cachedSchoolingSteering = {};
			runtime.schoolingTimer = 0.0f;
		}

		if (
			teamRuntime &&
			behavior.agentUseTeamHeading &&
			behavior.agentTeamHeadingWeight > 0.0f
		) {
			desired = AddScaled(
				desired,
				teamRuntime->heading,
				behavior.agentTeamHeadingWeight
			);
		}

		const Vector3 desiredDirection =
			SafeNormalize(desired, velocityDirection);
		const float speed =
			(
				behavior.agentMinSpeed +
				(behavior.agentMaxSpeed - behavior.agentMinSpeed) *
				Hash01(agent.entity->id, 97u)
			) *
			attractorSpeedScale;
		const Vector3 targetVelocity =
			Math::Multiply(desiredDirection, speed);
		const float turnLerp =
			std::clamp(behavior.agentTurnSpeed * dt, 0.0f, 1.0f);
		runtime.velocity = LerpVector(
			runtime.velocity,
			targetVelocity,
			turnLerp
		);

		const Vector3 movement = Math::Multiply(runtime.velocity, dt);
		const bool isFishingCatchEffect =
			behavior.agentGroupName.rfind("FishingCatchEffect", 0) == 0;
		if (hasAttractor && isFishingCatchEffect) {
			const Vector3 toAttractor = Math::Subtract(
				attractor.position, position
			);
			const float distanceToAttractor = Math::Length(toAttractor);
			const float movementDistance = Math::Length(movement);
			if (
				distanceToAttractor <= movementDistance &&
				Math::Dot(movement, toAttractor) > 0.0f
			) {
				// 演出魚は高速でも個別追従先を通り越さず、隊形を保つ。
				position = attractor.position;
				runtime.velocity = {};
			} else {
				position = Math::Add(position, movement);
			}
		} else {
			position = Math::Add(position, movement);
		}
		if (bounds.valid) {
			position = ClampToBounds(position, bounds);
		}
		transform.translate = position;

		Vector3 rotationDirection = SafeNormalize(
			runtime.velocity,
			desiredDirection
		);
		if (
			teamRuntime &&
			behavior.agentUseTeamRotation &&
			behavior.agentTeamRotationWeight > 0.0f
		) {
			const Vector3 teamRotationDirection =
				ForwardDirectionFromRotation(
					teamRuntime->rotation,
					teamRuntime->forwardAxis,
					teamRuntime->heading
				);
			rotationDirection = BlendDirections(
				rotationDirection,
				teamRotationDirection,
				behavior.agentTeamRotationWeight,
				rotationDirection
			);
		}
		runtime.rotation = BuildAgentVelocityRotation(
			behavior,
			runtime.rotation,
			rotationDirection,
			desiredDirection,
			dt
		);
		ApplyAgentRotation(transform, runtime.rotation);

		SynchronizeSceneTransform(
			document,
			*agent.entity,
			*agent.object,
			transform
		);
		agent.transform = transform;
	}

	// Teamメンバーの最低距離と固定カプセルは、通常更新後に4 passだけ補正する。
	// Leashはこの後に再適用せず、カプセルとWater boundsだけをhard constraintとして再適用する。
	std::vector<AgentUpdateEntry*> separationAgents;
	separationAgents.reserve(agents.size());
	for (AgentUpdateEntry& agent : agents) {
		const SceneTeamSettings* team = agent.teamKey.empty()
			? nullptr
			: document.ResolveEntityTeam(*agent.entity);
		const bool formationEnabled =
			team && team->agentFormationCapsuleEnabled;
		if (
			agent.teamKey.empty() ||
			(
				!formationEnabled &&
				(
					!std::isfinite(agent.behavior.agentMemberMinimumDistance) ||
					agent.behavior.agentMemberMinimumDistance <= 0.0f
				)
			)
		) {
			continue;
		}
		separationAgents.push_back(&agent);
	}
	std::sort(
		separationAgents.begin(),
		separationAgents.end(),
		[](const AgentUpdateEntry* left, const AgentUpdateEntry* right) {
			return left->entity->id < right->entity->id;
		}
	);
	constexpr uint32_t kTeamSeparationSolverPasses = 4;
	for (uint32_t pass = 0; pass < kTeamSeparationSolverPasses; ++pass) {
		for (size_t firstIndex = 0; firstIndex < separationAgents.size(); ++firstIndex) {
			AgentUpdateEntry& first = *separationAgents[firstIndex];
			for (
				size_t secondIndex = firstIndex + 1;
				secondIndex < separationAgents.size();
				++secondIndex
			) {
				AgentUpdateEntry& second = *separationAgents[secondIndex];
				if (first.teamKey != second.teamKey) {
					continue;
				}

				const float configuredMinimum = (std::max)(
					first.behavior.agentMemberMinimumDistance,
					second.behavior.agentMemberMinimumDistance
				);
				const float colliderMinimum =
					ResolveAgentColliderRadius(first.collider) +
					ResolveAgentColliderRadius(second.collider);
				const float effectiveMinimum = (std::max)(
					configuredMinimum,
					colliderMinimum
				);
				if (effectiveMinimum <= 0.0001f) {
					continue;
				}

				const Vector3 offset = Math::Subtract(
					first.transform.translate,
					second.transform.translate
				);
				const float distance = Math::Length(offset);
				if (distance >= effectiveMinimum) {
					continue;
				}
				const Vector3 normal = distance > 0.0001f
					? Math::Multiply(offset, 1.0f / distance)
					: BuildDeterministicPairNormal(
						first.entity->id,
						second.entity->id
					);
				const Vector3 displacement = Math::Multiply(
					normal,
					(effectiveMinimum - distance) * 0.5f
				);
				first.transform.translate = Math::Add(
					first.transform.translate,
					displacement
				);
				second.transform.translate = Math::Subtract(
					second.transform.translate,
					displacement
				);
			}
		}

		for (AgentUpdateEntry* agent : separationAgents) {
			const SceneTeamSettings* team = agent->teamKey.empty()
				? nullptr
				: document.ResolveEntityTeam(*agent->entity);
			if (!team || !team->agentFormationCapsuleEnabled) {
				continue;
			}
			const auto teamRuntimeIt = teamRuntimes_.find(agent->teamKey);
			if (teamRuntimeIt == teamRuntimes_.end()) {
				continue;
			}
			const TeamRuntime& teamRuntime = teamRuntimeIt->second;
			const float teamHeadingYaw = std::atan2(
				teamRuntime.heading.x,
				teamRuntime.heading.z
			);
			agent->transform.translate = ProjectWorldPositionToFormationCapsule(
			agent->transform.translate,
				teamRuntime.center,
				teamHeadingYaw,
				teamRuntime.formationRadius,
				teamRuntime.formationHalfSegmentLength,
				ResolveAgentColliderRadius(agent->collider)
			);
		}

		for (AgentUpdateEntry* agent : separationAgents) {
			AgentBounds bounds{};
			if (
				TryResolveAgentBounds(
					document,
					agent->behavior,
					agent->transform.translate,
					bounds
				) &&
				bounds.valid
			) {
				agent->transform.translate = ClampToBounds(
					agent->transform.translate,
					bounds
				);
			}
		}
	}
	for (AgentUpdateEntry* agent : separationAgents) {
		SynchronizeSceneTransform(
			document,
			*agent->entity,
			*agent->object,
			agent->transform
		);
	}
}

bool SceneAgentSystem::TryGetTeamFormationCapsuleState(
	const std::string& teamName,
	SceneAgentFormationCapsuleState& state
) const {
	state = {};
	if (teamName.empty()) {
		return false;
	}
	const auto iterator = teamRuntimes_.find("team:" + teamName);
	if (iterator == teamRuntimes_.end()) {
		return false;
	}
	const TeamRuntime& runtime = iterator->second;
	if (
		runtime.activeMemberCount == 0 ||
		!std::isfinite(runtime.formationRadius) ||
		runtime.formationRadius <= 0.0f ||
		!std::isfinite(runtime.formationHalfSegmentLength) ||
		runtime.formationHalfSegmentLength < 0.0f
	) {
		return false;
	}
	state.radius = runtime.formationRadius;
	state.halfSegmentLength = runtime.formationHalfSegmentLength;
	state.activeMemberCount = runtime.activeMemberCount;
	state.referenceMemberCount = runtime.referenceMemberCount;
	return true;
}

void SceneAgentSystem::ResetTeam(
	SceneDocument& document,
	const std::string& teamName
) {
	if (teamName.empty()) {
		return;
	}
	const std::string teamKey = "team:" + teamName;
	teamRuntimes_.erase(teamKey);
	for (auto iterator = agentRuntimes_.begin();
		iterator != agentRuntimes_.end();) {
		const SceneEntity* entity = document.FindEntity(iterator->first);
		const SceneTeamSettings* team = entity
			? document.ResolveEntityTeam(*entity)
			: nullptr;
		if (team && team->name == teamName) {
			iterator = agentRuntimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneAgentSystem::ResetAgent(uint64_t entityId) {
	if (entityId != 0) {
		agentRuntimes_.erase(entityId);
	}
}

void SceneAgentSystem::Clear() {
	agentRuntimes_.clear();
	teamRuntimes_.clear();
}
