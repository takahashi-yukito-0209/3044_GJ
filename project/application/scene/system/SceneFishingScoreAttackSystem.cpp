// 役割: Fishing Score AttackのComponent設定をRuntime stateへ展開する。
#include "SceneFishingScoreAttackSystem.h"

#include "SceneAgentSystem.h"
#include "FishingFormationMotion.h"
#include "../SceneRuntimeInput.h"

#include "../../../engine/collision/Collider.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/debug/DebugRenderer.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/3d/ModelManager.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneInputKey.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	constexpr float kTransformEpsilon = 0.0001f;
	constexpr float kTwoPi = 6.28318530717958647692f;

	float ExtractPlanarYaw(const Transform& transform) {
		if (transform.useQuaternionRotation) {
			const Matrix4x4 rotationMatrix = MakeRotateMatrix(
				transform.quaternionRotate
			);
			const float yaw = std::atan2(
				rotationMatrix.m[2][0],
				rotationMatrix.m[2][2]
			);
			if (std::isfinite(yaw)) {
				return yaw;
			}
		}
		return std::isfinite(transform.rotate.y) ? transform.rotate.y : 0.0f;
	}

	const SceneComponent* FindDirector(
		const SceneDocument& document,
		uint64_t& entityId,
		bool& duplicate
	) {
		const SceneComponent* found = nullptr;
		entityId = 0;
		duplicate = false;
		for (const SceneEntity& entity : document.GetEntities()) {
			const SceneComponent* component =
				FindEnabledComponent(entity, "FishingScoreAttackDirector");
			if (!component) {
				continue;
			}
			if (found) {
				duplicate = true;
				return nullptr;
			}
			found = component;
			entityId = entity.id;
		}
		return found;
	}

	const SceneComponent* FindComponent(
		const SceneDocument& document,
		uint64_t entityId,
		const char* type
	) {
		const SceneEntity* entity = document.FindEntity(entityId);
		return entity ? FindEnabledComponent(*entity, type) : nullptr;
	}

	SceneFishingHookRankDefinition ResolveFishingHookRank(
		const SceneComponent& director,
		int tier
	) {
		const size_t index = tier > 0
			? static_cast<size_t>(tier - 1)
			: static_cast<size_t>(10);
		if (index < director.fishingHookRanks.size()) {
			return director.fishingHookRanks[index];
		}
		SceneFishingHookRankDefinition rank{};
		if (index < director.fishingHookTierScoreMultipliers.size()) {
			rank.scoreMultiplier =
				director.fishingHookTierScoreMultipliers[index];
		}
		if (index < director.fishingHookMultiplierColors.size()) {
			rank.color = director.fishingHookMultiplierColors[index];
		}
		return rank;
	}

	bool IsFiniteNonNegative(float value) {
		return std::isfinite(value) && value >= 0.0f;
	}

	bool IsResourceRelativeModelPath(const std::string& path) {
		if (path.empty()) {
			return true;
		}
		return path.find(':') == std::string::npos &&
			path.front() != '/' && path.front() != '\\' &&
			path.find("..") == std::string::npos;
	}

	bool IsIdentityRotationScale(const Transform& transform) {
		return
			std::abs(transform.rotate.x) <= kTransformEpsilon &&
			std::abs(transform.rotate.y) <= kTransformEpsilon &&
			std::abs(transform.rotate.z) <= kTransformEpsilon &&
			std::abs(transform.scale.x - 1.0f) <= kTransformEpsilon &&
			std::abs(transform.scale.y - 1.0f) <= kTransformEpsilon &&
			std::abs(transform.scale.z - 1.0f) <= kTransformEpsilon;
	}

	bool HasTranslationOnlyAncestors(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		std::unordered_set<uint64_t> visited;
		uint64_t parentId = entity.parentId;
		while (parentId != 0 && visited.insert(parentId).second) {
			const SceneEntity* parent = document.FindEntity(parentId);
			if (!parent) {
				return false;
			}
			if (!IsIdentityRotationScale(
				SceneTransformResolver::ResolveScene3DTransform(document, *parent)
			)) {
				return false;
			}
			parentId = parent->parentId;
		}
		return parentId == 0;
	}

	float DistanceXZ(const Vector3& left, const Vector3& right) {
		const float deltaX = left.x - right.x;
		const float deltaZ = left.z - right.z;
		return std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
	}

	Vector3 ToSpawnWorldPosition(
		const Transform& areaTransform,
		float localX,
		float localZ,
		float y
	) {
		const float cosine = std::cos(areaTransform.rotate.y);
		const float sine = std::sin(areaTransform.rotate.y);
		return {
			areaTransform.translate.x + localX * cosine + localZ * sine,
			y,
			areaTransform.translate.z - localX * sine + localZ * cosine
		};
	}

	Vector3 ToLocalXZ(const Transform& transform, const Vector3& worldPosition) {
		const float cosine = std::cos(transform.rotate.y);
		const float sine = std::sin(transform.rotate.y);
		const float deltaX = worldPosition.x - transform.translate.x;
		const float deltaZ = worldPosition.z - transform.translate.z;
		return {
			deltaX * cosine - deltaZ * sine,
			0.0f,
			deltaX * sine + deltaZ * cosine
		};
	}

	float ColliderRadiusXZ(const SceneComponent& collider) {
		return std::sqrt(
			collider.colliderSizeMultiplier.x * collider.colliderSizeMultiplier.x +
			collider.colliderSizeMultiplier.z * collider.colliderSizeMultiplier.z
		);
	}

	struct XZPoint {
		float x = 0.0f;
		float z = 0.0f;
	};

	struct SharkObstacleFootprint {
		Vector3 center{};
		float axisXCosine = 1.0f;
		float axisXSine = 0.0f;
		float halfSizeX = 0.0f;
		float halfSizeZ = 0.0f;
	};

	struct SharkWaterBounds {
		Vector3 center{};
		float axisXCosine = 1.0f;
		float axisXSine = 0.0f;
		float halfSizeX = 0.0f;
		float halfSizeZ = 0.0f;
	};

	SharkObstacleFootprint BuildSharkObstacleFootprint(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& collider
	) {
		const Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const float yaw = ExtractPlanarYaw(transform);
		const float cosine = std::cos(yaw);
		const float sine = std::sin(yaw);
		const float scaleX = (std::max)(std::abs(transform.scale.x), 0.001f);
		const float scaleZ = (std::max)(std::abs(transform.scale.z), 0.001f);
		return {
			ToSpawnWorldPosition(
				transform,
				collider.colliderOffset.x * scaleX,
				collider.colliderOffset.z * scaleZ,
				transform.translate.y + collider.colliderOffset.y
			),
			cosine,
			sine,
			std::abs(collider.colliderSizeMultiplier.x) * scaleX,
			std::abs(collider.colliderSizeMultiplier.z) * scaleZ
		};
	}

	std::vector<SharkObstacleFootprint> BuildSharkObstacleFootprints(
		const SceneDocument& document
	) {
		std::vector<SharkObstacleFootprint> obstacles;
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity) ||
				!FindEnabledComponent(entity, "FishingObstacle")) {
				continue;
			}
			const SceneComponent* collider = FindEnabledComponent(
				entity, "OBBCollider"
			);
			if (!collider || collider->colliderIsTrigger) {
				continue;
			}
			obstacles.push_back(
				BuildSharkObstacleFootprint(document, entity, *collider)
			);
		}
		return obstacles;
	}

	float SharkColliderRadiusXZ(
		const SceneComponent& collider,
		const Transform& transform
	) {
		const float sizeX = std::abs(collider.colliderSizeMultiplier.x) *
			(std::max)(std::abs(transform.scale.x), 0.001f);
		const float sizeZ = std::abs(collider.colliderSizeMultiplier.z) *
			(std::max)(std::abs(transform.scale.z), 0.001f);
		return std::sqrt(sizeX * sizeX + sizeZ * sizeZ);
	}

	SharkWaterBounds BuildSharkWaterBounds(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& waterVolume
	) {
		Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const float scaleX = (std::max)(std::abs(transform.scale.x), 0.001f);
		const float scaleZ = (std::max)(std::abs(transform.scale.z), 0.001f);
		const float yaw = ExtractPlanarYaw(transform);
		transform.translate = ToSpawnWorldPosition(
			transform,
			waterVolume.waterOffset.x,
			waterVolume.waterOffset.z,
			transform.translate.y + waterVolume.waterOffset.y
		);
		return {
			transform.translate,
			std::cos(yaw),
			std::sin(yaw),
			waterVolume.waterHalfSize.x * scaleX,
			waterVolume.waterHalfSize.z * scaleZ
		};
	}

	XZPoint ToSharkLocalXZ(
		const Vector3& position,
		const Vector3& center,
		float axisXCosine,
		float axisXSine
	) {
		const float deltaX = position.x - center.x;
		const float deltaZ = position.z - center.z;
		return {
			deltaX * axisXCosine - deltaZ * axisXSine,
			deltaX * axisXSine + deltaZ * axisXCosine
		};
	}

	bool SegmentIntersectsSharkObstacle(
		const XZPoint& start,
		const XZPoint& end,
		const SharkObstacleFootprint& obstacle,
		float inflation
	) {
		const XZPoint localStart = ToSharkLocalXZ(
			{ start.x, 0.0f, start.z },
			obstacle.center,
			obstacle.axisXCosine,
			obstacle.axisXSine
		);
		const XZPoint localEnd = ToSharkLocalXZ(
			{ end.x, 0.0f, end.z },
			obstacle.center,
			obstacle.axisXCosine,
			obstacle.axisXSine
		);
		const float halfSizeX = obstacle.halfSizeX + inflation;
		const float halfSizeZ = obstacle.halfSizeZ + inflation;
		const float delta[2] = {
			localEnd.x - localStart.x,
			localEnd.z - localStart.z
		};
		const float origin[2] = { localStart.x, localStart.z };
		const float halfSize[2] = { halfSizeX, halfSizeZ };
		float minimum = 0.0f;
		float maximum = 1.0f;
		for (int axis = 0; axis < 2; ++axis) {
			if (std::abs(delta[axis]) <= kTransformEpsilon) {
				if (std::abs(origin[axis]) > halfSize[axis]) {
					return false;
				}
				continue;
			}
			float nearValue = (-halfSize[axis] - origin[axis]) / delta[axis];
			float farValue = (halfSize[axis] - origin[axis]) / delta[axis];
			if (nearValue > farValue) {
				std::swap(nearValue, farValue);
			}
			minimum = (std::max)(minimum, nearValue);
			maximum = (std::min)(maximum, farValue);
			if (minimum > maximum) {
				return false;
			}
		}
		return true;
	}

	bool IsPointInsideSharkWater(
		const XZPoint& point,
		const SharkWaterBounds& water,
		float margin
	) {
		const XZPoint local = ToSharkLocalXZ(
			{ point.x, 0.0f, point.z },
			water.center,
			water.axisXCosine,
			water.axisXSine
		);
		return std::abs(local.x) <= water.halfSizeX - margin &&
			std::abs(local.z) <= water.halfSizeZ - margin;
	}

	bool IsSharkSegmentClear(
		const XZPoint& start,
		const XZPoint& end,
		const SharkWaterBounds& water,
		const std::vector<SharkObstacleFootprint>& obstacles,
		float sharkRadius
	) {
		constexpr float kSafetyMargin = 0.25f;
		if (!IsPointInsideSharkWater(end, water, sharkRadius + kSafetyMargin)) {
			return false;
		}
		for (const SharkObstacleFootprint& obstacle : obstacles) {
			if (SegmentIntersectsSharkObstacle(
				start, end, obstacle, sharkRadius + kSafetyMargin
			)) {
				return false;
			}
		}
		return true;
	}

	float NormalizeSharkAngle(float angle) {
		while (angle > 3.14159265358979323846f) {
			angle -= kTwoPi;
		}
		while (angle < -3.14159265358979323846f) {
			angle += kTwoPi;
		}
		return angle;
	}

	float SharkAngleDelta(float from, float to) {
		return NormalizeSharkAngle(to - from);
	}

	float MoveSharkAngle(
		float current,
		float target,
		float maximumDelta
	) {
		const float delta = SharkAngleDelta(current, target);
		return NormalizeSharkAngle(
			current + std::clamp(delta, -maximumDelta, maximumDelta)
		);
	}

	Vector3 SharkHeadingVector(float heading) {
		return { std::sin(heading), 0.0f, std::cos(heading) };
	}

	float MeasureSharkHeadingClearance(
		const XZPoint& start,
		float heading,
		float lookahead,
		const SharkWaterBounds& water,
		const std::vector<SharkObstacleFootprint>& obstacles,
		float sharkRadius
	) {
		constexpr int kSamples = 8;
		const Vector3 direction = SharkHeadingVector(heading);
		for (int sample = 1; sample <= kSamples; ++sample) {
			const float distance = lookahead *
				static_cast<float>(sample) / static_cast<float>(kSamples);
			const XZPoint end = {
				start.x + direction.x * distance,
				start.z + direction.z * distance
			};
			if (!IsSharkSegmentClear(
				start, end, water, obstacles, sharkRadius
			)) {
				return lookahead * static_cast<float>(sample - 1) /
					static_cast<float>(kSamples);
			}
		}
		return lookahead;
	}

	struct FormationCapsule {
		Vector3 center{};
		float yaw = 0.0f;
		float radius = 0.0f;
		float halfSegmentLength = 0.0f;
	};

	float CrossXZ(const XZPoint& a, const XZPoint& b, const XZPoint& c) {
		return (b.x - a.x) * (c.z - a.z) -
			(b.z - a.z) * (c.x - a.x);
	}

	float DistanceSquared(const XZPoint& a, const XZPoint& b) {
		const float deltaX = a.x - b.x;
		const float deltaZ = a.z - b.z;
		return deltaX * deltaX + deltaZ * deltaZ;
	}

	float PointSegmentDistanceSquared(
		const XZPoint& point,
		const XZPoint& start,
		const XZPoint& end
	) {
		const float deltaX = end.x - start.x;
		const float deltaZ = end.z - start.z;
		const float lengthSquared = deltaX * deltaX + deltaZ * deltaZ;
		if (lengthSquared <= kTransformEpsilon * kTransformEpsilon) {
			return DistanceSquared(point, start);
		}
		const float projection = std::clamp(
			((point.x - start.x) * deltaX + (point.z - start.z) * deltaZ) /
				lengthSquared,
			0.0f,
			1.0f
		);
		const XZPoint closest = {
			start.x + deltaX * projection,
			start.z + deltaZ * projection
		};
		return DistanceSquared(point, closest);
	}

	bool IsOnSegment(
		const XZPoint& point,
		const XZPoint& start,
		const XZPoint& end
	) {
		return
			point.x >= (std::min)(start.x, end.x) - kTransformEpsilon &&
			point.x <= (std::max)(start.x, end.x) + kTransformEpsilon &&
			point.z >= (std::min)(start.z, end.z) - kTransformEpsilon &&
			point.z <= (std::max)(start.z, end.z) + kTransformEpsilon;
	}

	bool SegmentsIntersect(
		const XZPoint& leftStart,
		const XZPoint& leftEnd,
		const XZPoint& rightStart,
		const XZPoint& rightEnd
	) {
		const float first = CrossXZ(leftStart, leftEnd, rightStart);
		const float second = CrossXZ(leftStart, leftEnd, rightEnd);
		const float third = CrossXZ(rightStart, rightEnd, leftStart);
		const float fourth = CrossXZ(rightStart, rightEnd, leftEnd);
		const bool properIntersection =
			((first > kTransformEpsilon && second < -kTransformEpsilon) ||
				(first < -kTransformEpsilon && second > kTransformEpsilon)) &&
			((third > kTransformEpsilon && fourth < -kTransformEpsilon) ||
				(third < -kTransformEpsilon && fourth > kTransformEpsilon));
		if (properIntersection) {
			return true;
		}
		return
			(std::abs(first) <= kTransformEpsilon &&
				IsOnSegment(rightStart, leftStart, leftEnd)) ||
			(std::abs(second) <= kTransformEpsilon &&
				IsOnSegment(rightEnd, leftStart, leftEnd)) ||
			(std::abs(third) <= kTransformEpsilon &&
				IsOnSegment(leftStart, rightStart, rightEnd)) ||
			(std::abs(fourth) <= kTransformEpsilon &&
				IsOnSegment(leftEnd, rightStart, rightEnd));
	}

	std::vector<XZPoint> BuildObbProjection(const OBBCollider::OBB& obb) {
		std::vector<XZPoint> points;
		points.reserve(8);
		for (int axis0Sign = -1; axis0Sign <= 1; axis0Sign += 2) {
			for (int axis1Sign = -1; axis1Sign <= 1; axis1Sign += 2) {
				for (int axis2Sign = -1; axis2Sign <= 1; axis2Sign += 2) {
					points.push_back({
						obb.center.x +
							obb.axis[0].x * obb.halfSize.x * static_cast<float>(axis0Sign) +
							obb.axis[1].x * obb.halfSize.y * static_cast<float>(axis1Sign) +
							obb.axis[2].x * obb.halfSize.z * static_cast<float>(axis2Sign),
						obb.center.z +
							obb.axis[0].z * obb.halfSize.x * static_cast<float>(axis0Sign) +
							obb.axis[1].z * obb.halfSize.y * static_cast<float>(axis1Sign) +
							obb.axis[2].z * obb.halfSize.z * static_cast<float>(axis2Sign)
					});
				}
			}
		}
		std::sort(points.begin(), points.end(), [](const XZPoint& left, const XZPoint& right) {
			return left.x < right.x ||
				(left.x == right.x && left.z < right.z);
		});
		points.erase(
			std::unique(points.begin(), points.end(), [](const XZPoint& left, const XZPoint& right) {
				return left.x == right.x && left.z == right.z;
			}),
			points.end()
		);
		if (points.size() <= 2) {
			return points;
		}

		std::vector<XZPoint> hull(points.size() * 2);
		size_t hullSize = 0;
		for (const XZPoint& point : points) {
			while (hullSize >= 2 && CrossXZ(
				hull[hullSize - 2], hull[hullSize - 1], point
			) <= 0.0f) {
				--hullSize;
			}
			hull[hullSize++] = point;
		}
		const size_t lowerSize = hullSize;
		for (size_t index = points.size() - 1; index > 0; --index) {
			const XZPoint& point = points[index - 1];
			while (
				hullSize > lowerSize &&
				CrossXZ(hull[hullSize - 2], hull[hullSize - 1], point) <= 0.0f
			) {
				--hullSize;
			}
			hull[hullSize++] = point;
		}
		if (hullSize > 1) {
			--hullSize;
		}
		hull.resize(hullSize);
		return hull;
	}

	bool IsPointInsideConvexPolygon(
		const XZPoint& point,
		const std::vector<XZPoint>& polygon
	) {
		if (polygon.size() < 3) {
			return false;
		}
		bool hasPositive = false;
		bool hasNegative = false;
		for (size_t index = 0; index < polygon.size(); ++index) {
			const float cross = CrossXZ(
				polygon[index],
				polygon[(index + 1) % polygon.size()],
				point
			);
			hasPositive |= cross > kTransformEpsilon;
			hasNegative |= cross < -kTransformEpsilon;
		}
		return !(hasPositive && hasNegative);
	}

	bool IntersectsFormationCapsule(
		const FormationCapsule& capsule,
		const OBBCollider::OBB& hookObb
	) {
		std::vector<XZPoint> polygon = BuildObbProjection(hookObb);
		if (polygon.size() < 2) {
			return false;
		}
		const float cosine = std::cos(capsule.yaw);
		const float sine = std::sin(capsule.yaw);
		auto toLocal = [capsule, cosine, sine](const XZPoint& point) {
			const float deltaX = point.x - capsule.center.x;
			const float deltaZ = point.z - capsule.center.z;
			return XZPoint{
				deltaX * cosine - deltaZ * sine,
				deltaX * sine + deltaZ * cosine
			};
		};
		for (XZPoint& point : polygon) {
			point = toLocal(point);
		}
		const XZPoint capsuleStart = { 0.0f, -capsule.halfSegmentLength };
		const XZPoint capsuleEnd = { 0.0f, capsule.halfSegmentLength };
		if (
			IsPointInsideConvexPolygon(capsuleStart, polygon) ||
			IsPointInsideConvexPolygon(capsuleEnd, polygon)
		) {
			return true;
		}
		const float radiusSquared = capsule.radius * capsule.radius;
		for (size_t index = 0; index < polygon.size(); ++index) {
			const XZPoint edgeStart = polygon[index];
			const XZPoint edgeEnd = polygon[(index + 1) % polygon.size()];
			if (SegmentsIntersect(capsuleStart, capsuleEnd, edgeStart, edgeEnd)) {
				return true;
			}
			if (
				PointSegmentDistanceSquared(capsuleStart, edgeStart, edgeEnd) <= radiusSquared ||
				PointSegmentDistanceSquared(capsuleEnd, edgeStart, edgeEnd) <= radiusSquared ||
				PointSegmentDistanceSquared(edgeStart, capsuleStart, capsuleEnd) <= radiusSquared ||
				PointSegmentDistanceSquared(edgeEnd, capsuleStart, capsuleEnd) <= radiusSquared
			) {
				return true;
			}
		}
		return false;
	}

	bool TryGetPlayerFormationCapsule(
		const SceneDocument& document,
		const SceneComponent& director,
		const SceneAgentSystem& agentSystem,
		FormationCapsule& capsule
	) {
		const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
		const SceneTeamSettings* team = player
			? document.ResolveEntityTeam(*player)
			: nullptr;
		if (!player || !team || !team->agentFormationCapsuleEnabled) {
			return false;
		}
		const Transform playerTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *player);
		capsule.center = playerTransform.translate;
		capsule.yaw = ExtractPlanarYaw(playerTransform);
		SceneAgentFormationCapsuleState state{};
		if (!agentSystem.TryGetTeamFormationCapsuleState(team->name, state)) {
			return false;
		}
		capsule.radius = state.radius;
		capsule.halfSegmentLength = state.halfSegmentLength;
		return
			std::isfinite(capsule.center.x) &&
			std::isfinite(capsule.center.y) &&
			std::isfinite(capsule.center.z) &&
			std::isfinite(capsule.yaw) &&
			std::isfinite(capsule.radius) && capsule.radius > 0.0f &&
			std::isfinite(capsule.halfSegmentLength) &&
			capsule.halfSegmentLength >= 0.0f;
	}

	const SceneRuntimeObjectBinding* FindBinding(
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		uint64_t entityId
	) {
		const auto found = std::find_if(
			bindings.begin(),
			bindings.end(),
			[entityId](const SceneRuntimeObjectBinding& binding) {
				return binding.entity && binding.entity->id == entityId;
			}
		);
		return found == bindings.end() ? nullptr : &(*found);
	}

	std::string FormatOneDecimal(float value) {
		char buffer[32]{};
		std::snprintf(buffer, sizeof(buffer), "%.1f", value);
		return buffer;
	}

	std::string FormatHookScoreMultiplier(float value) {
		std::string formatted = FormatOneDecimal(value);
		if (formatted.size() >= 2 && formatted.ends_with(".0")) {
			formatted.resize(formatted.size() - 2);
		}
		return formatted;
	}
}

void SceneFishingScoreAttackSystem::UpdateBeforeSimulation(
	SceneDocument& document,
	float deltaTime,
	bool playing
) {
	uint64_t foundDirectorEntityId = 0;
	bool duplicateDirector = false;
	const SceneComponent* director = FindDirector(
		document,
		foundDirectorEntityId,
		duplicateDirector
	);
	if (!playing || !director) {
		if (duplicateDirector) {
			hasDirector_ = true;
			state_ = SceneFishingScoreAttackState::Faulted;
			diagnostic_ = "Multiple FishingScoreAttackDirector components are active";
			textRequests_.clear();
			iconRequests_.clear();
		} else {
			Clear();
		}
		return;
	}

	if (directorEntityId_ != foundDirectorEntityId) {
		Clear();
		directorEntityId_ = foundDirectorEntityId;
	}
	hasDirector_ = true;
	if (state_ == SceneFishingScoreAttackState::Result) {
		resultInputArmed_ = true;
	}
	if (state_ == SceneFishingScoreAttackState::Inactive) {
		std::string diagnostic;
		if (!Preflight(document, foundDirectorEntityId, *director, diagnostic)) {
			Fault(document, *director, std::move(diagnostic));
			return;
		}
		InitializeRun(document, *director);
	}

	if (timerRunning_) {
		elapsedSeconds_ += (std::max)(deltaTime, 0.0f);
		if (elapsedSeconds_ >= director->fishingDurationSeconds) {
			elapsedSeconds_ = director->fishingDurationSeconds;
			Finish(document, *director);
			return;
		}
	}

	if (
		state_ == SceneFishingScoreAttackState::SelectingInitial ||
		state_ == SceneFishingScoreAttackState::SelectingNext
	) {
		UpdateSelection(document, *director);
	}
	if (state_ == SceneFishingScoreAttackState::Navigating) {
		UpdateSharks(document, *director, deltaTime);
	}
	BuildTextRequests(*director);
}

void SceneFishingScoreAttackSystem::UpdateAfterSimulation(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	const SceneAgentSystem& agentSystem,
	bool playing,
	float deltaTime,
	const Vector3& planarVelocity
) {
	if (!playing || state_ != SceneFishingScoreAttackState::Navigating) {
		return;
	}
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
		return;
	}
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director) {
		Clear();
		return;
	}
	FormationCapsule formationCapsule{};
	if (
		director->fishingUseFormationCapsuleCollision &&
		!TryGetPlayerFormationCapsule(
			document,
			*director,
			agentSystem,
			formationCapsule
		)
	) {
		Fault(
			document,
			*director,
			"Fishing formation capsule is invalid for the Player Team"
		);
		return;
	}
	if (director->fishingUseFormationCapsuleCollision) {
		if (!hasLastSafePlayerPlanarPosition_) {
			Fault(
				document,
				*director,
				"Fishing formation has no safe position for obstacle correction"
			);
			return;
		}
		std::vector<FishingFormationMotion::Obstacle> obstacles;
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity) ||
				!FindEnabledComponent(entity, "FishingObstacle")) {
				continue;
			}
			const SceneRuntimeObjectBinding* obstacleBinding = FindBinding(
				bindings,
				entity.id
			);
			if (!obstacleBinding || !obstacleBinding->collider ||
				obstacleBinding->collider->GetType() != Collider::Type::OBB ||
				!obstacleBinding->collider->IsActive() ||
				obstacleBinding->collider->IsTrigger()) {
				Fault(
					document,
					*director,
					"FishingObstacle requires an active non-trigger OBB runtime binding"
				);
				return;
			}
			const auto* obstacleCollider = static_cast<const OBBCollider*>(
				obstacleBinding->collider
			);
			const std::vector<XZPoint> hull = BuildObbProjection(
				obstacleCollider->GetOBB()
			);
			FishingFormationMotion::Obstacle obstacle{};
			obstacle.entityId = entity.id;
			obstacle.hull.reserve(hull.size());
			for (const XZPoint& point : hull) {
				obstacle.hull.push_back({ point.x, point.z });
			}
			obstacles.push_back(std::move(obstacle));
		}
		FishingFormationMotion::Request motionRequest{};
		motionRequest.startCenter = {
			lastSafePlayerPlanarPosition_.x,
			lastSafePlayerPlanarPosition_.z
		};
		motionRequest.desiredCenter = {
			formationCapsule.center.x,
			formationCapsule.center.z
		};
		motionRequest.desiredVelocity = { planarVelocity.x, planarVelocity.z };
		motionRequest.startYaw = lastSafePlayerYaw_;
		motionRequest.desiredYaw = formationCapsule.yaw;
		motionRequest.radius = formationCapsule.radius;
		motionRequest.halfSegmentLength = formationCapsule.halfSegmentLength;
		motionRequest.bounds.enabled = hasPlayerWaterBounds_;
		if (hasPlayerWaterBounds_) {
			motionRequest.bounds.center = {
				playerWaterBounds_.center.x,
				playerWaterBounds_.center.z
			};
			motionRequest.bounds.yaw = playerWaterBounds_.yaw;
			motionRequest.bounds.halfSizeX = playerWaterBounds_.halfSizeX;
			motionRequest.bounds.halfSizeZ = playerWaterBounds_.halfSizeZ;
		}
		FishingFormationMotion::Result motionResult{};
		if (!FishingFormationMotion::Solve(
			motionRequest,
			obstacles,
			motionResult
		)) {
			Fault(
				document,
				*director,
				"Fishing formation motion solver rejected the current pose"
			);
			return;
		}
		formationCapsule.center.x = motionResult.center.x;
		formationCapsule.center.z = motionResult.center.y;
		formationCapsule.yaw = motionResult.yaw;
		lastSafePlayerPlanarPosition_ = formationCapsule.center;
		lastSafePlayerPlanarPosition_.y = 0.0f;
		lastSafePlayerYaw_ = formationCapsule.yaw;
		hasLastSafePlayerPlanarPosition_ = true;
		const bool positionChanged =
			std::abs(motionResult.center.x - motionRequest.desiredCenter.x) >
				kTransformEpsilon ||
			std::abs(motionResult.center.y - motionRequest.desiredCenter.y) >
				kTransformEpsilon;
		const float yawDelta = std::atan2(
			std::sin(motionResult.yaw - motionRequest.desiredYaw),
			std::cos(motionResult.yaw - motionRequest.desiredYaw)
		);
		const bool yawChanged = std::abs(yawDelta) > kTransformEpsilon;
		const bool velocityChanged =
			std::abs(motionResult.velocity.x - planarVelocity.x) >
				kTransformEpsilon ||
			std::abs(motionResult.velocity.y - planarVelocity.z) >
				kTransformEpsilon;
		if (positionChanged || yawChanged || velocityChanged) {
			playerConstraintRequest_.playerEntityId =
				director->fishingPlayerEntityId;
			playerConstraintRequest_.planarPosition = {
				motionResult.center.x,
				0.0f,
				motionResult.center.y
			};
			playerConstraintRequest_.yaw = motionResult.yaw;
			playerConstraintRequest_.planarVelocity = {
				motionResult.velocity.x,
				0.0f,
				motionResult.velocity.y
			};
			hasPlayerConstraintRequest_ = true;
		}
	}
	long long sharkPenaltyTotal = 0;
	bool sharkPenaltyApplied = false;
	if (director->fishingUseFormationCapsuleCollision) {
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity)) {
				continue;
			}
			const SceneComponent* shark = FindEnabledComponent(
				entity, "FishingShark"
			);
			if (!shark) {
				continue;
			}
			auto runtime = sharkRuntimes_.find(entity.id);
			if (runtime == sharkRuntimes_.end() ||
				runtime->second.hitCooldown > 0.0f) {
				continue;
			}
			const SceneRuntimeObjectBinding* sharkBinding = FindBinding(
				bindings, entity.id
			);
			if (!sharkBinding || !sharkBinding->collider ||
				sharkBinding->collider->GetType() != Collider::Type::OBB ||
				!sharkBinding->collider->IsActive() ||
				!sharkBinding->collider->IsTrigger()) {
				continue;
			}
			const auto* sharkCollider = static_cast<const OBBCollider*>(
				sharkBinding->collider
			);
			if (!IntersectsFormationCapsule(
				formationCapsule,
				sharkCollider->GetOBB()
			)) {
				continue;
			}
			const long long penalty = static_cast<long long>(
				(std::max)(shark->fishingSharkPenaltyScore, 0)
			);
			const long long maximumScore = (std::numeric_limits<long long>::max)();
			if (sharkPenaltyTotal > maximumScore - penalty) {
				sharkPenaltyTotal = maximumScore;
			} else {
				sharkPenaltyTotal += penalty;
			}
			runtime->second.hitCooldown = (std::max)(
				shark->fishingSharkHitCooldownSeconds,
				0.0f
			);
			sharkPenaltyApplied = true;
		}
	}
	if (sharkPenaltyApplied) {
		const long long minimumScore =
			(std::numeric_limits<long long>::lowest)();
		if (totalScore_ < minimumScore + sharkPenaltyTotal) {
			totalScore_ = minimumScore;
		} else {
			totalScore_ -= sharkPenaltyTotal;
		}
		DeactivatePoolHooks(document, *director);
		playerConstraintRequest_ = {};
		hasPlayerConstraintRequest_ = false;
		hasPlayerResetRequest_ = hasInitialPlayerTransform_;
		state_ = SceneFishingScoreAttackState::SelectingNext;
		SetFishPreview(document, *director);
		BuildTextRequests(*director);
		return;
	}
	const ActiveHook* hitHook = nullptr;
	const SceneComponent* hitHookComponent = nullptr;
	for (const ActiveHook& activeHook : activeHooks_) {
		const SceneRuntimeObjectBinding* hookBinding = FindBinding(bindings, activeHook.entityId);
		if (!hookBinding || !hookBinding->entity || !hookBinding->collider ||
			!IsEntityActiveInHierarchy(document, *hookBinding->entity)) {
			continue;
		}
		bool intersects = false;
		if (director->fishingUseFormationCapsuleCollision) {
			if (
				hookBinding->collider->GetType() != Collider::Type::OBB ||
				!hookBinding->collider->IsActive() ||
				!hookBinding->collider->IsTrigger()
			) {
				continue;
			}
			const auto* hookCollider = static_cast<const OBBCollider*>(
				hookBinding->collider
			);
			intersects = IntersectsFormationCapsule(
				formationCapsule,
				hookCollider->GetOBB()
			);
		} else {
			for (int fishIndex = 0; fishIndex < roundFishCount_; ++fishIndex) {
				if (fishIndex >= static_cast<int>(director->fishingFishEntityIds.size())) {
					break;
				}
				const SceneRuntimeObjectBinding* fishBinding = FindBinding(
					bindings,
					director->fishingFishEntityIds[static_cast<size_t>(fishIndex)]
				);
				if (
					!fishBinding ||
					!fishBinding->entity ||
					!fishBinding->collider ||
					!IsEntityActiveInHierarchy(document, *fishBinding->entity) ||
					!fishBinding->collider->CanCollideWith(*hookBinding->collider) ||
					!fishBinding->collider->Intersects(*hookBinding->collider)
				) {
					continue;
				}
				intersects = true;
				break;
			}
		}
		if (intersects) {
			hitHook = &activeHook;
			hitHookComponent = FindComponent(
				document,
				activeHook.entityId,
				"FishingHook"
			);
			break;
		}
		if (hitHook) {
			break;
		}
	}
	if (!hitHook || !hitHookComponent) {
		return;
	}
	const SceneFishingHookRankDefinition rank = ResolveFishingHookRank(
		*director,
		hitHook->hookMultiplierTier
	);
	double score = 0.0;
	if (director->fishingUseHookBandSettings) {
		const double fishMultiplier = (std::max)(
			0.0,
			static_cast<double>(director->fishingFishMultiplierBase) +
				static_cast<double>((std::max)(roundFishCount_ - 1, 0)) *
				static_cast<double>(director->fishingFishMultiplierPerAdditionalFish)
		);
		score = std::round(
			static_cast<double>(director->fishingHookScoreUnit) *
			static_cast<double>(hitHook->multiplier) *
			static_cast<double>(rank.scoreMultiplier) *
			fishMultiplier
		);
	} else {
		score = std::round(
			static_cast<double>(hitHook->multiplier) *
			static_cast<double>(roundFishCount_) *
			static_cast<double>(hitHookComponent->fishingHookBaseScore)
		);
	}
	const double maximumScore = static_cast<double>(
		(std::numeric_limits<long long>::max)()
	);
	if (score > 0.0 && score >= maximumScore - static_cast<double>(totalScore_)) {
		totalScore_ = (std::numeric_limits<long long>::max)();
	} else {
		totalScore_ += static_cast<long long>(score);
	}
	DeactivatePoolHooks(document, *director);
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasPlayerResetRequest_ = hasInitialPlayerTransform_;
	state_ = SceneFishingScoreAttackState::SelectingNext;
	SetFishPreview(document, *director);
	BuildTextRequests(*director);
}

void SceneFishingScoreAttackSystem::ApplyHookVisualOverrides(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director) {
		return;
	}
	const SceneComponent* pool = FindComponent(
		document,
		director->fishingHookPoolEntityId,
		"FishingHookPool"
	);
	if (!pool) {
		return;
	}
	std::unordered_map<uint64_t, int> activeHookTiers;
	for (const ActiveHook& activeHook : activeHooks_) {
		activeHookTiers[activeHook.entityId] = activeHook.hookMultiplierTier;
	}
	for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
		const SceneRuntimeObjectBinding* binding = FindBinding(
			bindings,
			entry.hookEntityId
		);
		if (!binding || !binding->object) {
			continue;
		}
		const SceneEntity* hookEntity = document.FindEntity(entry.hookEntityId);
		if (!hookEntity) {
			continue;
		}
		const SceneComponent* meshRenderer = FindEnabledComponent(
			*hookEntity,
			"MeshRenderer"
		);
		const std::string authoredModelPath = meshRenderer
			? meshRenderer->modelPath
			: hookEntity->modelPath;
		const auto activeIt = activeHookTiers.find(entry.hookEntityId);
		const bool active = activeIt != activeHookTiers.end() &&
			director->fishingUseHookBandSettings;
		const SceneFishingHookRankDefinition rank = active
			? ResolveFishingHookRank(*director, activeIt->second)
			: SceneFishingHookRankDefinition{};
		const std::string desiredModelPath = active && !rank.modelPath.empty()
			? rank.modelPath
			: authoredModelPath;
		const auto modelIt = hookVisualModelPaths_.find(entry.hookEntityId);
		if (modelIt == hookVisualModelPaths_.end() ||
			modelIt->second != desiredModelPath) {
			if (desiredModelPath.empty()) {
				binding->object->SetModel(static_cast<Model*>(nullptr));
				hookVisualModelPaths_[entry.hookEntityId] = desiredModelPath;
			} else {
				ModelManager::GetInstance()->LoadModel(desiredModelPath);
				Model* model = ModelManager::GetInstance()->FindModel(
					desiredModelPath
				);
				if (model) {
					binding->object->SetModel(model);
					hookVisualModelPaths_[entry.hookEntityId] = desiredModelPath;
				}
			}
		}
		if (active) {
			binding->object->SetColor(rank.color);
			binding->object->SetEmissive(
				director->fishingHookColorEmissiveIntensity,
				rank.color
			);
		} else {
			binding->object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			binding->object->SetEmissive(0.0f);
		}
	}
}

bool SceneFishingScoreAttackSystem::IsPlayerMovementAllowed() const {
	return !hasDirector_ || state_ == SceneFishingScoreAttackState::Navigating;
}

bool SceneFishingScoreAttackSystem::AcceptWheelZoom() const {
	return !hasDirector_ || state_ == SceneFishingScoreAttackState::Navigating;
}

uint64_t SceneFishingScoreAttackSystem::GetResultInputReadyDirectorEntityId() const {
	return state_ == SceneFishingScoreAttackState::Result && resultInputArmed_
		? directorEntityId_
		: 0;
}

void SceneFishingScoreAttackSystem::QueueFishCountAdjustment(
	uint64_t directorEntityId,
	int delta
) {
	if (
		directorEntityId != directorEntityId_ ||
		(state_ != SceneFishingScoreAttackState::SelectingInitial &&
			state_ != SceneFishingScoreAttackState::SelectingNext) ||
		(delta != 1 && delta != -1)
	) {
		return;
	}
	if (delta > 0) {
		if (pendingFishCountDelta_ < (std::numeric_limits<int64_t>::max)()) {
			++pendingFishCountDelta_;
		}
		return;
	}
	if (pendingFishCountDelta_ > (std::numeric_limits<int64_t>::min)()) {
		--pendingFishCountDelta_;
	}
}

bool SceneFishingScoreAttackSystem::TryGetPlayerWaterBounds(
	SceneFishingScoreAttackPlayerWaterBounds& bounds
) const {
	if (!hasPlayerWaterBounds_) {
		return false;
	}
	bounds = playerWaterBounds_;
	return true;
}

bool SceneFishingScoreAttackSystem::ConsumePlayerConstraintRequest(
	SceneFishingScoreAttackPlayerConstraintRequest& request
) {
	if (!hasPlayerConstraintRequest_) {
		return false;
	}
	request = playerConstraintRequest_;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	return true;
}

bool SceneFishingScoreAttackSystem::ConsumePlayerResetRequest(
	SceneFishingScoreAttackPlayerResetRequest& request
) {
	if (!hasPlayerResetRequest_) {
		return false;
	}
	request.playerEntityId = playerWaterBounds_.playerEntityId;
	request.transform = initialPlayerTransform_;
	request.teamName = fishingTeamName_;
	request.entityResets.clear();
	request.entityResets.reserve(initialFishTransforms_.size());
	for (size_t index = 0; index < initialFishTransforms_.size(); ++index) {
		request.entityResets.push_back({
			index < initialFishEntityIds_.size()
				? initialFishEntityIds_[index]
				: 0,
			initialFishTransforms_[index]
		});
	}
	 hasPlayerResetRequest_ = false;
	return true;
}

void SceneFishingScoreAttackSystem::AddFormationOutlineDebugDraw(
	const SceneDocument& document,
	const SceneAgentSystem& agentSystem
) const {
	if (state_ != SceneFishingScoreAttackState::Navigating || selectedFishCount_ < 1) {
		return;
	}
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director || !director->fishingFormationOutlineVisible) {
		return;
	}
	FormationCapsule capsule{};
	if (!TryGetPlayerFormationCapsule(
		document,
		*director,
		agentSystem,
		capsule
	)) {
		return;
	}
	const int outlineSegments = std::clamp(
		director->fishingFormationOutlineSegments,
		12,
		128
	);
	if (!std::isfinite(director->fishingFormationOutlineYOffset)) {
		return;
	}
	const Vector4 outlineColor = director->fishingFormationOutlineColor;
	const float outlineBloomIntensity = director->fishingFormationOutlineBloomIntensity;
	if (
		!std::isfinite(outlineColor.x) ||
		!std::isfinite(outlineColor.y) ||
		!std::isfinite(outlineColor.z) ||
		!std::isfinite(outlineColor.w) ||
		!std::isfinite(outlineBloomIntensity) ||
		outlineBloomIntensity < 0.0f ||
		outlineBloomIntensity > 32.0f
	) {
		return;
	}
	const Vector4 sanitizedOutlineColor = {
		std::clamp(outlineColor.x, 0.0f, 1.0f) * outlineBloomIntensity,
		std::clamp(outlineColor.y, 0.0f, 1.0f) * outlineBloomIntensity,
		std::clamp(outlineColor.z, 0.0f, 1.0f) * outlineBloomIntensity,
		std::clamp(outlineColor.w, 0.0f, 1.0f)
	};
	DebugRenderer* debugRenderer = DebugRenderer::GetInstance();
	if (!debugRenderer) {
		return;
	}

	const float cosine = std::cos(capsule.yaw);
	const float sine = std::sin(capsule.yaw);
	const float y = capsule.center.y + director->fishingFormationOutlineYOffset;
	auto toWorld = [capsule, cosine, sine, y](const XZPoint& point) {
		return Vector3{
			capsule.center.x + point.x * cosine + point.z * sine,
			y,
			capsule.center.z - point.x * sine + point.z * cosine
		};
	};
	auto addLine = [debugRenderer, &toWorld, sanitizedOutlineColor](
		const XZPoint& start,
		const XZPoint& end
	) {
		debugRenderer->AddLine(toWorld(start), toWorld(end), sanitizedOutlineColor);
	};

	const int arcSegments = (std::max)(6, outlineSegments / 2);
	XZPoint previous = { -capsule.radius, -capsule.halfSegmentLength };
	XZPoint current = { -capsule.radius, capsule.halfSegmentLength };
	addLine(previous, current);
	previous = current;
	for (int index = 1; index <= arcSegments; ++index) {
		const float angle = 3.14159265358979323846f *
			(1.0f - static_cast<float>(index) / static_cast<float>(arcSegments));
		current = {
			capsule.radius * std::cos(angle),
			capsule.halfSegmentLength + capsule.radius * std::sin(angle)
		};
		addLine(previous, current);
		previous = current;
	}
	current = { capsule.radius, -capsule.halfSegmentLength };
	addLine(previous, current);
	previous = current;
	for (int index = 1; index <= arcSegments; ++index) {
		const float angle = -3.14159265358979323846f *
			static_cast<float>(index) / static_cast<float>(arcSegments);
		current = {
			capsule.radius * std::cos(angle),
			-capsule.halfSegmentLength + capsule.radius * std::sin(angle)
		};
		addLine(previous, current);
		previous = current;
	}
}

bool SceneFishingScoreAttackSystem::Preflight(
	const SceneDocument& document,
	uint64_t directorEntityId,
	const SceneComponent& director,
	std::string& diagnostic
) const {
	const bool useHookBandSettings = director.fishingUseHookBandSettings;
	if (!IsSupportedSceneInput(director.fishingConfirmInput)) {
		diagnostic = "FishingScoreAttackDirector fish count confirm input is unsupported";
		return false;
	}
	if (
		director.fishingPlayerEntityId == 0 ||
		director.fishingHookSpawnAreaEntityId == 0 ||
		director.fishingHookPoolEntityId == 0 ||
		director.fishingWaterVolumeEntityId == 0 ||
		director.fishingFishEntityIds.empty() ||
		director.fishingMaxSelectableFishCount < 1 ||
		static_cast<size_t>(director.fishingMaxSelectableFishCount) >
			director.fishingFishEntityIds.size() ||
		!std::isfinite(director.fishingDurationSeconds) ||
		director.fishingDurationSeconds <= 0.0f ||
		(!useHookBandSettings && (
			director.fishingDistanceBandCount < 1 ||
			director.fishingHooksPerDistanceBand < 1 ||
			!IsFiniteNonNegative(director.fishingDistanceMultiplierBase) ||
			!IsFiniteNonNegative(director.fishingDistanceMultiplierStep)
		))
	) {
		diagnostic = "FishingScoreAttackDirector values are invalid";
		return false;
	}
	if (useHookBandSettings) {
		if (director.fishingHookBands.size() != 5) {
			diagnostic = "FishingScoreAttackDirector requires exactly five hook bands";
			return false;
		}
		for (size_t bandIndex = 0; bandIndex < director.fishingHookBands.size(); ++bandIndex) {
			const SceneFishingHookBandSettings& band = director.fishingHookBands[bandIndex];
			if (!IsFiniteNonNegative(band.distanceMultiplier) || band.hookCount < 0 ||
				(bandIndex == 0 && band.hookCount != 0) ||
				(bandIndex > 0 && band.hookCount == 0) ||
				band.hookMultiplierWeights.size() != 10) {
				diagnostic = "FishingScoreAttackDirector hook band settings are invalid";
				return false;
			}
			float totalWeight = 0.0f;
			for (const float weight : band.hookMultiplierWeights) {
				if (!IsFiniteNonNegative(weight)) {
					diagnostic = "FishingScoreAttackDirector hook tier weights are invalid";
					return false;
				}
				totalWeight += weight;
			}
			if (band.hookCount > 0 && (!std::isfinite(totalWeight) || totalWeight <= 0.0f)) {
				diagnostic = "FishingScoreAttackDirector hook band has no selectable tier";
				return false;
			}
		}
		if (!std::isfinite(director.fishingHookScoreUnit) ||
			director.fishingHookScoreUnit <= 0.0f ||
			!IsFiniteNonNegative(director.fishingFishMultiplierBase) ||
			!IsFiniteNonNegative(director.fishingFishMultiplierPerAdditionalFish) ||
			director.fishingHookRanks.size() != 10 ||
			!IsFiniteNonNegative(director.fishingHookColorEmissiveIntensity)) {
			diagnostic = "FishingScoreAttackDirector hook score settings are invalid";
			return false;
		}
		std::unordered_set<std::string> rankIds;
		for (const SceneFishingHookRankDefinition& rank : director.fishingHookRanks) {
			if (rank.id.empty() || !rankIds.insert(rank.id).second ||
				!IsFiniteNonNegative(rank.scoreMultiplier) ||
				!IsResourceRelativeModelPath(rank.modelPath) ||
				!std::isfinite(rank.color.x) || !std::isfinite(rank.color.y) ||
				!std::isfinite(rank.color.z) || !std::isfinite(rank.color.w) ||
				rank.color.x < 0.0f || rank.color.x > 1.0f ||
				rank.color.y < 0.0f || rank.color.y > 1.0f ||
				rank.color.z < 0.0f || rank.color.z > 1.0f ||
				rank.color.w < 0.0f || rank.color.w > 1.0f) {
				diagnostic = "FishingScoreAttackDirector hook rank definitions are invalid";
				return false;
			}
		}
	}
	bool hasFreeWanderShark = false;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* shark = FindEnabledComponent(entity, "FishingShark");
		if (!shark) {
			continue;
		}
		if (!IsFiniteNonNegative(shark->fishingSharkWanderMoveSpeed) ||
			!IsFiniteNonNegative(shark->fishingSharkWanderMaximumTurnRate)) {
			diagnostic = "FishingShark free-wander settings are invalid";
			return false;
		}
		if (shark->fishingSharkWanderMoveSpeed <= kTransformEpsilon) {
			continue;
		}
		hasFreeWanderShark = true;
		if (shark->fishingSharkWanderMaximumTurnRate <= kTransformEpsilon) {
			diagnostic = "FishingShark free-wander settings are invalid";
			return false;
		}
	}
	if (hasFreeWanderShark &&
		(!useHookBandSettings || director.fishingHookBands.size() < 2)) {
		diagnostic = "Free-wander FishingShark requires at least two hook bands";
		return false;
	}
	const SceneEntity* playerEntity = document.FindEntity(
		director.fishingPlayerEntityId
	);
	const SceneComponent* playerCollider = FindComponent(
		document,
		director.fishingPlayerEntityId,
		"OBBCollider"
	);
	const SceneComponent* playerBody = FindComponent(
		document,
		director.fishingPlayerEntityId,
		"PhysicsBody"
	);
	const SceneTeamSettings* playerTeam = playerEntity
		? document.ResolveEntityTeam(*playerEntity)
		: nullptr;
	const SceneComponent* playerLeaderController = playerEntity
		? FindEnabledComponent(*playerEntity, "AgentTeamLeaderController")
		: nullptr;
	if (
		!playerEntity ||
		playerEntity->parentId != 0 ||
		!IsEntityActiveInHierarchy(document, *playerEntity) ||
		!FindComponent(document, director.fishingPlayerEntityId, "PlayerBehavior") ||
		!playerTeam ||
		!playerLeaderController ||
		!playerCollider || !playerBody || !playerBody->physicsFreezePositionY
	) {
		diagnostic = "Fishing player requires PlayerBehavior, Y freeze, an owning Team, and AgentTeamLeaderController";
		return false;
	}
	int playerTeamControllerCount = 0;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			!IsEntityActiveInHierarchy(document, entity) ||
			!FindEnabledComponent(entity, "AgentTeamLeaderController")
		) {
			continue;
		}
		const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
		if (team && team->name == playerTeam->name) {
			++playerTeamControllerCount;
		}
	}
	if (playerTeamControllerCount != 1) {
		diagnostic = "Fishing player Team requires exactly one active AgentTeamLeaderController";
		return false;
	}
	const SceneEntity* waterEntity = document.FindEntity(
		director.fishingWaterVolumeEntityId
	);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	if (!waterEntity || !waterVolume ||
		!std::isfinite(waterVolume->waterHalfSize.x) ||
		!std::isfinite(waterVolume->waterHalfSize.z) ||
		waterVolume->waterHalfSize.x <= 0.0f ||
		waterVolume->waterHalfSize.z <= 0.0f) {
		diagnostic = "FishingScoreAttackDirector requires a valid WaterVolume";
		return false;
	}
	if (useHookBandSettings) {
		const Transform playerTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *playerEntity);
		Transform waterTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
		waterTransform.translate.x += waterVolume->waterOffset.x;
		waterTransform.translate.y += waterVolume->waterOffset.y;
		waterTransform.translate.z += waterVolume->waterOffset.z;
		const Vector3 playerWaterLocal = ToLocalXZ(
			waterTransform, playerTransform.translate
		);
		if (std::abs(playerWaterLocal.x) > waterVolume->waterHalfSize.x ||
			std::abs(playerWaterLocal.z) > waterVolume->waterHalfSize.z) {
			diagnostic = "Fishing player starts outside the WaterVolume";
			return false;
		}
		const float distanceToNegativeZ =
			playerWaterLocal.z + waterVolume->waterHalfSize.z;
		const float distanceToPositiveZ =
			waterVolume->waterHalfSize.z - playerWaterLocal.z;
		const bool startFromPositiveZ = distanceToPositiveZ < distanceToNegativeZ;
		const float normalizedZ = std::clamp(
			(playerWaterLocal.z + waterVolume->waterHalfSize.z) /
				(2.0f * waterVolume->waterHalfSize.z),
			0.0f,
			0.99999f
		);
		const float orientedZ = startFromPositiveZ ? 1.0f - normalizedZ : normalizedZ;
		if (static_cast<int>(std::floor(orientedZ * 5.0f)) != 0) {
			diagnostic = "Fishing player must start in hook band 0";
			return false;
		}
	}
	const SceneEntity* spawnAreaEntity = document.FindEntity(
		director.fishingHookSpawnAreaEntityId
	);
	const SceneComponent* spawnArea = FindComponent(
		document,
		director.fishingHookSpawnAreaEntityId,
		"FishingHookSpawnArea"
	);
	if (
		!spawnAreaEntity || !spawnArea ||
		!std::isfinite(spawnArea->fishingSpawnHalfSizeX) ||
		!std::isfinite(spawnArea->fishingSpawnHalfSizeZ) ||
		!std::isfinite(spawnArea->fishingSpawnMinimumDistance) ||
		spawnArea->fishingSpawnHalfSizeX <= 0.0f ||
		spawnArea->fishingSpawnHalfSizeZ <= 0.0f ||
		spawnArea->fishingSpawnMinimumDistance < 0.0f ||
		spawnArea->fishingSpawnMaxAttempts < 1
	) {
		diagnostic = "FishingHookSpawnArea values are invalid";
		return false;
	}
	const SceneComponent* pool = FindComponent(
		document,
		director.fishingHookPoolEntityId,
		"FishingHookPool"
	);
	int requiredHookCount = 0;
	if (useHookBandSettings) {
		for (const SceneFishingHookBandSettings& band : director.fishingHookBands) {
			requiredHookCount += band.hookCount;
		}
	} else {
		requiredHookCount = director.fishingDistanceBandCount *
			director.fishingHooksPerDistanceBand;
	}
	if (!pool || static_cast<int>(pool->fishingHookPoolEntries.size()) < requiredHookCount) {
		diagnostic = "FishingHookPool has too few entries for simultaneous hooks";
		return false;
	}

	std::unordered_set<uint64_t> fishIds;
	std::vector<const SceneComponent*> fishColliders;
	for (uint64_t fishEntityId : director.fishingFishEntityIds) {
		const SceneEntity* fish = document.FindEntity(fishEntityId);
		const SceneComponent* fishBehavior = fish
			? FindEnabledComponent(*fish, "AgentBehavior")
			: nullptr;
		const SceneComponent* fishCollider = fish
			? FindEnabledComponent(*fish, "OBBCollider")
			: nullptr;
		const SceneTeamSettings* fishTeam = fish
			? document.ResolveEntityTeam(*fish)
			: nullptr;
		if (
			fishEntityId == 0 || !fish ||
			!fishIds.insert(fishEntityId).second ||
			!fishBehavior ||
			!fishTeam || fishTeam->name != playerTeam->name ||
			!fishCollider || !fishCollider->colliderActive ||
			!fishCollider->colliderIsTrigger ||
			!HasTranslationOnlyAncestors(document, *fish)
		) {
			diagnostic = "Fishing fish require unique AgentBehavior, same Team, identity ancestors, and active Trigger Colliders";
			return false;
		}
		fishColliders.push_back(fishCollider);
	}
	if (
		director.fishingUseFormationCapsuleCollision ||
		director.fishingFormationOutlineVisible
	) {
		if (!playerTeam->agentFormationCapsuleEnabled) {
			diagnostic =
				"Fishing formation features require an enabled Player Team capsule: " +
				playerTeam->name;
			return false;
		}
		if (
			!std::isfinite(playerTeam->agentFormationCapsuleRadius) ||
			playerTeam->agentFormationCapsuleRadius <= 0.0f ||
			!std::isfinite(playerTeam->agentFormationCapsuleHalfSegmentLength) ||
			playerTeam->agentFormationCapsuleHalfSegmentLength < 0.0f
		) {
			diagnostic =
				"Fishing Player Team capsule dimensions are invalid: " +
				playerTeam->name;
			return false;
		}
		if (director.fishingFormationOutlineVisible) {
			const Vector4 color = director.fishingFormationOutlineColor;
			if (
				!std::isfinite(director.fishingFormationOutlineYOffset) ||
				!std::isfinite(director.fishingFormationOutlineBloomIntensity) ||
				director.fishingFormationOutlineBloomIntensity < 0.0f ||
				director.fishingFormationOutlineBloomIntensity > 32.0f ||
				director.fishingFormationOutlineSegments < 12 ||
				director.fishingFormationOutlineSegments > 128 ||
				!std::isfinite(color.x) || !std::isfinite(color.y) ||
				!std::isfinite(color.z) || !std::isfinite(color.w)
			) {
				diagnostic =
					"Fishing formation outline values are invalid for Player Team: " +
					playerTeam->name;
				return false;
			}
		}
		if (director.fishingUseFormationCapsuleCollision) {
			for (const SceneComponent* fishCollider : fishColliders) {
				if (
					ColliderRadiusXZ(*fishCollider) >=
					playerTeam->agentFormationCapsuleRadius
				) {
					diagnostic =
						"Fishing Player Team capsule radius is smaller than a fish collider: " +
						playerTeam->name;
					return false;
				}
			}
		}
	}

	std::unordered_set<uint64_t> hookIds;
	std::vector<float> weightTotals;
	if (!useHookBandSettings) {
		weightTotals.assign(
			static_cast<size_t>(director.fishingDistanceBandCount),
			0.0f
		);
	}
	for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
		const SceneComponent* hook = FindComponent(
			document,
			entry.hookEntityId,
			"FishingHook"
		);
		const SceneComponent* hookCollider = FindComponent(
			document,
			entry.hookEntityId,
			"OBBCollider"
		);
		if (
			entry.hookEntityId == 0 || !hook || !hookCollider ||
			!hookIds.insert(entry.hookEntityId).second ||
			!hookCollider->colliderIsTrigger || !hookCollider->colliderActive ||
			(!useHookBandSettings &&
				entry.weightsByDistanceBand.size() != weightTotals.size()) ||
			std::any_of(
				fishColliders.begin(),
				fishColliders.end(),
				[hookCollider](const SceneComponent* fishCollider) {
					return
						(fishCollider->colliderMask & hookCollider->colliderLayer) == 0 ||
						(hookCollider->colliderMask & fishCollider->colliderLayer) == 0;
				}
			)
		) {
			diagnostic = "FishingHookPool entry or Fish collision settings are invalid";
			return false;
		}
		if (!useHookBandSettings) {
			for (size_t bandIndex = 0; bandIndex < weightTotals.size(); ++bandIndex) {
				const float weight = entry.weightsByDistanceBand[bandIndex];
				if (!IsFiniteNonNegative(weight)) {
					diagnostic = "FishingHookPool contains an invalid weight";
					return false;
				}
				weightTotals[bandIndex] += weight;
			}
		}
	}
	if (!useHookBandSettings && std::any_of(weightTotals.begin(), weightTotals.end(), [](float total) {
		return !std::isfinite(total) || total <= 0.0f;
	})) {
		diagnostic = "FishingHookPool has no selectable hook for a distance band";
		return false;
	}

	const uint64_t textEntityIds[] = {
		director.fishingFishCountTextEntityId,
		director.fishingTimerTextEntityId,
		director.fishingScoreTextEntityId,
		director.fishingMultiplierTextEntityId,
		director.fishingResultTextEntityId
	};
	for (uint64_t textEntityId : textEntityIds) {
		if (textEntityId != 0 && !FindComponent(document, textEntityId, "TextRenderer")) {
			diagnostic = "Fishing HUD reference requires TextRenderer";
			return false;
		}
	}
	(void)directorEntityId;
	return true;
}

void SceneFishingScoreAttackSystem::InitializeRun(
	SceneDocument& document,
	const SceneComponent& director
) {
	if (director.fishingRandomizeSeedOnPlay) {
		std::random_device randomDevice;
		random_.seed(randomDevice());
	} else {
		random_.seed(static_cast<std::mt19937::result_type>(director.fishingRandomSeed));
	}
	selectedFishCount_ = 1;
	pendingFishCountDelta_ = 0;
	roundFishCount_ = 0;
	roundDistanceBand_ = 0;
	roundMultiplier_ = director.fishingUseHookBandSettings
		? (director.fishingHookBands.empty()
			? 0.0f
			: director.fishingHookBands.front().distanceMultiplier)
		: director.fishingDistanceMultiplierBase;
	elapsedSeconds_ = 0.0;
	totalScore_ = 0;
	timerRunning_ = false;
	resultInputArmed_ = false;
	diagnostic_.clear();
	initialFishEntityIds_ = director.fishingFishEntityIds;
	initialFishTransforms_.clear();
	initialFishTransforms_.reserve(initialFishEntityIds_.size());
	const SceneTeamSettings* playerTeam = nullptr;
	const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
	const SceneEntity* waterEntity = document.FindEntity(director.fishingWaterVolumeEntityId);
	const SceneComponent* playerCollider = FindComponent(
		document,
		director.fishingPlayerEntityId,
		"OBBCollider"
	);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	if (player && waterEntity && playerCollider && waterVolume) {
		initialPlayerTransform_ =
			SceneTransformResolver::ResolveScene3DTransform(document, *player);
		Transform waterTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
		waterTransform.translate.x += waterVolume->waterOffset.x;
		waterTransform.translate.y += waterVolume->waterOffset.y;
		waterTransform.translate.z += waterVolume->waterOffset.z;
		if (director.fishingUseHookBandSettings) {
			const Vector3 playerWaterLocal = ToLocalXZ(
				waterTransform, initialPlayerTransform_.translate
			);
			const float distanceToNegativeZ =
				playerWaterLocal.z + waterVolume->waterHalfSize.z;
			const float distanceToPositiveZ =
				waterVolume->waterHalfSize.z - playerWaterLocal.z;
			startFromPositiveWaterZ_ = distanceToPositiveZ < distanceToNegativeZ;
		} else {
			startFromPositiveWaterZ_ = false;
		}
		constexpr float kWaterBoundarySafetyMargin = 0.1f;
		const float playerRadius = ColliderRadiusXZ(*playerCollider);
		playerWaterBounds_.playerEntityId = player->id;
		playerWaterBounds_.center = {
			waterTransform.translate.x,
			waterTransform.translate.y,
			waterTransform.translate.z
		};
		playerWaterBounds_.yaw = waterTransform.rotate.y;
		playerWaterBounds_.halfSizeX = (std::max)(
			waterVolume->waterHalfSize.x - playerRadius - kWaterBoundarySafetyMargin,
			0.001f
		);
		playerWaterBounds_.halfSizeZ = (std::max)(
			waterVolume->waterHalfSize.z - playerRadius - kWaterBoundarySafetyMargin,
			0.001f
		);
		hasInitialPlayerTransform_ = true;
		hasPlayerWaterBounds_ = true;
	}
	if (player) {
		playerTeam = document.ResolveEntityTeam(*player);
	}
	fishingTeamName_ = playerTeam ? playerTeam->name : std::string{};
	for (uint64_t fishEntityId : initialFishEntityIds_) {
		const SceneEntity* fish = document.FindEntity(fishEntityId);
		initialFishTransforms_.push_back(
			fish
				? SceneTransformResolver::ResolveScene3DTransform(document, *fish)
				: Transform{}
		);
	}
	sharkRuntimes_.clear();
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity) ||
			!FindEnabledComponent(entity, "FishingShark")) {
			continue;
		}
		const SceneComponent* shark = FindEnabledComponent(entity, "FishingShark");
		SharkRuntime runtime{};
		runtime.initialTransform.scale = entity.transform.scale;
		runtime.initialTransform.rotate = MakeEulerFromQuaternion(
			entity.transform.rotate
		);
		runtime.initialTransform.translate = entity.transform.translate;
		const float pathRandomness = shark && std::isfinite(
			shark->fishingSharkPathRandomness
		) ? std::clamp(shark->fishingSharkPathRandomness, 0.0f, 1.0f) : 0.0f;
		runtime.wanderRandom.seed(random_());
		std::uniform_real_distribution<float> scaleDistribution(
			1.0f - pathRandomness,
			1.0f + pathRandomness
		);
		std::uniform_real_distribution<float> retargetDistribution(1.5f, 3.5f);
		std::uniform_real_distribution<float> phaseDistribution(0.0f, kTwoPi);
		runtime.radiusXScale = scaleDistribution(runtime.wanderRandom);
		runtime.radiusZScale = scaleDistribution(runtime.wanderRandom);
		runtime.angularSpeedScale = scaleDistribution(runtime.wanderRandom);
		runtime.targetRadiusXScale = scaleDistribution(runtime.wanderRandom);
		runtime.targetRadiusZScale = scaleDistribution(runtime.wanderRandom);
		runtime.targetAngularSpeedScale = scaleDistribution(runtime.wanderRandom);
		runtime.retargetRemainingSeconds = retargetDistribution(runtime.wanderRandom);
		runtime.wobblePhase = phaseDistribution(runtime.wanderRandom);
		sharkRuntimes_.emplace(entity.id, runtime);
	}
	hasPlayerResetRequest_ = false;
	lastSafePlayerPlanarPosition_ = {};
	lastSafePlayerYaw_ = 0.0f;
	hasLastSafePlayerPlanarPosition_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	DeactivatePoolHooks(document, director);
	SetFishPreview(document, director);
	state_ = SceneFishingScoreAttackState::SelectingInitial;
	BuildTextRequests(director);
}

void SceneFishingScoreAttackSystem::UpdateSelection(
	SceneDocument& document,
	const SceneComponent& director
) {
	const int64_t pendingDelta = pendingFishCountDelta_;
	pendingFishCountDelta_ = 0;
	Input* input = Input::GetInstance();
	if (!input) {
		return;
	}
	int64_t nextFishCount = selectedFishCount_;
	if (
		(pendingDelta > 0 && nextFishCount > (std::numeric_limits<int64_t>::max)() - pendingDelta) ||
		(pendingDelta < 0 && nextFishCount < (std::numeric_limits<int64_t>::min)() - pendingDelta)
	) {
		nextFishCount = pendingDelta > 0
			? (std::numeric_limits<int64_t>::max)()
			: (std::numeric_limits<int64_t>::min)();
	} else {
		nextFishCount += pendingDelta;
	}
	const float wheel = input->GetMouseWheel();
	if (std::abs(wheel) > 0.000001f) {
		const int wheelNotches = static_cast<int>(std::round(wheel));
		if (
			(wheelNotches > 0 && nextFishCount > (std::numeric_limits<int64_t>::max)() - wheelNotches) ||
			(wheelNotches < 0 && nextFishCount < (std::numeric_limits<int64_t>::min)() - wheelNotches)
		) {
			nextFishCount = wheelNotches > 0
				? (std::numeric_limits<int64_t>::max)()
				: (std::numeric_limits<int64_t>::min)();
		} else {
			nextFishCount += wheelNotches;
		}
	}
	const int64_t clampedFishCount = std::clamp<int64_t>(
		nextFishCount,
		1,
		director.fishingMaxSelectableFishCount
	);
	if (selectedFishCount_ != clampedFishCount) {
		selectedFishCount_ = static_cast<int>(clampedFishCount);
		SetFishPreview(document, director);
	}
	if (!SceneRuntimeInput::EvaluateExpression(
		director.fishingConfirmInputExpression,
		director.fishingConfirmInput
	)) {
		return;
	}
	if (state_ == SceneFishingScoreAttackState::SelectingInitial) {
		timerRunning_ = true;
	}
	StartRound(document, director);
}

void SceneFishingScoreAttackSystem::StartRound(
	SceneDocument& document,
	const SceneComponent& director
) {
	const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
	const SceneEntity* spawnAreaEntity = document.FindEntity(
		director.fishingHookSpawnAreaEntityId
	);
	const SceneComponent* spawnArea = FindComponent(
		document,
		director.fishingHookSpawnAreaEntityId,
		"FishingHookSpawnArea"
	);
	const SceneComponent* pool = FindComponent(
		document,
		director.fishingHookPoolEntityId,
		"FishingHookPool"
	);
	const SceneEntity* waterEntity = document.FindEntity(director.fishingWaterVolumeEntityId);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	if (!player || !spawnAreaEntity || !spawnArea || !pool || !waterEntity || !waterVolume) {
		Fault(document, director, "Fishing round references became invalid");
		return;
	}

	const Transform playerTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *player);
	const Transform areaTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *spawnAreaEntity);
	Transform waterTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
	waterTransform.translate.x += waterVolume->waterOffset.x;
	waterTransform.translate.y += waterVolume->waterOffset.y;
	waterTransform.translate.z += waterVolume->waterOffset.z;
	std::uniform_real_distribution<float> xDistribution(
		-spawnArea->fishingSpawnHalfSizeX,
		spawnArea->fishingSpawnHalfSizeX
	);
	std::uniform_real_distribution<float> zDistribution(
		-spawnArea->fishingSpawnHalfSizeZ,
		spawnArea->fishingSpawnHalfSizeZ
	);
	DeactivatePoolHooks(document, director);
	std::unordered_set<uint64_t> usedHookIds;
	std::vector<Vector3> spawnPositions;
	std::vector<float> spawnRadii;
	const bool useHookBandSettings = director.fishingUseHookBandSettings;
	const int distanceBandCount = useHookBandSettings ? 5 : director.fishingDistanceBandCount;
	for (int bandIndex = 0; bandIndex < distanceBandCount; ++bandIndex) {
		const int hooksInBand = useHookBandSettings
			? director.fishingHookBands[static_cast<size_t>(bandIndex)].hookCount
			: director.fishingHooksPerDistanceBand;
		for (int hookIndex = 0; hookIndex < hooksInBand; ++hookIndex) {
			const SceneFishingHookPoolEntry* selectedEntry = nullptr;
			int hookMultiplierTier = 1;
			if (useHookBandSettings) {
				std::vector<size_t> availableEntryIndices;
				for (size_t entryIndex = 0;
					entryIndex < pool->fishingHookPoolEntries.size(); ++entryIndex) {
					const SceneFishingHookPoolEntry& entry = pool->fishingHookPoolEntries[entryIndex];
					if (usedHookIds.find(entry.hookEntityId) == usedHookIds.end()) {
						availableEntryIndices.push_back(entryIndex);
					}
				}
				if (availableEntryIndices.empty()) {
					Fault(document, director, "FishingHookPool cannot select unique hooks");
					return;
				}
				std::uniform_int_distribution<size_t> entryDistribution(
					0, availableEntryIndices.size() - 1
				);
				selectedEntry = &pool->fishingHookPoolEntries[
					availableEntryIndices[entryDistribution(random_)]
				];
				const std::vector<float>& tierWeights =
					director.fishingHookBands[static_cast<size_t>(bandIndex)].hookMultiplierWeights;
				float totalWeight = 0.0f;
				for (const float weight : tierWeights) {
					totalWeight += weight;
				}
				std::uniform_real_distribution<float> weightDistribution(0.0f, totalWeight);
				float remainingWeight = weightDistribution(random_);
				int selectedTierIndex = -1;
				for (size_t tierIndex = 0; tierIndex < tierWeights.size(); ++tierIndex) {
					if (tierWeights[tierIndex] <= 0.0f) {
						continue;
					}
					remainingWeight -= tierWeights[tierIndex];
					if (remainingWeight <= 0.0f) {
						selectedTierIndex = static_cast<int>(tierIndex);
						break;
					}
				}
				if (selectedTierIndex < 0) {
					for (int tierIndex = static_cast<int>(tierWeights.size()) - 1;
						tierIndex >= 0; --tierIndex) {
						if (tierWeights[static_cast<size_t>(tierIndex)] > 0.0f) {
							selectedTierIndex = tierIndex;
							break;
						}
					}
				}
				if (selectedTierIndex < 0) {
					Fault(document, director, "FishingHookBand has no selectable tier");
					return;
				}
				hookMultiplierTier = selectedTierIndex + 1;
			} else {
				float totalWeight = 0.0f;
				for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
					if (usedHookIds.find(entry.hookEntityId) == usedHookIds.end()) {
						totalWeight += entry.weightsByDistanceBand[static_cast<size_t>(bandIndex)];
					}
				}
				if (!std::isfinite(totalWeight) || totalWeight <= 0.0f) {
					Fault(document, director, "FishingHookPool cannot select unique hooks for a distance band");
					return;
				}
				std::uniform_real_distribution<float> weightDistribution(0.0f, totalWeight);
				float remainingWeight = weightDistribution(random_);
				for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
					if (usedHookIds.find(entry.hookEntityId) != usedHookIds.end()) {
						continue;
					}
					const float weight = entry.weightsByDistanceBand[static_cast<size_t>(bandIndex)];
					if (weight <= 0.0f) {
						continue;
					}
					remainingWeight -= weight;
					if (remainingWeight <= 0.0f) {
						selectedEntry = &entry;
						break;
					}
				}
				if (!selectedEntry) {
					Fault(document, director, "FishingHookPool selection failed");
					return;
				}
			}
			const SceneComponent* hookCollider = FindComponent(
				document, selectedEntry->hookEntityId, "OBBCollider"
			);
			Vector3 spawnPosition{};
			bool foundPosition = false;
			for (int attempt = 0; attempt < spawnArea->fishingSpawnMaxAttempts; ++attempt) {
				const Vector3 candidate = ToSpawnWorldPosition(
					areaTransform, xDistribution(random_), zDistribution(random_), playerTransform.translate.y
				);
				const Vector3 waterLocal = ToLocalXZ(waterTransform, candidate);
				const float normalizedZ = (waterLocal.z + waterVolume->waterHalfSize.z) /
					(2.0f * waterVolume->waterHalfSize.z);
				const float orientedZ = startFromPositiveWaterZ_
					? 1.0f - std::clamp(normalizedZ, 0.0f, 0.99999f)
					: std::clamp(normalizedZ, 0.0f, 0.99999f);
				const int candidateBand = (std::min)(
					static_cast<int>(std::floor(orientedZ * distanceBandCount)),
					distanceBandCount - 1
				);
				if (std::abs(waterLocal.x) > waterVolume->waterHalfSize.x ||
					std::abs(waterLocal.z) > waterVolume->waterHalfSize.z || candidateBand != bandIndex ||
					DistanceXZ(candidate, playerTransform.translate) < spawnArea->fishingSpawnMinimumDistance) {
					continue;
				}
				const float hookRadius = hookCollider ? ColliderRadiusXZ(*hookCollider) : 0.0f;
				bool overlaps = false;
				for (size_t existingIndex = 0; existingIndex < spawnPositions.size(); ++existingIndex) {
					if (DistanceXZ(candidate, spawnPositions[existingIndex]) < hookRadius + spawnRadii[existingIndex]) {
						overlaps = true;
						break;
					}
				}
				for (const SceneEntity& entity : document.GetEntities()) {
					if (!FindEnabledComponent(entity, "FishingObstacle")) { continue; }
					const SceneComponent* obstacleCollider = FindEnabledComponent(entity, "OBBCollider");
					if (!obstacleCollider) { continue; }
					const Transform obstacleTransform = SceneTransformResolver::ResolveScene3DTransform(document, entity);
					const Vector3 obstacleCenter = ToSpawnWorldPosition(
						obstacleTransform,
						obstacleCollider->colliderOffset.x,
						obstacleCollider->colliderOffset.z,
						obstacleTransform.translate.y + obstacleCollider->colliderOffset.y
					);
					if (DistanceXZ(candidate, obstacleCenter) < hookRadius + ColliderRadiusXZ(*obstacleCollider)) {
						overlaps = true; break;
					}
				}
				if (!overlaps) { spawnPosition = candidate; foundPosition = true; break; }
			}
			if (!foundPosition) {
				Fault(document, director, "FishingHookSpawnArea has no valid position for a distance band");
				return;
			}
			SceneEntity* hookEntity = document.FindEntity(selectedEntry->hookEntityId);
			if (!hookEntity) { Fault(document, director, "Selected FishingHook is missing"); return; }
			hookEntity->transform.translate = spawnPosition;
			hookEntity->active = true;
			usedHookIds.insert(hookEntity->id);
			spawnPositions.push_back(spawnPosition);
			spawnRadii.push_back(hookCollider ? ColliderRadiusXZ(*hookCollider) : 0.0f);
			const float distanceMultiplier = useHookBandSettings
				? director.fishingHookBands[static_cast<size_t>(bandIndex)].distanceMultiplier
				: director.fishingDistanceMultiplierBase +
					director.fishingDistanceMultiplierStep * static_cast<float>(bandIndex);
			activeHooks_.push_back({ hookEntity->id, bandIndex, distanceMultiplier, hookMultiplierTier });
		}
	}
	roundFishCount_ = selectedFishCount_;
	roundDistanceBand_ = 0;
	roundMultiplier_ = useHookBandSettings
		? director.fishingHookBands.front().distanceMultiplier
		: director.fishingDistanceMultiplierBase;
	lastSafePlayerPlanarPosition_ = {
		playerTransform.translate.x,
		0.0f,
		playerTransform.translate.z
	};
	lastSafePlayerYaw_ = ExtractPlanarYaw(playerTransform);
	hasLastSafePlayerPlanarPosition_ = true;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	if (!ResetSharksForRound(document, director)) {
		return;
	}
	pendingFishCountDelta_ = 0;
	state_ = SceneFishingScoreAttackState::Navigating;
	SetFishPreview(document, director);
}

void SceneFishingScoreAttackSystem::UpdateSharks(
	SceneDocument& document,
	const SceneComponent& director,
	float deltaTime
) {
	const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
	const SceneEntity* waterEntity = document.FindEntity(
		director.fishingWaterVolumeEntityId
	);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	const bool hasWaterBounds = waterEntity && waterVolume;
	const SharkWaterBounds waterBounds = hasWaterBounds
		? BuildSharkWaterBounds(document, *waterEntity, *waterVolume)
		: SharkWaterBounds{};
	const std::vector<SharkObstacleFootprint> obstacles =
		BuildSharkObstacleFootprints(document);
	for (auto& [entityId, runtime] : sharkRuntimes_) {
		SceneEntity* entity = document.FindEntity(entityId);
		if (!entity || !IsEntityActiveInHierarchy(document, *entity)) {
			continue;
		}
		const SceneComponent* shark = FindEnabledComponent(
			*entity, "FishingShark"
		);
		if (!shark) {
			continue;
		}
		const float wanderMoveSpeed = std::isfinite(
			shark->fishingSharkWanderMoveSpeed
		) ? (std::max)(shark->fishingSharkWanderMoveSpeed, 0.0f) : 0.0f;
		if (wanderMoveSpeed > kTransformEpsilon) {
			const Transform sharkTransform =
				SceneTransformResolver::ResolveScene3DTransform(document, *entity);
			const SceneComponent* sharkCollider = FindEnabledComponent(
				*entity, "OBBCollider"
			);
			const float sharkRadius = sharkCollider
				? SharkColliderRadiusXZ(*sharkCollider, sharkTransform)
				: 0.0f;
			const float pathRandomness = std::isfinite(
				shark->fishingSharkPathRandomness
			) ? std::clamp(shark->fishingSharkPathRandomness, 0.0f, 1.0f) : 0.0f;
			const float maximumTurnRate = std::isfinite(
				shark->fishingSharkWanderMaximumTurnRate
			) ? (std::max)(shark->fishingSharkWanderMaximumTurnRate, 0.0f) : 0.0f;
			if (!std::isfinite(runtime.wanderHeading)) {
				runtime.wanderHeading = ExtractPlanarYaw(sharkTransform);
			}
			if (!std::isfinite(runtime.wanderTargetHeading)) {
				runtime.wanderTargetHeading = runtime.wanderHeading;
			}
			runtime.retargetRemainingSeconds -= safeDeltaTime;
			if (runtime.retargetRemainingSeconds <= 0.0f) {
				std::uniform_real_distribution<float> intervalDistribution(0.6f, 1.6f);
				runtime.retargetRemainingSeconds = intervalDistribution(
					runtime.wanderRandom
				);
				if (pathRandomness > kTransformEpsilon) {
					std::uniform_real_distribution<float> angleDistribution(
						0.25f * 3.14159265358979323846f * pathRandomness,
						3.14159265358979323846f * pathRandomness
					);
					std::uniform_int_distribution<int> signDistribution(0, 1);
					const float sign = signDistribution(runtime.wanderRandom) == 0
						? -1.0f
						: 1.0f;
					runtime.wanderTargetHeading = NormalizeSharkAngle(
						runtime.wanderHeading + sign * angleDistribution(
							runtime.wanderRandom
						)
					);
				} else {
					runtime.wanderTargetHeading = runtime.wanderHeading;
				}
			}

			const XZPoint start = {
				runtime.previousPosition.x,
				runtime.previousPosition.z
			};
			const float configuredAvoidanceDistance = std::isfinite(
				shark->fishingSharkObstacleAvoidanceDistance
			) ? (std::max)(shark->fishingSharkObstacleAvoidanceDistance, 0.0f) : 0.0f;
			const float lookahead = (std::max)(
				configuredAvoidanceDistance,
				wanderMoveSpeed * 0.75f
			);
			const float desiredHeading = runtime.wanderTargetHeading;
			const Vector3 desiredDirection = SharkHeadingVector(desiredHeading);
			const XZPoint lookaheadEnd = {
				start.x + desiredDirection.x * lookahead,
				start.z + desiredDirection.z * lookahead
			};
			const bool navigationReady = hasWaterBounds && sharkCollider;
			const bool threat = navigationReady &&
				!IsSharkSegmentClear(
					start, lookaheadEnd, waterBounds, obstacles, sharkRadius
				);
			float turnRate = maximumTurnRate;
			if (threat) {
				const float avoidanceStrength = std::isfinite(
					shark->fishingSharkObstacleAvoidanceStrength
				) ? std::clamp(shark->fishingSharkObstacleAvoidanceStrength, 0.0f, 1.0f) : 0.0f;
				const float obstacleTurnRate = std::isfinite(
					shark->fishingSharkObstacleAvoidanceResponse
				) ? (std::max)(shark->fishingSharkObstacleAvoidanceResponse, 0.0f) : 0.0f;
				turnRate = (std::max)(turnRate, obstacleTurnRate);
				const float offsets[] = {
					0.0f,
					-0.52359877559829887308f,
					0.52359877559829887308f,
					-1.04719755119659774615f,
					1.04719755119659774615f,
					-1.57079632679489661923f,
					1.57079632679489661923f
				};
				float bestClearance = -1.0f;
				float bestOffset = 0.0f;
				for (float offset : offsets) {
					const float clearance = MeasureSharkHeadingClearance(
						start,
						runtime.wanderHeading + offset,
						lookahead,
						waterBounds,
						obstacles,
						sharkRadius
					);
					const int candidateSide = offset < -kTransformEpsilon
						? -1
						: offset > kTransformEpsilon ? 1 : 0;
					if (clearance > bestClearance + kTransformEpsilon ||
						(std::abs(clearance - bestClearance) <= kTransformEpsilon &&
							candidateSide == runtime.wanderAvoidanceSide)) {
						bestClearance = clearance;
						bestOffset = offset;
					}
				}
				if (bestClearance <= kTransformEpsilon) {
					bestOffset = runtime.wanderAvoidanceSide < 0
						? -1.57079632679489661923f
						: 1.57079632679489661923f;
				}
				runtime.wanderAvoidanceSide = bestOffset < -kTransformEpsilon
					? -1
					: bestOffset > kTransformEpsilon ? 1 : 0;
				const float avoidanceHeading = NormalizeSharkAngle(
					runtime.wanderHeading + bestOffset
				);
				runtime.wanderTargetHeading = NormalizeSharkAngle(
					runtime.wanderHeading +
						SharkAngleDelta(runtime.wanderHeading, avoidanceHeading) *
						avoidanceStrength
				);
			} else {
				runtime.wanderAvoidanceSide = 0;
			}
			runtime.wanderHeading = MoveSharkAngle(
				runtime.wanderHeading,
				runtime.wanderTargetHeading,
				turnRate * safeDeltaTime
			);
			const Vector3 direction = SharkHeadingVector(runtime.wanderHeading);
			const XZPoint candidate = {
				start.x + direction.x * wanderMoveSpeed * safeDeltaTime,
				start.z + direction.z * wanderMoveSpeed * safeDeltaTime
			};
			XZPoint finalPosition = start;
			if (navigationReady && IsSharkSegmentClear(
				start, candidate, waterBounds, obstacles, sharkRadius
			)) {
				finalPosition = candidate;
			}
			entity->transform.scale = runtime.initialTransform.scale;
			entity->transform.translate = {
				finalPosition.x,
				runtime.initialTransform.translate.y,
				finalPosition.z
			};
			entity->transform.rotate = MakeQuaternionFromEuler({
				0.0f,
				runtime.wanderHeading,
				0.0f
			});
			runtime.previousPosition = entity->transform.translate;
			runtime.hasPreviousPosition = true;
			runtime.hitCooldown = (std::max)(
				runtime.hitCooldown - safeDeltaTime,
				0.0f
			);
			continue;
		}
		const float pathRandomness = std::isfinite(
			shark->fishingSharkPathRandomness
		) ? std::clamp(shark->fishingSharkPathRandomness, 0.0f, 1.0f) : 0.0f;
		const float minimumScale = 1.0f - pathRandomness;
		const float maximumScale = 1.0f + pathRandomness;
		runtime.radiusXScale = std::clamp(
			runtime.radiusXScale, minimumScale, maximumScale
		);
		runtime.radiusZScale = std::clamp(
			runtime.radiusZScale, minimumScale, maximumScale
		);
		runtime.angularSpeedScale = std::clamp(
			runtime.angularSpeedScale, minimumScale, maximumScale
		);
		runtime.targetRadiusXScale = std::clamp(
			runtime.targetRadiusXScale, minimumScale, maximumScale
		);
		runtime.targetRadiusZScale = std::clamp(
			runtime.targetRadiusZScale, minimumScale, maximumScale
		);
		runtime.targetAngularSpeedScale = std::clamp(
			runtime.targetAngularSpeedScale, minimumScale, maximumScale
		);
		runtime.retargetRemainingSeconds -= safeDeltaTime;
		if (runtime.retargetRemainingSeconds <= 0.0f) {
			std::uniform_real_distribution<float> scaleDistribution(
				minimumScale, maximumScale
			);
			std::uniform_real_distribution<float> retargetDistribution(1.5f, 3.5f);
			runtime.targetRadiusXScale = scaleDistribution(runtime.wanderRandom);
			runtime.targetRadiusZScale = scaleDistribution(runtime.wanderRandom);
			runtime.targetAngularSpeedScale = scaleDistribution(runtime.wanderRandom);
			runtime.retargetRemainingSeconds = retargetDistribution(runtime.wanderRandom);
		}
		const float scaleSmoothing = std::clamp(
			1.0f - std::exp(-1.5f * safeDeltaTime),
			0.0f,
			1.0f
		);
		runtime.radiusXScale = Math::Lerp(
			runtime.radiusXScale,
			runtime.targetRadiusXScale,
			scaleSmoothing
		);
		runtime.radiusZScale = Math::Lerp(
			runtime.radiusZScale,
			runtime.targetRadiusZScale,
			scaleSmoothing
		);
		runtime.angularSpeedScale = Math::Lerp(
			runtime.angularSpeedScale,
			runtime.targetAngularSpeedScale,
			scaleSmoothing
		);
		const float angularSpeed = shark->fishingSharkAngularSpeed *
			runtime.angularSpeedScale;
		runtime.phase += angularSpeed * safeDeltaTime;
		const float cosine = std::cos(runtime.phase);
		const float sine = std::sin(runtime.phase);
		const float xWobbleArgument = runtime.phase * 1.37f + runtime.wobblePhase;
		const float zWobbleArgument = runtime.phase * 0.91f +
			runtime.wobblePhase * 1.61f;
		const float xFactor = 1.0f + pathRandomness * 0.15f *
			std::sin(xWobbleArgument);
		const float zFactor = 1.0f + pathRandomness * 0.15f *
			std::sin(zWobbleArgument);
		const float xFactorDerivative = pathRandomness * 0.15f * 1.37f *
			std::cos(xWobbleArgument);
		const float zFactorDerivative = pathRandomness * 0.15f * 0.91f *
			std::cos(zWobbleArgument);
		const float baseRadiusX = shark->fishingSharkRadiusX * runtime.radiusXScale;
		const float baseRadiusZ = shark->fishingSharkRadiusZ * runtime.radiusZScale;
		const float radiusX = baseRadiusX * xFactor;
		const float radiusZ = baseRadiusZ * zFactor;
		const Vector3 basePosition = {
			runtime.initialTransform.translate.x + radiusX * cosine,
			runtime.initialTransform.translate.y,
			runtime.initialTransform.translate.z + radiusZ * sine
		};
		Vector3 desiredAvoidanceOffset{};
		const float avoidanceDistance = std::isfinite(
			shark->fishingSharkObstacleAvoidanceDistance
		) ? (std::max)(shark->fishingSharkObstacleAvoidanceDistance, 0.0f) : 0.0f;
		const float avoidanceStrength = std::isfinite(
			shark->fishingSharkObstacleAvoidanceStrength
		) ? std::clamp(shark->fishingSharkObstacleAvoidanceStrength, 0.0f, 1.0f) : 0.0f;
		const SceneComponent* sharkCollider = FindEnabledComponent(*entity, "OBBCollider");
		if (sharkCollider && avoidanceDistance > 0.0f && avoidanceStrength > 0.0f) {
			const float sharkRadius = ColliderRadiusXZ(*sharkCollider);
			for (const SceneEntity& obstacleEntity : document.GetEntities()) {
				if (!IsEntityActiveInHierarchy(document, obstacleEntity) ||
					!FindEnabledComponent(obstacleEntity, "FishingObstacle")) {
					continue;
				}
				const SceneComponent* obstacleCollider = FindEnabledComponent(
					obstacleEntity, "OBBCollider"
				);
				if (!obstacleCollider || obstacleCollider->colliderIsTrigger) {
					continue;
				}
				const Transform obstacleTransform =
					SceneTransformResolver::ResolveScene3DTransform(
						document, obstacleEntity
					);
				const Vector3 obstacleCenter = ToSpawnWorldPosition(
					obstacleTransform,
					obstacleCollider->colliderOffset.x,
					obstacleCollider->colliderOffset.z,
					obstacleTransform.translate.y + obstacleCollider->colliderOffset.y
				);
				Vector3 away = {
					basePosition.x - obstacleCenter.x,
					0.0f,
					basePosition.z - obstacleCenter.z
				};
				float distance = Math::Length(away);
				const float threshold = sharkRadius + ColliderRadiusXZ(*obstacleCollider) +
					avoidanceDistance;
				if (distance >= threshold || threshold <= 0.0f) {
					continue;
				}
				if (distance <= kTransformEpsilon) {
					away = { cosine, 0.0f, sine };
					distance = 1.0f;
				}
				const float pushMagnitude = (threshold - distance) * avoidanceStrength;
				desiredAvoidanceOffset = Math::Add(
					desiredAvoidanceOffset,
					Math::Multiply(Math::Normalize(away), pushMagnitude)
				);
			}
		}
		const float maxAvoidanceOffset = avoidanceDistance * avoidanceStrength;
		const float avoidanceLength = Math::Length(desiredAvoidanceOffset);
		if (avoidanceLength > maxAvoidanceOffset && avoidanceLength > kTransformEpsilon) {
			desiredAvoidanceOffset = Math::Multiply(
				desiredAvoidanceOffset,
				maxAvoidanceOffset / avoidanceLength
			);
		}
		const float response = std::isfinite(
			shark->fishingSharkObstacleAvoidanceResponse
		) ? (std::max)(shark->fishingSharkObstacleAvoidanceResponse, 0.0f) : 0.0f;
		const float smoothing = std::clamp(
			1.0f - std::exp(-response * safeDeltaTime),
			0.0f,
			1.0f
		);
		runtime.avoidanceOffset = Math::Lerp(
			runtime.avoidanceOffset,
			desiredAvoidanceOffset,
			smoothing
		);
		runtime.avoidanceOffset.y = 0.0f;
		entity->transform.scale = runtime.initialTransform.scale;
		entity->transform.rotate = MakeQuaternionFromEuler(
			runtime.initialTransform.rotate
		);
		const Vector3 finalPosition = Math::Add(
			basePosition,
			runtime.avoidanceOffset
		);
		entity->transform.translate = finalPosition;
		const float movementX = finalPosition.x - runtime.previousPosition.x;
		const float movementZ = finalPosition.z - runtime.previousPosition.z;
		if (runtime.hasPreviousPosition &&
			std::sqrt(movementX * movementX + movementZ * movementZ) >
			kTransformEpsilon) {
			const float yaw = std::atan2(movementX, movementZ);
			entity->transform.rotate = MakeQuaternionFromEuler({ 0.0f, yaw, 0.0f });
		} else if (std::abs(angularSpeed) > kTransformEpsilon) {
			const float tangentX = baseRadiusX *
				(xFactorDerivative * cosine - xFactor * sine) * angularSpeed;
			const float tangentZ = baseRadiusZ *
				(zFactorDerivative * sine + zFactor * cosine) * angularSpeed;
			const float yaw = std::atan2(tangentX, tangentZ);
			entity->transform.rotate = MakeQuaternionFromEuler({ 0.0f, yaw, 0.0f });
		}
		runtime.previousPosition = finalPosition;
		runtime.hasPreviousPosition = true;
		runtime.hitCooldown = (std::max)(
			runtime.hitCooldown - safeDeltaTime,
		0.0f
		);
	}
}

bool SceneFishingScoreAttackSystem::ResetSharksForRound(
	SceneDocument& document,
	const SceneComponent& director
) {
	const SceneEntity* waterEntity = document.FindEntity(
		director.fishingWaterVolumeEntityId
	);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	const SceneEntity* spawnAreaEntity = document.FindEntity(
		director.fishingHookSpawnAreaEntityId
	);
	const SceneComponent* spawnArea = FindComponent(
		document,
		director.fishingHookSpawnAreaEntityId,
		"FishingHookSpawnArea"
	);
	const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
	if (!waterEntity || !waterVolume || !spawnAreaEntity || !spawnArea || !player) {
		Fault(document, director, "FishingShark spawn references became invalid");
		return false;
	}
	const SharkWaterBounds waterBounds = BuildSharkWaterBounds(
		document, *waterEntity, *waterVolume
	);
	const std::vector<SharkObstacleFootprint> obstacles =
		BuildSharkObstacleFootprints(document);
	const Transform playerTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *player);
	const Transform areaTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *spawnAreaEntity);
	Transform bandWaterTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
	bandWaterTransform.translate.x += waterVolume->waterOffset.x;
	bandWaterTransform.translate.y += waterVolume->waterOffset.y;
	bandWaterTransform.translate.z += waterVolume->waterOffset.z;
	std::uniform_real_distribution<float> xDistribution(
		-spawnArea->fishingSpawnHalfSizeX,
		spawnArea->fishingSpawnHalfSizeX
	);
	std::uniform_real_distribution<float> zDistribution(
		-spawnArea->fishingSpawnHalfSizeZ,
		spawnArea->fishingSpawnHalfSizeZ
	);
	std::uniform_int_distribution<int> bandDistribution(
		1,
		(std::max)(static_cast<int>(director.fishingHookBands.size()) - 1, 1)
	);
	std::uniform_real_distribution<float> headingDistribution(
		-3.14159265358979323846f,
		3.14159265358979323846f
	);
	std::uniform_real_distribution<float> intervalDistribution(0.6f, 1.6f);
	std::vector<Vector3> spawnedSharkPositions;
	std::vector<float> spawnedSharkRadii;
	for (auto& [entityId, runtime] : sharkRuntimes_) {
		SceneEntity* entity = document.FindEntity(entityId);
		if (!entity) {
			continue;
		}
		const SceneComponent* shark = FindEnabledComponent(
			*entity, "FishingShark"
		);
		if (!shark) {
			continue;
		}
		const float wanderMoveSpeed = std::isfinite(
			shark->fishingSharkWanderMoveSpeed
		) ? (std::max)(shark->fishingSharkWanderMoveSpeed, 0.0f) : 0.0f;
		if (wanderMoveSpeed > kTransformEpsilon) {
			const Transform sharkTransform =
				SceneTransformResolver::ResolveScene3DTransform(document, *entity);
			const SceneComponent* sharkCollider = FindEnabledComponent(
				*entity, "OBBCollider"
			);
			if (!sharkCollider) {
				Fault(document, director, "FishingShark requires an OBB Collider");
				return false;
			}
			const float sharkRadius = SharkColliderRadiusXZ(
				*sharkCollider, sharkTransform
			);
			const int selectedBand = bandDistribution(runtime.wanderRandom);
			Vector3 spawnPosition{};
			float spawnHeading = 0.0f;
			bool foundPosition = false;
			for (int attempt = 0; attempt < spawnArea->fishingSpawnMaxAttempts; ++attempt) {
				const Vector3 candidate = ToSpawnWorldPosition(
					areaTransform,
					xDistribution(runtime.wanderRandom),
					zDistribution(runtime.wanderRandom),
					playerTransform.translate.y
				);
				const Vector3 rawWaterLocal = ToLocalXZ(
					bandWaterTransform,
					candidate
				);
				const float normalizedZ = (rawWaterLocal.z + waterVolume->waterHalfSize.z) /
					(2.0f * waterVolume->waterHalfSize.z);
				const float orientedZ = startFromPositiveWaterZ_
					? 1.0f - std::clamp(normalizedZ, 0.0f, 0.99999f)
					: std::clamp(normalizedZ, 0.0f, 0.99999f);
				const int candidateBand = (std::min)(
					static_cast<int>(std::floor(orientedZ * 5.0f)),
					4
				);
				if (candidateBand != selectedBand ||
					!IsPointInsideSharkWater(
						{ candidate.x, candidate.z }, waterBounds, sharkRadius + 0.25f
					) ||
					DistanceXZ(candidate, playerTransform.translate) <
						spawnArea->fishingSpawnMinimumDistance ||
					!IsSharkSegmentClear(
						{ candidate.x, candidate.z },
						{ candidate.x, candidate.z },
						waterBounds, obstacles, sharkRadius
					)) {
					continue;
				}
				bool overlapsShark = false;
				for (size_t index = 0; index < spawnedSharkPositions.size(); ++index) {
					if (DistanceXZ(candidate, spawnedSharkPositions[index]) <
						sharkRadius + spawnedSharkRadii[index] + 0.25f) {
						overlapsShark = true;
						break;
					}
				}
				if (overlapsShark) {
					continue;
				}
				spawnHeading = headingDistribution(runtime.wanderRandom);
				const float lookahead = (std::max)(
					shark->fishingSharkObstacleAvoidanceDistance,
					wanderMoveSpeed * 0.75f
				);
				if (MeasureSharkHeadingClearance(
					{ candidate.x, candidate.z },
					spawnHeading,
					lookahead,
					waterBounds,
					obstacles,
					sharkRadius
				) < lookahead - kTransformEpsilon) {
					continue;
				}
				spawnPosition = candidate;
				foundPosition = true;
				break;
			}
			if (!foundPosition) {
				Fault(document, director, "FishingShark has no valid position in hook bands 2-5");
				return false;
			}
			entity->transform.scale = runtime.initialTransform.scale;
			entity->transform.translate = spawnPosition;
			runtime.wanderHeading = spawnHeading;
			runtime.wanderTargetHeading = spawnHeading;
			runtime.wanderAvoidanceSide = 0;
			runtime.retargetRemainingSeconds = intervalDistribution(
				runtime.wanderRandom
		);
			runtime.hitCooldown = 0.0f;
			runtime.previousPosition = spawnPosition;
			runtime.hasPreviousPosition = true;
			entity->transform.rotate = MakeQuaternionFromEuler({
				0.0f, spawnHeading, 0.0f
			});
			spawnedSharkPositions.push_back(spawnPosition);
			spawnedSharkRadii.push_back(sharkRadius);
			continue;
		}
		runtime.phase = shark->fishingSharkInitialPhase;
		runtime.hitCooldown = 0.0f;
		runtime.avoidanceOffset = {};
		const float pathRandomness = std::isfinite(
			shark->fishingSharkPathRandomness
		) ? std::clamp(shark->fishingSharkPathRandomness, 0.0f, 1.0f) : 0.0f;
		const float cosine = std::cos(runtime.phase);
		const float sine = std::sin(runtime.phase);
		const float xWobbleArgument = runtime.phase * 1.37f + runtime.wobblePhase;
		const float zWobbleArgument = runtime.phase * 0.91f +
			runtime.wobblePhase * 1.61f;
		const float xFactor = 1.0f + pathRandomness * 0.15f *
			std::sin(xWobbleArgument);
		const float zFactor = 1.0f + pathRandomness * 0.15f *
			std::sin(zWobbleArgument);
		const float xFactorDerivative = pathRandomness * 0.15f * 1.37f *
			std::cos(xWobbleArgument);
		const float zFactorDerivative = pathRandomness * 0.15f * 0.91f *
			std::cos(zWobbleArgument);
		const float baseRadiusX = shark->fishingSharkRadiusX * runtime.radiusXScale;
		const float baseRadiusZ = shark->fishingSharkRadiusZ * runtime.radiusZScale;
		const float radiusX = baseRadiusX * xFactor;
		const float radiusZ = baseRadiusZ * zFactor;
		const float angularSpeed = shark->fishingSharkAngularSpeed *
			runtime.angularSpeedScale;
		entity->transform.scale = runtime.initialTransform.scale;
		entity->transform.rotate = MakeQuaternionFromEuler(
			runtime.initialTransform.rotate
		);
		entity->transform.translate = {
			runtime.initialTransform.translate.x + radiusX * cosine,
			runtime.initialTransform.translate.y,
			runtime.initialTransform.translate.z + radiusZ * sine
		};
		runtime.previousPosition = entity->transform.translate;
		runtime.hasPreviousPosition = true;
		if (std::abs(angularSpeed) > kTransformEpsilon) {
			const float tangentX = baseRadiusX *
				(xFactorDerivative * cosine - xFactor * sine) * angularSpeed;
			const float tangentZ = baseRadiusZ *
				(zFactorDerivative * sine + zFactor * cosine) * angularSpeed;
			const float yaw = std::atan2(tangentX, tangentZ);
			entity->transform.rotate = MakeQuaternionFromEuler({ 0.0f, yaw, 0.0f });
		}
	}
	return true;
}

void SceneFishingScoreAttackSystem::Finish(
	SceneDocument& document,
	const SceneComponent& director
) {
	timerRunning_ = false;
	pendingFishCountDelta_ = 0;
	resultInputArmed_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasLastSafePlayerPlanarPosition_ = false;
	lastSafePlayerYaw_ = 0.0f;
	DeactivatePoolHooks(document, director);
	for (uint64_t fishEntityId : director.fishingFishEntityIds) {
		if (SceneEntity* fish = document.FindEntity(fishEntityId)) {
			fish->active = false;
		}
	}
	state_ = SceneFishingScoreAttackState::Result;
	BuildTextRequests(director);
}

void SceneFishingScoreAttackSystem::Fault(
	SceneDocument& document,
	const SceneComponent& director,
	std::string diagnostic
) {
	diagnostic_ = std::move(diagnostic);
	timerRunning_ = false;
	pendingFishCountDelta_ = 0;
	resultInputArmed_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasLastSafePlayerPlanarPosition_ = false;
	lastSafePlayerYaw_ = 0.0f;
	DeactivatePoolHooks(document, director);
	for (uint64_t fishEntityId : director.fishingFishEntityIds) {
		if (SceneEntity* fish = document.FindEntity(fishEntityId)) {
			fish->active = false;
		}
	}
	state_ = SceneFishingScoreAttackState::Faulted;
	BuildTextRequests(director);
}

void SceneFishingScoreAttackSystem::SetFishPreview(
	SceneDocument& document,
	const SceneComponent& director
) {
	for (size_t index = 0; index < director.fishingFishEntityIds.size(); ++index) {
		if (SceneEntity* fish = document.FindEntity(
			director.fishingFishEntityIds[index]
		)) {
			fish->active = static_cast<int>(index) < selectedFishCount_;
		}
	}
}

void SceneFishingScoreAttackSystem::DeactivatePoolHooks(
	SceneDocument& document,
	const SceneComponent& director
) {
	const SceneComponent* pool = FindComponent(
		document,
		director.fishingHookPoolEntityId,
		"FishingHookPool"
	);
	if (pool) {
		for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
			if (SceneEntity* hook = document.FindEntity(entry.hookEntityId)) {
				hook->active = false;
			}
		}
	}
	activeHooks_.clear();
}

void SceneFishingScoreAttackSystem::BuildTextRequests(
	const SceneComponent& director
) {
	textRequests_.clear();
	iconRequests_.clear();
	const auto addText = [this](uint64_t entityId, std::string text) {
		if (entityId != 0) {
			textRequests_.push_back({ entityId, std::move(text) });
		}
	};
	addText(
		director.fishingFishCountTextEntityId,
		director.fishingFishCountPrefix + std::to_string(selectedFishCount_)
	);
	const double remainingSeconds = (std::max)(
		static_cast<double>(director.fishingDurationSeconds) - elapsedSeconds_,
		0.0
	);
	addText(
		director.fishingTimerTextEntityId,
		director.fishingTimerPrefix + FormatOneDecimal(
			static_cast<float>(remainingSeconds)
		)
	);
	addText(
		director.fishingScoreTextEntityId,
		director.fishingScorePrefix + std::to_string(totalScore_)
	);
	addText(
		director.fishingMultiplierTextEntityId,
		state_ == SceneFishingScoreAttackState::Navigating
			? director.fishingMultiplierPrefix +
				FormatOneDecimal(director.fishingDistanceMultiplierBase) + "x - " +
				FormatOneDecimal(
					director.fishingDistanceMultiplierBase +
					director.fishingDistanceMultiplierStep *
						static_cast<float>(director.fishingDistanceBandCount - 1)
				) + "x"
			: std::string{}
	);
	addText(
		director.fishingResultTextEntityId,
		state_ == SceneFishingScoreAttackState::Result
			? director.fishingResultPrefix + std::to_string(totalScore_)
			: std::string{}
	);
	const bool showLegend =
		director.fishingUseHookBandSettings &&
		director.fishingHookLegendVisible &&
		(state_ == SceneFishingScoreAttackState::SelectingInitial ||
			state_ == SceneFishingScoreAttackState::Navigating ||
			state_ == SceneFishingScoreAttackState::SelectingNext) &&
		director.fishingHookRanks.size() == 10;
	if (director.fishingUseHookBandSettings) {
		if (director.fishingHookLegendTitleTextEntityId != 0) {
			textRequests_.push_back({
				director.fishingHookLegendTitleTextEntityId,
				showLegend ? director.fishingHookLegendTitle : std::string{},
				false,
				{}
			});
		}
		for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
			const uint64_t entityId = tierIndex < director.fishingHookLegendTextEntityIds.size()
				? director.fishingHookLegendTextEntityIds[tierIndex]
				: 0;
			if (entityId == 0) {
				continue;
			}
			textRequests_.push_back({
				entityId,
				showLegend
					? director.fishingHookLegendPrefix + FormatHookScoreMultiplier(
						director.fishingHookRanks[tierIndex].scoreMultiplier
					)
					: std::string{},
				showLegend,
				showLegend
					? director.fishingHookRanks[tierIndex].color
					: Vector4{}
			});
		}
	}
	for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
		const uint64_t iconEntityId = tierIndex < director.fishingHookLegendIconEntityIds.size()
			? director.fishingHookLegendIconEntityIds[tierIndex]
			: 0;
		if (iconEntityId == 0) {
			continue;
		}
		const bool rankReady = director.fishingHookRanks.size() == 10;
		const std::string texturePath = rankReady
			? director.fishingHookRanks[tierIndex].iconTexturePath
			: std::string{};
		iconRequests_.push_back({
			iconEntityId,
			texturePath,
			director.fishingHookLegendIconSize,
			showLegend && !texturePath.empty()
		});
	}
}

void SceneFishingScoreAttackSystem::Clear() {
	state_ = SceneFishingScoreAttackState::Inactive;
	directorEntityId_ = 0;
	resultInputArmed_ = false;
	activeHooks_.clear();
	hookVisualModelPaths_.clear();
	sharkRuntimes_.clear();
	initialPlayerTransform_ = {};
	playerWaterBounds_ = {};
	hasInitialPlayerTransform_ = false;
	hasPlayerWaterBounds_ = false;
	lastSafePlayerPlanarPosition_ = {};
	lastSafePlayerYaw_ = 0.0f;
	hasLastSafePlayerPlanarPosition_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasPlayerResetRequest_ = false;
	startFromPositiveWaterZ_ = false;
	initialFishEntityIds_.clear();
	initialFishTransforms_.clear();
	fishingTeamName_.clear();
	selectedFishCount_ = 0;
	pendingFishCountDelta_ = 0;
	roundFishCount_ = 0;
	roundDistanceBand_ = 0;
	roundMultiplier_ = 0.0f;
	elapsedSeconds_ = 0.0;
	totalScore_ = 0;
	timerRunning_ = false;
	hasDirector_ = false;
	diagnostic_.clear();
	textRequests_.clear();
	iconRequests_.clear();
}
