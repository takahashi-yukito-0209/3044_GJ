// 役割: AttackRunnerの一発Effect Requestを既存ParticleManagerへ橋渡しする。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "SceneAttackRunnerSystem.h"
#include "SceneCombatSystem.h"
#include "SceneHitReactionSystem.h"

class SceneDocument;
class ScenePhysicsSystem;

// 描画側へ渡すだけの値。形状・寿命・GPUリソースの所有はEffect Render側に置く。
struct SceneGroundCrackSpawnRequest {
	Vector3 position{};
	Vector3 normal{ 0.0f, 1.0f, 0.0f };
	float radius = 0.0f;
	uint32_t primaryBranchCount = 0;
	uint32_t segmentsPerBranch = 0;
	float branchProbability = 0.0f;
	float width = 0.0f;
	float lifetime = 0.0f;
	float surfaceOffset = 0.0f;
	uint32_t seed = 0;
};

class SceneRuntimeEffectSystem {
public:
	void Spawn(
		SceneDocument& document,
		const ScenePhysicsSystem& physicsSystem,
		const std::vector<SceneAttackEffectRequest>& requests
	);
	// Combatが確定したHit位置へ、攻撃時刻Effectとは独立したImpact Particleを出す。
	void SpawnHitEffects(const std::vector<SceneCombatHitEvent>& events);
	void SpawnDeathEffects(
		const SceneDocument& document,
		const std::vector<SceneDeathEffectRequest>& requests
	);
	std::vector<SceneGroundCrackSpawnRequest> ConsumeGroundCrackRequests();
	void Advance(SceneDocument& document, float deltaTime);
	void SetWorldEffectsPaused(const std::string& ownerKey, bool paused);
	void Clear(SceneDocument* document = nullptr);

private:
	struct RuntimeGroundPrefab {
		uint64_t rootEntityId = 0;
		float remainingLifetime = 0.0f;
	};
	std::vector<RuntimeGroundPrefab> groundPrefabs_;
	std::vector<SceneGroundCrackSpawnRequest> groundCrackRequests_;
	uint32_t groundCrackSpawnSerial_ = 0;
	std::unordered_set<std::string> particleGroupNames_;
	std::string pauseOwnerKey_;
};
