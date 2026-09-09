// 役割: Agent個体とチームのRuntime状態を保持し、群れの移動を更新する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/math/Vector3.h"

class SceneDocument;

struct SceneAgentFormationCapsuleState {
	float radius = 0.0f;
	float halfSegmentLength = 0.0f;
	uint32_t activeMemberCount = 0;
	uint32_t referenceMemberCount = 0;
};

// TeamRuntimeを群れの共通基準、AgentRuntimeを個体差として保持する。
// SceneDocumentとObjectは所有せず、bindingsを通して同じフレームのTransformを更新する。
class SceneAgentSystem {
public:
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime
	);
	bool TryGetTeamFormationCapsuleState(
		const std::string& teamName,
		SceneAgentFormationCapsuleState& state
	) const;
	void ResetTeam(SceneDocument& document, const std::string& teamName);
	/// <summary>指定した個体の移動Runtimeを破棄し、次回更新で初期化し直す。</summary>
	void ResetAgent(uint64_t entityId);
	void Clear();

private:
	struct AgentRuntime {
		Vector3 velocity{};
		Vector3 rotation{};
		Vector3 wanderDirection = { 0.0f, 0.0f, 1.0f };
		Vector3 jitterOffset{};
		Vector3 jitterTargetLocal{};
		Vector3 formationAnchorLocal{};
		Vector3 cachedSchoolingSteering{};
		Vector3 cachedSeparationSteering{};
		float phase = 0.0f;
		float wanderTimer = 0.0f;
		float jitterTimer = 0.0f;
		float schoolingTimer = 0.0f;
		float separationTimer = 0.0f;
		uint64_t flockSeedId = 0;
		uint64_t formationSeedId = 0;
		uint64_t formationRevision = 0;
		uint64_t wanderSeedId = 0;
		uint32_t wanderStep = 0;
		uint32_t jitterStep = 0;
		bool initialized = false;
		bool flockInitialized = false;
		bool formationAnchorInitialized = false;
		bool schoolingCacheValid = false;
		bool separationCacheValid = false;
	};

	struct TeamRuntime {
		Vector3 center{};
		Vector3 velocity{};
		Vector3 heading = { 0.0f, 0.0f, 1.0f };
		Vector3 rotation{};
		Vector3 wanderDirection = { 0.0f, 0.0f, 1.0f };
		Vector3 desiredDirection = { 0.0f, 0.0f, 1.0f };
		std::string forwardAxis = "+Z";
		float phase = 0.0f;
		float wanderTimer = 0.0f;
		float decisionTimer = 0.0f;
		float desiredSpeed = 0.0f;
		uint64_t seedId = 0;
		uint32_t wanderStep = 0;
		uint32_t activeMemberCount = 0;
		uint32_t referenceMemberCount = 0;
		uint64_t formationRevision = 0;
		float formationRadius = 0.0f;
		float formationHalfSegmentLength = 0.0f;
		bool initialized = false;
		bool decisionValid = false;
	};

	std::unordered_map<uint64_t, AgentRuntime> agentRuntimes_;
	std::unordered_map<std::string, TeamRuntime> teamRuntimes_;
};
