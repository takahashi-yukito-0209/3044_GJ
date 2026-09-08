// 役割: 描画ComponentをObject3d/Spriteへ反映し、実行時の所有関係を管理する。
#include "SceneObjectSystem.h"

#include "ScenePhysicsSystem.h"
#include "../../../engine/2d/Sprite.h"
#include "../../../engine/2d/SpriteCommon.h"
#include "../../../engine/2d/TextureManager.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/ModelManager.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/3d/Object3dCommon.h"
#include "../../../engine/agent/AgentSettingsResolver.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <random>
#include <sstream>
#include <unordered_set>

namespace {
	using AgentSettingsResolver::ResolveAgentBehaviorSettings;
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene2DTransform;

	constexpr std::array<const char*, 6> kFishingObstacleRockModelPaths = {
		"rock/Rock_Chunky.obj",
		"rock/Rock_Flat.obj",
		"rock/Rock_Round.obj",
		"rock/Rock_Spire.obj",
		"rock/Rock_Tall.obj",
		"rock/Rock_Wide.obj"
	};

	std::string BuildMaterialOverrideSignature(
		const std::vector<SceneMeshMaterialOverride>& overrides
	) {
		std::ostringstream stream;
		for (const SceneMeshMaterialOverride& override : overrides) {
			stream << override.materialName << '|'
				<< override.enabled << '|'
				<< override.colorOverrideEnabled << '|'
				<< override.color.x << ',' << override.color.y << ','
				<< override.color.z << ',' << override.color.w << '|'
				<< override.texturePath << ';';
		}
		return stream.str();
	}

	Object3dCommon::CullMode ToObjectCullMode(const std::string& cullMode) {
		if (cullMode == "None") {
			return Object3dCommon::CullMode::kNone;
		}
		if (cullMode == "Front") {
			return Object3dCommon::CullMode::kFront;
		}
		return Object3dCommon::CullMode::kBack;
	}

	const SceneFishingObstacleColliderProfile*
	FindFishingObstacleColliderProfile(
		const SceneFishingObstacleSettings& settings,
		const std::string& modelPath
	) {
		const auto found = std::find_if(
			settings.colliderProfiles.begin(),
			settings.colliderProfiles.end(),
			[&modelPath](const SceneFishingObstacleColliderProfile& profile) {
				return profile.enabled && profile.modelPath == modelPath;
			}
		);
		return found == settings.colliderProfiles.end()
			? nullptr
			: &(*found);
	}

	// Play用Documentだけを書き換えるため、Editorで保存している岩配置には影響しない。
	// WaterVolumeの範囲内で、十分な間隔を空けながらXZ座標を抽選する。
	void RandomizeFishingObstacleLayout(
		SceneDocument& document,
		std::mt19937& randomEngine
	) {
		std::vector<SceneEntity*> obstacles;
		const SceneEntity* waterEntity = nullptr;
		const SceneComponent* waterVolume = nullptr;
		const SceneEntity* playerEntity = nullptr;
		for (SceneEntity& entity : document.GetEntities()) {
			if (FindEnabledComponent(entity, "FishingObstacle")) {
				obstacles.push_back(&entity);
			}
			if (!playerEntity && FindEnabledComponent(entity, "PlayerBehavior")) {
				playerEntity = &entity;
			}
			// 水域は描画を止めるためEntity自体をinactiveにする場合がある。
			// WaterVolume Componentが有効なら、配置範囲としては使用する。
			if (!waterEntity) {
				if (const SceneComponent* candidate =
					FindEnabledComponent(entity, "WaterVolume")) {
					waterEntity = &entity;
					waterVolume = candidate;
				}
			}
		}
		if (obstacles.size() < 2 || !waterEntity || !waterVolume) {
			return;
		}

		const Transform waterTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *waterEntity);
		const float halfSizeX = waterVolume->waterHalfSize.x *
			(std::max)(std::abs(waterTransform.scale.x), 0.001f);
		const float halfSizeZ = waterVolume->waterHalfSize.z *
			(std::max)(std::abs(waterTransform.scale.z), 0.001f);
		if (halfSizeX < 0.001f || halfSizeZ < 0.001f) {
			return;
		}

		const float yaw = waterTransform.rotate.y;
		const float cosine = std::cos(yaw);
		const float sine = std::sin(yaw);
		const Vector3 playerSpawnPosition = playerEntity
			? SceneTransformResolver::ResolveScene3DTransform(
				document, *playerEntity
			).translate
			: Vector3{};
		const Vector3 waterCenter = {
			waterTransform.translate.x +
				waterVolume->waterOffset.x * cosine +
				waterVolume->waterOffset.z * sine,
			waterTransform.translate.y + waterVolume->waterOffset.y,
			waterTransform.translate.z -
				waterVolume->waterOffset.x * sine +
				waterVolume->waterOffset.z * cosine
		};
		// 岩Colliderが水域外にはみ出さないための余白。
		const float edgePadding = (std::min)(
			24.0f, (std::min)(halfSizeX, halfSizeZ) * 0.25f
		);
		std::uniform_real_distribution<float> xDistribution(
			-halfSizeX + edgePadding, halfSizeX - edgePadding
		);
		std::uniform_real_distribution<float> zDistribution(
			-halfSizeZ + edgePadding, halfSizeZ - edgePadding
		);
		std::uniform_real_distribution<float> yawDistribution(
			0.0f, 6.28318530717958647692f
		);
		std::uniform_real_distribution<float> scaleDistribution(0.8f, 1.2f);
		std::vector<Vector3> placedPositions;
		placedPositions.reserve(obstacles.size());
		constexpr int kPlacementAttempts = 128;
		// 最大スケール時でも現在の岩Collider同士が重なりにくい距離を確保する。
		constexpr float kInitialMinimumDistance = 56.0f;
		// スポーン直後の移動不能を防ぐため、プレイヤー開始地点の周囲を空ける。
		constexpr float kPlayerSpawnExclusionRadius = 50.0f;
		for (SceneEntity* obstacle : obstacles) {
			const Transform obstacleWorld =
				SceneTransformResolver::ResolveScene3DTransform(document, *obstacle);
			Vector3 selectedPosition = obstacleWorld.translate;
			bool placed = false;
			for (const float minimumDistance : {
				kInitialMinimumDistance,
				kInitialMinimumDistance * 0.8f,
				kInitialMinimumDistance * 0.6f
			}) {
				const float minimumDistanceSquared =
					minimumDistance * minimumDistance;
				for (int attempt = 0; attempt < kPlacementAttempts; ++attempt) {
					const float localX = xDistribution(randomEngine);
					const float localZ = zDistribution(randomEngine);
					const Vector3 candidate = {
						waterCenter.x + localX * cosine + localZ * sine,
						obstacleWorld.translate.y,
						waterCenter.z - localX * sine + localZ * cosine
					};
					const float playerDeltaX = candidate.x - playerSpawnPosition.x;
					const float playerDeltaZ = candidate.z - playerSpawnPosition.z;
					const bool overlapsPlayerSpawn = playerEntity &&
						playerDeltaX * playerDeltaX + playerDeltaZ * playerDeltaZ <
							kPlayerSpawnExclusionRadius * kPlayerSpawnExclusionRadius;
					const bool overlapsObstacle = std::any_of(
						placedPositions.begin(),
						placedPositions.end(),
						[&candidate, minimumDistanceSquared](const Vector3& placedPosition) {
							const float deltaX = candidate.x - placedPosition.x;
							const float deltaZ = candidate.z - placedPosition.z;
							return deltaX * deltaX + deltaZ * deltaZ <
								minimumDistanceSquared;
						}
					);
					if (!overlapsPlayerSpawn && !overlapsObstacle) {
						selectedPosition = candidate;
						placed = true;
						break;
					}
				}
				if (placed) {
					break;
				}
			}
			Transform targetWorld = obstacleWorld;
			targetWorld.translate = selectedPosition;
			Vector3 randomRotation = MakeEulerFromQuaternion(
				obstacleWorld.quaternionRotate
			);
			randomRotation.y = yawDistribution(randomEngine);
			targetWorld.rotate = randomRotation;
			targetWorld.useQuaternionRotation = true;
			targetWorld.quaternionRotate = MakeQuaternionFromEuler(
				randomRotation
			);
			const float scaleMultiplier = scaleDistribution(randomEngine);
			targetWorld.scale = {
				obstacleWorld.scale.x * scaleMultiplier,
				obstacleWorld.scale.y * scaleMultiplier,
				obstacleWorld.scale.z * scaleMultiplier
			};
			Transform targetLocal{};
			if (SceneTransformResolver::TryConvertSceneWorldTransformToLocal(
				document, *obstacle, targetWorld, targetLocal
			)) {
				obstacle->transform.scale = targetLocal.scale;
				obstacle->transform.rotate = targetLocal.quaternionRotate;
				obstacle->transform.translate = targetLocal.translate;
			}
			placedPositions.push_back(selectedPosition);
		}
	}
}

SceneObjectSystem::SceneObjectSystem() = default;

SceneObjectSystem::~SceneObjectSystem() {
	Finalize();
}

void SceneObjectSystem::SyncModels(
	SceneDocument* document,
	ScenePhysicsSystem& physicsSystem,
	float deltaTime,
	bool allowAnimatorAutoPlay,
	bool editing
) {
	// Entity IDを安定キーにし、Model変更時だけRuntime一式を作り直す。
	if (!document) {
		ClearModels();
		return;
	}
	// 編集中はSceneに保存されたModel/配置をそのまま表示する。Playへ切り替わった
	// 最初の同期でだけ岩を抽選し、その実行中は同じ結果を維持する。
	if (editing) {
		fishingObstacleModelPaths_.clear();
		fishingObstacleModelDocument_ = document;
		fishingObstacleLayoutRandomizedForCurrentPlay_ = false;
	} else if (
		fishingObstacleModelDocument_ != document ||
		!fishingObstacleLayoutRandomizedForCurrentPlay_
	) {
		fishingObstacleModelPaths_.clear();
		fishingObstacleModelDocument_ = document;
		RandomizeFishingObstacleLayout(*document, fishingObstacleRandomEngine_);
		fishingObstacleLayoutRandomizedForCurrentPlay_ = true;
	}

	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		std::string modelPath = meshRenderer
			? meshRenderer->modelPath
			: std::string{};
		if (!editing && FindEnabledComponent(entity, "FishingObstacle")) {
			auto [assignment, inserted] = fishingObstacleModelPaths_.try_emplace(
				entity.id
			);
			if (inserted) {
				std::uniform_int_distribution<size_t> distribution(
					0, kFishingObstacleRockModelPaths.size() - 1
				);
				assignment->second = kFishingObstacleRockModelPaths[
					distribution(fishingObstacleRandomEngine_)
				];
			}
			modelPath = assignment->second;
		}
		const bool hasRenderer = !modelPath.empty();
		requiredIds.insert(entity.id);
		auto found = models_.find(entity.id);
		if (
			found != models_.end() &&
			(
				found->second.modelPath != modelPath ||
				found->second.hasRenderer != hasRenderer
			)
		) {
			models_.erase(found);
			found = models_.end();
		}

		if (found == models_.end()) {
			ModelRuntime runtime{};
			runtime.object = std::make_unique<Object3d>();
			runtime.object->Initialize(Object3dCommon::GetInstance());
			if (hasRenderer) {
				ModelManager::GetInstance()->LoadModel(modelPath);
				runtime.object->SetModel(modelPath);
			}
			runtime.modelPath = modelPath;
			runtime.hasRenderer = hasRenderer;
			found = models_.emplace(entity.id, std::move(runtime)).first;
		}

		const SceneComponent* animator =
			FindEnabledComponent(entity, "Animator");
		ModelRuntime& runtime = found->second;
		const bool hasAnimator = animator && runtime.object->HasAnimation();
		const size_t clipCount = runtime.object->GetAnimationClipCount();
		const int defaultClip = clipCount > 0
			? std::clamp(
				animator ? animator->animatorDefaultClip : 0,
				0,
				static_cast<int>(clipCount - 1)
			)
			: 0;
		const AnimationBlendCurve blendCurve =
			animator && animator->animatorBlendCurve == "Linear"
			? AnimationBlendCurve::Linear
			: AnimationBlendCurve::SmoothStep;

		if (
			!runtime.animatorInitialized ||
			runtime.hasAnimator != hasAnimator ||
			runtime.animatorAutoPlayAllowed != allowAnimatorAutoPlay
		) {
			runtime.object->SetAnimationEnabled(hasAnimator);
			if (hasAnimator) {
				runtime.object->SetAnimationLoop(animator->animatorLoop);
				runtime.object->SetAnimationSpeed(animator->animatorSpeed);
				runtime.object->SetAnimationBlendCurve(blendCurve);
				runtime.object->PlayAnimation(
					static_cast<size_t>(defaultClip),
					0.0f,
					true
				);
				runtime.object->SetAnimationPlaying(
					allowAnimatorAutoPlay && animator->animatorPlayOnStart
				);
			}
			runtime.animatorInitialized = true;
			runtime.animatorAutoPlayAllowed = allowAnimatorAutoPlay;
		} else if (hasAnimator) {
			if (runtime.animatorPlayOnStart != animator->animatorPlayOnStart) {
				runtime.object->SetAnimationPlaying(
					allowAnimatorAutoPlay && animator->animatorPlayOnStart
				);
			}
			if (runtime.animatorBlendCurve != animator->animatorBlendCurve) {
				runtime.object->SetAnimationBlendCurve(blendCurve);
			}
			if (runtime.animatorLoop != animator->animatorLoop) {
				runtime.object->SetAnimationLoop(animator->animatorLoop);
			}
			if (runtime.animatorSpeed != animator->animatorSpeed) {
				runtime.object->SetAnimationSpeed(animator->animatorSpeed);
			}
			if (runtime.animatorDefaultClip != defaultClip) {
				runtime.object->PlayAnimation(
					static_cast<size_t>(defaultClip),
					(std::max)(animator->animatorTransitionDuration, 0.0f),
					true
				);
			}
		}
		runtime.hasAnimator = hasAnimator;
		if (animator) {
			runtime.animatorPlayOnStart = animator->animatorPlayOnStart;
			runtime.animatorLoop = animator->animatorLoop;
			runtime.animatorSpeed = animator->animatorSpeed;
			runtime.animatorDefaultClip = defaultClip;
			runtime.animatorTransitionDuration =
				animator->animatorTransitionDuration;
			runtime.animatorBlendCurve = animator->animatorBlendCurve;
		}

		Transform& runtimeTransform = runtime.object->GetTransform();
		runtimeTransform.scale = entity.transform.scale;
		runtimeTransform.rotate = MakeEulerFromQuaternion(entity.transform.rotate);
		runtimeTransform.translate = entity.transform.translate;
		runtimeTransform.useQuaternionRotation = true;
		runtimeTransform.quaternionRotate = entity.transform.rotate;
		runtime.object->SetVisualLocalRotation(
			meshRenderer ? meshRenderer->meshVisualRotation : Vector3{}
		);
		const bool isWaterVolume = HasComponent(entity, "WaterVolume");
		runtime.object->SetCullMode(
			isWaterVolume
				? Object3dCommon::CullMode::kNone
				: (
					meshRenderer
						? ToObjectCullMode(meshRenderer->meshCullMode)
						: Object3dCommon::CullMode::kBack
				)
		);
		if (isWaterVolume) {
			runtime.object->SetColor({ 0.08f, 0.48f, 0.95f, 0.34f });
			runtime.object->SetEnableLighting(false);
			runtime.object->SetEnvironmentCoefficient(0.0f);
			runtime.object->SetEmissive(
				0.18f,
				{ 0.30f, 0.78f, 1.0f, 1.0f }
			);
		} else if (const SceneComponent* agentBehavior =
			FindEnabledComponent(entity, "AgentBehavior")) {
			const SceneComponent resolvedAgentBehavior =
				ResolveAgentBehaviorSettings(
					*document,
					entity,
					*agentBehavior
				);
			runtime.object->SetColor(
				resolvedAgentBehavior.agentVisualColor
			);
			runtime.object->SetEnableLighting(
				resolvedAgentBehavior.agentEnableLighting
			);
			runtime.object->SetEnvironmentCoefficient(0.05f);
			runtime.object->SetEmissive(
				resolvedAgentBehavior.agentEnableLighting ? 0.0f : 0.12f,
				resolvedAgentBehavior.agentVisualColor
			);
		} else if (const SceneComponent* agentAttractor =
			FindEnabledComponent(entity, "AgentAttractor")) {
			runtime.object->SetColor(agentAttractor->attractorVisualColor);
			runtime.object->SetEnableLighting(true);
			runtime.object->SetEnvironmentCoefficient(0.08f);
			runtime.object->SetEmissive(
				0.05f,
				agentAttractor->attractorVisualColor
			);
		} else if (HasComponent(entity, "MonitorRenderer")) {
			runtime.object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			runtime.object->SetEnableLighting(false);
			runtime.object->SetEnvironmentCoefficient(0.0f);
			runtime.object->SetEmissive(0.0f);
		} else {
			runtime.object->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			runtime.object->SetEnableLighting(true);
			runtime.object->SetEmissive(0.0f);
		}

		const std::vector<SceneMeshMaterialOverride> emptyMaterialOverrides;
		const std::vector<SceneMeshMaterialOverride>& materialOverrides =
			meshRenderer
				? meshRenderer->meshMaterialOverrides
				: emptyMaterialOverrides;
		const std::string materialOverrideSignature =
			BuildMaterialOverrideSignature(materialOverrides);
		if (runtime.materialOverrideSignature != materialOverrideSignature) {
			std::vector<Object3d::MaterialOverride> objectOverrides;
			objectOverrides.reserve(materialOverrides.size());
			for (const SceneMeshMaterialOverride& override : materialOverrides) {
				objectOverrides.push_back({
					override.materialName,
					override.enabled,
					override.colorOverrideEnabled,
					override.color,
					override.texturePath
				});
			}
			runtime.object->SetMaterialOverrides(objectOverrides);
			runtime.materialOverrideSignature = materialOverrideSignature;
		}
		if (HasComponent(entity, "PlayerBehavior")) {
			runtime.object->SetDissolve(0.0f);
		}

		const SceneComponent* obbCollider =
			FindEnabledComponent(entity, "OBBCollider");
		runtime.hasCollider = obbCollider != nullptr;
		if (obbCollider) {
			const SceneFishingObstacleColliderProfile* obstacleProfile =
				FindEnabledComponent(entity, "FishingObstacle")
				? FindFishingObstacleColliderProfile(
					document->GetFishingObstacleSettings(), modelPath
				)
				: nullptr;
			const Vector3 colliderOffset = obstacleProfile
				? obstacleProfile->colliderOffset
				: obbCollider->colliderOffset;
			const Vector3 colliderSizeMultiplier = obstacleProfile
				? obstacleProfile->colliderSizeMultiplier
				: obbCollider->colliderSizeMultiplier;
			const Vector3 colliderRotation = obstacleProfile
				? obstacleProfile->colliderRotation
				: Vector3{};
			const float colliderSphereRadius = obstacleProfile
				? obstacleProfile->colliderSphereRadius
				: obbCollider->colliderSphereRadius;
			Collider* runtimeCollider = obbCollider->colliderShape == "Sphere"
				? static_cast<Collider*>(&runtime.sphereCollider)
				: static_cast<Collider*>(&runtime.boxCollider);
			runtimeCollider->SetWorldMatrix(&runtime.object->GetWorldMatrix());
			runtimeCollider->SetOffset(colliderOffset);
			runtimeCollider->SetTrigger(obbCollider->colliderIsTrigger);
			runtimeCollider->SetActive(obbCollider->colliderActive);
			runtimeCollider->SetCollisionAttribute(obbCollider->colliderLayer);
			runtimeCollider->SetCollisionMask(obbCollider->colliderMask);
			if (obbCollider->colliderShape == "Sphere") {
				runtime.sphereCollider.SetRadius(
					(std::max)(colliderSphereRadius, 0.001f)
				);
			} else {
				runtime.boxCollider.SetLocalRotation(colliderRotation);
				runtime.boxCollider.SetHalfSize({
					(std::max)(
						std::abs(colliderSizeMultiplier.x),
						0.001f
					),
					(std::max)(
						std::abs(colliderSizeMultiplier.y),
						0.001f
					),
					(std::max)(
						std::abs(colliderSizeMultiplier.z),
						0.001f
					)
				});
			}
			runtime.collider = runtimeCollider;
			runtime.colliderDebugColor = obbCollider->colliderDebugColor;
			runtime.colliderDebugVisible = obbCollider->colliderDebugVisible;
			runtime.colliderDebugDrawMode =
				obbCollider->colliderDebugDrawMode;
			runtime.colliderDebugSegments =
				static_cast<uint32_t>(obbCollider->colliderDebugSegments);
		} else {
			runtime.collider = nullptr;
		}

		const SceneComponent* physicsBody =
			FindEnabledComponent(entity, "PhysicsBody");
		const bool wasPhysicsBody = runtime.hasPhysicsBody;
		runtime.hasPhysicsBody = physicsBody != nullptr;
		if (physicsBody) {
			physicsSystem.ApplyBodyComponent(
				runtime.physicsBody,
				*physicsBody,
				runtime.object.get(),
				runtime.collider,
				!wasPhysicsBody || editing
			);
		} else {
			runtime.physicsBody.syncTransform = {};
		}
	}

	for (auto iterator = models_.begin(); iterator != models_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = models_.erase(iterator);
		} else {
			++iterator;
		}
	}

	auto resolveObject = [this](uint64_t entityId) -> Object3d* {
		return entityId == 0 ? nullptr : FindObject(entityId);
	};
	for (const SceneEntity& entity : document->GetEntities()) {
		if (Object3d* object = resolveObject(entity.id)) {
			object->SetParent(resolveObject(entity.parentId));
		}
	}

	std::unordered_set<uint64_t> updatedIds;
	std::unordered_set<uint64_t> updatingIds;
	std::function<void(uint64_t)> updateEntity;
	updateEntity = [&](uint64_t entityId) {
		if (updatedIds.contains(entityId) || updatingIds.contains(entityId)) {
			return;
		}
		const SceneEntity* entity = document->FindEntity(entityId);
		Object3d* object = resolveObject(entityId);
		if (!entity || !object) {
			return;
		}
		updatingIds.insert(entityId);
		if (resolveObject(entity->parentId)) {
			updateEntity(entity->parentId);
		}
		if (IsEntityActiveInHierarchy(*document, *entity)) {
			object->UpdateAnimation(deltaTime);
			object->Update();
		}
		updatingIds.erase(entityId);
		updatedIds.insert(entityId);
	};
	for (const SceneEntity& entity : document->GetEntities()) {
		updateEntity(entity.id);
	}
}

void SceneObjectSystem::SyncSprites(const SceneDocument* document) {
	if (!document) {
		ClearSprites();
		return;
	}

	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* spriteRenderer =
			FindEnabledComponent(entity, "SpriteRenderer");
		const auto overrideIterator = spriteOverrides_.find(entity.id);
		const SceneSpriteRuntimeOverride* runtimeOverride =
			overrideIterator != spriteOverrides_.end()
			? &overrideIterator->second
			: nullptr;
		if (!spriteRenderer) {
			continue;
		}
		const std::string& texturePath = runtimeOverride
			? runtimeOverride->texturePath
			: spriteRenderer->texturePath;
		const bool visible = runtimeOverride
			? runtimeOverride->visible
			: true;
		if (!visible || texturePath.empty()) {
			continue;
		}

		auto found = sprites_.find(entity.id);
		if (
			found != sprites_.end() &&
			found->second.texturePath != texturePath
		) {
			sprites_.erase(found);
			found = sprites_.end();
		}

		if (found == sprites_.end()) {
			if (!TextureManager::GetInstance()->LoadTexture(texturePath)) {
				continue;
			}
			SpriteRuntime runtime{};
			runtime.sprite = std::make_unique<Sprite>();
			runtime.sprite->Initialize(
				SpriteCommon::GetInstance(),
				texturePath
			);
			runtime.texturePath = texturePath;
			found = sprites_.emplace(entity.id, std::move(runtime)).first;
		}

		requiredIds.insert(entity.id);
		Sprite* sprite = found->second.sprite.get();
		const Transform transform = ResolveScene2DTransform(*document, entity);
		const Vector2 size = runtimeOverride
			? runtimeOverride->size
			: spriteRenderer->spriteSize;
		const Vector4 color = runtimeOverride
			? runtimeOverride->color
			: spriteRenderer->spriteColor;
		sprite->SetPosition({ transform.translate.x, transform.translate.y });
		sprite->SetRotation(transform.rotate.z);
		sprite->SetSize({
			size.x * transform.scale.x,
			size.y * transform.scale.y
		});
		sprite->SetAnchorPoint(spriteRenderer->spriteAnchor);
		sprite->SetColor(color);
		sprite->SetIsFlipX(spriteRenderer->spriteFlipX);
		sprite->SetIsFlipY(spriteRenderer->spriteFlipY);
		if (IsEntityActiveInHierarchy(*document, entity)) {
			sprite->Update();
		}
	}

	for (auto iterator = sprites_.begin(); iterator != sprites_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = sprites_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneObjectSystem::ClearSpriteOverrides() {
	spriteOverrides_.clear();
}

void SceneObjectSystem::SetSpriteRuntimeOverride(
	const SceneSpriteRuntimeOverride& overrideValue
) {
	if (overrideValue.entityId == 0) {
		return;
	}
	spriteOverrides_[overrideValue.entityId] = overrideValue;
}

void SceneObjectSystem::BuildBindings(
	SceneDocument& document,
	std::vector<SceneRuntimeObjectBinding>& bindings
) {
	// 外部Systemへ渡す参照なので、所有コンテナの再配置後に必ず作り直す。
	bindings.clear();
	bindings.reserve(document.GetEntities().size());
	for (SceneEntity& entity : document.GetEntities()) {
		ModelRuntime* runtime = FindModelRuntime(entity.id);
		if (!runtime) {
			continue;
		}
		bindings.push_back(SceneRuntimeObjectBinding{
			&entity,
			runtime->object.get(),
			runtime->hasCollider ? runtime->collider : nullptr,
		runtime->hasPhysicsBody
				? &runtime->physicsBody
				: nullptr
		});
	}
}

void SceneObjectSystem::ApplyRenderCamera(Camera* camera) {
	Object3dCommon::GetInstance()->SetDefaultCamera(camera);
	for (auto& [entityId, runtime] : models_) {
		(void)entityId;
		if (runtime.object) {
			runtime.object->UpdateForCamera(camera);
		}
	}
}

void SceneObjectSystem::PrepareModelDraw() const {
	Object3dCommon::GetInstance()->SetCommonRenderState();
}

void SceneObjectSystem::DrawModels(
	const SceneDocument& document,
	uint64_t skipEntityId,
	bool hidePlayerModel
) const {
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			entity.id == skipEntityId ||
			!IsEntityActiveInHierarchy(document, entity) ||
			HasComponent(entity, "WaterVolume") ||
			(hidePlayerModel && HasComponent(entity, "PlayerBehavior"))
		) {
			continue;
		}
		if (Object3d* object = FindObject(entity.id)) {
			object->Draw();
		}
	}
}

void SceneObjectSystem::DrawSprites(
	const SceneDocument& document,
	uint64_t skipEntityId
) const {
	if (sprites_.empty()) {
		return;
	}

	SpriteCommon::GetInstance()->SetCommonRenderState();
	for (const SceneEntity& entity : document.GetEntities()) {
		const auto found = sprites_.find(entity.id);
		if (
			entity.id != skipEntityId &&
			found != sprites_.end() &&
			IsEntityActiveInHierarchy(document, entity) &&
			found->second.sprite &&
			[&entity]() {
				const SceneComponent* spriteRenderer =
					FindEnabledComponent(entity, "SpriteRenderer");
				return spriteRenderer &&
					spriteRenderer->spriteRenderSpace != "ScreenOverlay";
			}()
		) {
			found->second.sprite->Draw();
		}
	}
}

bool SceneObjectSystem::HasScreenOverlaySprites(
	const SceneDocument& document
) const {
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* spriteRenderer =
			FindEnabledComponent(entity, "SpriteRenderer");
		if (
			spriteRenderer &&
			spriteRenderer->spriteRenderSpace == "ScreenOverlay" &&
			IsEntityActiveInHierarchy(document, entity) &&
			sprites_.contains(entity.id) &&
			sprites_.at(entity.id).sprite
		) {
			return true;
		}
	}
	return false;
}

void SceneObjectSystem::DrawScreenOverlaySprites(
	const SceneDocument& document,
	uint32_t viewportWidth,
	uint32_t viewportHeight
) const {
	if (!HasScreenOverlaySprites(document)) {
		return;
	}

	SpriteCommon::GetInstance()->SetCommonRenderState();
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* spriteRenderer =
			FindEnabledComponent(entity, "SpriteRenderer");
		const auto found = sprites_.find(entity.id);
		if (
			!spriteRenderer ||
			spriteRenderer->spriteRenderSpace != "ScreenOverlay" ||
			found == sprites_.end() ||
			!found->second.sprite ||
			!IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}

		const auto overrideIterator = spriteOverrides_.find(entity.id);
		const SceneSpriteRuntimeOverride* runtimeOverride =
			overrideIterator != spriteOverrides_.end()
			? &overrideIterator->second
			: nullptr;
		if (runtimeOverride && !runtimeOverride->visible) {
			continue;
		}
		const Transform transform = ResolveScene2DTransform(document, entity);
		const Vector2 size = runtimeOverride
			? runtimeOverride->size
			: spriteRenderer->spriteSize;
		const Vector4 color = runtimeOverride
			? runtimeOverride->color
			: spriteRenderer->spriteColor;
		Sprite* sprite = found->second.sprite.get();
		sprite->SetPosition({
			spriteRenderer->spriteViewportAnchor.x * viewportWidth +
				transform.translate.x,
			spriteRenderer->spriteViewportAnchor.y * viewportHeight +
				transform.translate.y
		});
		sprite->SetRotation(transform.rotate.z);
		sprite->SetSize({
			size.x * transform.scale.x,
			size.y * transform.scale.y
		});
		sprite->SetAnchorPoint(spriteRenderer->spriteAnchor);
		sprite->SetColor(color);
		sprite->SetIsFlipX(spriteRenderer->spriteFlipX);
		sprite->SetIsFlipY(spriteRenderer->spriteFlipY);
		sprite->Update(viewportWidth, viewportHeight);
		sprite->Draw();
	}
}

void SceneObjectSystem::CollectShadowCasters(
	const SceneDocument& document,
	bool hidePlayerModel,
	std::vector<Object3d*>& shadowCasters
) const {
	shadowCasters.clear();
	shadowCasters.reserve(models_.size());
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		if (
			!IsEntityActiveInHierarchy(document, entity) ||
			HasComponent(entity, "WaterVolume") ||
			(meshRenderer && !meshRenderer->meshCastsShadow) ||
			(hidePlayerModel && HasComponent(entity, "PlayerBehavior"))
		) {
			continue;
		}
		if (Object3d* object = FindObject(entity.id)) {
			shadowCasters.push_back(object);
		}
	}
}

Object3d* SceneObjectSystem::FindObject(uint64_t entityId) const {
	const ModelRuntime* runtime = FindModelRuntime(entityId);
	return runtime ? runtime->object.get() : nullptr;
}

Object3d* SceneObjectSystem::FindObjectByName(
	const SceneDocument* document,
	const char* name
) const {
	if (!document || !name) {
		return nullptr;
	}
	const SceneEntity* entity = document->FindEntityByName(name);
	return entity ? FindObject(entity->id) : nullptr;
}

SceneObjectSystem::ModelRuntime* SceneObjectSystem::FindModelRuntime(
	uint64_t entityId
) {
	const auto found = models_.find(entityId);
	return found == models_.end() ? nullptr : &found->second;
}

const SceneObjectSystem::ModelRuntime* SceneObjectSystem::FindModelRuntime(
	uint64_t entityId
) const {
	const auto found = models_.find(entityId);
	return found == models_.end() ? nullptr : &found->second;
}

void SceneObjectSystem::ClearModels() {
	models_.clear();
	fishingObstacleModelPaths_.clear();
	fishingObstacleModelDocument_ = nullptr;
	fishingObstacleLayoutRandomizedForCurrentPlay_ = false;
}

void SceneObjectSystem::ClearSprites() {
	sprites_.clear();
	spriteOverrides_.clear();
}

void SceneObjectSystem::Finalize() {
	ClearSprites();
	ClearModels();
}
