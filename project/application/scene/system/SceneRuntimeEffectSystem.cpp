// 役割: Attack EffectのResource解決と一発Particle発火を実装する。
#include "SceneRuntimeEffectSystem.h"

#include "../../../engine/math/Math.h"
#include "../../../engine/particle/ParticleEffectResource.h"
#include "../../../engine/particle/ParticleManager.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "ScenePhysicsSystem.h"

#include <algorithm>
#include <cmath>

namespace {
	constexpr const char* kDefaultHitParticlePath =
		"resources/particles/core_burst.json";

	Vector3 TransformCoord(const Vector3& value, const Matrix4x4& matrix) {
		const float x = value.x * matrix.m[0][0] + value.y * matrix.m[1][0] +
			value.z * matrix.m[2][0] + matrix.m[3][0];
		const float y = value.x * matrix.m[0][1] + value.y * matrix.m[1][1] +
			value.z * matrix.m[2][1] + matrix.m[3][1];
		const float z = value.x * matrix.m[0][2] + value.y * matrix.m[1][2] +
			value.z * matrix.m[2][2] + matrix.m[3][2];
		const float w = value.x * matrix.m[0][3] + value.y * matrix.m[1][3] +
			value.z * matrix.m[2][3] + matrix.m[3][3];
		const float inverseW = std::abs(w) > 0.000001f ? 1.0f / w : 1.0f;
		return { x * inverseW, y * inverseW, z * inverseW };
	}

	std::string EmitCpuParticle(const std::string& particleEffectPath, const Vector3& position) {
		ParticleEffectDesc effect{};
		if (
			particleEffectPath.empty() ||
			!ParticleEffectResource::Load(particleEffectPath, effect) ||
			effect.simulationType != ParticleSimulationType::kCPU
		) {
			return {};
		}
		// CreateEmitterは同名Groupを消去するため、既存粒子を保つPrepare+Emitを使う。
		ParticleEffectResource::PrepareParticleGroup(effect, false);
		ParticleManager::GetInstance()->Emit(
			effect.name,
			position,
			effect.emitter.spawnSize,
			effect.emitter.count,
			effect.behavior
		);
		return effect.name;
	}
}

void SceneRuntimeEffectSystem::Spawn(
	SceneDocument& document,
	const ScenePhysicsSystem& physicsSystem,
	const std::vector<SceneAttackEffectRequest>& requests
) {
	for (const SceneAttackEffectRequest& request : requests) {
		const bool useLegacyPrefab = request.groundEffectType == "Prefab" ||
			(request.groundEffectType.empty() && !request.groundPrefabPath.empty());
		const bool useProceduralCrack = request.groundEffectType == "ProceduralCrack";
		if (request.particleEffectPath.empty() && !useLegacyPrefab && !useProceduralCrack) {
			continue;
		}
		const SceneEntity* spawnEntity = document.FindEntity(request.spawnEntityId);
		if (!spawnEntity) {
			spawnEntity = document.FindEntity(request.ownerEntityId);
		}
		if (!spawnEntity) {
			continue;
		}
		const Vector3 position = TransformCoord(
			request.localOffset,
			SceneTransformResolver::ResolveSceneWorldMatrix(document, *spawnEntity)
		);
		if (const std::string groupName = EmitCpuParticle(request.particleEffectPath, position);
			!groupName.empty()) {
			particleGroupNames_.insert(groupName);
		}
		if (!useLegacyPrefab && !useProceduralCrack) {
			continue;
		}
		const float probeDistance = (std::max)(request.groundProbeDistance, 0.0f);
		SceneStaticRaycastHit groundHit{};
		if (!physicsSystem.RaycastStatic(
			Math::Add(position, { 0.0f, probeDistance * 0.5f, 0.0f }),
			{ 0.0f, -1.0f, 0.0f },
			probeDistance,
			groundHit
		)) {
			continue;
		}
		if (useProceduralCrack) {
			groundCrackRequests_.push_back({
				groundHit.position, groundHit.normal,
				request.groundCrackRadius, request.groundCrackPrimaryBranchCount,
				request.groundCrackSegmentsPerBranch, request.groundCrackBranchProbability,
				request.groundCrackWidth, request.groundCrackLifetime,
				request.groundCrackSurfaceOffset,
				++groundCrackSpawnSerial_ * 747796405u
			});
			continue;
		}
		if (request.groundPrefabPath.empty()) { continue; }
		const uint64_t prefabId = document.InstantiatePrefab(
			request.groundPrefabPath, 0, true
		);
		SceneEntity* prefabRoot = document.FindEntity(prefabId);
		if (!prefabRoot) {
			continue;
		}
		prefabRoot->transform.translate = groundHit.position;
		groundPrefabs_.push_back({
			prefabId,
			(std::max)(request.groundPrefabLifetime, 0.0f)
		});
	}
}

std::vector<SceneGroundCrackSpawnRequest>
SceneRuntimeEffectSystem::ConsumeGroundCrackRequests() {
	std::vector<SceneGroundCrackSpawnRequest> result = std::move(groundCrackRequests_);
	groundCrackRequests_.clear();
	return result;
}

void SceneRuntimeEffectSystem::SpawnHitEffects(
	const std::vector<SceneCombatHitEvent>& events
) {
	for (const SceneCombatHitEvent& event : events) {
		if (const std::string groupName = EmitCpuParticle(kDefaultHitParticlePath, event.hitPosition);
			!groupName.empty()) {
			particleGroupNames_.insert(groupName);
		}
	}
}

void SceneRuntimeEffectSystem::SpawnDeathEffects(
	const SceneDocument& document,
	const std::vector<SceneDeathEffectRequest>& requests
) {
	for (const SceneDeathEffectRequest& request : requests) {
		const SceneEntity* entity = document.FindEntity(request.entityId);
		if (!entity) {
			continue;
		}
		const Vector3 position = TransformCoord(
			{}, SceneTransformResolver::ResolveSceneWorldMatrix(document, *entity)
		);
		if (const std::string groupName = EmitCpuParticle(request.particleEffectPath, position);
			!groupName.empty()) {
			particleGroupNames_.insert(groupName);
		}
	}
}

void SceneRuntimeEffectSystem::Advance(SceneDocument& document, float deltaTime) {
	const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
	for (auto iterator = groundPrefabs_.begin(); iterator != groundPrefabs_.end();) {
		iterator->remainingLifetime -= safeDeltaTime;
		if (iterator->remainingLifetime > 0.0f) {
			++iterator;
			continue;
		}
		const SceneEntity* entity = document.FindEntity(iterator->rootEntityId);
		const bool runtimeOnly = entity && entity->runtimeOnly;
		const bool wasDirty = document.IsDirty();
		document.RemoveEntity(iterator->rootEntityId);
		// Play中だけの短命Prefabの回収で、Authoring Documentを未保存にしない。
		if (runtimeOnly && !wasDirty) {
			document.MarkClean();
		}
		iterator = groundPrefabs_.erase(iterator);
	}
}

void SceneRuntimeEffectSystem::SetWorldEffectsPaused(
	const std::string& ownerKey, bool paused
) {
	ParticleManager* particleManager = ParticleManager::GetInstance();
	const std::string effectiveOwnerKey = "runtime-effects:" + ownerKey;
	if (!pauseOwnerKey_.empty() && pauseOwnerKey_ != effectiveOwnerKey) {
		for (const std::string& groupName : particleGroupNames_) {
			particleManager->SetParticleGroupSimulationPaused(groupName, pauseOwnerKey_, false);
		}
	}
	pauseOwnerKey_ = effectiveOwnerKey;
	for (const std::string& groupName : particleGroupNames_) {
		particleManager->SetParticleGroupSimulationPaused(groupName, effectiveOwnerKey, paused);
	}
}

void SceneRuntimeEffectSystem::Clear(SceneDocument* document) {
	if (!pauseOwnerKey_.empty()) {
		ParticleManager* particleManager = ParticleManager::GetInstance();
		for (const std::string& groupName : particleGroupNames_) {
			particleManager->SetParticleGroupSimulationPaused(groupName, pauseOwnerKey_, false);
		}
	}
	if (document) {
		for (const RuntimeGroundPrefab& effect : groundPrefabs_) {
			const SceneEntity* entity = document->FindEntity(effect.rootEntityId);
			const bool runtimeOnly = entity && entity->runtimeOnly;
			const bool wasDirty = document->IsDirty();
			document->RemoveEntity(effect.rootEntityId);
			// Clearでも同じくRuntime生成物だけをAuthoring Dirty対象から外す。
			if (runtimeOnly && !wasDirty) {
				document->MarkClean();
			}
		}
	}
	groundPrefabs_.clear();
	groundCrackRequests_.clear();
	particleGroupNames_.clear();
	pauseOwnerKey_.clear();
}
