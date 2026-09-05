// 役割: Fishing Score AttackのRuntime state、入力、Round抽選、接触得点を所有する。
#pragma once

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/math/Transform.h"
#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector3.h"
#include "../../../engine/math/Vector4.h"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class SceneDocument;
class SceneAgentSystem;
struct SceneComponent;

enum class SceneFishingScoreAttackState {
	Inactive,
	SelectingInitial,
	Navigating,
	SelectingNext,
	Result,
	Faulted
};

struct SceneFishingScoreAttackTextRequest {
	uint64_t entityId = 0;
	std::string text;
	bool hasColor = false;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

struct SceneFishingScoreAttackIconRequest {
	uint64_t entityId = 0;
	std::string texturePath;
	Vector2 size = { 32.0f, 32.0f };
	bool visible = false;
};

struct SceneFishingScoreAttackPlayerWaterBounds {
	uint64_t playerEntityId = 0;
	Vector3 center{};
	float yaw = 0.0f;
	float halfSizeX = 0.0f;
	float halfSizeZ = 0.0f;
};

struct SceneFishingScoreAttackPlayerConstraintRequest {
	uint64_t playerEntityId = 0;
	Vector3 planarPosition{};
	float yaw = 0.0f;
	Vector3 planarVelocity{};
};

struct SceneFishingScoreAttackPlayerResetRequest {
	uint64_t playerEntityId = 0;
	Transform transform{};
	std::string teamName;
	struct EntityReset {
		uint64_t entityId = 0;
		Transform transform{};
	};
	std::vector<EntityReset> entityResets;
};

// SceneやObject、Colliderの所有権は持たず、保存済みComponentからRuntimeの判断だけを行う。
class SceneFishingScoreAttackSystem {
public:
	void UpdateBeforeSimulation(
		SceneDocument& document,
		float deltaTime,
		bool playing
	);
	void UpdateAfterSimulation(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		const SceneAgentSystem& agentSystem,
		bool playing,
		float deltaTime,
		const Vector3& planarVelocity
	);
	void ApplyHookVisualOverrides(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);

	bool IsPlayerMovementAllowed() const;
	bool AcceptWheelZoom() const;
	uint64_t GetResultInputReadyDirectorEntityId() const;
	void QueueFishCountAdjustment(uint64_t directorEntityId, int delta);
	bool TryGetPlayerWaterBounds(SceneFishingScoreAttackPlayerWaterBounds& bounds) const;
	bool ConsumePlayerConstraintRequest(
		SceneFishingScoreAttackPlayerConstraintRequest& request
	);
	bool ConsumePlayerResetRequest(SceneFishingScoreAttackPlayerResetRequest& request);
	void AddFormationOutlineDebugDraw(
		const SceneDocument& document,
		const SceneAgentSystem& agentSystem
	) const;
	const std::vector<SceneFishingScoreAttackTextRequest>& GetTextRequests() const {
		return textRequests_;
	}
	const std::vector<SceneFishingScoreAttackIconRequest>& GetIconRequests() const {
		return iconRequests_;
	}
	const std::string& GetDiagnostic() const { return diagnostic_; }
	void Clear();

private:
	bool Preflight(
		const SceneDocument& document,
		uint64_t directorEntityId,
		const SceneComponent& director,
		std::string& diagnostic
	) const;
	void InitializeRun(SceneDocument& document, const SceneComponent& director);
	void UpdateSelection(SceneDocument& document, const SceneComponent& director);
	void StartRound(SceneDocument& document, const SceneComponent& director);
	void UpdateSharks(
		SceneDocument& document,
		const SceneComponent& director,
		float deltaTime
	);
	bool ResetSharksForRound(
		SceneDocument& document,
		const SceneComponent& director
	);
	void Finish(SceneDocument& document, const SceneComponent& director);
	void Fault(SceneDocument& document, const SceneComponent& director, std::string diagnostic);
	void SetFishPreview(SceneDocument& document, const SceneComponent& director);
	void DeactivatePoolHooks(SceneDocument& document, const SceneComponent& director);
	void BuildTextRequests(const SceneComponent& director);

	SceneFishingScoreAttackState state_ = SceneFishingScoreAttackState::Inactive;
	uint64_t directorEntityId_ = 0;
	struct ActiveHook {
		uint64_t entityId = 0;
		int distanceBand = 0;
		float multiplier = 0.0f;
		int hookMultiplierTier = 1;
	};
	std::vector<ActiveHook> activeHooks_;
	Transform initialPlayerTransform_{};
	std::vector<uint64_t> initialFishEntityIds_;
	std::vector<Transform> initialFishTransforms_;
	struct SharkRuntime {
		Transform initialTransform{};
		float phase = 0.0f;
		float hitCooldown = 0.0f;
		float radiusXScale = 1.0f;
		float radiusZScale = 1.0f;
		float angularSpeedScale = 1.0f;
		float targetRadiusXScale = 1.0f;
		float targetRadiusZScale = 1.0f;
		float targetAngularSpeedScale = 1.0f;
		float wobblePhase = 0.0f;
		float retargetRemainingSeconds = 0.0f;
		Vector3 avoidanceOffset{};
		Vector3 previousPosition{};
		bool hasPreviousPosition = false;
		float wanderHeading = 0.0f;
		float wanderTargetHeading = 0.0f;
		int wanderAvoidanceSide = 0;
		std::mt19937 wanderRandom{};
	};
	std::unordered_map<uint64_t, SharkRuntime> sharkRuntimes_;
	std::string fishingTeamName_;
	SceneFishingScoreAttackPlayerWaterBounds playerWaterBounds_{};
	bool hasInitialPlayerTransform_ = false;
	bool hasPlayerWaterBounds_ = false;
	Vector3 lastSafePlayerPlanarPosition_{};
	float lastSafePlayerYaw_ = 0.0f;
	bool hasLastSafePlayerPlanarPosition_ = false;
	SceneFishingScoreAttackPlayerConstraintRequest playerConstraintRequest_{};
	bool hasPlayerConstraintRequest_ = false;
	bool hasPlayerResetRequest_ = false;
	bool resultInputArmed_ = false;
	std::unordered_map<uint64_t, std::string> hookVisualModelPaths_;
	bool startFromPositiveWaterZ_ = false;
	int selectedFishCount_ = 0;
	int64_t pendingFishCountDelta_ = 0;
	int roundFishCount_ = 0;
	int roundDistanceBand_ = 0;
	float roundMultiplier_ = 0.0f;
	double elapsedSeconds_ = 0.0;
	long long totalScore_ = 0;
	bool timerRunning_ = false;
	bool hasDirector_ = false;
	std::mt19937 random_{};
	std::string diagnostic_;
	std::vector<SceneFishingScoreAttackTextRequest> textRequests_;
	std::vector<SceneFishingScoreAttackIconRequest> iconRequests_;
};
