// 役割: Fishing Score AttackのComponent設定をRuntime stateへ展開する。
#include "SceneFishingScoreAttackSystem.h"

#include "SceneAgentSystem.h"
#include "FishingFormationMotion.h"
#include "SceneSoundEffectPlayer.h"
#include "../SceneRuntimeInput.h"

#include "../../../engine/collision/Collider.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/collision/SphereCollider.h"
#include "../../../engine/debug/DebugRenderer.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/3d/ModelManager.h"
#include "../../../engine/particle/ParticleManager.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneInputKey.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>

#if defined(_DEBUG) || defined(DEVELOPMENT)
#include "../../../externals/imgui/imgui.h"
#include "../../../externals/imgui/imgui_internal.h"
#endif

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	constexpr float kTransformEpsilon = 0.0001f;
	constexpr float kTwoPi = 6.28318530717958647692f;
	constexpr float kFormationRecoveryYawProgress = kTwoPi / 360.0f;
	constexpr float kFormationRecoveryNoProgressSeconds = 0.5f;
	constexpr size_t kFormationRecoveryPoseCapacity = 8;
	constexpr float kTutorialMovePracticeRequiredSeconds = 0.75f; // 移動練習完了に必要な移動秒数。
	constexpr float kTutorialMoveInputSpeedThreshold = 0.25f; // 移動入力とみなす最低速度。
	constexpr int kTutorialMultiScoreRequiredCount = 3; // 複数得点練習で必要な得点回数。
	constexpr int kTutorialMinimumHookDistanceBand = 3; // チュートリアル中に釣り針を出し始める遠距離帯。
	constexpr float kHookDropHeight = 8.0f; // 釣り針を出現させる上方距離。
	constexpr float kHookDropDurationSeconds = 0.65f; // 釣り針が水面位置へ到達する時間。
	constexpr const char* kTutorialMessageTextEntityName =
		"Fishing Tutorial Message"; // チュートリアル説明専用Text Entity名。

	/// <summary>
	/// チュートリアル説明送り入力が押されたかを判定する。
	/// </summary>
	bool IsTutorialAdvanceInputTriggered() {
		const bool keyboardAdvance =
			SceneRuntimeInput::IsTriggered("ENTER", "Pressed"); // キーボードの説明送り入力。
		const bool gamepadAdvance =
			SceneRuntimeInput::IsTriggered("Gamepad Y", "Pressed"); // ゲームパッドの説明送り入力。
		return keyboardAdvance || gamepadAdvance;
	}

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

	const SceneComponent* FindResultTracker(
		const SceneDocument& document,
		uint64_t directorEntityId
	) {
		const SceneEntity* entity = document.FindEntity(directorEntityId);
		return entity
			? FindEnabledComponent(*entity, "FishingResultTracker")
			: nullptr;
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
		const int activeRankCount = std::clamp(
			director.fishingHookRankCount, 1, 10
		);
		const size_t index = static_cast<size_t>(std::clamp(
			tier, 1, activeRankCount
		) - 1);
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

	/// <summary>
	/// チュートリアルの得点練習中かを判定する。
	/// </summary>
	bool IsTutorialScorePracticeStep(SceneFishingScoreAttackTutorialStep step) {
		switch (step) {
		case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
		case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
		case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
			return true;
		default:
			return false;
		}
	}

	/// <summary>
	/// チュートリアル中に正の得点ランクだけへ絞るかを判定する。
	/// </summary>
	bool RequiresPositiveTutorialHookRank(SceneFishingScoreAttackTutorialStep step) {
		return step != SceneFishingScoreAttackTutorialStep::Disabled &&
			step != SceneFishingScoreAttackTutorialStep::FreePlay;
	}

	/// <summary>
	/// 指定された釣り針ランクが正の得点を持つかを判定する。
	/// </summary>
	bool HasPositiveHookScoreRank(
		const SceneComponent& director,
		size_t tierIndex
	) {
		const SceneFishingHookRankDefinition rank = ResolveFishingHookRank(
			director,
			static_cast<int>(tierIndex) + 1
		); // 判定対象の釣り針ランク。
		return std::isfinite(rank.scoreMultiplier) &&
			rank.scoreMultiplier > 0.0f;
	}

	/// <summary>
	/// チュートリアル上でサメを表示してよい段階かを判定する。
	/// </summary>
	bool IsTutorialSharkVisibleStep(SceneFishingScoreAttackTutorialStep step) {
		return step == SceneFishingScoreAttackTutorialStep::Disabled ||
			step == SceneFishingScoreAttackTutorialStep::SharkExplanation ||
			step == SceneFishingScoreAttackTutorialStep::FreePlay;
	}

	/// <summary>
	/// Scene上のサメEntityを非表示にする。
	/// </summary>
	void DeactivateSceneSharks(SceneDocument& document) {
		for (SceneEntity& entity : document.GetEntities()) {
			if (FindEnabledComponent(entity, "FishingShark")) {
				entity.active = false;
			}
		}
	}

	/// <summary>
	/// サメのスポーン候補帯を優先帯から順に構築する。
	/// </summary>
	std::vector<int> BuildSharkSpawnBandOrder(int preferredBand, int maxBand) {
		const int clampedMaxBand = (std::max)(maxBand, 1); // 使用できる最大距離帯。
		const int clampedPreferredBand = std::clamp(
			preferredBand,
			1,
			clampedMaxBand
		); // 最初に試す距離帯。
		std::vector<int> bandOrder;
		bandOrder.reserve(static_cast<size_t>(clampedMaxBand));
		bandOrder.push_back(clampedPreferredBand);
		for (int band = 1; band <= clampedMaxBand; ++band) {
			if (band != clampedPreferredBand) {
				bandOrder.push_back(band);
			}
		}
		return bandOrder;
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

	// Spawn判定はRuntime Colliderと同じワールド倍率を使う。
	// これを省くと、Play開始時にランダム倍率が掛かった岩へ釣り針が重なる。
	float ColliderRadiusXZ(
		const SceneComponent& collider,
		const Transform& transform
	) {
		const float sizeX = std::abs(collider.colliderSizeMultiplier.x) *
			(std::max)(std::abs(transform.scale.x), 0.001f);
		const float sizeZ = std::abs(collider.colliderSizeMultiplier.z) *
			(std::max)(std::abs(transform.scale.z), 0.001f);
		return std::sqrt(sizeX * sizeX + sizeZ * sizeZ);
	}

	Vector3 ColliderCenterXZ(
		const Transform& transform,
		const SceneComponent& collider
	) {
		const float scaleX = (std::max)(std::abs(transform.scale.x), 0.001f);
		const float scaleY = (std::max)(std::abs(transform.scale.y), 0.001f);
		const float scaleZ = (std::max)(std::abs(transform.scale.z), 0.001f);
		return ToSpawnWorldPosition(
			transform,
			collider.colliderOffset.x * scaleX,
			collider.colliderOffset.z * scaleZ,
			transform.translate.y + collider.colliderOffset.y * scaleY
		);
	}

	// 岩はPlay開始時にモデルがランダムに差し替わるため、各プロファイルを
	// 包含する半径を使う。Entity原点から判定すれば、差し替え後のoffset差も
	// 含めて釣り針との重なりを防げる。
	float FishingObstacleSpawnExclusionRadius(
		const SceneDocument& document,
		const SceneComponent& collider,
		const Transform& transform
	) {
		const float scaleX = (std::max)(std::abs(transform.scale.x), 0.001f);
		const float scaleZ = (std::max)(std::abs(transform.scale.z), 0.001f);
		const float fallbackRadius = ColliderRadiusXZ(collider, transform) +
			std::sqrt(
				collider.colliderOffset.x * collider.colliderOffset.x * scaleX * scaleX +
				collider.colliderOffset.z * collider.colliderOffset.z * scaleZ * scaleZ
			);
		float exclusionRadius = fallbackRadius;
		for (const SceneFishingObstacleColliderProfile& profile :
			document.GetFishingObstacleSettings().colliderProfiles) {
			if (!profile.enabled ||
				!std::isfinite(profile.colliderOffset.x) ||
				!std::isfinite(profile.colliderOffset.z) ||
				!std::isfinite(profile.colliderSizeMultiplier.x) ||
				!std::isfinite(profile.colliderSizeMultiplier.z)) {
				continue;
			}
			const float offsetRadius = std::sqrt(
				profile.colliderOffset.x * profile.colliderOffset.x * scaleX * scaleX +
				profile.colliderOffset.z * profile.colliderOffset.z * scaleZ * scaleZ
			);
			const float colliderRadius = std::sqrt(
				profile.colliderSizeMultiplier.x * profile.colliderSizeMultiplier.x * scaleX * scaleX +
				profile.colliderSizeMultiplier.z * profile.colliderSizeMultiplier.z * scaleZ * scaleZ
			);
			exclusionRadius = (std::max)(
				exclusionRadius, offsetRadius + colliderRadius
			);
		}
		return exclusionRadius;
	}

	struct FishingObstacleSphereGeometry {
		Vector3 center{};
		float radius = 0.0f;
	};

	float WorldAxisScale(const Matrix4x4& matrix, uint32_t axis) {
		return Math::Length({
			matrix.m[axis][0], matrix.m[axis][1], matrix.m[axis][2]
		});
	}

	bool BuildFishingObstacleSphereGeometry(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& collider,
		FishingObstacleSphereGeometry& geometry
	) {
		const Matrix4x4 world =
			SceneTransformResolver::ResolveSceneWorldMatrix(document, entity);
		geometry.center = {
			collider.colliderOffset.x * world.m[0][0] +
			collider.colliderOffset.y * world.m[1][0] +
			collider.colliderOffset.z * world.m[2][0] + world.m[3][0],
			collider.colliderOffset.x * world.m[0][1] +
			collider.colliderOffset.y * world.m[1][1] +
			collider.colliderOffset.z * world.m[2][1] + world.m[3][1],
			collider.colliderOffset.x * world.m[0][2] +
			collider.colliderOffset.y * world.m[1][2] +
			collider.colliderOffset.z * world.m[2][2] + world.m[3][2]
		};
		const float scaleX = WorldAxisScale(world, 0);
		const float scaleY = WorldAxisScale(world, 1);
		const float scaleZ = WorldAxisScale(world, 2);
		const float maxScale = (std::max)(scaleX, (std::max)(scaleY, scaleZ));
		geometry.radius = collider.colliderSphereRadius * maxScale;
		return std::isfinite(geometry.center.x) &&
			std::isfinite(geometry.center.y) &&
			std::isfinite(geometry.center.z) &&
			std::isfinite(geometry.radius) && geometry.radius > 0.0f;
	}

	bool IsSphereFishingObstacle(const SceneComponent& collider) {
		return collider.colliderShape == "Sphere";
	}

	struct XZPoint {
		float x = 0.0f;
		float z = 0.0f;
	};

	struct SharkObstacleFootprint {
		Vector3 center{};
		bool isCircle = false;
		float radius = 0.0f;
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
		SharkObstacleFootprint result{};
		result.axisXCosine = cosine;
		result.axisXSine = sine;
		if (IsSphereFishingObstacle(collider)) {
			FishingObstacleSphereGeometry sphere{};
			if (!BuildFishingObstacleSphereGeometry(document, entity, collider, sphere)) {
				return result;
			}
			result.center = sphere.center;
			result.isCircle = true;
			result.radius = sphere.radius;
			return result;
		}
		result.center = ToSpawnWorldPosition(
			transform,
			collider.colliderOffset.x * scaleX,
			collider.colliderOffset.z * scaleZ,
			transform.translate.y + collider.colliderOffset.y
		);
		result.halfSizeX = std::abs(collider.colliderSizeMultiplier.x) * scaleX;
		result.halfSizeZ = std::abs(collider.colliderSizeMultiplier.z) * scaleZ;
		return result;
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
			SharkObstacleFootprint footprint =
				BuildSharkObstacleFootprint(document, entity, *collider);
			if (IsSphereFishingObstacle(*collider) && footprint.radius <= 0.0f) {
				continue;
			}
			obstacles.push_back(std::move(footprint));
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
		if (obstacle.isCircle) {
			const float deltaX = localEnd.x - localStart.x;
			const float deltaZ = localEnd.z - localStart.z;
			const float lengthSquared = deltaX * deltaX + deltaZ * deltaZ;
			const float parameter = lengthSquared > kTransformEpsilon
				? std::clamp(
					-(localStart.x * deltaX + localStart.z * deltaZ) /
					lengthSquared,
					0.0f,
					1.0f
				)
				: 0.0f;
			const float closestX = localStart.x + deltaX * parameter;
			const float closestZ = localStart.z + deltaZ * parameter;
			const float inflatedRadius = obstacle.radius + inflation;
			return closestX * closestX + closestZ * closestZ <=
				inflatedRadius * inflatedRadius;
		}
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
		float sharkRadius,
		float additionalClearance = 0.0f
	) {
		const float safetyMargin = 0.25f + (std::max)(additionalClearance, 0.0f);
		if (!IsPointInsideSharkWater(end, water, sharkRadius + safetyMargin)) {
			return false;
		}
		for (const SharkObstacleFootprint& obstacle : obstacles) {
			if (SegmentIntersectsSharkObstacle(
				start, end, obstacle, sharkRadius + safetyMargin
			)) {
				return false;
			}
		}
		return true;
	}

	Vector3 SharkLocalToWorld(
		const SharkWaterBounds& water,
		float localX,
		float localZ
	) {
		return {
			water.center.x + localX * water.axisXCosine + localZ * water.axisXSine,
			water.center.y,
			water.center.z - localX * water.axisXSine + localZ * water.axisXCosine
		};
	}

	struct SharkGridSearchNode {
		int index = -1;
		float priority = 0.0f;
		bool operator<(const SharkGridSearchNode& right) const {
			return priority > right.priority;
		}
	};

	struct SharkNavigationGrid {
		int width = 0;
		int height = 0;
		float minX = 0.0f;
		float minZ = 0.0f;
		float cellSize = 0.0f;
		std::vector<uint8_t> blocked;
	};

	int SharkGridIndex(int x, int z, int width) {
		return z * width + x;
	}

	Vector3 SharkGridCellWorldPoint(
		const SharkWaterBounds& water,
		const SharkNavigationGrid& grid,
		int index
	) {
		const int x = index % grid.width;
		const int z = index / grid.width;
		return SharkLocalToWorld(
			water,
			grid.minX + (static_cast<float>(x) + 0.5f) * grid.cellSize,
			grid.minZ + (static_cast<float>(z) + 0.5f) * grid.cellSize
		);
	}

	int NearestOpenSharkGridCell(
		const SharkNavigationGrid& grid,
		const Vector3& worldPosition,
		const SharkWaterBounds& water,
		const std::vector<uint8_t>* allowedCells = nullptr
	) {
		if (grid.width <= 0 || grid.height <= 0) {
			return -1;
		}
		const XZPoint local = ToSharkLocalXZ(
			worldPosition, water.center, water.axisXCosine, water.axisXSine
		);
		const int preferredX = std::clamp(
			static_cast<int>(std::floor((local.x - grid.minX) / grid.cellSize)),
			0,
			grid.width - 1
		);
		const int preferredZ = std::clamp(
			static_cast<int>(std::floor((local.z - grid.minZ) / grid.cellSize)),
			0,
			grid.height - 1
		);
		int bestIndex = -1;
		float bestDistance = (std::numeric_limits<float>::max)();
		for (int z = 0; z < grid.height; ++z) {
			for (int x = 0; x < grid.width; ++x) {
				const int index = SharkGridIndex(x, z, grid.width);
				if (grid.blocked[static_cast<size_t>(index)] != 0 ||
					(allowedCells && (
						static_cast<size_t>(index) >= allowedCells->size() ||
						(*allowedCells)[static_cast<size_t>(index)] == 0
					))) {
					continue;
				}
				const float dx = static_cast<float>(x - preferredX);
				const float dz = static_cast<float>(z - preferredZ);
				const float distance = dx * dx + dz * dz;
				if (distance < bestDistance) {
					bestDistance = distance;
					bestIndex = index;
				}
			}
		}
		return bestIndex;
	}

	bool BuildSharkNavigationRoute(
		const SharkWaterBounds& water,
		const std::vector<SharkObstacleFootprint>& obstacles,
		const SceneComponent& shark,
		float sharkRadius,
		const Vector3& start,
		const Vector3* chaseTarget,
		std::mt19937& random,
		std::vector<Vector3>& route,
		std::vector<int>& patrolVisitCounts,
		int& patrolGridWidth,
		int& patrolGridHeight,
		float& patrolGridOriginX,
		float& patrolGridOriginZ,
		float& patrolGridCellSize
	) {
		std::vector<Vector3> nextRoute;
		const float clearance = std::isfinite(shark.fishingSharkObstacleClearance)
			? (std::max)(shark.fishingSharkObstacleClearance, 0.0f)
			: 0.0f;
		const float requestedCellSize = std::isfinite(
			shark.fishingSharkNavigationCellSize
		) ? (std::max)(shark.fishingSharkNavigationCellSize, 0.25f) : 4.0f;
		const float margin = sharkRadius + 0.25f + clearance;
		const float availableX = water.halfSizeX * 2.0f - margin * 2.0f;
		const float availableZ = water.halfSizeZ * 2.0f - margin * 2.0f;
		if (availableX <= requestedCellSize || availableZ <= requestedCellSize) {
			return false;
		}
		SharkNavigationGrid grid{};
		grid.width = std::clamp(
			static_cast<int>(std::floor(availableX / requestedCellSize)),
			1,
			256
		);
		grid.height = std::clamp(
			static_cast<int>(std::floor(availableZ / requestedCellSize)),
			1,
			256
		);
		grid.minX = -water.halfSizeX + margin;
		grid.minZ = -water.halfSizeZ + margin;
		grid.cellSize = requestedCellSize;
		grid.blocked.assign(
			static_cast<size_t>(grid.width * grid.height), 0
		);
		for (int z = 0; z < grid.height; ++z) {
			for (int x = 0; x < grid.width; ++x) {
				const int index = SharkGridIndex(x, z, grid.width);
				const Vector3 point = SharkGridCellWorldPoint(water, grid, index);
				if (!IsSharkSegmentClear(
					{ point.x, point.z },
					{ point.x, point.z },
					water,
					obstacles,
					sharkRadius,
					clearance
				)) {
					grid.blocked[static_cast<size_t>(index)] = 1;
				}
			}
		}
		const int startIndex = NearestOpenSharkGridCell(grid, start, water);
		if (startIndex < 0) {
			return false;
		}
		const int directions[8][2] = {
			{ -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
			{ 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 }
		};
		std::vector<uint8_t> reachable(grid.blocked.size(), 0);
		std::queue<int> reachabilityQueue;
		reachable[static_cast<size_t>(startIndex)] = 1;
		reachabilityQueue.push(startIndex);
		while (!reachabilityQueue.empty()) {
			const int current = reachabilityQueue.front();
			reachabilityQueue.pop();
			const int currentX = current % grid.width;
			const int currentZ = current / grid.width;
			for (const auto& direction : directions) {
				const int nextX = currentX + direction[0];
				const int nextZ = currentZ + direction[1];
				if (nextX < 0 || nextX >= grid.width ||
					nextZ < 0 || nextZ >= grid.height) {
					continue;
				}
				const int next = SharkGridIndex(nextX, nextZ, grid.width);
				if (grid.blocked[static_cast<size_t>(next)] != 0 ||
					reachable[static_cast<size_t>(next)] != 0) {
					continue;
				}
				if (direction[0] != 0 && direction[1] != 0) {
					const int sideA = SharkGridIndex(
						currentX + direction[0], currentZ, grid.width
					);
					const int sideB = SharkGridIndex(
						currentX, currentZ + direction[1], grid.width
					);
					if (grid.blocked[static_cast<size_t>(sideA)] != 0 ||
						grid.blocked[static_cast<size_t>(sideB)] != 0) {
						continue;
					}
				}
				reachable[static_cast<size_t>(next)] = 1;
				reachabilityQueue.push(next);
			}
		}
		if (patrolGridWidth != grid.width ||
			patrolGridHeight != grid.height ||
			std::abs(patrolGridOriginX - grid.minX) > kTransformEpsilon ||
			std::abs(patrolGridOriginZ - grid.minZ) > kTransformEpsilon ||
			std::abs(patrolGridCellSize - grid.cellSize) > kTransformEpsilon) {
			patrolVisitCounts.assign(
				static_cast<size_t>(grid.width * grid.height), 0
			);
		}
		patrolGridWidth = grid.width;
		patrolGridHeight = grid.height;
		patrolGridOriginX = grid.minX;
		patrolGridOriginZ = grid.minZ;
		patrolGridCellSize = grid.cellSize;
		int targetIndex = -1;
		if (chaseTarget) {
			targetIndex = NearestOpenSharkGridCell(
				grid, *chaseTarget, water, &reachable
			);
		} else {
			int minimumVisits = (std::numeric_limits<int>::max)();
			std::vector<int> candidates;
			const float minimumDistance = std::sqrt(
				water.halfSizeX * water.halfSizeX +
				water.halfSizeZ * water.halfSizeZ
			) * 0.35f;
			for (int index = 0; index < grid.width * grid.height; ++index) {
				if (grid.blocked[static_cast<size_t>(index)] != 0 ||
					reachable[static_cast<size_t>(index)] == 0) {
					continue;
				}
				const Vector3 point = SharkGridCellWorldPoint(water, grid, index);
				if (DistanceXZ(point, start) < minimumDistance) {
					continue;
				}
				const int visits = patrolVisitCounts[static_cast<size_t>(index)];
				minimumVisits = (std::min)(minimumVisits, visits);
			}
			if (minimumVisits == (std::numeric_limits<int>::max)()) {
				for (int index = 0; index < grid.width * grid.height; ++index) {
					if (grid.blocked[static_cast<size_t>(index)] == 0 &&
						reachable[static_cast<size_t>(index)] != 0) {
						minimumVisits = (std::min)(
							minimumVisits,
							patrolVisitCounts[static_cast<size_t>(index)]
						);
					}
				}
			}
			const int visitWindow = static_cast<int>(std::ceil(
				std::clamp(shark.fishingSharkPathRandomness, 0.0f, 1.0f) * 2.0f
			));
			for (int index = 0; index < grid.width * grid.height; ++index) {
				if (grid.blocked[static_cast<size_t>(index)] != 0 ||
					reachable[static_cast<size_t>(index)] == 0 ||
					patrolVisitCounts[static_cast<size_t>(index)] > minimumVisits + visitWindow) {
					continue;
				}
				const Vector3 point = SharkGridCellWorldPoint(water, grid, index);
				if (minimumVisits == 0 && DistanceXZ(point, start) < minimumDistance) {
					continue;
				}
				candidates.push_back(index);
			}
			if (candidates.empty()) {
				for (int index = 0; index < grid.width * grid.height; ++index) {
					if (grid.blocked[static_cast<size_t>(index)] == 0 &&
						reachable[static_cast<size_t>(index)] != 0) {
						candidates.push_back(index);
					}
				}
			}
			std::sort(candidates.begin(), candidates.end(), [&](int left, int right) {
				return DistanceXZ(
					SharkGridCellWorldPoint(water, grid, left), start
				) > DistanceXZ(
					SharkGridCellWorldPoint(water, grid, right), start
				);
			});
			if (!candidates.empty()) {
				const float randomness = std::clamp(
					shark.fishingSharkPathRandomness, 0.0f, 1.0f
				);
				const size_t candidateCount = randomness > kTransformEpsilon
					? (std::max)(size_t{ 1 }, static_cast<size_t>(std::ceil(
						static_cast<float>(candidates.size()) * 0.25f
					)))
					: size_t{ 1 };
				std::uniform_int_distribution<size_t> distribution(
					0, candidateCount - 1
				);
				targetIndex = candidates[distribution(random)];
			}
		}
		if (targetIndex < 0 || targetIndex == startIndex) {
			return false;
		}
		std::vector<float> costs(static_cast<size_t>(grid.width * grid.height),
			(std::numeric_limits<float>::max)());
		std::vector<int> parents(costs.size(), -1);
		std::priority_queue<SharkGridSearchNode> open;
		costs[static_cast<size_t>(startIndex)] = 0.0f;
		open.push({ startIndex, 0.0f });
		while (!open.empty()) {
			const int current = open.top().index;
			open.pop();
			if (current == targetIndex) {
				break;
			}
			const int currentX = current % grid.width;
			const int currentZ = current / grid.width;
			for (const auto& direction : directions) {
				const int nextX = currentX + direction[0];
				const int nextZ = currentZ + direction[1];
				if (nextX < 0 || nextX >= grid.width || nextZ < 0 || nextZ >= grid.height) {
					continue;
				}
				const int next = SharkGridIndex(nextX, nextZ, grid.width);
				if (grid.blocked[static_cast<size_t>(next)] != 0) {
					continue;
				}
				if (direction[0] != 0 && direction[1] != 0) {
					const int sideA = SharkGridIndex(currentX + direction[0], currentZ, grid.width);
					const int sideB = SharkGridIndex(currentX, currentZ + direction[1], grid.width);
					if (grid.blocked[static_cast<size_t>(sideA)] != 0 ||
						grid.blocked[static_cast<size_t>(sideB)] != 0) {
						continue;
					}
				}
				const float stepCost = direction[0] != 0 && direction[1] != 0
					? 1.41421356237f : 1.0f;
				const float nextCost = costs[static_cast<size_t>(current)] + stepCost;
				if (nextCost >= costs[static_cast<size_t>(next)]) {
					continue;
				}
				costs[static_cast<size_t>(next)] = nextCost;
				parents[static_cast<size_t>(next)] = current;
				const int targetX = targetIndex % grid.width;
				const int targetZ = targetIndex / grid.width;
				const float dx = static_cast<float>(targetX - nextX);
				const float dz = static_cast<float>(targetZ - nextZ);
				open.push({ next, nextCost + std::sqrt(dx * dx + dz * dz) });
			}
		}
		if (targetIndex != startIndex && parents[static_cast<size_t>(targetIndex)] < 0) {
			return false;
		}
		std::vector<Vector3> rawPath;
		for (int current = targetIndex; current >= 0; current = parents[static_cast<size_t>(current)]) {
			rawPath.push_back(SharkGridCellWorldPoint(water, grid, current));
			if (current == startIndex) {
				break;
			}
		}
		std::reverse(rawPath.begin(), rawPath.end());
		Vector3 anchor = start;
		for (size_t index = 1; index < rawPath.size(); ++index) {
			size_t furthest = index;
			for (size_t candidate = index; candidate < rawPath.size(); ++candidate) {
				if (!IsSharkSegmentClear(
					{ anchor.x, anchor.z },
					{ rawPath[candidate].x, rawPath[candidate].z },
					water, obstacles, sharkRadius, clearance
				)) {
					break;
				}
				furthest = candidate;
			}
			nextRoute.push_back(rawPath[furthest]);
			anchor = rawPath[furthest];
			index = furthest;
		}
		if (nextRoute.empty()) {
			return false;
		}
		route = std::move(nextRoute);
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
		float sharkRadius,
		float additionalClearance = 0.0f
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
				start, end, water, obstacles, sharkRadius, additionalClearance
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
		uint32_t activeMemberCount = 0;
	};

	std::vector<XZPoint> BuildFormationOutlinePoints(
		float radius,
		float halfSegmentLength,
		int outlineSegments
	) {
		const int arcSegments = (std::max)(6, outlineSegments / 2);
		std::vector<XZPoint> points;
		points.reserve(static_cast<size_t>(2 + arcSegments * 2));
		points.push_back({ -radius, -halfSegmentLength });
		points.push_back({ -radius, halfSegmentLength });
		for (int index = 1; index <= arcSegments; ++index) {
			const float angle = 3.14159265358979323846f *
				(1.0f - static_cast<float>(index) / static_cast<float>(arcSegments));
			points.push_back({
				radius * std::cos(angle),
				halfSegmentLength + radius * std::sin(angle)
			});
		}
		points.push_back({ radius, -halfSegmentLength });
		for (int index = 1; index < arcSegments; ++index) {
			const float angle = -3.14159265358979323846f *
				static_cast<float>(index) / static_cast<float>(arcSegments);
			points.push_back({
				radius * std::cos(angle),
				-halfSegmentLength + radius * std::sin(angle)
			});
		}
		return points;
	}

	std::vector<XZPoint> BuildFormationParticlePoints(
		float radius,
		float halfSegmentLength,
		int sampleCount
	) {
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kLengthEpsilon = 0.000001f;
		if (
			!std::isfinite(radius) ||
			!std::isfinite(halfSegmentLength) ||
			radius <= kLengthEpsilon ||
			halfSegmentLength < 0.0f
		) {
			return {};
		}
		const int clampedSampleCount = std::clamp(sampleCount, 12, 128);
		const float straightLength = 2.0f * halfSegmentLength;
		const float arcLength = kPi * radius;
		const float totalLength =
			2.0f * straightLength + 2.0f * arcLength;
		if (!std::isfinite(totalLength) || totalLength <= kLengthEpsilon) {
			return {};
		}
		std::vector<XZPoint> points;
		points.reserve(static_cast<size_t>(clampedSampleCount));
		for (int index = 0; index < clampedSampleCount; ++index) {
			const float distance = totalLength *
				static_cast<float>(index) /
				static_cast<float>(clampedSampleCount);
			if (distance < straightLength || arcLength <= kLengthEpsilon) {
				const float ratio = straightLength > kLengthEpsilon
					? distance / straightLength
					: 0.0f;
				points.push_back({
					-radius,
					-halfSegmentLength +
						ratio * 2.0f * halfSegmentLength
				});
				continue;
			}
			const float topArcEnd = straightLength + arcLength;
			if (distance < topArcEnd) {
				const float angle = kPi -
					(distance - straightLength) / radius;
				points.push_back({
					radius * std::cos(angle),
					halfSegmentLength + radius * std::sin(angle)
				});
				continue;
			}
			const float rightLineEnd = topArcEnd + straightLength;
			if (distance < rightLineEnd) {
				const float ratio = straightLength > kLengthEpsilon
					? (distance - topArcEnd) / straightLength
					: 0.0f;
				points.push_back({
					radius,
					halfSegmentLength -
					ratio * 2.0f * halfSegmentLength
				});
				continue;
			}
			const float angle = -
				(distance - rightLineEnd) / radius;
			points.push_back({
				radius * std::cos(angle),
				-halfSegmentLength + radius * std::sin(angle)
			});
		}
		return points;
	}

	ParticleManager::ParticleBehavior BuildFormationParticleBehavior(
		float startSize,
		float endSize,
		float lifetime,
		const Vector4& startColor,
		const Vector4& endColor,
		float emissiveIntensity
	) {
		ParticleManager::ParticleBehavior behavior{};
		behavior.life.lifeTimeMin = lifetime;
		behavior.life.lifeTimeMax = lifetime;
		behavior.life.enableLifeFade = true;
		behavior.life.fadeOutStartRatio = 0.45f;
		behavior.scale.startScaleMin = { startSize, startSize, startSize };
		behavior.scale.startScaleMax = { startSize, startSize, startSize };
		behavior.scale.enableScaleOverLife = true;
		behavior.scale.endScaleMin = { endSize, endSize, endSize };
		behavior.scale.endScaleMax = { endSize, endSize, endSize };
		behavior.motion.linear.baseVelocity = { 0.0f, 0.08f, 0.0f };
		behavior.motion.linear.velocityRandomRange = { 0.05f, 0.04f, 0.05f };
		behavior.motion.linear.enableAcceleration = false;
		behavior.motion.sway.amplitude = 0.04f;
		behavior.motion.sway.frequency = 2.0f;
		behavior.color.mode = ParticleManager::ColorChangeMode::kOverLife;
		behavior.color.startColorMin = startColor;
		behavior.color.startColorMax = startColor;
		behavior.color.endColorMin = endColor;
		behavior.color.endColorMax = endColor;
		behavior.render.billboardMode = ParticleManager::BillboardMode::kBillboard;
		behavior.render.primitiveType = ParticleManager::PrimitiveType::kPlane;
		behavior.render.depthTest = true;
		behavior.render.depthWrite = false;
		behavior.render.emissiveIntensity = emissiveIntensity;
		return behavior;
	}

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
		capsule.activeMemberCount = state.activeMemberCount;
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
				return binding.entityId == entityId;
			}
		);
		return found == bindings.end() ? nullptr : &(*found);
	}

	XZPoint ToWaterLocalPoint(
		const SceneFishingScoreAttackPlayerWaterBounds& bounds,
		const XZPoint& worldPoint
	) {
		const float cosine = std::cos(bounds.yaw);
		const float sine = std::sin(bounds.yaw);
		const float deltaX = worldPoint.x - bounds.center.x;
		const float deltaZ = worldPoint.z - bounds.center.z;
		return {
			deltaX * cosine - deltaZ * sine,
			deltaX * sine + deltaZ * cosine
		};
	}

	bool BuildEffectiveFormationBounds(
		const SceneComponent& director,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		const SceneFishingScoreAttackPlayerWaterBounds& waterBounds,
		float playerRadius,
		FishingFormationMotion::CenterBounds& bounds
	) {
		bounds = {};
		if (
			waterBounds.playerEntityId == 0 ||
			!std::isfinite(waterBounds.center.x) ||
			!std::isfinite(waterBounds.center.z) ||
			!std::isfinite(waterBounds.yaw) ||
			!std::isfinite(waterBounds.halfSizeX) ||
			!std::isfinite(waterBounds.halfSizeZ) ||
			waterBounds.halfSizeX <= 0.0f ||
			waterBounds.halfSizeZ <= 0.0f ||
			!std::isfinite(playerRadius) || playerRadius < 0.0f
		) {
			return false;
		}
		bounds.enabled = true;
		bounds.center = { waterBounds.center.x, waterBounds.center.z };
		bounds.yaw = waterBounds.yaw;
		bounds.halfSizeX = waterBounds.halfSizeX;
		bounds.halfSizeZ = waterBounds.halfSizeZ;

		const std::array<uint64_t, 4> wallIds = {
			director.fishingBoundaryNegativeXWallEntityId,
			director.fishingBoundaryPositiveXWallEntityId,
			director.fishingBoundaryNegativeZWallEntityId,
			director.fishingBoundaryPositiveZWallEntityId
		};
		const bool hasAnyWall = std::any_of(
			wallIds.begin(), wallIds.end(),
			[](uint64_t entityId) { return entityId != 0; }
		);
		if (!hasAnyWall) {
			return true;
		}
		if (!std::all_of(
			wallIds.begin(), wallIds.end(),
			[](uint64_t entityId) { return entityId != 0; }
		)) {
			return false;
		}

		auto resolveInnerPlane = [&bindings, &waterBounds](
			uint64_t entityId,
			bool useX,
			bool maximum,
			float& plane
		) {
			const SceneRuntimeObjectBinding* binding = FindBinding(
				bindings, entityId
			);
			if (!binding || !binding->collider ||
				binding->collider->GetType() != Collider::Type::OBB ||
				!binding->collider->IsActive() || binding->collider->IsTrigger()) {
				return false;
			}
			const auto* collider = static_cast<const OBBCollider*>(binding->collider);
			const std::vector<XZPoint> hull = BuildObbProjection(collider->GetOBB());
			if (hull.empty()) {
				return false;
			}
			plane = maximum ? -(std::numeric_limits<float>::max)() :
				(std::numeric_limits<float>::max)();
			for (const XZPoint& point : hull) {
				const XZPoint local = ToWaterLocalPoint(waterBounds, point);
				const float value = useX ? local.x : local.z;
				if (!std::isfinite(value)) {
					return false;
				}
				plane = maximum ? (std::max)(plane, value) :
					(std::min)(plane, value);
			}
			return std::isfinite(plane);
		};

		float negativeX = 0.0f;
		float positiveX = 0.0f;
		float negativeZ = 0.0f;
		float positiveZ = 0.0f;
		if (
			!resolveInnerPlane(wallIds[0], true, true, negativeX) ||
			!resolveInnerPlane(wallIds[1], true, false, positiveX) ||
			!resolveInnerPlane(wallIds[2], false, true, negativeZ) ||
			!resolveInnerPlane(wallIds[3], false, false, positiveZ) ||
			negativeX >= 0.0f || positiveX <= 0.0f ||
			negativeZ >= 0.0f || positiveZ <= 0.0f
		) {
			return false;
		}

		constexpr float kWaterBoundarySafetyMargin = 0.1f;
		const float wallMargin = playerRadius + kWaterBoundarySafetyMargin;
		const float minimumX = (std::max)(
			-waterBounds.halfSizeX, negativeX + wallMargin
		);
		const float maximumX = (std::min)(
			waterBounds.halfSizeX, positiveX - wallMargin
		);
		const float minimumZ = (std::max)(
			-waterBounds.halfSizeZ, negativeZ + wallMargin
		);
		const float maximumZ = (std::min)(
			waterBounds.halfSizeZ, positiveZ - wallMargin
		);
		if (minimumX >= maximumX || minimumZ >= maximumZ) {
			return false;
		}
		const float localCenterX = (minimumX + maximumX) * 0.5f;
		const float localCenterZ = (minimumZ + maximumZ) * 0.5f;
		const float cosine = std::cos(waterBounds.yaw);
		const float sine = std::sin(waterBounds.yaw);
		bounds.center = {
			waterBounds.center.x + localCenterX * cosine + localCenterZ * sine,
			waterBounds.center.z - localCenterX * sine + localCenterZ * cosine
		};
		bounds.halfSizeX = (maximumX - minimumX) * 0.5f;
		bounds.halfSizeZ = (maximumZ - minimumZ) * 0.5f;
		return true;
	}

	float AngleDistance(float first, float second) {
		return std::abs(std::atan2(
			std::sin(first - second), std::cos(first - second)
		));
	}

	bool NormalizePlanarVector(Vector2 value, Vector2& normalized) {
		const float lengthSquared = value.x * value.x + value.y * value.y;
		if (!std::isfinite(lengthSquared) ||
			lengthSquared <= kTransformEpsilon * kTransformEpsilon) {
			return false;
		}
		const float inverseLength = 1.0f / std::sqrt(lengthSquared);
		normalized = { value.x * inverseLength, value.y * inverseLength };
		return std::isfinite(normalized.x) && std::isfinite(normalized.y);
	}

	float MoveTowardsPlanarAngle(float current, float target, float maximumDelta) {
		const float delta = std::atan2(
			std::sin(target - current), std::cos(target - current)
		);
		return current + std::clamp(delta, -maximumDelta, maximumDelta);
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
	const std::string& sceneId,
	float deltaTime,
	bool playing
) {
	// Runtime専用の追加・削除はObject/Colliderのbindingを作り直す前に完了させる。
	MaterializePendingFishCatchEffects(document);
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
			hookBubbleRequests_.clear();
		} else {
			Clear(&document);
		}
		return;
	}

	if (directorEntityId_ != foundDirectorEntityId) {
		Clear(&document);
		director = FindDirector(document, foundDirectorEntityId, duplicateDirector);
		if (!director || duplicateDirector) {
			return;
		}
		directorEntityId_ = foundDirectorEntityId;
	}
	hasDirector_ = true;
	const bool tutorialScene = IsTutorialScene(sceneId); // チュートリアル専用制御を使うか。
	if (!tutorialScene) {
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::Disabled;
		tutorialMovePracticeSeconds_ = 0.0f;
		tutorialFishCountPracticeStart_ = 1;
		tutorialFishCountAdjusted_ = false;
		tutorialMultiScoreCount_ = 0;
		tutorialAutoStartNextRound_ = false;
	}
	if (scorePopup_.active) {
		scorePopup_.elapsedSeconds += (std::max)(deltaTime, 0.0f);
		if (scorePopup_.elapsedSeconds >= scorePopup_.durationSeconds) {
			scorePopup_ = {};
		}
	}
	const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
	fishCatchEffectPoolElapsedSeconds_ += safeDeltaTime;
	for (FishCatchAnimation& animation : fishCatchAnimations_) {
		animation.elapsedSeconds += safeDeltaTime;
	}
	if (state_ == SceneFishingScoreAttackState::Result) {
		resultInputArmed_ = true;
	}
	if (state_ == SceneFishingScoreAttackState::Inactive) {
		std::string diagnostic;
		if (!Preflight(document, foundDirectorEntityId, *director, diagnostic)) {
			Fault(document, *director, std::move(diagnostic));
			return;
		}
		InitializeRun(document, *director, tutorialScene);
	}

	if (tutorialScene && AdvanceTutorialByInput(document, *director)) {
		UpdateCurrentPositionMultiplier(document, *director);
		BuildTextRequests(document, *director);
		return;
	}
	if (
		tutorialScene &&
		tutorialAutoStartNextRound_ &&
		IsTutorialScorePracticeStep(tutorialStep_) &&
		state_ == SceneFishingScoreAttackState::SelectingNext
	) {
		tutorialAutoStartNextRound_ = false;
		StartRound(document, *director);
		UpdateCurrentPositionMultiplier(document, *director);
		BuildTextRequests(document, *director);
		return;
	}

	const bool selectingFishCount =
		state_ == SceneFishingScoreAttackState::SelectingInitial ||
		state_ == SceneFishingScoreAttackState::SelectingNext; // 魚数選択中か。
	const bool tutorialSelectionPausesTimer =
		tutorialScene &&
		tutorialStep_ != SceneFishingScoreAttackTutorialStep::FreePlay &&
		selectingFishCount; // チュートリアル練習中の選択待ちでタイマーを止めるか。
	const bool fishSelectionPausesTimer =
		(!director->fishingTimerRunsDuringFishSelection &&
			selectingFishCount) ||
		tutorialSelectionPausesTimer; // 現在の魚数選択でタイマーを止めるか。
	if (timerRunning_ && !fishSelectionPausesTimer &&
		(!tutorialScene || IsTutorialTimerAllowed())) {
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
		if (!tutorialScene || IsTutorialFishSelectionAllowed()) {
			UpdateSelection(document, *director);
		}
	}
	UpdateHookDrops(document, safeDeltaTime);
	if (state_ == SceneFishingScoreAttackState::Navigating &&
		(!tutorialScene || IsTutorialSharkAllowed())) {
		UpdateSharks(document, *director, deltaTime);
	}
	UpdateCurrentPositionMultiplier(document, *director);
	BuildTextRequests(document, *director);
}

void SceneFishingScoreAttackSystem::UpdateAfterSimulation(
	SceneDocument& document,
	const std::string& sceneId,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneAgentSystem& agentSystem,
	bool playing,
	float deltaTime,
	const Vector3& planarVelocity
) {
	if (!playing || state_ != SceneFishingScoreAttackState::Navigating) {
		return;
	}
	const bool tutorialScene = IsTutorialScene(sceneId); // チュートリアル専用制御を使うか。
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
		return;
	}
	formationContactResponseCooldownSeconds_ = (std::max)(
		0.0f,
		formationContactResponseCooldownSeconds_ - deltaTime
	);
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director) {
		Clear(&document);
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
				(obstacleBinding->collider->GetType() != Collider::Type::OBB &&
					obstacleBinding->collider->GetType() != Collider::Type::Sphere) ||
				!obstacleBinding->collider->IsActive() ||
				obstacleBinding->collider->IsTrigger()) {
				Fault(
					document,
					*director,
					"FishingObstacle requires an active non-trigger Box or Sphere runtime binding"
				);
				return;
			}
			FishingFormationMotion::Obstacle obstacle{};
			obstacle.entityId = entity.id;
			if (obstacleBinding->collider->GetType() == Collider::Type::OBB) {
				const auto* obstacleCollider = static_cast<const OBBCollider*>(
					obstacleBinding->collider
				);
				const std::vector<XZPoint> hull = BuildObbProjection(
					obstacleCollider->GetOBB()
				);
				obstacle.hull.reserve(hull.size());
				for (const XZPoint& point : hull) {
					obstacle.hull.push_back({ point.x, point.z });
				}
			} else {
				const auto* obstacleCollider = static_cast<const SphereCollider*>(
					obstacleBinding->collider
				);
				obstacle.shape = FishingFormationMotion::ObstacleShape::Circle;
				const Vector3 center = obstacleCollider->GetWorldCenter();
				obstacle.center = { center.x, center.z };
				obstacle.radius = obstacleCollider->GetRadius();
			}
			obstacles.push_back(std::move(obstacle));
		}
		FishingFormationMotion::CenterBounds effectiveBounds{};
		if (hasPlayerWaterBounds_ && !BuildEffectiveFormationBounds(
			*director,
			bindings,
			playerWaterBounds_,
			playerPlanarColliderRadius_,
			effectiveBounds
		)) {
			Fault(
				document,
				*director,
				"Fishing boundary walls cannot form a valid Player center bounds"
			);
			return;
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
		motionRequest.slideAssistStrength =
			director->fishingFormationSlideAssistStrength;
		motionRequest.rockVisualClearance =
			director->fishingFormationRockVisualClearance;
		motionRequest.bounds = effectiveBounds;
		FishingFormationMotion::Result motionResult{};
		if (!FishingFormationMotion::Solve(
			motionRequest,
			obstacles,
			motionResult
		)) {
			// 高速移動などで今回の姿勢を解けなくても、設定異常として
			// セッションをFaultedへ遷移させず、直近の安全位置へ復帰する。
			playerConstraintRequest_.playerEntityId =
				director->fishingPlayerEntityId;
			playerConstraintRequest_.planarPosition =
				lastSafePlayerPlanarPosition_;
			playerConstraintRequest_.yaw = lastSafePlayerYaw_;
			playerConstraintRequest_.planarVelocity = {};
			hasPlayerConstraintRequest_ = true;
			formationNoProgressReferencePosition_ = {};
			formationNoProgressReferenceYaw_ = 0.0f;
			formationNoProgressSeconds_ = 0.0f;
			hasFormationNoProgressReference_ = false;
			if (formationContactResponseActive_) {
				EndFormationContactResponse(
					director->fishingFormationContactCooldownSeconds
				);
			}
			BuildTextRequests(document, *director);
			return;
		}
		Vector2 incomingDirection{};
		const Vector2 requestedTranslation = {
			motionRequest.desiredCenter.x - motionRequest.startCenter.x,
			motionRequest.desiredCenter.y - motionRequest.startCenter.y
		};
		const bool hasIncomingDirection = NormalizePlanarVector(
			requestedTranslation,
			incomingDirection
		) || NormalizePlanarVector(motionRequest.desiredVelocity, incomingDirection);
		const bool hasEligibleMemberCount =
			director->fishingFormationContactResponseMaxFishCount > 0 &&
			formationCapsule.activeMemberCount <= static_cast<uint32_t>(
				director->fishingFormationContactResponseMaxFishCount
			);
		bool runContactResponse = false;
		bool contactResponseSolveRan = false;
		if (formationContactResponseActive_) {
			const float inwardDot = hasIncomingDirection
				? incomingDirection.x * formationContactResponseNormal_.x +
					incomingDirection.y * formationContactResponseNormal_.y
				: 0.0f;
			if (!hasEligibleMemberCount ||
				formationContactResponseRemainingSeconds_ <= 0.0f ||
				!hasIncomingDirection || inwardDot >= -0.05f) {
				EndFormationContactResponse(
					director->fishingFormationContactCooldownSeconds
				);
			} else {
				runContactResponse = true;
			}
		}
		if (!formationContactResponseActive_ &&
			hasEligibleMemberCount &&
			formationContactResponseCooldownSeconds_ <= 0.0f &&
			motionResult.translationBlocked &&
			motionResult.obstacleContact &&
			hasIncomingDirection) {
			const float inwardDot = incomingDirection.x *
				motionResult.obstacleContactNormal.x + incomingDirection.y *
				motionResult.obstacleContactNormal.y;
			if (inwardDot < -0.05f) {
				Vector2 reflectedDirection = {
					incomingDirection.x - 2.0f * inwardDot *
						motionResult.obstacleContactNormal.x,
					incomingDirection.y - 2.0f * inwardDot *
						motionResult.obstacleContactNormal.y
				};
				if (!NormalizePlanarVector(reflectedDirection, reflectedDirection)) {
					reflectedDirection = motionResult.obstacleContactNormal;
				}
				formationContactResponseActive_ =
					director->fishingFormationContactDurationSeconds > 0.0f;
				formationContactResponseNormal_ = motionResult.obstacleContactNormal;
				formationContactResponseTargetYaw_ = std::atan2(
					reflectedDirection.x, reflectedDirection.y
				);
				formationContactResponseRemainingSeconds_ =
					director->fishingFormationContactDurationSeconds;
				if (formationContactResponseActive_) {
					runContactResponse = true;
				} else {
					EndFormationContactResponse(
						director->fishingFormationContactCooldownSeconds
					);
				}
			}
		}
		if (runContactResponse) {
			contactResponseSolveRan = true;
			FishingFormationMotion::Request responseRequest = motionRequest;
			responseRequest.startCenter = motionResult.center;
			responseRequest.startYaw = motionResult.yaw;
			responseRequest.desiredCenter = {
				motionResult.center.x + formationContactResponseNormal_.x *
					director->fishingFormationContactPushSpeed * deltaTime,
				motionResult.center.y + formationContactResponseNormal_.y *
					director->fishingFormationContactPushSpeed * deltaTime
			};
			const float turnStep = director->fishingFormationContactTurnSpeedDegrees *
				(kTwoPi / 360.0f) * deltaTime;
			responseRequest.desiredYaw = MoveTowardsPlanarAngle(
				motionResult.yaw,
				formationContactResponseTargetYaw_,
				turnStep
			);
			const float normalVelocity = motionResult.velocity.x *
				formationContactResponseNormal_.x + motionResult.velocity.y *
				formationContactResponseNormal_.y;
			const Vector2 tangentVelocity = {
				motionResult.velocity.x - formationContactResponseNormal_.x * normalVelocity,
				motionResult.velocity.y - formationContactResponseNormal_.y * normalVelocity
			};
			const float outwardVelocity = (std::max)(
				director->fishingFormationContactPushSpeed,
				(std::max)(normalVelocity, 0.0f)
			);
			responseRequest.desiredVelocity = {
				tangentVelocity.x + formationContactResponseNormal_.x * outwardVelocity,
				tangentVelocity.y + formationContactResponseNormal_.y * outwardVelocity
			};
			FishingFormationMotion::Result responseResult{};
			if (FishingFormationMotion::Solve(
				responseRequest,
				obstacles,
				responseResult
			)) {
				motionResult = responseResult;
				formationContactResponseRemainingSeconds_ = (std::max)(
					0.0f,
					formationContactResponseRemainingSeconds_ - deltaTime
				);
				if (formationContactResponseRemainingSeconds_ <= 0.0f) {
					EndFormationContactResponse(
						director->fishingFormationContactCooldownSeconds
					);
				}
			} else {
				EndFormationContactResponse(
					director->fishingFormationContactCooldownSeconds
				);
			}
		}
		formationCapsule.center.x = motionResult.center.x;
		formationCapsule.center.z = motionResult.center.y;
		formationCapsule.yaw = motionResult.yaw;
		const Vector3 previousSafePosition = lastSafePlayerPlanarPosition_;
		const float previousSafeYaw = lastSafePlayerYaw_;
		const Vector3 currentSafePosition = {
			formationCapsule.center.x, 0.0f, formationCapsule.center.z
		};
		const bool canRecordRecoveryPose =
			!motionResult.translationBlocked &&
			!motionResult.rotationBlocked &&
			!motionResult.iterationLimited;
		if (canRecordRecoveryPose && (
			formationRecoveryPoses_.empty() ||
			DistanceXZ(
				currentSafePosition,
				formationRecoveryPoses_.back().planarPosition
			) >= (std::max)(formationCapsule.radius * 0.5f, 0.25f)
		)) {
			if (formationRecoveryPoses_.size() == kFormationRecoveryPoseCapacity) {
				formationRecoveryPoses_.erase(formationRecoveryPoses_.begin());
			}
			formationRecoveryPoses_.push_back({
				currentSafePosition, formationCapsule.yaw
			});
		}

		const bool hasRotationRequest = AngleDistance(
			motionRequest.desiredYaw, motionRequest.startYaw
		) > kFormationRecoveryYawProgress;
		const float translationRequestX =
			motionRequest.desiredCenter.x - motionRequest.startCenter.x;
		const float translationRequestZ =
			motionRequest.desiredCenter.y - motionRequest.startCenter.y;
		const bool hasTranslationRequest =
			translationRequestX * translationRequestX +
			translationRequestZ * translationRequestZ >
				kTransformEpsilon * kTransformEpsilon;
		const bool hasBlockedRecoveryRequest =
			(hasRotationRequest && motionResult.rotationBlocked) ||
			(hasTranslationRequest && motionResult.translationBlocked);
		bool shouldRecoverFormation = false;
		if (hasBlockedRecoveryRequest) {
			if (!hasFormationNoProgressReference_) {
				formationNoProgressReferencePosition_ = previousSafePosition;
				formationNoProgressReferenceYaw_ = previousSafeYaw;
				formationNoProgressSeconds_ = 0.0f;
				hasFormationNoProgressReference_ = true;
			} else if (
				AngleDistance(
					motionResult.yaw, formationNoProgressReferenceYaw_
				) >= kFormationRecoveryYawProgress ||
				DistanceXZ(
					currentSafePosition,
					formationNoProgressReferencePosition_
				) >= (std::max)(formationCapsule.radius * 0.25f, 0.25f)
			) {
				formationNoProgressReferencePosition_ = currentSafePosition;
				formationNoProgressReferenceYaw_ = motionResult.yaw;
				formationNoProgressSeconds_ = 0.0f;
			} else {
				formationNoProgressSeconds_ += deltaTime;
			}
			shouldRecoverFormation =
				formationNoProgressSeconds_ >= kFormationRecoveryNoProgressSeconds;
		} else {
			formationNoProgressReferencePosition_ = {};
			formationNoProgressReferenceYaw_ = 0.0f;
			formationNoProgressSeconds_ = 0.0f;
			hasFormationNoProgressReference_ = false;
		}

		if (shouldRecoverFormation && !contactResponseSolveRan) {
			size_t preferredRecoveryIndex = formationRecoveryPoses_.size();
			size_t oldestRecoveryIndex = formationRecoveryPoses_.size();
			const float minimumRecoveryDistance = (std::max)(
				formationCapsule.radius, formationCapsule.halfSegmentLength
			);
			for (size_t index = formationRecoveryPoses_.size(); index > 0; --index) {
				const size_t recoveryIndex = index - 1;
				const FormationRecoveryPose& recoveryPose =
					formationRecoveryPoses_[recoveryIndex];
				FishingFormationMotion::Request validationRequest = motionRequest;
				validationRequest.startCenter = {
					recoveryPose.planarPosition.x, recoveryPose.planarPosition.z
				};
				validationRequest.desiredCenter = validationRequest.startCenter;
				validationRequest.desiredVelocity = {};
				validationRequest.startYaw = recoveryPose.yaw;
				validationRequest.desiredYaw = recoveryPose.yaw;
				FishingFormationMotion::Result validationResult{};
				if (!FishingFormationMotion::Solve(
					validationRequest, obstacles, validationResult
				)) {
					continue;
				}
				oldestRecoveryIndex = recoveryIndex;
				if (
					preferredRecoveryIndex == formationRecoveryPoses_.size() &&
					DistanceXZ(
						recoveryPose.planarPosition, currentSafePosition
					) >= minimumRecoveryDistance
				) {
					preferredRecoveryIndex = recoveryIndex;
				}
			}
			const size_t recoveryIndex =
				preferredRecoveryIndex != formationRecoveryPoses_.size()
					? preferredRecoveryIndex
					: oldestRecoveryIndex;
			const FormationRecoveryPose recoveryPose =
				recoveryIndex != formationRecoveryPoses_.size()
					? formationRecoveryPoses_[recoveryIndex]
					: FormationRecoveryPose{ previousSafePosition, previousSafeYaw };
			playerConstraintRequest_.playerEntityId =
				director->fishingPlayerEntityId;
			playerConstraintRequest_.planarPosition = recoveryPose.planarPosition;
			playerConstraintRequest_.yaw = recoveryPose.yaw;
			playerConstraintRequest_.planarVelocity = {};
			hasPlayerConstraintRequest_ = true;
			lastSafePlayerPlanarPosition_ = recoveryPose.planarPosition;
			lastSafePlayerYaw_ = recoveryPose.yaw;
			hasLastSafePlayerPlanarPosition_ = true;
			if (recoveryIndex != formationRecoveryPoses_.size()) {
				formationRecoveryPoses_.erase(
					formationRecoveryPoses_.begin() +
						static_cast<std::ptrdiff_t>(recoveryIndex + 1),
					formationRecoveryPoses_.end()
				);
			}
			formationNoProgressReferencePosition_ = {};
			formationNoProgressReferenceYaw_ = 0.0f;
			formationNoProgressSeconds_ = 0.0f;
			hasFormationNoProgressReference_ = false;
			formationCapsule.center = recoveryPose.planarPosition;
			formationCapsule.yaw = recoveryPose.yaw;
			motionResult.center = {
				recoveryPose.planarPosition.x, recoveryPose.planarPosition.z
			};
			motionResult.yaw = recoveryPose.yaw;
			motionResult.velocity = {};
		} else if (!shouldRecoverFormation) {
			lastSafePlayerPlanarPosition_ = currentSafePosition;
			lastSafePlayerYaw_ = formationCapsule.yaw;
			hasLastSafePlayerPlanarPosition_ = true;
		}
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
	if (tutorialScene &&
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::MovePractice) {
		const float planarSpeed = Math::Length(Vector3{
			planarVelocity.x,
			0.0f,
			planarVelocity.z
		}); // プレイヤーが実際に移動入力で動いた速度。
		if (planarSpeed > kTutorialMoveInputSpeedThreshold) {
			tutorialMovePracticeSeconds_ += deltaTime;
		}
		if (tutorialMovePracticeSeconds_ >=
			kTutorialMovePracticeRequiredSeconds) {
			tutorialStep_ =
				SceneFishingScoreAttackTutorialStep::HookExplanation;
			tutorialMovePracticeSeconds_ = 0.0f;
			BuildTextRequests(document, *director);
		}
		return;
	}
	if (tutorialScene && !IsTutorialScoringAllowed()) {
		return;
	}
	long long sharkPenaltyTotal = 0;
	bool sharkPenaltyApplied = false;
	Vector3 sharkPenaltyWorldPosition{};
	bool hasSharkPenaltyWorldPosition = false;
	if (director->fishingUseFormationCapsuleCollision &&
		(!tutorialScene || IsTutorialSharkAllowed())) {
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
			const long long maximumScore = (std::numeric_limits<long long>::max)();
			const long long basePenalty = static_cast<long long>(
				(std::max)(shark->fishingSharkPenaltyScore, 0)
			);
			// 減点はこのラウンドで編成したプレイヤー（魚群）数に比例させる。
			const long long playerCount = static_cast<long long>(
				(std::max)(roundFishCount_, 1)
			);
			const long long penalty = basePenalty > maximumScore / playerCount
				? maximumScore
				: basePenalty * playerCount;
			if (sharkPenaltyTotal > maximumScore - penalty) {
				sharkPenaltyTotal = maximumScore;
			} else {
				sharkPenaltyTotal += penalty;
			}
			runtime->second.hitCooldown = (std::max)(
				shark->fishingSharkHitCooldownSeconds,
				0.0f
			);
			const uint64_t maximumCount = (std::numeric_limits<uint64_t>::max)();
			if (sharkHitCount_ < maximumCount) {
				++sharkHitCount_;
			}
			const uint64_t fishWeightedCount = static_cast<uint64_t>(
				(std::max)(roundFishCount_, 0)
			);
			if (fishWeightedCount > maximumCount - sharkFishWeightedCount_) {
				sharkFishWeightedCount_ = maximumCount;
			} else {
				sharkFishWeightedCount_ += fishWeightedCount;
			}
			if (!hasSharkPenaltyWorldPosition && sharkBinding->object) {
				const Matrix4x4& sharkWorld = sharkBinding->object->GetWorldMatrix();
				sharkPenaltyWorldPosition = {
					sharkWorld.m[3][0], sharkWorld.m[3][1] + 1.25f,
					sharkWorld.m[3][2]
				};
				hasSharkPenaltyWorldPosition = true;
			}
			sharkPenaltyApplied = true;
		}
	}
	if (sharkPenaltyApplied) {
		SceneSoundEffectPlayer::PlaySharkEat();
		const long long minimumScore =
			(std::numeric_limits<long long>::lowest)();
		if (totalScore_ < minimumScore + sharkPenaltyTotal) {
			totalScore_ = minimumScore;
		} else {
			totalScore_ -= sharkPenaltyTotal;
		}
		if (hasSharkPenaltyWorldPosition &&
			director->fishingResultTextEntityId != 0) {
			scorePopup_.entityId = director->fishingResultTextEntityId;
			scorePopup_.text = "-" + std::to_string(sharkPenaltyTotal);
			scorePopup_.color = { 1.0f, 0.18f, 0.18f, 1.0f };
			scorePopup_.worldPosition = sharkPenaltyWorldPosition;
			scorePopup_.elapsedSeconds = 0.0f;
			scorePopup_.active = true;
		}
		playerConstraintRequest_ = {};
		hasPlayerConstraintRequest_ = false;
		hasPlayerResetRequest_ = hasInitialPlayerTransform_;
		if (!SpawnHooks(document, *director)) {
			return;
		}
		if (!ResetSharksForRound(document, *director)) {
			return;
		}
		ResetFormationContactResponse();
		state_ = SceneFishingScoreAttackState::SelectingNext;
		SetFishPreview(document, *director);
		BuildTextRequests(document, *director);
		return;
	}
	const ActiveHook* hitHook = nullptr;
	const SceneComponent* hitHookComponent = nullptr;
	const SceneRuntimeObjectBinding* hitHookBinding = nullptr;
	for (const ActiveHook& activeHook : activeHooks_) {
		if (activeHook.isDropping) {
			continue;
		}
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
			hitHookBinding = hookBinding;
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
	SceneSoundEffectPlayer::PlayFishingScore();
	const SceneFishingHookRankDefinition rank = ResolveFishingHookRank(
		*director,
		hitHook->hookMultiplierTier
	);
	if (resultTrackingEnabled_) {
		const size_t rankIndex = static_cast<size_t>(std::clamp(
			hitHook->hookMultiplierTier,
			1,
			director->fishingHookRankCount
		) - 1);
		if (rankIndex < resultRankRecords_.size()) {
			SceneFishingResultRankRecord& resultRank =
				resultRankRecords_[rankIndex];
			const uint64_t maximumCount =
				(std::numeric_limits<uint64_t>::max)();
			if (resultRank.catchCount < maximumCount) {
				++resultRank.catchCount;
			}
			const uint64_t fishCount = static_cast<uint64_t>(
				(std::max)(roundFishCount_, 0)
			);
			if (resultRank.fishWeightedCount > maximumCount - fishCount) {
				resultRank.fishWeightedCount = maximumCount;
			} else {
				resultRank.fishWeightedCount += fishCount;
			}
		}
	}
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
	long long awardedScore = 0;
	if (score) {
		const long long maximumTotalScore =
			(std::numeric_limits<long long>::max)();
		const long long requestedScore = static_cast<long long>((std::min)(
			score,
			static_cast<double>(maximumTotalScore)
		));
		if (totalScore_ > 0 &&
			requestedScore > maximumTotalScore - totalScore_) {
			awardedScore = maximumTotalScore - totalScore_;
		} else {
			awardedScore = requestedScore;
		}
		totalScore_ += awardedScore;
	}
	if (awardedScore != 0 &&
		director->fishingResultTextEntityId != 0 &&
		hitHookBinding && hitHookBinding->object) {
		const Matrix4x4& hookWorld = hitHookBinding->object->GetWorldMatrix();
		scorePopup_.entityId = director->fishingResultTextEntityId;
		scorePopup_.text = awardedScore > 0
			? "+" + std::to_string(awardedScore)
			: std::to_string(awardedScore);
		scorePopup_.color = awardedScore > 0
			? rank.color
			: Vector4{ 1.0f, 0.18f, 0.18f, 1.0f };
		scorePopup_.worldPosition = {
			hookWorld.m[3][0], hookWorld.m[3][1] + 1.25f, hookWorld.m[3][2]
		};
		scorePopup_.elapsedSeconds = 0.0f;
		scorePopup_.active = true;
	}
	if (awardedScore > 0) {
		NotifyTutorialHookScored();
	} else if (tutorialScene && IsTutorialScorePracticeStep(tutorialStep_)) {
		tutorialAutoStartNextRound_ = true;
	}
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasPlayerResetRequest_ = hasInitialPlayerTransform_;
	if (!SpawnHooks(document, *director)) {
		return;
	}
	if (!ResetSharksForRound(document, *director)) {
		return;
	}
	ResetFormationContactResponse();
	state_ = SceneFishingScoreAttackState::SelectingNext;
	SetFishPreview(document, *director);
	BuildTextRequests(document, *director);
	// 複製はSceneEntity配列を再確保し得るため、director参照を使い切った後に行う。
	StartFishCatchAnimation(
		document, *director, bindings, agentSystem, rank.color
	);
}

void SceneFishingScoreAttackSystem::ApplyHookVisualOverrides(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	hookBubbleRequests_.clear();
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
		const SceneComponent* hook = FindEnabledComponent(
			*hookEntity,
			"FishingHook"
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
		if (hook && (
			hook->fishingHookBubbleSpriteEntityId != 0 ||
			hook->fishingHookRankIconSpriteEntityId != 0
		)) {
			const Matrix4x4& worldMatrix = binding->object->GetWorldMatrix();
			SceneFishingScoreAttackHookBubbleRequest request{};
			request.hookEntityId = entry.hookEntityId;
			request.bubbleSpriteEntityId = hook->fishingHookBubbleSpriteEntityId;
			request.rankIconSpriteEntityId = hook->fishingHookRankIconSpriteEntityId;
			request.worldAnchor = {
				worldMatrix.m[3][0] + director->fishingHookRankBubbleWorldOffset.x,
				worldMatrix.m[3][1] + director->fishingHookRankBubbleWorldOffset.y,
				worldMatrix.m[3][2] + director->fishingHookRankBubbleWorldOffset.z
			};
			request.bubbleTexturePath = director->fishingHookRankBubbleTexturePath;
			request.rankIconTexturePath = active ? rank.iconTexturePath : std::string{};
			request.bubbleSize = director->fishingHookRankBubbleSize;
			request.bubbleScreenOffset = director->fishingHookRankBubbleScreenOffset;
			request.rankIconSize = {
				director->fishingHookRankBubbleIconBaseSize.x * rank.bubbleIconScale.x,
				director->fishingHookRankBubbleIconBaseSize.y * rank.bubbleIconScale.y
			};
			request.rankIconScreenOffset = {
				director->fishingHookRankBubbleScreenOffset.x +
					director->fishingHookRankBubbleIconBaseOffset.x +
					rank.bubbleIconOffset.x,
				director->fishingHookRankBubbleScreenOffset.y +
					director->fishingHookRankBubbleIconBaseOffset.y +
					rank.bubbleIconOffset.y
			};
			request.bubbleVisible = active &&
				director->fishingHookRankBubbleVisible &&
				!request.bubbleTexturePath.empty();
			request.rankIconVisible = request.bubbleVisible &&
				!request.rankIconTexturePath.empty();
			hookBubbleRequests_.push_back(std::move(request));
		}
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

void SceneFishingScoreAttackSystem::ApplySharkVisualOverrides(
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
	for (const auto& [entityId, runtime] : sharkRuntimes_) {
		const SceneRuntimeObjectBinding* binding = FindBinding(bindings, entityId);
		if (!binding || !binding->object) {
			continue;
		}
		const SceneEntity* entity = document.FindEntity(entityId);
		const SceneComponent* shark = entity
			? FindEnabledComponent(*entity, "FishingShark")
			: nullptr;
		if (!entity || !shark || !IsEntityActiveInHierarchy(document, *entity)) {
			continue;
		}
		if (runtime.navigationState != SharkNavigationState::Alert) {
			binding->object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			binding->object->SetEmissive(0.0f);
			continue;
		}
		const float pulseSpeed = std::isfinite(
			shark->fishingSharkAlertPulseSpeed
		) ? (std::max)(shark->fishingSharkAlertPulseSpeed, 0.0f) : 5.0f;
		const float pulse = 0.5f + 0.5f * std::sin(
			runtime.alertPulseElapsedSeconds * pulseSpeed
		);
		const Vector4 alertColor = shark->fishingSharkAlertColor;
		const Vector4 color = {
			1.0f + (alertColor.x - 1.0f) * pulse,
			1.0f + (alertColor.y - 1.0f) * pulse,
			1.0f + (alertColor.z - 1.0f) * pulse,
			1.0f
		};
		const float emissiveIntensity = std::isfinite(
			shark->fishingSharkAlertEmissiveIntensity
		) ? (std::max)(shark->fishingSharkAlertEmissiveIntensity, 0.0f) : 2.0f;
		binding->object->SetColor(color);
		binding->object->SetEmissive(emissiveIntensity * pulse, alertColor);
	}
}

void SceneFishingScoreAttackSystem::ApplyFishCatchVisualOverrides(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	const Camera* camera
) {
	constexpr float kCameraReachSeconds = 1.5f;
	std::vector<uint64_t> completedEffectEntityIds;
	std::vector<uint64_t> completedEffectHookEntityIds;
	for (FishCatchAnimation& animation : fishCatchAnimations_) {
		const SceneRuntimeObjectBinding* binding = FindBinding(
			bindings, animation.entityId
		);
		SceneEntity* fish = document.FindEntity(animation.entityId);
		if (!binding || !binding->object || !fish ||
			!IsEntityActiveInHierarchy(document, *fish)) {
			continue;
		}
		Transform catchWorldTransform = animation.sourceWorldTransform;
		if (camera && !animation.hasTargetCameraHeight) {
			animation.targetCameraHeight = camera->GetTranslate().y;
			animation.hasTargetCameraHeight = true;
		}
		if (animation.hasTargetCameraHeight) {
			const float progress = std::clamp(
				animation.elapsedSeconds / kCameraReachSeconds, 0.0f, 1.0f
			);
			catchWorldTransform.translate.y +=
				(animation.targetCameraHeight - catchWorldTransform.translate.y) *
				progress;
		} else {
			catchWorldTransform.translate.y += 60.0f * animation.elapsedSeconds;
		}
		if (camera) {
			const Vector3 toCamera = Math::Subtract(
				camera->GetTranslate(), catchWorldTransform.translate
			);
			if (Math::Length(toCamera) > kTransformEpsilon) {
				const Vector3 cameraDirection = Math::Normalize(toCamera);
				Vector3 screenRight = Math::Cross(
					{ 0.0f, 1.0f, 0.0f }, cameraDirection
				);
				if (Math::Length(screenRight) <= kTransformEpsilon) {
					screenRight = { 1.0f, 0.0f, 0.0f };
				} else {
					screenRight = Math::Normalize(screenRight);
				}
				Vector3 screenUp = Math::Cross(cameraDirection, screenRight);
				if (Math::Length(screenUp) <= kTransformEpsilon) {
					screenUp = { 0.0f, 1.0f, 0.0f };
				} else {
					screenUp = Math::Normalize(screenUp);
				}
				// カメラ正面で上下・左右へ広がる魚群として配置する。
				catchWorldTransform.translate = Math::Add(
					catchWorldTransform.translate,
					Math::Multiply(
						screenRight, animation.screenHorizontalOffset
					)
				);
				catchWorldTransform.translate = Math::Add(
					catchWorldTransform.translate,
					Math::Multiply(
						screenUp, animation.verticalOffset
					)
				);
				catchWorldTransform.translate = Math::Add(
					catchWorldTransform.translate,
					Math::Multiply(cameraDirection, animation.depthOffset)
				);
			}
		}
		if (animation.controlsAttractor) {
			SceneEntity* attractor = document.FindEntity(
				animation.attractorEntityId
			);
			if (!attractor) {
				continue;
			}
			// リーダーだけが上昇目標を追い、他の魚はリーダーへ追従する。
			attractor->transform.translate = catchWorldTransform.translate;
		}
		if (animation.effectHookEntityId != 0) {
			const SceneRuntimeObjectBinding* hookBinding = FindBinding(
				bindings, animation.effectHookEntityId
			);
			SceneEntity* hookEntity = document.FindEntity(
				animation.effectHookEntityId
			);
			Transform hookLocalTransform{};
			const Transform fishWorldTransform =
				SceneTransformResolver::ResolveScene3DTransform(document, *fish);
			if (hookBinding && hookBinding->object && hookEntity &&
				SceneTransformResolver::TryConvertSceneWorldTransformToLocal(
					document, *hookEntity, fishWorldTransform, hookLocalTransform
				)) {
				hookBinding->object->GetTransform().translate =
					hookLocalTransform.translate;
				// 命中した釣り針ランクの色を、演出針の発光色へ引き継ぐ。
				hookBinding->object->SetColor(animation.effectHookColor);
				hookBinding->object->SetEmissive(
					100.0f, animation.effectHookColor
				);
			}
		}
		const Matrix4x4& fishWorldMatrix = binding->object->GetWorldMatrix();
		const bool reachedCameraHeight = camera &&
			fishWorldMatrix.m[3][1] >= camera->GetTranslate().y;
		if ((animation.controlsAttractor && reachedCameraHeight) ||
			animation.elapsedSeconds >= 4.0f) {
			completedEffectEntityIds.push_back(animation.entityId);
			if (animation.controlsAttractor) {
				completedEffectEntityIds.push_back(animation.attractorEntityId);
				if (animation.effectHookEntityId != 0) {
					completedEffectHookEntityIds.push_back(
						animation.effectHookEntityId
					);
				}
			}
		}
	}
	for (uint64_t entityId : completedEffectEntityIds) {
		if (SceneEntity* effectFish = document.FindEntity(entityId)) {
			effectFish->active = false;
		}
	}
	for (uint64_t effectHookEntityId : completedEffectHookEntityIds) {
		if (SceneEntity* effectHook = document.FindEntity(effectHookEntityId)) {
			effectHook->active = false;
		}
	}
	std::erase_if(fishCatchAnimations_, [&completedEffectEntityIds](
		const FishCatchAnimation& animation
	) {
		return std::find(
			completedEffectEntityIds.begin(),
			completedEffectEntityIds.end(),
			animation.entityId
		) != completedEffectEntityIds.end();
	});
}

void SceneFishingScoreAttackSystem::MaterializePendingFishCatchEffects(
	SceneDocument& document
) {
	for (uint64_t entityId : pendingFishCatchEffectRemovals_) {
		const SceneEntity* effectFish = document.FindEntity(entityId);
		if (!effectFish || !effectFish->runtimeOnly) {
			continue;
		}
		const bool wasDirty = document.IsDirty();
		document.RemoveEntity(entityId);
		if (!wasDirty) {
			document.MarkClean();
		}
	}
	pendingFishCatchEffectRemovals_.clear();
	uint64_t leaderEffectEntityId = 0;
	uint64_t leaderTargetEntityId = 0;
	uint64_t materializingGroupId = 0;
	for (const PendingFishCatchEffect& pending : pendingFishCatchEffects_) {
		if (pending.groupId != materializingGroupId) {
			materializingGroupId = pending.groupId;
			leaderEffectEntityId = 0;
			leaderTargetEntityId = 0;
		}
		const SceneEntity* sourceFish = document.FindEntity(
			pending.sourceFishEntityId
		);
		if (!sourceFish || !IsEntityActiveInHierarchy(document, *sourceFish)) {
			continue;
		}
		const bool wasDirty = document.IsDirty();
		const uint64_t effectEntityId = document.DuplicateEntity(
			pending.sourceFishEntityId
		);
		SceneEntity* effectFish = document.FindEntity(effectEntityId);
		if (!effectFish) {
			continue;
		}
		effectFish->name = "Fishing Catch Effect";
		effectFish->runtimeOnly = true;
		effectFish->locked = true;
		// 元の魚群の親・Team制御から切り離し、演出専用のAgentにする。
		effectFish->parentId = 0;
		effectFish->teamName.clear();
		effectFish->transform.scale = pending.sourceWorldTransform.scale;
		effectFish->transform.rotate = pending.sourceWorldTransform.quaternionRotate;
		effectFish->transform.translate = pending.sourceWorldTransform.translate;
		// 追従開始前から球面上の位置へ置き、縦列化を防ぐ。
		effectFish->transform.translate.x += pending.screenHorizontalOffset;
		effectFish->transform.translate.y += pending.verticalOffset;
		effectFish->transform.translate.z += pending.depthOffset;
		effectFish->components.erase(
			std::remove_if(
				effectFish->components.begin(),
				effectFish->components.end(),
				[](const SceneComponent& component) {
					return component.type != "MeshRenderer" &&
						component.type != "Animator" &&
						component.type != "AgentBehavior";
				}
			),
			effectFish->components.end()
		);
		const bool isLeader = leaderEffectEntityId == 0;
		uint64_t followerTargetEntityId = 0;
		if (isLeader) {
			SceneEntity& target = document.CreateEntity("Fishing Catch Attractor");
			leaderTargetEntityId = target.id;
			target.runtimeOnly = true;
			target.locked = true;
			target.transform.translate = pending.sourceWorldTransform.translate;
			document.AddComponent(leaderTargetEntityId, "AgentAttractor");
			document.AddComponent(effectEntityId, "AgentAttractor");
			leaderEffectEntityId = effectEntityId;
		} else {
			// リーダーの子として置くことで、上昇には追従しつつ左右の位置を保つ。
			SceneEntity& followerTarget = document.CreateEntity(
				"Fishing Catch Follower Target", leaderEffectEntityId
			);
			followerTargetEntityId = followerTarget.id;
			followerTarget.runtimeOnly = true;
			followerTarget.locked = true;
			followerTarget.transform.translate = {
				pending.screenHorizontalOffset,
				pending.verticalOffset,
				pending.depthOffset
			};
			document.AddComponent(followerTargetEntityId, "AgentAttractor");
		}
		SceneEntity* configuredEffectFish = document.FindEntity(effectEntityId);
		if (!configuredEffectFish) {
			continue;
		}
		for (SceneComponent& component : configuredEffectFish->components) {
			if (component.type == "AgentAttractor") {
				component.attractorRadius = 0.25f;
				component.attractorStrength = 12.0f;
			}
		}
		const uint64_t agentTargetEntityId = isLeader
			? leaderTargetEntityId
			: followerTargetEntityId;
		if (SceneEntity* target = document.FindEntity(agentTargetEntityId)) {
			for (SceneComponent& component : target->components) {
				if (component.type == "AgentAttractor") {
					component.attractorRadius = 0.25f;
					component.attractorStrength = 12.0f;
				}
			}
		}
		for (SceneComponent& component : configuredEffectFish->components) {
			if (component.type != "AgentBehavior") {
				continue;
			}
			component.agentAttractorEntityId = agentTargetEntityId;
			component.agentAttractorTag.clear();
			component.agentAttractorWeight = 8.0f;
			component.agentUseWaterBounds = false;
			component.agentBoundsEntityId = 0;
			component.agentBoundsName.clear();
			// 個別Attractorの位置が隊形を決めるため、群れ補正は使わない。
			// 高速時の分離反発による魚群の爆散を防ぐ。
			component.agentMinSpeed = 45.0f;
			component.agentMaxSpeed = 60.0f;
			component.agentTurnSpeed = 60.0f;
			component.agentWanderStrength = 0.0f;
			component.agentSchooling = false;
			component.agentSeparationRadius = 0.0f;
			component.agentSeparationWeight = 0.0f;
			component.agentCohesionWeight = 0.0f;
			component.agentAlignForwardToVelocity = false;
			component.agentRotateAxisX = false;
			component.agentRotateAxisY = false;
			component.agentRotateAxisZ = false;
			component.agentGroupName = "FishingCatchEffect" +
				std::to_string(leaderEffectEntityId);
		}
		configuredEffectFish->transform.rotate = MakeQuaternionFromEuler(
			{ 0.0f, 0.0f, -1.5f }
		);
		if (!wasDirty) {
			document.MarkClean();
		}
		FishCatchAnimation animation{};
		animation.groupId = pending.groupId;
		animation.entityId = effectEntityId;
		animation.attractorEntityId = agentTargetEntityId;
		animation.controlsAttractor = isLeader;
		animation.sourceWorldTransform = pending.sourceWorldTransform;
		animation.screenHorizontalOffset = pending.screenHorizontalOffset;
		animation.depthOffset = pending.depthOffset;
		animation.verticalOffset = pending.verticalOffset;
		fishCatchAnimations_.push_back(std::move(animation));
	}
	pendingFishCatchEffects_.clear();
}

void SceneFishingScoreAttackSystem::PrepareFishCatchEffectPool(
	SceneDocument& document
) {
	if (!fishCatchEffectPool_.empty()) {
		return;
	}
	uint64_t directorEntityId = 0;
	bool duplicateDirector = false;
	const SceneComponent* director = FindDirector(
		document, directorEntityId, duplicateDirector
	);
	if (!director || duplicateDirector) {
		return;
	}
	const SceneComponent* hookPool = FindComponent(
		document, director->fishingHookPoolEntityId, "FishingHookPool"
	);
	const uint64_t hookSourceEntityId = hookPool &&
		!hookPool->fishingHookPoolEntries.empty()
		? hookPool->fishingHookPoolEntries.front().hookEntityId
		: 0;
	struct PoolCandidate {
		uint64_t entityId = 0;
		Transform worldTransform{};
	};
	std::vector<PoolCandidate> candidates;
	Vector3 groupCenter{};
	for (uint64_t fishEntityId : director->fishingFishEntityIds) {
		const SceneEntity* fish = document.FindEntity(fishEntityId);
		if (!fish || !IsEntityActiveInHierarchy(document, *fish)) {
			continue;
		}
		const Transform worldTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *fish);
		candidates.push_back({ fishEntityId, worldTransform });
		groupCenter = Math::Add(groupCenter, worldTransform.translate);
	}
	if (candidates.empty()) {
		return;
	}
	groupCenter = Math::Multiply(
		groupCenter, 1.0f / static_cast<float>(candidates.size())
	);
	constexpr uint32_t kPoolGroupCount = 2;
	constexpr float kFishSphereRadius = 2.5f;
	constexpr float kGoldenAngle = 2.39996323f;
	for (uint32_t slotIndex = 0; slotIndex < kPoolGroupCount; ++slotIndex) {
		const uint64_t groupId = nextFishCatchEffectGroupId_++;
		for (size_t index = 0; index < candidates.size(); ++index) {
			const float normalizedIndex =
				(static_cast<float>(index) + 0.5f) /
				static_cast<float>(candidates.size());
			const float vertical = 1.0f - normalizedIndex * 2.0f;
			const float horizontalRadius = std::sqrt((std::max)(
				0.0f, 1.0f - vertical * vertical
			));
			const float angle = static_cast<float>(index) * kGoldenAngle;
			Transform sourceTransform = candidates[index].worldTransform;
			sourceTransform.translate = groupCenter;
			pendingFishCatchEffects_.push_back({
				groupId,
				candidates[index].entityId,
				sourceTransform,
				std::cos(angle) * horizontalRadius * kFishSphereRadius,
				std::sin(angle) * horizontalRadius * kFishSphereRadius,
				vertical * kFishSphereRadius
			});
		}
		MaterializePendingFishCatchEffects(document);
		FishCatchEffectPoolSlot slot{};
		slot.groupId = groupId;
		if (hookSourceEntityId != 0) {
			const bool wasDirty = document.IsDirty();
			const uint64_t effectHookEntityId = document.DuplicateEntity(
				hookSourceEntityId
			);
			if (SceneEntity* effectHook = document.FindEntity(
				effectHookEntityId
			)) {
				effectHook->name = "Fishing Catch Effect Hook";
				effectHook->runtimeOnly = true;
				effectHook->locked = true;
				effectHook->parentId = 0;
			effectHook->teamName.clear();
			effectHook->active = false;
			effectHook->components.erase(
				std::remove_if(
					effectHook->components.begin(), effectHook->components.end(),
					[](const SceneComponent& component) {
						return component.type != "MeshRenderer" &&
							component.type != "Animator";
					}
				),
				effectHook->components.end()
			);
			slot.effectHookEntityId = effectHookEntityId;
			}
			if (!wasDirty) {
				document.MarkClean();
			}
		}
		for (const FishCatchAnimation& animation : fishCatchAnimations_) {
			if (animation.groupId != groupId) {
				continue;
			}
			slot.fishEntityIds.push_back(animation.entityId);
			slot.followerAttractorEntityIds.push_back(animation.attractorEntityId);
			if (animation.controlsAttractor) {
				slot.leaderAttractorEntityId = animation.attractorEntityId;
			}
			if (SceneEntity* fish = document.FindEntity(animation.entityId)) {
				fish->active = false;
			}
			if (SceneEntity* attractor = document.FindEntity(
				animation.attractorEntityId
			)) {
				attractor->active = false;
			}
		}
		std::erase_if(fishCatchAnimations_, [groupId](
			const FishCatchAnimation& animation
		) {
			return animation.groupId == groupId;
		});
		if (!slot.fishEntityIds.empty() && slot.leaderAttractorEntityId != 0) {
			fishCatchEffectPool_.push_back(std::move(slot));
		}
	}
}

void SceneFishingScoreAttackSystem::StartFishCatchAnimation(
	SceneDocument& document,
	const SceneComponent& director,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneAgentSystem& agentSystem,
	const Vector4& effectHookColor
) {
	// 演出用Entityの生成は次フレームへ遅延するため、対象IDと現在位置を先に記録する。
	const std::vector<uint64_t> fishEntityIds = director.fishingFishEntityIds;
	struct FishCatchCandidate {
		uint64_t entityId = 0;
		Transform worldTransform{};
	};
	std::vector<FishCatchCandidate> candidates;
	Vector3 groupCenter{};
	for (int fishIndex = 0; fishIndex < roundFishCount_; ++fishIndex) {
		if (fishIndex >= static_cast<int>(fishEntityIds.size())) {
			break;
		}
		const uint64_t fishEntityId = fishEntityIds[
			static_cast<size_t>(fishIndex)
		];
		const SceneRuntimeObjectBinding* fishBinding = FindBinding(
			bindings, fishEntityId
		);
		const SceneEntity* fish = document.FindEntity(fishEntityId);
		if (!fishBinding || !fishBinding->object || !fish ||
			!IsEntityActiveInHierarchy(document, *fish)) {
			continue;
		}
		const Transform sourceWorldTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *fish);
		groupCenter = Math::Add(groupCenter, sourceWorldTransform.translate);
		candidates.push_back({ fishEntityId, sourceWorldTransform });
	}
	if (candidates.empty()) {
		return;
	}
	if (fishCatchEffectPool_.empty()) {
		return;
	}
	FishCatchEffectPoolSlot* slot = nullptr;
	for (FishCatchEffectPoolSlot& candidate : fishCatchEffectPool_) {
		const bool inUse = std::any_of(
			fishCatchAnimations_.begin(), fishCatchAnimations_.end(),
			[&candidate](const FishCatchAnimation& animation) {
				return animation.groupId == candidate.groupId;
			}
		);
		if (!inUse) {
			slot = &candidate;
			break;
		}
	}
	if (!slot) {
		slot = &*std::min_element(
			fishCatchEffectPool_.begin(), fishCatchEffectPool_.end(),
			[](const FishCatchEffectPoolSlot& left,
				const FishCatchEffectPoolSlot& right) {
				return left.lastUsedSeconds < right.lastUsedSeconds;
			}
		);
		for (const FishCatchAnimation& animation : fishCatchAnimations_) {
			if (animation.groupId != slot->groupId) {
				continue;
			}
			if (SceneEntity* fish = document.FindEntity(animation.entityId)) {
				fish->active = false;
			}
		}
		if (SceneEntity* attractor = document.FindEntity(
			slot->leaderAttractorEntityId
		)) {
			attractor->active = false;
		}
		std::erase_if(fishCatchAnimations_, [slot](
			const FishCatchAnimation& animation
		) {
			return animation.groupId == slot->groupId;
		});
	}
	const size_t fishCount = (std::min)(
		candidates.size(), slot->fishEntityIds.size()
	);
	if (fishCount == 0 || slot->followerAttractorEntityIds.size() < fishCount) {
		return;
	}
	groupCenter = Math::Multiply(
		groupCenter, 1.0f / static_cast<float>(fishCount)
	);
	constexpr float kFishSphereRadius = 2.5f;
	constexpr float kGoldenAngle = 2.39996323f;
	if (SceneEntity* leaderAttractor = document.FindEntity(
		slot->leaderAttractorEntityId
	)) {
		leaderAttractor->active = true;
		leaderAttractor->transform.translate = groupCenter;
	}
	if (SceneEntity* effectHook = document.FindEntity(
		slot->effectHookEntityId
	)) {
		effectHook->active = true;
	}
	for (size_t index = 0; index < slot->fishEntityIds.size(); ++index) {
		if (SceneEntity* fish = document.FindEntity(slot->fishEntityIds[index])) {
			fish->active = index < fishCount;
		}
		if (index > 0) {
			if (SceneEntity* attractor = document.FindEntity(
				slot->followerAttractorEntityIds[index]
			)) {
				attractor->active = index < fishCount;
			}
		}
	}
	for (size_t index = 0; index < fishCount; ++index) {
		const float normalizedIndex =
			(static_cast<float>(index) + 0.5f) /
			static_cast<float>(candidates.size());
		const float vertical = 1.0f - normalizedIndex * 2.0f;
		const float horizontalRadius = std::sqrt((std::max)(
			0.0f, 1.0f - vertical * vertical
		));
		const float angle = static_cast<float>(index) * kGoldenAngle;
		const float horizontalOffset =
			std::cos(angle) * horizontalRadius * kFishSphereRadius;
		const float depthOffset =
			std::sin(angle) * horizontalRadius * kFishSphereRadius;
		const float verticalOffset = vertical * kFishSphereRadius;
		SceneEntity* effectFish = document.FindEntity(slot->fishEntityIds[index]);
		if (!effectFish) {
			continue;
		}
		effectFish->transform.scale = candidates[index].worldTransform.scale;
		effectFish->transform.rotate = MakeQuaternionFromEuler(
			{ 0.0f, 0.0f, -1.5f }
		);
		effectFish->transform.translate = {
			groupCenter.x + horizontalOffset,
			groupCenter.y + verticalOffset,
			groupCenter.z + depthOffset
		};
		// 前回の上昇速度を再利用しない。休止済みスロットからの再開でも
		// 最初のフレームを追従先へ向けて安定させる。
		agentSystem.ResetAgent(effectFish->id);
		if (index > 0) {
			if (SceneEntity* followerAttractor = document.FindEntity(
				slot->followerAttractorEntityIds[index]
			)) {
				followerAttractor->transform.translate = {
					horizontalOffset, verticalOffset, depthOffset
				};
			}
		}
		FishCatchAnimation animation{};
		animation.groupId = slot->groupId;
		animation.entityId = effectFish->id;
		animation.effectHookEntityId = index == 0
			? slot->effectHookEntityId
			: 0;
		animation.effectHookColor = effectHookColor;
		animation.attractorEntityId = slot->followerAttractorEntityIds[index];
		animation.controlsAttractor = index == 0;
		animation.sourceWorldTransform = candidates[index].worldTransform;
		animation.sourceWorldTransform.translate = groupCenter;
		animation.screenHorizontalOffset = horizontalOffset;
		animation.depthOffset = depthOffset;
		animation.verticalOffset = verticalOffset;
		fishCatchAnimations_.push_back(std::move(animation));
	}
	slot->lastUsedSeconds = fishCatchEffectPoolElapsedSeconds_;
}

bool SceneFishingScoreAttackSystem::IsPlayerMovementAllowed() const {
	if (!hasDirector_) {
		return true;
	}
	if (state_ != SceneFishingScoreAttackState::Navigating) {
		return false;
	}
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Disabled:
	case SceneFishingScoreAttackTutorialStep::MovePractice:
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
	case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
	case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
	case SceneFishingScoreAttackTutorialStep::FreePlay:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// チュートリアル中にカメラ操作を受け付けるかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsCameraControlAllowed() const {
	return tutorialStep_ == SceneFishingScoreAttackTutorialStep::Disabled ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::MovePractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreOnePractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreMultiPractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::FreePlay;
}

bool SceneFishingScoreAttackSystem::AcceptWheelZoom() const {
	if (!hasDirector_) {
		return true;
	}
	if (state_ != SceneFishingScoreAttackState::Navigating) {
		return false;
	}
	return tutorialStep_ == SceneFishingScoreAttackTutorialStep::Disabled ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::MovePractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreOnePractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreMultiPractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::FreePlay;
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
		(tutorialStep_ != SceneFishingScoreAttackTutorialStep::Disabled &&
			!IsTutorialFishSelectionAllowed()) ||
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

bool SceneFishingScoreAttackSystem::RequestPlayerRespawn() {
	if (!hasInitialPlayerTransform_) {
		return false;
	}
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	ResetFormationContactResponse();
	hasPlayerResetRequest_ = true;
	return true;
}

void SceneFishingScoreAttackSystem::ResetFormationContactResponse() {
	formationContactResponseActive_ = false;
	formationContactResponseNormal_ = {};
	formationContactResponseTargetYaw_ = 0.0f;
	formationContactResponseRemainingSeconds_ = 0.0f;
	formationContactResponseCooldownSeconds_ = 0.0f;
}

void SceneFishingScoreAttackSystem::EndFormationContactResponse(
	float cooldownSeconds
) {
	formationContactResponseActive_ = false;
	formationContactResponseNormal_ = {};
	formationContactResponseTargetYaw_ = 0.0f;
	formationContactResponseRemainingSeconds_ = 0.0f;
	formationContactResponseCooldownSeconds_ = std::isfinite(cooldownSeconds)
		? std::clamp(cooldownSeconds, 0.0f, 2.0f)
		: 0.0f;
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
	ResetFormationContactResponse();
	hasPlayerResetRequest_ = false;
	return true;
}

void SceneFishingScoreAttackSystem::UpdateFormationParticleEffect(
	const SceneDocument& document,
	const SceneAgentSystem& agentSystem,
	float deltaTime,
	const std::function<bool(uint64_t)>& shouldProcessWorldEffects,
	const std::string& pauseOwnerKey
) {
	constexpr const char* kGroupName = "FishingFormationCloudCpu";
	constexpr const char* kTexturePath = "resources/circleEntity.png";
	ParticleManager* particleManager = ParticleManager::GetInstance();
	const std::string effectivePauseOwnerKey = "formation:" + pauseOwnerKey;
	if (!formationParticlePauseOwnerKey_.empty() &&
		formationParticlePauseOwnerKey_ != effectivePauseOwnerKey) {
		particleManager->SetParticleGroupSimulationPaused(
			kGroupName, formationParticlePauseOwnerKey_, false
		);
	}
	formationParticlePauseOwnerKey_ = effectivePauseOwnerKey;
	auto setPaused = [&](bool paused) {
		particleManager->SetParticleGroupSimulationPaused(
			kGroupName, formationParticlePauseOwnerKey_, paused
		);
	};
	auto stop = [&]() {
		setPaused(false);
		particleManager->ClearParticleGroup(kGroupName);
		particleManager->ClearParticleGroupParentTransform(kGroupName);
		formationParticleEmissionAccumulator_ = 0.0f;
		formationParticlePointCursor_ = 0;
		formationParticleActive_ = false;
	};
	if (formationParticleTuningDirty_) {
		stop();
		formationParticleTuningDirty_ = false;
	}

	if (state_ != SceneFishingScoreAttackState::Navigating || selectedFishCount_ < 1) {
		if (formationParticleActive_ || particleManager->HasParticleGroup(kGroupName)) {
			stop();
		}
		return;
	}
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director || !director->fishingFormationOutlineVisible ||
		!std::isfinite(director->fishingFormationOutlineYOffset)) {
		stop();
		return;
	}
	if (shouldProcessWorldEffects && !shouldProcessWorldEffects(directorEntity->id)) {
		setPaused(true);
		return;
	}
	setPaused(false);
	LoadFormationParticleTuning(*director);
	FormationCapsule capsule{};
	if (!TryGetPlayerFormationCapsule(document, *director, agentSystem, capsule)) {
		stop();
		return;
	}
	const int outlineSegments = std::clamp(formationParticlePointCount_, 12, 128);
	const std::vector<XZPoint> points = BuildFormationParticlePoints(
		capsule.radius,
		capsule.halfSegmentLength,
		outlineSegments
	);
	if (points.empty()) {
		stop();
		return;
	}
	particleManager->CreateParticleGroupIfNeeded(kGroupName, kTexturePath);
	particleManager->SetParticleGroupTexture(kGroupName, kTexturePath);
	particleManager->SetGroupBlendMode(
		kGroupName,
		ParticleCommon::BlendMode::kBlendModeNormal
	);
	ParticleManager::ParticleRenderDesc render{};
	render.billboardMode = ParticleManager::BillboardMode::kBillboard;
	render.primitiveType = ParticleManager::PrimitiveType::kPlane;
	render.depthTest = true;
	render.depthWrite = false;
	render.emissiveIntensity = std::clamp(
		formationParticleEmissiveIntensity_,
		0.0f,
		8.0f
	);
	particleManager->SetGroupRenderDesc(kGroupName, render);
	const Vector3 parentTranslation = {
		capsule.center.x,
		capsule.center.y + director->fishingFormationOutlineYOffset,
		capsule.center.z
	};
	if (!particleManager->SetParticleGroupParentTransform(
		kGroupName,
		parentTranslation,
		capsule.yaw
	)) {
		stop();
		return;
	}
	const ParticleManager::ParticleBehavior behavior =
		BuildFormationParticleBehavior(
			formationParticleStartSize_,
			formationParticleEndSize_,
			formationParticleLifetime_,
			formationParticleStartColor_,
			formationParticleEndColor_,
			formationParticleEmissiveIntensity_
		);
	const float emitterSpread = std::clamp(
		formationParticleEmitterSpread_,
		0.0f,
		0.5f
	);
	const Vector3 emitterRandomRange = {
		emitterSpread,
		emitterSpread,
		emitterSpread
	};
	if (!formationParticleActive_) {
		for (const XZPoint& point : points) {
			particleManager->Emit(
				kGroupName,
				{ point.x, 0.0f, point.z },
				emitterRandomRange,
				formationParticleCountPerEmission_,
				behavior
			);
		}
		formationParticlePointCursor_ = 0;
		formationParticleEmissionAccumulator_ = 0.0f;
		formationParticleActive_ = true;
	}
	if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) {
		return;
	}
	const float interval = (std::max)(
		0.001f,
		0.80f / static_cast<float>(points.size())
	);
	formationParticleEmissionAccumulator_ = (std::min)(
		formationParticleEmissionAccumulator_ + deltaTime,
		interval * 8.0f
	);
	uint32_t emitCount = static_cast<uint32_t>(
		formationParticleEmissionAccumulator_ / interval
	);
	emitCount = (std::min)(emitCount, 8u);
	formationParticleEmissionAccumulator_ -=
		interval * static_cast<float>(emitCount);
	for (uint32_t index = 0; index < emitCount; ++index) {
		const XZPoint& point = points[formationParticlePointCursor_ % points.size()];
		particleManager->Emit(
			kGroupName,
			{ point.x, 0.0f, point.z },
			emitterRandomRange,
			formationParticleCountPerEmission_,
			behavior
		);
		formationParticlePointCursor_ =
			(formationParticlePointCursor_ + 1) % points.size();
	}
}

void SceneFishingScoreAttackSystem::DrawFormationParticleTuningImGui(
	const SceneDocument& document,
	bool runtimeControlsEnabled
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (ImGuiWindow* inspectorWindow = ImGui::FindWindowByName("Inspector")) {
		if (ImGuiDockNode* dockNode = inspectorWindow->DockNode) {
			ImGui::SetNextWindowDockID(dockNode->ID, ImGuiCond_FirstUseEver);
		}
	}
	if (!ImGui::Begin(
		"Formation Particle Tuning###FormationParticleTuningDocked",
		nullptr,
		ImGuiWindowFlags_NoFocusOnAppearing
	)) {
		ImGui::End();
		return;
	}

	uint64_t directorEntityId = 0;
	bool duplicateDirector = false;
	const SceneComponent* director = FindDirector(
		document,
		directorEntityId,
		duplicateDirector
	);
	if (!director || duplicateDirector) {
		ImGui::TextDisabled("FishingScoreAttackDirector is not available.");
		ImGui::End();
		return;
	}
	if (!runtimeControlsEnabled) {
		formationParticlePointCount_ = 0;
	}
	LoadFormationParticleTuning(*director);
	bool changed = false;
	if (!runtimeControlsEnabled) {
		ImGui::TextDisabled("Available during Play or Pause.");
	}
	ImGui::BeginDisabled(!runtimeControlsEnabled);
	changed |= ImGui::SliderInt(
		"Outline Points",
		&formationParticlePointCount_,
		12,
		128
	);
	changed |= ImGui::DragFloat(
		"Start Size",
		&formationParticleStartSize_,
		0.01f,
		0.01f,
		5.0f,
		"%.2f"
	);
	changed |= ImGui::DragFloat(
		"End Size",
		&formationParticleEndSize_,
		0.01f,
		0.01f,
		5.0f,
		"%.2f"
	);
	int countPerEmission = static_cast<int>(formationParticleCountPerEmission_);
	if (ImGui::SliderInt("Particles Per Emission", &countPerEmission, 1, 16)) {
		formationParticleCountPerEmission_ = static_cast<uint32_t>(countPerEmission);
		changed = true;
	}
	changed |= ImGui::DragFloat(
		"Emitter Spread",
		&formationParticleEmitterSpread_,
		0.005f,
		0.0f,
		0.5f,
		"%.3f"
	);
	changed |= ImGui::DragFloat(
		"Lifetime",
		&formationParticleLifetime_,
		0.01f,
		0.1f,
		3.0f,
		"%.2f s"
	);
	changed |= ImGui::ColorEdit4(
		"Start Color",
		&formationParticleStartColor_.x
	);
	changed |= ImGui::ColorEdit4(
		"End Color",
		&formationParticleEndColor_.x
	);
	changed |= ImGui::DragFloat(
		"Emissive Intensity",
		&formationParticleEmissiveIntensity_,
		0.05f,
		0.0f,
		8.0f,
		"%.2f"
	);
	if (ImGui::Button("Reset")) {
		formationParticlePointCount_ = 48;
		formationParticleStartSize_ = 0.26f;
		formationParticleEndSize_ = 0.43f;
		formationParticleCountPerEmission_ = 1;
		formationParticleEmitterSpread_ = 0.0f;
		formationParticleLifetime_ = 0.8f;
		formationParticleStartColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
		formationParticleEndColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
		formationParticleEmissiveIntensity_ = 1.0f;
		changed = true;
	}
	if (ImGui::Button("Save to Scene")) {
		formationParticleSaveRequest_.directorEntityId = directorEntityId;
		formationParticleSaveRequest_.pointCount = std::clamp(
			formationParticlePointCount_,
			12,
			128
		);
		formationParticleSaveRequest_.startSize = std::clamp(
			formationParticleStartSize_,
			0.01f,
			5.0f
		);
		formationParticleSaveRequest_.endSize = std::clamp(
			formationParticleEndSize_,
			0.01f,
			5.0f
		);
		formationParticleSaveRequest_.countPerEmission = std::clamp(
			formationParticleCountPerEmission_,
			1u,
			16u
		);
		formationParticleSaveRequest_.emitterSpread = std::clamp(
			formationParticleEmitterSpread_,
			0.0f,
			0.5f
		);
		formationParticleSaveRequest_.lifetime = std::clamp(
			formationParticleLifetime_,
			0.1f,
			3.0f
		);
		formationParticleSaveRequest_.startColor = formationParticleStartColor_;
		formationParticleSaveRequest_.endColor = formationParticleEndColor_;
		formationParticleSaveRequest_.emissiveIntensity = std::clamp(
			formationParticleEmissiveIntensity_,
			0.0f,
			8.0f
		);
		formationParticleSaveRequested_ = true;
		formationParticleSaveStatus_ = "Save requested.";
		formationParticleSaveStatusIsError_ = false;
	}
	ImGui::EndDisabled();
	ImGui::TextUnformatted(
		"Runtime tuning. Save to Scene writes only these particle settings."
	);
	if (!formationParticleSaveStatus_.empty()) {
		if (formationParticleSaveStatusIsError_) {
			ImGui::TextColored(
				ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"%s",
				formationParticleSaveStatus_.c_str()
			);
		} else {
			ImGui::TextDisabled("%s", formationParticleSaveStatus_.c_str());
		}
	}
	ImGui::End();
	if (changed) {
		formationParticlePointCount_ = std::clamp(
			formationParticlePointCount_,
			12,
			128
		);
		formationParticleStartSize_ = std::clamp(
			formationParticleStartSize_,
			0.01f,
			5.0f
		);
		formationParticleEndSize_ = std::clamp(
			formationParticleEndSize_,
			0.01f,
			5.0f
		);
		formationParticleCountPerEmission_ = std::clamp(
			formationParticleCountPerEmission_,
			1u,
			16u
		);
		formationParticleEmitterSpread_ = std::clamp(
			formationParticleEmitterSpread_,
			0.0f,
			0.5f
		);
		formationParticleLifetime_ = std::clamp(
			formationParticleLifetime_,
			0.1f,
			3.0f
		);
		formationParticleStartColor_.x = std::clamp(
			formationParticleStartColor_.x, 0.0f, 1.0f
		);
		formationParticleStartColor_.y = std::clamp(
			formationParticleStartColor_.y, 0.0f, 1.0f
		);
		formationParticleStartColor_.z = std::clamp(
			formationParticleStartColor_.z, 0.0f, 1.0f
		);
		formationParticleStartColor_.w = std::clamp(
			formationParticleStartColor_.w, 0.0f, 1.0f
		);
		formationParticleEndColor_.x = std::clamp(
			formationParticleEndColor_.x, 0.0f, 1.0f
		);
		formationParticleEndColor_.y = std::clamp(
			formationParticleEndColor_.y, 0.0f, 1.0f
		);
		formationParticleEndColor_.z = std::clamp(
			formationParticleEndColor_.z, 0.0f, 1.0f
		);
		formationParticleEndColor_.w = std::clamp(
			formationParticleEndColor_.w, 0.0f, 1.0f
		);
		formationParticleEmissiveIntensity_ = std::clamp(
			formationParticleEmissiveIntensity_,
			0.0f,
			8.0f
		);
		formationParticleTuningDirty_ = true;
	}
#else
	(void)document;
	(void)runtimeControlsEnabled;
#endif
}

bool SceneFishingScoreAttackSystem::ConsumeFormationParticleSaveRequest(
	SceneFishingScoreAttackFormationParticleSaveRequest& request
) {
	if (!formationParticleSaveRequested_) {
		return false;
	}
	request = formationParticleSaveRequest_;
	formationParticleSaveRequested_ = false;
	return true;
}

bool SceneFishingScoreAttackSystem::ConsumeResultSessionBeginRequest(
	SceneFishingScoreAttackSessionBeginRequest& request
) {
	if (!resultSessionBeginRequested_) {
		return false;
	}
	request = resultSessionBeginRequest_;
	resultSessionBeginRequested_ = false;
	return true;
}

bool SceneFishingScoreAttackSystem::ConsumeResultSessionPublishRequest(
	SceneFishingScoreAttackSessionPublishRequest& request
) {
	if (!resultSessionPublishRequested_) {
		return false;
	}
	request = std::move(resultSessionPublishRequest_);
	resultSessionPublishRequested_ = false;
	return true;
}

void SceneFishingScoreAttackSystem::SetFormationParticleSaveResult(
	bool success,
	std::string message
) {
	formationParticleSaveStatusIsError_ = !success;
	formationParticleSaveStatus_ = std::move(message);
}

void SceneFishingScoreAttackSystem::LoadFormationParticleTuning(
	const SceneComponent& director
) {
	if (formationParticlePointCount_ > 0) {
		return;
	}
	formationParticlePointCount_ = std::clamp(
		director.fishingFormationParticlePointCount,
		12,
		128
	);
	formationParticleStartSize_ = std::clamp(
		director.fishingFormationParticleStartSize,
		0.01f,
		5.0f
	);
	formationParticleEndSize_ = std::clamp(
		director.fishingFormationParticleEndSize,
		0.01f,
		5.0f
	);
	formationParticleCountPerEmission_ = static_cast<uint32_t>(std::clamp(
		director.fishingFormationParticleCountPerEmission,
		1,
		16
	));
	formationParticleEmitterSpread_ = std::clamp(
		director.fishingFormationParticleEmitterSpread,
		0.0f,
		0.5f
	);
	formationParticleLifetime_ = std::clamp(
		director.fishingFormationParticleLifetime,
		0.1f,
		3.0f
	);
	const auto sanitizeColor = [](const Vector4& color) {
		return Vector4{
			std::isfinite(color.x) ? std::clamp(color.x, 0.0f, 1.0f) : 0.1f,
			std::isfinite(color.y) ? std::clamp(color.y, 0.0f, 1.0f) : 0.9f,
			std::isfinite(color.z) ? std::clamp(color.z, 0.0f, 1.0f) : 1.0f,
			std::isfinite(color.w) ? std::clamp(color.w, 0.0f, 1.0f) : 0.65f
		};
	};
	formationParticleStartColor_ = sanitizeColor(
		director.fishingFormationParticleStartColor
	);
	formationParticleEndColor_ = sanitizeColor(
		director.fishingFormationParticleEndColor
	);
	formationParticleEmissiveIntensity_ = std::clamp(
		director.fishingFormationParticleEmissiveIntensity,
		0.0f,
		8.0f
	);
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

	const std::vector<XZPoint> points = BuildFormationOutlinePoints(
		capsule.radius,
		capsule.halfSegmentLength,
		outlineSegments
	);
	for (size_t index = 0; index < points.size(); ++index) {
		addLine(points[index], points[(index + 1) % points.size()]);
	}
}

void SceneFishingScoreAttackSystem::AddSharkNavigationDebugDraw(
	const SceneDocument& document
) const {
	if (state_ != SceneFishingScoreAttackState::Navigating) {
		return;
	}
	const SceneEntity* directorEntity = document.FindEntity(directorEntityId_);
	const SceneComponent* director = directorEntity
		? FindEnabledComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director || !director->fishingSharkRouteDebugVisible) {
		return;
	}
	DebugRenderer* debugRenderer = DebugRenderer::GetInstance();
	if (!debugRenderer) {
		return;
	}
	for (const auto& [entityId, runtime] : sharkRuntimes_) {
		const SceneEntity* entity = document.FindEntity(entityId);
		if (!entity || !IsEntityActiveInHierarchy(document, *entity) ||
			runtime.navigationRoute.empty()) {
			continue;
		}
		Vector4 routeColor = { 0.1f, 0.9f, 1.0f, 1.0f };
		switch (runtime.navigationState) {
		case SharkNavigationState::Alert:
			routeColor = { 1.0f, 0.9f, 0.1f, 1.0f };
			break;
		case SharkNavigationState::Chase:
			routeColor = { 1.0f, 0.15f, 0.1f, 1.0f };
			break;
		case SharkNavigationState::Lost:
			routeColor = { 1.0f, 0.45f, 0.1f, 1.0f };
			break;
		case SharkNavigationState::Patrol:
			if (runtime.reacquireCooldownRemainingSeconds > 0.0f) {
				routeColor = { 0.5f, 0.5f, 0.5f, 1.0f };
			}
			break;
		}
		const Vector3 currentPosition = entity->transform.translate;
		Vector3 previous = currentPosition;
		for (size_t index = runtime.navigationRouteIndex;
			index < runtime.navigationRoute.size(); ++index) {
			const Vector3& waypoint = runtime.navigationRoute[index];
			debugRenderer->AddLine(previous, waypoint, routeColor);
			debugRenderer->AddSphere(waypoint, 0.2f, routeColor);
			previous = waypoint;
		}
		Vector3 targetPosition{};
		bool hasTarget = false;
		if (runtime.navigationState == SharkNavigationState::Chase) {
			const SceneEntity* player = document.FindEntity(
				director->fishingPlayerEntityId
			);
			if (player && IsEntityActiveInHierarchy(document, *player)) {
				targetPosition = SceneTransformResolver::ResolveScene3DTransform(
					document, *player
				).translate;
				hasTarget = true;
			}
		} else if (runtime.navigationState == SharkNavigationState::Lost &&
			runtime.hasLastSeenPlayerPosition) {
			targetPosition = runtime.lastSeenPlayerPosition;
			hasTarget = true;
		} else if (runtime.navigationRouteIndex < runtime.navigationRoute.size()) {
			targetPosition = runtime.navigationRoute[runtime.navigationRouteIndex];
			hasTarget = true;
		}
		if (hasTarget) {
			debugRenderer->AddSphere(targetPosition, 0.35f, routeColor);
		}
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
		director.fishingHookRankCount < 1 ||
		director.fishingHookRankCount > 10 ||
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
			float activeWeight = 0.0f;
			for (size_t tierIndex = 0;
				tierIndex < band.hookMultiplierWeights.size(); ++tierIndex) {
				const float weight = band.hookMultiplierWeights[tierIndex];
				if (!IsFiniteNonNegative(weight)) {
					diagnostic = "FishingScoreAttackDirector hook tier weights are invalid";
					return false;
				}
				if (tierIndex < static_cast<size_t>(director.fishingHookRankCount)) {
					activeWeight += weight;
				}
			}
			if (band.hookCount > 0 && (!std::isfinite(activeWeight) || activeWeight <= 0.0f)) {
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
				!std::isfinite(rank.scoreMultiplier) ||
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
	if (const SceneComponent* tracker = FindResultTracker(
		document, directorEntityId
	)) {
		if (!useHookBandSettings || tracker->fishingResultChannelId.empty() ||
			(tracker->fishingResultTieBreakMode != "HigherRank" &&
			 tracker->fishingResultTieBreakMode != "LowerRank")) {
			diagnostic = "FishingResultTracker settings are invalid";
			return false;
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
	const std::array<uint64_t, 4> boundaryWallIds = {
		director.fishingBoundaryNegativeXWallEntityId,
		director.fishingBoundaryPositiveXWallEntityId,
		director.fishingBoundaryNegativeZWallEntityId,
		director.fishingBoundaryPositiveZWallEntityId
	};
	const bool hasAnyBoundaryWall = std::any_of(
		boundaryWallIds.begin(), boundaryWallIds.end(),
		[](uint64_t entityId) { return entityId != 0; }
	);
	if (hasAnyBoundaryWall && !std::all_of(
		boundaryWallIds.begin(), boundaryWallIds.end(),
		[](uint64_t entityId) { return entityId != 0; }
	)) {
		diagnostic = "Fishing boundary walls must be all configured or all unset";
		return false;
	}
	if (hasAnyBoundaryWall) {
		std::unordered_set<uint64_t> uniqueBoundaryWalls;
		for (uint64_t boundaryWallId : boundaryWallIds) {
			const SceneEntity* boundaryWall = document.FindEntity(boundaryWallId);
			const SceneComponent* boundaryCollider = boundaryWall
				? FindEnabledComponent(*boundaryWall, "OBBCollider")
				: nullptr;
			if (!uniqueBoundaryWalls.insert(boundaryWallId).second ||
				!boundaryWall ||
				!IsEntityActiveInHierarchy(document, *boundaryWall) ||
				!boundaryCollider || !boundaryCollider->colliderActive ||
				boundaryCollider->colliderIsTrigger ||
				boundaryCollider->colliderShape != "Box") {
				diagnostic = "Fishing boundary walls require unique active non-trigger Box Colliders";
				return false;
			}
		}
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
		if (
			!std::isfinite(director.fishingFormationSlideAssistStrength) ||
			director.fishingFormationSlideAssistStrength < 0.0f ||
			director.fishingFormationSlideAssistStrength > 1.0f
		) {
			diagnostic =
				"Fishing formation slide assist strength must be between 0 and 1";
			return false;
		}
		if (
			director.fishingFormationContactResponseMaxFishCount < 0 ||
			director.fishingFormationContactResponseMaxFishCount >
				director.fishingMaxSelectableFishCount ||
			!std::isfinite(director.fishingFormationContactTurnSpeedDegrees) ||
			director.fishingFormationContactTurnSpeedDegrees < 0.0f ||
			director.fishingFormationContactTurnSpeedDegrees > 720.0f ||
			!std::isfinite(director.fishingFormationContactPushSpeed) ||
			director.fishingFormationContactPushSpeed < 0.0f ||
			director.fishingFormationContactPushSpeed > 200.0f ||
			!std::isfinite(director.fishingFormationContactDurationSeconds) ||
			director.fishingFormationContactDurationSeconds < 0.0f ||
			director.fishingFormationContactDurationSeconds > 2.0f ||
			!std::isfinite(director.fishingFormationContactCooldownSeconds) ||
			director.fishingFormationContactCooldownSeconds < 0.0f ||
			director.fishingFormationContactCooldownSeconds > 2.0f
		) {
			diagnostic =
				"Fishing formation contact response values are outside their valid ranges";
			return false;
		}
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

void SceneFishingScoreAttackSystem::InitializeResultTracking(
	const SceneDocument& document,
	const SceneComponent& director
) {
	resultTrackingEnabled_ = false;
	resultChannelId_.clear();
	resultTieBreakMode_ = "HigherRank";
	resultRankRecords_.clear();
	sharkHitCount_ = 0;
	sharkFishWeightedCount_ = 0;
	resultSessionBeginRequested_ = false;
	resultSessionBeginRequest_ = {};
	resultSessionPublishRequested_ = false;
	resultSessionPublishRequest_ = {};

	const SceneComponent* tracker = FindResultTracker(
		document, directorEntityId_
	);
	if (!tracker) {
		return;
	}
	resultTrackingEnabled_ = true;
	resultChannelId_ = tracker->fishingResultChannelId;
	resultTieBreakMode_ = tracker->fishingResultTieBreakMode;
	const int rankCapacity = 10;
	resultRankRecords_.reserve(rankCapacity);
	for (int rankIndex = 0; rankIndex < rankCapacity; ++rankIndex) {
		SceneFishingHookRankDefinition rank{};
		if (static_cast<size_t>(rankIndex) < director.fishingHookRanks.size()) {
			rank = director.fishingHookRanks[static_cast<size_t>(rankIndex)];
		}
		SceneFishingResultRankRecord record{};
		record.rankId = rank.id.empty()
			? "rank_" + std::to_string(rankIndex + 1)
			: rank.id;
		record.displayName = rank.displayName;
		record.scoreMultiplier = rank.scoreMultiplier;
		record.color = rank.color;
		record.iconTexturePath = rank.iconTexturePath;
		resultRankRecords_.push_back(std::move(record));
	}
	resultSessionBeginRequested_ = true;
	resultSessionBeginRequest_.channelId = resultChannelId_;
}

/// <summary>
/// チュートリアル対象シーンかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialScene(
	const std::string& sceneId
) const {
	return sceneId == "tutorial" || sceneId == "TUTORIAL";
}

/// <summary>
/// 現在のチュートリアル段階で説明送り入力を受け付けるかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialAdvanceStep() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Overview:
	case SceneFishingScoreAttackTutorialStep::MoveExplanation:
	case SceneFishingScoreAttackTutorialStep::HookExplanation:
	case SceneFishingScoreAttackTutorialStep::FishCountExplanation:
	case SceneFishingScoreAttackTutorialStep::SharkExplanation:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// 現在のチュートリアル段階で魚数選択入力を受け付けるかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialFishSelectionAllowed() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Disabled:
	case SceneFishingScoreAttackTutorialStep::FishCountPractice:
	case SceneFishingScoreAttackTutorialStep::FreePlay:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// 現在のチュートリアル段階で釣り針得点判定を受け付けるかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialScoringAllowed() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Disabled:
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
	case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
	case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
	case SceneFishingScoreAttackTutorialStep::FreePlay:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// 現在のチュートリアル段階でサメ処理を動かすかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialSharkAllowed() const {
	return tutorialStep_ == SceneFishingScoreAttackTutorialStep::Disabled ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::FreePlay;
}

/// <summary>
/// 現在のチュートリアル段階でタイマーを進めるかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialTimerAllowed() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Disabled:
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
	case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
	case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
	case SceneFishingScoreAttackTutorialStep::FreePlay:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// 現在のチュートリアル段階で釣り針の生成数を1本に絞るかを判定する。
/// </summary>
bool SceneFishingScoreAttackSystem::IsTutorialSingleHookSpawnStep() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Overview:
	case SceneFishingScoreAttackTutorialStep::MoveExplanation:
	case SceneFishingScoreAttackTutorialStep::MovePractice:
	case SceneFishingScoreAttackTutorialStep::HookExplanation:
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
		return true;
	default:
		return false;
	}
}

/// <summary>
/// チュートリアル説明送り入力を処理する。
/// </summary>
bool SceneFishingScoreAttackSystem::AdvanceTutorialByInput(
	SceneDocument& document,
	const SceneComponent& director
) {
	if (!IsTutorialAdvanceStep() || !IsTutorialAdvanceInputTriggered()) {
		return false;
	}

	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Overview:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::MoveExplanation;
		break;
	case SceneFishingScoreAttackTutorialStep::MoveExplanation:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::MovePractice;
		tutorialMovePracticeSeconds_ = 0.0f;
		StartRound(document, director);
		timerRunning_ = false;
		break;
	case SceneFishingScoreAttackTutorialStep::HookExplanation:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::ScoreOnePractice;
		timerRunning_ = true;
		break;
	case SceneFishingScoreAttackTutorialStep::FishCountExplanation:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::FishCountPractice;
		tutorialFishCountPracticeStart_ = selectedFishCount_;
		tutorialFishCountAdjusted_ = false;
		break;
	case SceneFishingScoreAttackTutorialStep::SharkExplanation:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::FreePlay;
		StartRound(document, director);
		timerRunning_ = true;
		break;
	default:
		break;
	}
	return true;
}

/// <summary>
/// チュートリアルの得点成功を段階へ反映する。
/// </summary>
void SceneFishingScoreAttackSystem::NotifyTutorialHookScored() {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::ScoreMultiPractice;
		tutorialMultiScoreCount_ = 1;
		tutorialAutoStartNextRound_ = true;
		return;
	case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
		++tutorialMultiScoreCount_;
		if (tutorialMultiScoreCount_ >= kTutorialMultiScoreRequiredCount) {
			tutorialStep_ = SceneFishingScoreAttackTutorialStep::FishCountExplanation;
			tutorialAutoStartNextRound_ = false;
			return;
		}
		tutorialAutoStartNextRound_ = true;
		return;
	case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
		++tutorialMultiScoreCount_;
		if (tutorialMultiScoreCount_ >= kTutorialMultiScoreRequiredCount) {
			tutorialStep_ = SceneFishingScoreAttackTutorialStep::SharkExplanation;
			tutorialAutoStartNextRound_ = false;
			return;
		}
		tutorialAutoStartNextRound_ = true;
		return;
	default:
		break;
	}
}

/// <summary>
/// 現在のチュートリアル説明文を取得する。
/// </summary>
std::string SceneFishingScoreAttackSystem::GetTutorialMessage() const {
	switch (tutorialStep_) {
	case SceneFishingScoreAttackTutorialStep::Overview:
		return "プレイヤーはツナ缶になることを\n"
			"とても栄誉に思っているぞ！ ENTER / Y";
	case SceneFishingScoreAttackTutorialStep::MoveExplanation:
		return "WASD / 左スティックで移動 ENTER / Y";
	case SceneFishingScoreAttackTutorialStep::MovePractice:
		return "移動して水中を進んでみよう";
	case SceneFishingScoreAttackTutorialStep::HookExplanation:
		return "釣り針に触れると得点になる ENTER / Y";
	case SceneFishingScoreAttackTutorialStep::ScoreOnePractice:
		return "釣り針を1つ取ってみよう";
	case SceneFishingScoreAttackTutorialStep::FishCountExplanation:
		return "魚を増やすと得点も増える ENTER / Y";
	case SceneFishingScoreAttackTutorialStep::FishCountPractice:
		return "ホイール/上下キーで魚数を変えて SPACE / ENTER";
	case SceneFishingScoreAttackTutorialStep::ScoreMultiPractice:
		return "1匹のまま何度か取ってみよう";
	case SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice:
		return "増やした魚数で何度か取ってみよう";
	case SceneFishingScoreAttackTutorialStep::SharkExplanation:
		return "サメに当たると減点される ENTER / Y";
	default:
		return {};
	}
}

void SceneFishingScoreAttackSystem::InitializeRun(
	SceneDocument& document,
	const SceneComponent& director,
	bool tutorialScene
) {
	ResetFormationContactResponse();
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
	tutorialStep_ = tutorialScene
		? SceneFishingScoreAttackTutorialStep::Overview
		: SceneFishingScoreAttackTutorialStep::Disabled;
	tutorialMovePracticeSeconds_ = 0.0f;
	tutorialFishCountPracticeStart_ = selectedFishCount_;
	tutorialFishCountAdjusted_ = false;
	tutorialMultiScoreCount_ = 0;
	tutorialAutoStartNextRound_ = false;
	InitializeResultTracking(document, director);
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
		playerPlanarColliderRadius_ = playerRadius;
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
	formationRecoveryPoses_.clear();
	formationNoProgressReferencePosition_ = {};
	formationNoProgressReferenceYaw_ = 0.0f;
	formationNoProgressSeconds_ = 0.0f;
	hasFormationNoProgressReference_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	DeactivatePoolHooks(document, director);
	if (!SpawnHooks(document, director)) {
		return;
	}
	if (!ResetSharksForRound(document, director)) {
		return;
	}
	SetFishPreview(document, director);
	state_ = SceneFishingScoreAttackState::SelectingInitial;
	BuildTextRequests(document, director);
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
		SceneSoundEffectPlayer::PlayFishSelect();
		if (tutorialStep_ ==
			SceneFishingScoreAttackTutorialStep::FishCountPractice &&
			selectedFishCount_ != tutorialFishCountPracticeStart_) {
			tutorialFishCountAdjusted_ = true;
		}
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
	if (tutorialStep_ == SceneFishingScoreAttackTutorialStep::FishCountPractice) {
		if (!tutorialFishCountAdjusted_) {
			return;
		}
		tutorialStep_ = SceneFishingScoreAttackTutorialStep::ScoreAdjustedPractice;
		tutorialMultiScoreCount_ = 0;
	}
	SceneSoundEffectPlayer::PlayDecision();
	StartRound(document, director);
}

bool SceneFishingScoreAttackSystem::SpawnHooks(
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
		Fault(document, director, "Fishing hook spawn references became invalid");
		return false;
	}

	const Transform playerTransform = hasInitialPlayerTransform_
		? initialPlayerTransform_
		: SceneTransformResolver::ResolveScene3DTransform(document, *player);
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
	const bool restrictTutorialHookDistance =
		useHookBandSettings &&
		tutorialStep_ != SceneFishingScoreAttackTutorialStep::Disabled &&
		tutorialStep_ != SceneFishingScoreAttackTutorialStep::FreePlay; // チュートリアル中に近距離帯を除外するか。
	const int tutorialHookSpawnLimit =
		IsTutorialSingleHookSpawnStep()
			? 1
			: (std::numeric_limits<int>::max)(); // 序盤チュートリアルで生成する釣り針の上限。
	const int tutorialMinimumHookBand =
		distanceBandCount > kTutorialMinimumHookDistanceBand
			? kTutorialMinimumHookDistanceBand
			: distanceBandCount - 1; // チュートリアル中に残す最初の距離帯。
	const bool requirePositiveTutorialHookRank =
		RequiresPositiveTutorialHookRank(tutorialStep_); // チュートリアル中にマイナス得点ランクを除外するか。
	int spawnedHookCount = 0; // 今回の配置で有効化した釣り針数。
	for (int bandIndex = 0; bandIndex < distanceBandCount; ++bandIndex) {
		if (spawnedHookCount >= tutorialHookSpawnLimit) {
			break;
		}
		const bool skipTutorialNearHookBand =
			restrictTutorialHookDistance &&
			bandIndex < tutorialMinimumHookBand; // チュートリアル中に使わない近距離帯か。
		if (skipTutorialNearHookBand) {
			continue;
		}
		const int hooksInBand = useHookBandSettings
			? director.fishingHookBands[static_cast<size_t>(bandIndex)].hookCount
			: director.fishingHooksPerDistanceBand;
		for (int hookIndex = 0; hookIndex < hooksInBand; ++hookIndex) {
			if (spawnedHookCount >= tutorialHookSpawnLimit) {
				break;
			}
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
					return false;
				}
				std::uniform_int_distribution<size_t> entryDistribution(
					0, availableEntryIndices.size() - 1
				);
				selectedEntry = &pool->fishingHookPoolEntries[
					availableEntryIndices[entryDistribution(random_)]
				];
				const std::vector<float>& tierWeights =
					director.fishingHookBands[static_cast<size_t>(bandIndex)].hookMultiplierWeights;
				const size_t activeRankCount = static_cast<size_t>(std::clamp(
					director.fishingHookRankCount, 1, 10
				));
				float totalWeight = 0.0f;
				for (size_t tierIndex = 0; tierIndex < activeRankCount; ++tierIndex) {
					if (
						requirePositiveTutorialHookRank &&
						!HasPositiveHookScoreRank(director, tierIndex)
					) {
						continue;
					}
					totalWeight += tierWeights[tierIndex];
				}
				if (!std::isfinite(totalWeight) || totalWeight <= 0.0f) {
					Fault(document, director, "FishingHookBand has no selectable tutorial tier");
					return false;
				}
				std::uniform_real_distribution<float> weightDistribution(0.0f, totalWeight);
				float remainingWeight = weightDistribution(random_);
				int selectedTierIndex = -1;
				for (size_t tierIndex = 0; tierIndex < activeRankCount; ++tierIndex) {
					if (
						requirePositiveTutorialHookRank &&
						!HasPositiveHookScoreRank(director, tierIndex)
					) {
						continue;
					}
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
					for (int tierIndex = static_cast<int>(activeRankCount) - 1;
						tierIndex >= 0; --tierIndex) {
						const size_t fallbackTierIndex =
							static_cast<size_t>(tierIndex); // 抽選漏れ時に使う候補ランク。
						if (
							requirePositiveTutorialHookRank &&
							!HasPositiveHookScoreRank(director, fallbackTierIndex)
						) {
							continue;
						}
						if (tierWeights[fallbackTierIndex] > 0.0f) {
							selectedTierIndex = tierIndex;
							break;
						}
					}
				}
				if (selectedTierIndex < 0) {
					Fault(document, director, "FishingHookBand has no selectable tier");
					return false;
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
					return false;
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
					return false;
				}
			}
			const SceneComponent* hookCollider = FindComponent(
				document, selectedEntry->hookEntityId, "OBBCollider"
			);
			const SceneEntity* hookEntityForPlacement = document.FindEntity(
				selectedEntry->hookEntityId
			);
			if (!hookEntityForPlacement) {
				Fault(document, director, "Selected FishingHook is missing");
				return false;
			}
			const Transform hookTransform =
				SceneTransformResolver::ResolveScene3DTransform(
					document, *hookEntityForPlacement
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
				Transform candidateHookTransform = hookTransform;
				candidateHookTransform.translate = candidate;
				const Vector3 candidateHookCenter = hookCollider
					? ColliderCenterXZ(candidateHookTransform, *hookCollider)
					: candidate;
				const float hookRadius = hookCollider
					? ColliderRadiusXZ(*hookCollider, candidateHookTransform)
					: 0.0f;
				bool overlaps = false;
				for (size_t existingIndex = 0; existingIndex < spawnPositions.size(); ++existingIndex) {
					if (DistanceXZ(
						candidateHookCenter, spawnPositions[existingIndex]
					) < hookRadius + spawnRadii[existingIndex]) {
						overlaps = true;
						break;
					}
				}
				for (const SceneEntity& entity : document.GetEntities()) {
					if (!IsEntityActiveInHierarchy(document, entity) ||
						!FindEnabledComponent(entity, "FishingObstacle")) {
						continue;
					}
					const SceneComponent* obstacleCollider = FindEnabledComponent(entity, "OBBCollider");
					if (!obstacleCollider || !obstacleCollider->colliderActive ||
						obstacleCollider->colliderIsTrigger) {
						continue;
					}
					if (IsSphereFishingObstacle(*obstacleCollider)) {
						FishingObstacleSphereGeometry sphere{};
						if (!BuildFishingObstacleSphereGeometry(
							document, entity, *obstacleCollider, sphere
						)) {
							continue;
						}
						if (DistanceXZ(candidateHookCenter, sphere.center) <
							hookRadius + sphere.radius) {
							overlaps = true;
							break;
						}
					} else {
						const Transform obstacleTransform =
							SceneTransformResolver::ResolveScene3DTransform(document, entity);
						if (DistanceXZ(candidateHookCenter, obstacleTransform.translate) <
							hookRadius + FishingObstacleSpawnExclusionRadius(
								document, *obstacleCollider, obstacleTransform
							)) {
							overlaps = true;
							break;
						}
					}
				}
				if (!overlaps) { spawnPosition = candidate; foundPosition = true; break; }
			}
			if (!foundPosition) {
				Fault(document, director, "FishingHookSpawnArea has no valid position for a distance band");
				return false;
			}
			SceneEntity* hookEntity = document.FindEntity(selectedEntry->hookEntityId);
			hookEntity->transform.translate = spawnPosition;
			hookEntity->transform.translate.y += kHookDropHeight;
			hookEntity->active = true;
			usedHookIds.insert(hookEntity->id);
			Transform placedHookTransform = hookTransform;
			placedHookTransform.translate = spawnPosition;
			spawnPositions.push_back(hookCollider
				? ColliderCenterXZ(placedHookTransform, *hookCollider)
				: spawnPosition);
			spawnRadii.push_back(hookCollider
				? ColliderRadiusXZ(*hookCollider, placedHookTransform)
				: 0.0f);
			const float distanceMultiplier = useHookBandSettings
				? director.fishingHookBands[static_cast<size_t>(bandIndex)].distanceMultiplier
				: director.fishingDistanceMultiplierBase +
					director.fishingDistanceMultiplierStep * static_cast<float>(bandIndex);
			activeHooks_.push_back({
				hookEntity->id,
				bandIndex,
				distanceMultiplier,
				hookMultiplierTier,
				spawnPosition,
				0.0f,
				true
			});
			++spawnedHookCount;
		}
	}
	return true;
}

void SceneFishingScoreAttackSystem::UpdateHookDrops(
	SceneDocument& document,
	float deltaTime
) {
	const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
	for (ActiveHook& activeHook : activeHooks_) {
		if (!activeHook.isDropping) {
			continue;
		}
		SceneEntity* hook = document.FindEntity(activeHook.entityId);
		if (!hook || !hook->active) {
			activeHook.isDropping = false;
			continue;
		}
		activeHook.dropElapsedSeconds = (std::min)(
			activeHook.dropElapsedSeconds + safeDeltaTime,
			kHookDropDurationSeconds
		);
		const float normalizedTime = std::clamp(
			activeHook.dropElapsedSeconds / kHookDropDurationSeconds,
			0.0f,
			1.0f
		);
		// 等加速度で落下させ、最後のフレームで必ず生成先へ一致させる。
		hook->transform.translate = activeHook.landingPosition;
		hook->transform.translate.y +=
			kHookDropHeight * (1.0f - normalizedTime * normalizedTime);
		if (normalizedTime >= 1.0f) {
			activeHook.isDropping = false;
		}
	}
}

void SceneFishingScoreAttackSystem::StartRound(
	SceneDocument& document,
	const SceneComponent& director
) {
	ResetFormationContactResponse();
	const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
	if (!player) {
		Fault(document, director, "Fishing player reference became invalid");
		return;
	}
	const Transform playerTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *player);
	roundFishCount_ = selectedFishCount_;
	roundDistanceBand_ = 0;
	roundMultiplier_ = director.fishingUseHookBandSettings
		? director.fishingHookBands.front().distanceMultiplier
		: director.fishingDistanceMultiplierBase;
	lastSafePlayerPlanarPosition_ = {
		playerTransform.translate.x,
		0.0f,
		playerTransform.translate.z
	};
	lastSafePlayerYaw_ = ExtractPlanarYaw(playerTransform);
	hasLastSafePlayerPlanarPosition_ = true;
	formationRecoveryPoses_.clear();
	formationRecoveryPoses_.push_back({
		lastSafePlayerPlanarPosition_, lastSafePlayerYaw_
	});
	formationNoProgressReferencePosition_ = {};
	formationNoProgressReferenceYaw_ = 0.0f;
	formationNoProgressSeconds_ = 0.0f;
	hasFormationNoProgressReference_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
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
			const SceneEntity* player = document.FindEntity(
				director.fishingPlayerEntityId
			);
			const bool navigationReady = hasWaterBounds && sharkCollider;
			const bool hasPlayer = player && IsEntityActiveInHierarchy(document, *player);
			const Transform playerTransform = hasPlayer
				? SceneTransformResolver::ResolveScene3DTransform(document, *player)
				: Transform{};
			const float detectionDistance = std::isfinite(
				shark->fishingSharkDetectionDistance
			) ? (std::max)(shark->fishingSharkDetectionDistance, 0.0f) : 30.0f;
			const float loseDistance = std::isfinite(
				shark->fishingSharkLoseDistance
			) ? (std::max)(shark->fishingSharkLoseDistance, detectionDistance) :
				detectionDistance;
			const float detectionDelay = std::isfinite(
				shark->fishingSharkDetectionDelaySeconds
			) ? (std::max)(shark->fishingSharkDetectionDelaySeconds, 0.0f) : 0.75f;
			const float lostTargetDelay = std::isfinite(
				shark->fishingSharkLostTargetDelaySeconds
			) ? (std::max)(shark->fishingSharkLostTargetDelaySeconds, 0.0f) : 2.0f;
			const float reacquireCooldown = std::isfinite(
				shark->fishingSharkReacquireCooldownSeconds
			) ? (std::max)(shark->fishingSharkReacquireCooldownSeconds, 0.0f) : 4.0f;
			const float patrolRouteInterval = std::isfinite(
				shark->fishingSharkPatrolRouteRebuildIntervalSeconds
			) ? (std::max)(shark->fishingSharkPatrolRouteRebuildIntervalSeconds, 0.001f) : 8.0f;
			const float chaseRouteInterval = std::isfinite(
				shark->fishingSharkChaseRouteRebuildIntervalSeconds
			) ? (std::max)(shark->fishingSharkChaseRouteRebuildIntervalSeconds, 0.001f) : 0.35f;
			const float waypointDistance = std::isfinite(
				shark->fishingSharkWaypointAcceptanceDistance
			) ? (std::max)(shark->fishingSharkWaypointAcceptanceDistance, 0.001f) : 1.5f;
			const float obstacleClearance = std::isfinite(
				shark->fishingSharkObstacleClearance
			) ? (std::max)(shark->fishingSharkObstacleClearance, 0.0f) : 2.0f;
			const float distanceToPlayer = hasPlayer
				? DistanceXZ(sharkTransform.translate, playerTransform.translate)
				: (std::numeric_limits<float>::max)();
			const auto enterPatrol = [&]() {
				runtime.navigationState = SharkNavigationState::Patrol;
				runtime.navigationRoute.clear();
				runtime.navigationRouteIndex = 0;
				runtime.patrolRouteRemainingSeconds = 0.0f;
				runtime.chaseRouteRemainingSeconds = 0.0f;
				runtime.alertElapsedSeconds = 0.0f;
				runtime.lostElapsedSeconds = 0.0f;
				runtime.reacquireCooldownRemainingSeconds = reacquireCooldown;
				runtime.alertPulseElapsedSeconds = 0.0f;
			};
			if (runtime.reacquireCooldownRemainingSeconds > 0.0f) {
				runtime.reacquireCooldownRemainingSeconds = (std::max)(
					runtime.reacquireCooldownRemainingSeconds - safeDeltaTime,
					0.0f
				);
			}
			switch (runtime.navigationState) {
			case SharkNavigationState::Patrol:
				if (runtime.reacquireCooldownRemainingSeconds <= 0.0f &&
					hasPlayer && distanceToPlayer <= detectionDistance) {
					runtime.navigationState = SharkNavigationState::Alert;
					runtime.alertElapsedSeconds = 0.0f;
					runtime.alertPulseElapsedSeconds = 0.0f;
					runtime.lastSeenPlayerPosition = playerTransform.translate;
					runtime.hasLastSeenPlayerPosition = true;
				}
				break;
			case SharkNavigationState::Alert:
				runtime.alertElapsedSeconds += safeDeltaTime;
				runtime.alertPulseElapsedSeconds += safeDeltaTime;
				if (!hasPlayer || distanceToPlayer > loseDistance) {
					enterPatrol();
				} else {
					runtime.lastSeenPlayerPosition = playerTransform.translate;
					runtime.hasLastSeenPlayerPosition = true;
					if (runtime.alertElapsedSeconds >= detectionDelay) {
						runtime.navigationState = SharkNavigationState::Chase;
						runtime.navigationRoute.clear();
						runtime.navigationRouteIndex = 0;
						runtime.chaseRouteRemainingSeconds = 0.0f;
						runtime.lostElapsedSeconds = 0.0f;
					}
				}
				break;
			case SharkNavigationState::Chase:
				if (!hasPlayer || distanceToPlayer > loseDistance) {
					runtime.navigationState = SharkNavigationState::Lost;
					runtime.lostElapsedSeconds = 0.0f;
					runtime.navigationRoute.clear();
					runtime.navigationRouteIndex = 0;
					runtime.chaseRouteRemainingSeconds = 0.0f;
				} else {
					runtime.lastSeenPlayerPosition = playerTransform.translate;
					runtime.hasLastSeenPlayerPosition = true;
				}
				break;
			case SharkNavigationState::Lost:
				if (hasPlayer && distanceToPlayer <= detectionDistance) {
					runtime.navigationState = SharkNavigationState::Chase;
					runtime.lostElapsedSeconds = 0.0f;
					runtime.navigationRoute.clear();
					runtime.navigationRouteIndex = 0;
					runtime.chaseRouteRemainingSeconds = 0.0f;
				} else {
					runtime.lostElapsedSeconds += safeDeltaTime;
					if (runtime.lostElapsedSeconds >= lostTargetDelay) {
						enterPatrol();
					}
				}
				break;
			}
			const bool patrolState = runtime.navigationState == SharkNavigationState::Patrol ||
				runtime.navigationState == SharkNavigationState::Alert;
			float& routeRemainingSeconds = patrolState
				? runtime.patrolRouteRemainingSeconds
				: runtime.chaseRouteRemainingSeconds;
			const float routeInterval = patrolState ? patrolRouteInterval : chaseRouteInterval;
			runtime.alertPulseElapsedSeconds = runtime.navigationState == SharkNavigationState::Alert
				? runtime.alertPulseElapsedSeconds
				: 0.0f;
			if (!navigationReady) {
				runtime.hitCooldown = (std::max)(runtime.hitCooldown - safeDeltaTime, 0.0f);
				continue;
			}
			const Vector3* routeTarget = nullptr;
			if (runtime.navigationState == SharkNavigationState::Chase && hasPlayer) {
				routeTarget = &playerTransform.translate;
			} else if (runtime.navigationState == SharkNavigationState::Lost &&
				runtime.hasLastSeenPlayerPosition) {
				routeTarget = &runtime.lastSeenPlayerPosition;
			}
			routeRemainingSeconds -= safeDeltaTime;
			if (runtime.navigationRoute.empty() ||
				runtime.navigationRouteIndex >= runtime.navigationRoute.size() ||
				routeRemainingSeconds <= 0.0f) {
				const Vector3 startPosition = sharkTransform.translate;
				const bool routeBuilt = BuildSharkNavigationRoute(
					waterBounds,
					obstacles,
					*shark,
					sharkRadius,
					startPosition,
					routeTarget,
					runtime.wanderRandom,
					runtime.navigationRoute,
					runtime.patrolVisitCounts,
					runtime.patrolGridWidth,
					runtime.patrolGridHeight,
					runtime.patrolGridOriginX,
					runtime.patrolGridOriginZ,
					runtime.patrolGridCellSize
				);
				if (routeBuilt) {
					runtime.navigationRouteIndex = 0;
					runtime.navigationRejectedFrames = 0;
					routeRemainingSeconds = routeInterval;
				} else {
					routeRemainingSeconds = 0.5f;
				}
			}
			const auto recordPatrolVisit = [&](const Vector3& position) {
				if (!patrolState || runtime.patrolGridWidth <= 0 ||
					runtime.patrolGridHeight <= 0 || runtime.patrolGridCellSize <= 0.0f ||
					runtime.patrolVisitCounts.empty()) {
					return;
				}
				const XZPoint local = ToSharkLocalXZ(
					position,
					waterBounds.center,
					waterBounds.axisXCosine,
					waterBounds.axisXSine
				);
				const int x = std::clamp(
					static_cast<int>(std::floor(
						(local.x - runtime.patrolGridOriginX) / runtime.patrolGridCellSize
					)),
					0,
					runtime.patrolGridWidth - 1
				);
				const int z = std::clamp(
					static_cast<int>(std::floor(
						(local.z - runtime.patrolGridOriginZ) / runtime.patrolGridCellSize
					)),
					0,
					runtime.patrolGridHeight - 1
				);
				const size_t index = static_cast<size_t>(
					SharkGridIndex(x, z, runtime.patrolGridWidth)
				);
				if (index < runtime.patrolVisitCounts.size()) {
					runtime.patrolVisitCounts[index] = (std::min)(
						runtime.patrolVisitCounts[index] + 1,
						1000000
					);
				}
			};
			const Vector3 currentPosition = sharkTransform.translate;
			while (runtime.navigationRouteIndex < runtime.navigationRoute.size() &&
				DistanceXZ(currentPosition,
					runtime.navigationRoute[runtime.navigationRouteIndex]) <= waypointDistance) {
				recordPatrolVisit(runtime.navigationRoute[runtime.navigationRouteIndex]);
				++runtime.navigationRouteIndex;
			}
			if (runtime.navigationRouteIndex >= runtime.navigationRoute.size()) {
				runtime.navigationRoute.clear();
				runtime.navigationRouteIndex = 0;
				routeRemainingSeconds = 0.0f;
			}
			Vector3 finalPosition = currentPosition;
			if (!runtime.navigationRoute.empty()) {
				const Vector3& waypoint = runtime.navigationRoute[runtime.navigationRouteIndex];
				const float desiredHeading = std::atan2(
					waypoint.x - currentPosition.x,
					waypoint.z - currentPosition.z
				);
				const float patrolTurnRate = std::isfinite(
					shark->fishingSharkWanderMaximumTurnRate
				) ? (std::max)(shark->fishingSharkWanderMaximumTurnRate, 0.0f) : 0.0f;
				const float chaseTurnRate = std::isfinite(
					shark->fishingSharkChaseMaximumTurnRate
				) ? (std::max)(shark->fishingSharkChaseMaximumTurnRate, 0.0f) : 2.2f;
				const float obstacleTurnRate = std::isfinite(
					shark->fishingSharkObstacleAvoidanceResponse
				) ? (std::max)(shark->fishingSharkObstacleAvoidanceResponse, 0.0f) : 0.0f;
				const float turnRate = (std::max)(
					patrolState ? patrolTurnRate : chaseTurnRate,
					obstacleTurnRate
				);
				runtime.wanderHeading = MoveSharkAngle(
					runtime.wanderHeading,
					desiredHeading,
					turnRate * safeDeltaTime
				);
				const float chaseSpeed = std::isfinite(
					shark->fishingSharkChaseMoveSpeed
				) ? (std::max)(shark->fishingSharkChaseMoveSpeed, 0.0f) : 13.0f;
				const float moveSpeed = patrolState ? wanderMoveSpeed : chaseSpeed;
				const Vector3 direction = SharkHeadingVector(runtime.wanderHeading);
				const XZPoint start = { currentPosition.x, currentPosition.z };
				const XZPoint candidate = {
					start.x + direction.x * moveSpeed * safeDeltaTime,
					start.z + direction.z * moveSpeed * safeDeltaTime
				};
				if (IsSharkSegmentClear(
					start,
					candidate,
					waterBounds,
					obstacles,
					sharkRadius,
					obstacleClearance
				)) {
					finalPosition = {
						candidate.x,
						currentPosition.y,
						candidate.z
					};
					runtime.navigationRejectedFrames = 0;
				} else {
					++runtime.navigationRejectedFrames;
					if (runtime.navigationRejectedFrames >= 2) {
						runtime.navigationRoute.clear();
						runtime.navigationRouteIndex = 0;
						routeRemainingSeconds = 0.0f;
						runtime.navigationRejectedFrames = 0;
					}
				}
			}
			entity->transform.scale = runtime.initialTransform.scale;
			entity->transform.translate = {
				finalPosition.x,
				currentPosition.y,
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
				Vector3 obstacleCenter{};
				float obstacleRadius = 0.0f;
				if (IsSphereFishingObstacle(*obstacleCollider)) {
					FishingObstacleSphereGeometry sphere{};
					if (!BuildFishingObstacleSphereGeometry(
						document, obstacleEntity, *obstacleCollider, sphere
					)) {
						continue;
					}
					obstacleCenter = sphere.center;
					obstacleRadius = sphere.radius;
				} else {
					const Transform obstacleTransform =
						SceneTransformResolver::ResolveScene3DTransform(
							document, obstacleEntity
						);
					obstacleCenter = ToSpawnWorldPosition(
						obstacleTransform,
						obstacleCollider->colliderOffset.x,
						obstacleCollider->colliderOffset.z,
						obstacleTransform.translate.y + obstacleCollider->colliderOffset.y
					);
					obstacleRadius = ColliderRadiusXZ(*obstacleCollider);
				}
				Vector3 away = {
					basePosition.x - obstacleCenter.x,
					0.0f,
					basePosition.z - obstacleCenter.z
				};
				float distance = Math::Length(away);
				const float threshold = sharkRadius + obstacleRadius +
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
	if (!IsTutorialSharkVisibleStep(tutorialStep_)) {
		DeactivateSceneSharks(document);
		return true;
	}
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
	const Transform playerTransform = hasInitialPlayerTransform_
		? initialPlayerTransform_
		: SceneTransformResolver::ResolveScene3DTransform(document, *player);
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
	const int maximumSpawnBand =
		(std::max)(static_cast<int>(director.fishingHookBands.size()) - 1, 1); // サメを出す最大距離帯。
	const int spawnBandCount =
		maximumSpawnBand + 1; // 0番帯を含めた距離帯数。
	std::uniform_int_distribution<int> bandDistribution(
		1,
		maximumSpawnBand
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
		entity->active = true;
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
			const float obstacleClearance = std::isfinite(
				shark->fishingSharkObstacleClearance
			) ? (std::max)(shark->fishingSharkObstacleClearance, 0.0f) : 0.0f;
			const int preferredBand =
				bandDistribution(runtime.wanderRandom); // 最初に試す距離帯。
			const std::vector<int> bandOrder =
				BuildSharkSpawnBandOrder(preferredBand, maximumSpawnBand); // 失敗時に試す距離帯順。
			Vector3 spawnPosition{};
			float spawnHeading = 0.0f;
			bool foundPosition = false;
			for (int selectedBand : bandOrder) {
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
						static_cast<int>(std::floor(
							orientedZ * static_cast<float>(spawnBandCount)
						)),
						maximumSpawnBand
					);
					if (candidateBand != selectedBand ||
						!IsPointInsideSharkWater(
							{ candidate.x, candidate.z },
							waterBounds,
							sharkRadius + 0.25f + obstacleClearance
						) ||
						DistanceXZ(candidate, playerTransform.translate) <
							spawnArea->fishingSpawnMinimumDistance ||
						!IsSharkSegmentClear(
							{ candidate.x, candidate.z },
							{ candidate.x, candidate.z },
							waterBounds, obstacles, sharkRadius, obstacleClearance
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
						sharkRadius,
						obstacleClearance
					) < lookahead - kTransformEpsilon) {
						continue;
					}
					spawnPosition = candidate;
					foundPosition = true;
					break;
				}
				if (foundPosition) {
					break;
				}
			}
			if (!foundPosition) {
				Fault(
					document,
					director,
					"FishingShark has no valid position in hook bands 2-5 (entity=" +
						std::to_string(entityId) +
						", attempts=" +
						std::to_string(spawnArea->fishingSpawnMaxAttempts) + ")"
				);
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
			runtime.navigationState = SharkNavigationState::Patrol;
			runtime.navigationRoute.clear();
			runtime.navigationRouteIndex = 0;
			runtime.patrolRouteRemainingSeconds = 0.0f;
			runtime.chaseRouteRemainingSeconds = 0.0f;
			runtime.alertElapsedSeconds = 0.0f;
			runtime.lostElapsedSeconds = 0.0f;
			runtime.reacquireCooldownRemainingSeconds = 0.0f;
			runtime.alertPulseElapsedSeconds = 0.0f;
			runtime.lastSeenPlayerPosition = playerTransform.translate;
			runtime.hasLastSeenPlayerPosition = false;
			runtime.navigationRejectedFrames = 0;
			runtime.patrolVisitCounts.clear();
			runtime.patrolGridWidth = 0;
			runtime.patrolGridHeight = 0;
			runtime.patrolGridOriginX = 0.0f;
			runtime.patrolGridOriginZ = 0.0f;
			runtime.patrolGridCellSize = 0.0f;
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
	ResetFormationContactResponse();
	timerRunning_ = false;
	pendingFishCountDelta_ = 0;
	resultInputArmed_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasLastSafePlayerPlanarPosition_ = false;
	lastSafePlayerYaw_ = 0.0f;
	formationRecoveryPoses_.clear();
	formationNoProgressReferencePosition_ = {};
	formationNoProgressReferenceYaw_ = 0.0f;
	formationNoProgressSeconds_ = 0.0f;
	hasFormationNoProgressReference_ = false;
	DeactivatePoolHooks(document, director);
	for (uint64_t fishEntityId : director.fishingFishEntityIds) {
		if (SceneEntity* fish = document.FindEntity(fishEntityId)) {
			fish->active = false;
		}
	}
	if (resultTrackingEnabled_) {
		SceneFishingResultRecord record{};
		record.channelId = resultChannelId_;
		record.sourceDirectorEntityId = directorEntityId_;
		record.activeRankCount = std::clamp(
			director.fishingHookRankCount, 1, 10
		);
		record.totalScore = totalScore_;
		record.elapsedSeconds = elapsedSeconds_;
		record.sharkHitCount = sharkHitCount_;
		record.sharkFishWeightedCount = sharkFishWeightedCount_;
		record.ranks.assign(
			resultRankRecords_.begin(),
			resultRankRecords_.begin() + record.activeRankCount
		);
		uint64_t winningFishWeightedCount = 0;
		int winningRankIndex = -1;
		for (int rankIndex = 0; rankIndex < record.activeRankCount; ++rankIndex) {
			const SceneFishingResultRankRecord& rank =
				record.ranks[static_cast<size_t>(rankIndex)];
			if (rank.fishWeightedCount == 0 ||
				rank.fishWeightedCount < winningFishWeightedCount) {
				continue;
			}
			const bool tieBreakWins =
				rank.fishWeightedCount == winningFishWeightedCount &&
				winningRankIndex >= 0 &&
				(resultTieBreakMode_ == "HigherRank"
					? rankIndex > winningRankIndex
					: rankIndex < winningRankIndex);
			if (rank.fishWeightedCount > winningFishWeightedCount ||
				winningRankIndex < 0 || tieBreakWins) {
				winningFishWeightedCount = rank.fishWeightedCount;
				winningRankIndex = rankIndex;
			}
		}
		if (winningRankIndex >= 0) {
			record.hasWinner = true;
			record.winningRankIndex = winningRankIndex;
			record.winningRankId = record.ranks[
				static_cast<size_t>(winningRankIndex)
			].rankId;
			record.winningFishWeightedCount = winningFishWeightedCount;
		}
		resultSessionPublishRequested_ = true;
		resultSessionPublishRequest_.record = std::move(record);
	}
	state_ = SceneFishingScoreAttackState::Result;
	BuildTextRequests(document, director);
}

void SceneFishingScoreAttackSystem::Fault(
	SceneDocument& document,
	const SceneComponent& director,
	std::string diagnostic
) {
	ResetFormationContactResponse();
	diagnostic_ = std::move(diagnostic);
	timerRunning_ = false;
	pendingFishCountDelta_ = 0;
	resultInputArmed_ = false;
	playerConstraintRequest_ = {};
	hasPlayerConstraintRequest_ = false;
	hasLastSafePlayerPlanarPosition_ = false;
	lastSafePlayerYaw_ = 0.0f;
	formationRecoveryPoses_.clear();
	formationNoProgressReferencePosition_ = {};
	formationNoProgressReferenceYaw_ = 0.0f;
	formationNoProgressSeconds_ = 0.0f;
	hasFormationNoProgressReference_ = false;
	DeactivatePoolHooks(document, director);
	for (uint64_t fishEntityId : director.fishingFishEntityIds) {
		if (SceneEntity* fish = document.FindEntity(fishEntityId)) {
			fish->active = false;
		}
	}
	state_ = SceneFishingScoreAttackState::Faulted;
	BuildTextRequests(document, director);
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

void SceneFishingScoreAttackSystem::UpdateCurrentPositionMultiplier(
	const SceneDocument& document,
	const SceneComponent& director
) {
	hasCurrentPositionMultiplier_ = false;
	if (state_ != SceneFishingScoreAttackState::Navigating) {
		return;
	}
	const SceneEntity* player = document.FindEntity(director.fishingPlayerEntityId);
	const SceneEntity* waterEntity = document.FindEntity(
		director.fishingWaterVolumeEntityId
	);
	const SceneComponent* waterVolume = FindComponent(
		document,
		director.fishingWaterVolumeEntityId,
		"WaterVolume"
	);
	if (!player || !waterEntity || !waterVolume ||
		waterVolume->waterHalfSize.z <= 0.0f) {
		return;
	}
	const Transform playerTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *player);
	Transform waterTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
	waterTransform.translate.x += waterVolume->waterOffset.x;
	waterTransform.translate.y += waterVolume->waterOffset.y;
	waterTransform.translate.z += waterVolume->waterOffset.z;
	const int distanceBandCount = director.fishingUseHookBandSettings
		? 5
		: director.fishingDistanceBandCount;
	if (distanceBandCount <= 0) {
		return;
	}
	const Vector3 playerWaterLocal = ToLocalXZ(
		waterTransform, playerTransform.translate
	);
	const float normalizedZ = (playerWaterLocal.z + waterVolume->waterHalfSize.z) /
		(2.0f * waterVolume->waterHalfSize.z);
	const float orientedZ = startFromPositiveWaterZ_
		? 1.0f - std::clamp(normalizedZ, 0.0f, 0.99999f)
		: std::clamp(normalizedZ, 0.0f, 0.99999f);
	const int distanceBand = (std::min)(
		static_cast<int>(std::floor(orientedZ * distanceBandCount)),
		distanceBandCount - 1
	);
	currentPositionMultiplier_ = director.fishingUseHookBandSettings
		? director.fishingHookBands[static_cast<size_t>(distanceBand)].distanceMultiplier
		: director.fishingDistanceMultiplierBase +
			director.fishingDistanceMultiplierStep * static_cast<float>(distanceBand);
	hasCurrentPositionMultiplier_ = std::isfinite(currentPositionMultiplier_);
}

void SceneFishingScoreAttackSystem::BuildTextRequests(
	const SceneDocument& document,
	const SceneComponent& director
) {
	textRequests_.clear();
	iconRequests_.clear();
	hookBubbleRequests_.clear();
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
		state_ == SceneFishingScoreAttackState::Navigating &&
			hasCurrentPositionMultiplier_
			? director.fishingMultiplierPrefix +
				FormatOneDecimal(currentPositionMultiplier_)
			: std::string{}
	);
	const std::string tutorialMessage = GetTutorialMessage(); // 中央HUDへ表示するチュートリアル説明文。
	const SceneEntity* tutorialMessageEntity =
		document.FindEntityByName(kTutorialMessageTextEntityName); // 説明専用Text Entity。
	const uint64_t tutorialMessageTextEntityId =
		tutorialMessageEntity &&
			IsEntityActiveInHierarchy(document, *tutorialMessageEntity) &&
			FindEnabledComponent(*tutorialMessageEntity, "TextRenderer")
			? tutorialMessageEntity->id
			: 0; // 説明専用Text Entity ID。
	if (tutorialMessageTextEntityId != 0) {
		addText(tutorialMessageTextEntityId, tutorialMessage);
	}
	if (!tutorialMessage.empty() && tutorialMessageTextEntityId == 0) {
		scorePopup_ = {}; // 説明専用Entityがない場合のみ、既存Entity共有の競合を避ける。
		addText(
			director.fishingResultTextEntityId,
			tutorialMessage
		);
	} else if (state_ == SceneFishingScoreAttackState::Result) {
		addText(
			director.fishingResultTextEntityId,
			director.fishingFinishText.empty()
				? director.fishingResultPrefix + std::to_string(totalScore_)
				: director.fishingFinishText
		);
	} else if (scorePopup_.active && scorePopup_.entityId != 0) {
		textRequests_.push_back({
			scorePopup_.entityId,
			scorePopup_.text,
			true,
			scorePopup_.color
		});
	} else {
		addText(director.fishingResultTextEntityId, {});
	}
	const bool tutorialHidesHookLegend =
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::Overview ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::MoveExplanation ||
		tutorialStep_ == SceneFishingScoreAttackTutorialStep::MovePractice; // 釣り針説明前は凡例を隠すか。
	const bool showLegend =
		director.fishingUseHookBandSettings &&
		director.fishingHookLegendVisible &&
		!tutorialHidesHookLegend &&
		(state_ == SceneFishingScoreAttackState::SelectingInitial ||
			state_ == SceneFishingScoreAttackState::Navigating ||
			state_ == SceneFishingScoreAttackState::SelectingNext) &&
		director.fishingHookRanks.size() == 10;
	const size_t activeRankCount = static_cast<size_t>(std::clamp(
		director.fishingHookRankCount, 1, 10
	));
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
				showLegend && tierIndex < activeRankCount
					? director.fishingHookLegendPrefix + FormatHookScoreMultiplier(
						director.fishingHookRanks[tierIndex].scoreMultiplier
					)
					: std::string{},
				showLegend && tierIndex < activeRankCount,
				showLegend && tierIndex < activeRankCount
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
			showLegend && tierIndex < activeRankCount && !texturePath.empty()
		});
	}
}

void SceneFishingScoreAttackSystem::Clear(SceneDocument* document) {
	ResetFormationContactResponse();
	if (document) {
		for (const FishCatchAnimation& animation : fishCatchAnimations_) {
			const SceneEntity* effectFish = document->FindEntity(animation.entityId);
			if (!effectFish || !effectFish->runtimeOnly) {
				continue;
			}
			const bool wasDirty = document->IsDirty();
			document->RemoveEntity(animation.entityId);
			if (!wasDirty) {
				document->MarkClean();
			}
			const SceneEntity* attractor = document->FindEntity(
				animation.attractorEntityId
			);
			if (!attractor || !attractor->runtimeOnly) {
				continue;
			}
			const bool wasDirtyAfterFishRemoval = document->IsDirty();
			document->RemoveEntity(animation.attractorEntityId);
			if (!wasDirtyAfterFishRemoval) {
				document->MarkClean();
			}
		}
		for (uint64_t entityId : pendingFishCatchEffectRemovals_) {
			const SceneEntity* effectFish = document->FindEntity(entityId);
			if (!effectFish || !effectFish->runtimeOnly) {
				continue;
			}
			const bool wasDirty = document->IsDirty();
			document->RemoveEntity(entityId);
			if (!wasDirty) {
				document->MarkClean();
			}
		}
	}
	state_ = SceneFishingScoreAttackState::Inactive;
	directorEntityId_ = 0;
	resultInputArmed_ = false;
	activeHooks_.clear();
	fishCatchAnimations_.clear();
	pendingFishCatchEffects_.clear();
	pendingFishCatchEffectRemovals_.clear();
	nextFishCatchEffectGroupId_ = 1;
	// 演出魚群プールはRuntimeSceneの初期化時に生成済み。Edit→Play切替で
	// Clearが呼ばれても保持し、次の得点時に再利用する。
	hookVisualModelPaths_.clear();
	sharkRuntimes_.clear();
	tutorialStep_ = SceneFishingScoreAttackTutorialStep::Disabled;
	tutorialMovePracticeSeconds_ = 0.0f;
	tutorialFishCountPracticeStart_ = 1;
	tutorialFishCountAdjusted_ = false;
	tutorialMultiScoreCount_ = 0;
	tutorialAutoStartNextRound_ = false;
	initialPlayerTransform_ = {};
	playerWaterBounds_ = {};
	hasInitialPlayerTransform_ = false;
	hasPlayerWaterBounds_ = false;
	playerPlanarColliderRadius_ = 0.0f;
	lastSafePlayerPlanarPosition_ = {};
	lastSafePlayerYaw_ = 0.0f;
	hasLastSafePlayerPlanarPosition_ = false;
	formationRecoveryPoses_.clear();
	formationNoProgressReferencePosition_ = {};
	formationNoProgressReferenceYaw_ = 0.0f;
	formationNoProgressSeconds_ = 0.0f;
	hasFormationNoProgressReference_ = false;
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
	currentPositionMultiplier_ = 0.0f;
	hasCurrentPositionMultiplier_ = false;
	elapsedSeconds_ = 0.0;
	totalScore_ = 0;
	timerRunning_ = false;
	hasDirector_ = false;
	resultTrackingEnabled_ = false;
	resultChannelId_.clear();
	resultTieBreakMode_ = "HigherRank";
	resultRankRecords_.clear();
	sharkHitCount_ = 0;
	sharkFishWeightedCount_ = 0;
	resultSessionBeginRequested_ = false;
	resultSessionBeginRequest_ = {};
	resultSessionPublishRequested_ = false;
	resultSessionPublishRequest_ = {};
	diagnostic_.clear();
	textRequests_.clear();
	iconRequests_.clear();
	scorePopup_ = {};
	hookBubbleRequests_.clear();
	if (formationParticleActive_) {
		ParticleManager* particleManager = ParticleManager::GetInstance();
		if (!formationParticlePauseOwnerKey_.empty()) {
			particleManager->SetParticleGroupSimulationPaused(
				"FishingFormationCloudCpu", formationParticlePauseOwnerKey_, false
			);
		}
		particleManager->ClearParticleGroup("FishingFormationCloudCpu");
		particleManager->ClearParticleGroupParentTransform("FishingFormationCloudCpu");
	}
	formationParticleEmissionAccumulator_ = 0.0f;
	formationParticlePointCursor_ = 0;
	formationParticleActive_ = false;
	formationParticlePauseOwnerKey_.clear();
	formationParticlePointCount_ = 0;
	formationParticleStartSize_ = 0.26f;
	formationParticleEndSize_ = 0.43f;
	formationParticleCountPerEmission_ = 1;
	formationParticleEmitterSpread_ = 0.0f;
	formationParticleLifetime_ = 0.8f;
	formationParticleStartColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
	formationParticleEndColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
	formationParticleEmissiveIntensity_ = 1.0f;
	formationParticleTuningDirty_ = false;
	formationParticleSaveRequested_ = false;
	formationParticleSaveRequest_ = {};
	formationParticleSaveStatus_.clear();
	formationParticleSaveStatusIsError_ = false;
}
