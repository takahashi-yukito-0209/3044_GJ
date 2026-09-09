// 役割: Fishing Score AttackのRuntime state、入力、Round抽選、接触得点を所有する。
#pragma once

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/math/Transform.h"
#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector3.h"
#include "../../../engine/math/Vector4.h"
#include "../../../engine/scene/SceneRuntimeSessionState.h"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class SceneDocument;
class SceneAgentSystem;
class Camera;
struct SceneComponent;

enum class SceneFishingScoreAttackState {
	Inactive,
	SelectingInitial,
	Navigating,
	SelectingNext,
	Result,
	Faulted
};

enum class SceneFishingScoreAttackTutorialStep {
	Disabled,
	Overview,
	MoveExplanation,
	MovePractice,
	HookExplanation,
	ScoreOnePractice,
	FishCountExplanation,
	FishCountPractice,
	ScoreMultiPractice,
	ScoreAdjustedPractice,
	SharkExplanation,
	FreePlay
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

struct SceneFishingScoreAttackHookBubbleRequest {
	uint64_t hookEntityId = 0;
	uint64_t bubbleSpriteEntityId = 0;
	uint64_t rankIconSpriteEntityId = 0;
	Vector3 worldAnchor{};
	std::string bubbleTexturePath;
	std::string rankIconTexturePath;
	Vector2 bubbleSize = { 128.0f, 128.0f };
	Vector2 bubbleScreenOffset = { 0.0f, 0.0f };
	Vector2 rankIconSize = { 64.0f, 64.0f };
	Vector2 rankIconScreenOffset = { 0.0f, 0.0f };
	bool bubbleVisible = false;
	bool rankIconVisible = false;
};

// 得点した釣り針の位置に重ねる、一時的な加点表示。
struct SceneFishingScoreAttackScorePopup {
	uint64_t entityId = 0;
	std::string text;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	Vector3 worldPosition{};
	float elapsedSeconds = 0.0f;
	float durationSeconds = 0.9f;
	bool active = false;
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

struct SceneFishingScoreAttackFormationParticleSaveRequest {
	uint64_t directorEntityId = 0;
	int pointCount = 48;
	float startSize = 0.26f;
	float endSize = 0.43f;
	uint32_t countPerEmission = 1;
	float emitterSpread = 0.0f;
	float lifetime = 0.8f;
	Vector4 startColor = { 0.1f, 0.9f, 1.0f, 0.65f };
	Vector4 endColor = { 0.1f, 0.9f, 1.0f, 0.65f };
	float emissiveIntensity = 1.0f;
};

struct SceneFishingScoreAttackSessionBeginRequest {
	std::string channelId;
};

struct SceneFishingScoreAttackSessionPublishRequest {
	SceneFishingResultRecord record;
};

// SceneやObject、Colliderの所有権は持たず、保存済みComponentからRuntimeの判断だけを行う。
class SceneFishingScoreAttackSystem {
public:
	void UpdateBeforeSimulation(
		SceneDocument& document,
		const std::string& sceneId,
		float deltaTime,
		bool playing
	);
	void UpdateAfterSimulation(
		SceneDocument& document,
		const std::string& sceneId,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneAgentSystem& agentSystem,
		bool playing,
		float deltaTime,
		const Vector3& planarVelocity
	);
	void ApplyHookVisualOverrides(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
	void ApplySharkVisualOverrides(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
	/// <summary>
	/// 実魚とは別に複製した釣り上げ演出用の魚群を、上方へ引き上げる。
	/// </summary>
	void ApplyFishCatchVisualOverrides(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		const Camera* camera
	);
	/// <summary>
	/// 演出魚群を固定プールとして事前生成する。Runtime binding構築前にのみ呼び出す。
	/// </summary>
	void PrepareFishCatchEffectPool(SceneDocument& document);

	bool IsPlayerMovementAllowed() const;
	/// <summary>
	/// チュートリアル中にカメラ操作を受け付けるかを判定する。
	/// </summary>
	bool IsCameraControlAllowed() const;
	bool AcceptWheelZoom() const;
	uint64_t GetResultInputReadyDirectorEntityId() const;
	void QueueFishCountAdjustment(uint64_t directorEntityId, int delta);
	bool TryGetPlayerWaterBounds(SceneFishingScoreAttackPlayerWaterBounds& bounds) const;
	bool ConsumePlayerConstraintRequest(
		SceneFishingScoreAttackPlayerConstraintRequest& request
	);
	// ポーズメニューから、現在のラウンドを維持して開始位置へ戻す。
	bool RequestPlayerRespawn();
	bool ConsumePlayerResetRequest(SceneFishingScoreAttackPlayerResetRequest& request);
	void AddFormationOutlineDebugDraw(
		const SceneDocument& document,
		const SceneAgentSystem& agentSystem
	) const;
	void AddSharkNavigationDebugDraw(
		const SceneDocument& document
	) const;
	void UpdateFormationParticleEffect(
		const SceneDocument& document,
		const SceneAgentSystem& agentSystem,
		float deltaTime,
		const std::function<bool(uint64_t)>& shouldProcessWorldEffects,
		const std::string& pauseOwnerKey
	);
	void DrawFormationParticleTuningImGui(
		const SceneDocument& document,
		bool runtimeControlsEnabled
	);
	bool ConsumeFormationParticleSaveRequest(
		SceneFishingScoreAttackFormationParticleSaveRequest& request
	);
	bool ConsumeResultSessionBeginRequest(
		SceneFishingScoreAttackSessionBeginRequest& request
	);
	bool ConsumeResultSessionPublishRequest(
		SceneFishingScoreAttackSessionPublishRequest& request
	);
	void SetFormationParticleSaveResult(bool success, std::string message);
	const std::vector<SceneFishingScoreAttackTextRequest>& GetTextRequests() const {
		return textRequests_;
	}
	const std::vector<SceneFishingScoreAttackIconRequest>& GetIconRequests() const {
		return iconRequests_;
	}
	const std::vector<SceneFishingScoreAttackHookBubbleRequest>& GetHookBubbleRequests() const {
		return hookBubbleRequests_;
	}
	const SceneFishingScoreAttackScorePopup& GetScorePopup() const {
		return scorePopup_;
	}
	const std::string& GetDiagnostic() const { return diagnostic_; }
	void Clear(SceneDocument* document = nullptr);

private:
	bool Preflight(
		const SceneDocument& document,
		uint64_t directorEntityId,
		const SceneComponent& director,
		std::string& diagnostic
	) const;
	void InitializeRun(
		SceneDocument& document,
		const SceneComponent& director,
		bool tutorialScene
	);
	void UpdateSelection(SceneDocument& document, const SceneComponent& director);
	bool SpawnHooks(SceneDocument& document, const SceneComponent& director);
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
	void LoadFormationParticleTuning(const SceneComponent& director);
	void UpdateCurrentPositionMultiplier(
		const SceneDocument& document,
		const SceneComponent& director
	);
	void BuildTextRequests(const SceneDocument& document, const SceneComponent& director);
	void InitializeResultTracking(
		const SceneDocument& document,
		const SceneComponent& director
	);
	/// <summary>
	/// チュートリアル対象シーンかを判定する。
	/// </summary>
	bool IsTutorialScene(const std::string& sceneId) const;
	/// <summary>
	/// 現在のチュートリアル段階で説明送り入力を受け付けるかを判定する。
	/// </summary>
	bool IsTutorialAdvanceStep() const;
	/// <summary>
	/// 現在のチュートリアル段階で魚数選択入力を受け付けるかを判定する。
	/// </summary>
	bool IsTutorialFishSelectionAllowed() const;
	/// <summary>
	/// 現在のチュートリアル段階で釣り針得点判定を受け付けるかを判定する。
	/// </summary>
	bool IsTutorialScoringAllowed() const;
	/// <summary>
	/// 現在のチュートリアル段階でサメ処理を動かすかを判定する。
	/// </summary>
	bool IsTutorialSharkAllowed() const;
	/// <summary>
	/// 現在のチュートリアル段階でタイマーを進めるかを判定する。
	/// </summary>
	bool IsTutorialTimerAllowed() const;
	/// <summary>
	/// 現在のチュートリアル段階で釣り針の生成数を1本に絞るかを判定する。
	/// </summary>
	bool IsTutorialSingleHookSpawnStep() const;
	/// <summary>
	/// チュートリアル説明送り入力を処理する。
	/// </summary>
	bool AdvanceTutorialByInput(SceneDocument& document, const SceneComponent& director);
	/// <summary>
	/// チュートリアルの得点成功を段階へ反映する。
	/// </summary>
	void NotifyTutorialHookScored();
	void StartFishCatchAnimation(
		SceneDocument& document,
		const SceneComponent& director,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneAgentSystem& agentSystem,
		const Vector4& effectHookColor
	);
	void MaterializePendingFishCatchEffects(SceneDocument& document);
	/// <summary>
	/// 現在のチュートリアル説明文を取得する。
	/// </summary>
	std::string GetTutorialMessage() const;

	SceneFishingScoreAttackState state_ = SceneFishingScoreAttackState::Inactive;
	uint64_t directorEntityId_ = 0;
	SceneFishingScoreAttackTutorialStep tutorialStep_ =
		SceneFishingScoreAttackTutorialStep::Disabled; // チュートリアルの現在段階。
	float tutorialMovePracticeSeconds_ = 0.0f; // 移動練習で入力移動した累計秒数。
	int tutorialFishCountPracticeStart_ = 1; // 魚数調整練習を始めた時点の魚数。
	bool tutorialFishCountAdjusted_ = false; // 魚数調整練習で数を変更したか。
	int tutorialMultiScoreCount_ = 0; // 複数得点練習で得点した回数。
	bool tutorialAutoStartNextRound_ = false; // 次フレームで1匹ラウンドを自動開始するか。
	struct ActiveHook {
		uint64_t entityId = 0;
		int distanceBand = 0;
		float multiplier = 0.0f;
		int hookMultiplierTier = 1;
	};
	std::vector<ActiveHook> activeHooks_;
	struct FishCatchAnimation {
		uint64_t groupId = 0;
		uint64_t entityId = 0;
		uint64_t effectHookEntityId = 0;
		Vector4 effectHookColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		uint64_t attractorEntityId = 0;
		bool controlsAttractor = false;
		Transform sourceWorldTransform{};
		float elapsedSeconds = 0.0f;
		float targetCameraHeight = 0.0f;
		bool hasTargetCameraHeight = false;
		float screenHorizontalOffset = 0.0f;
		float depthOffset = 0.0f;
		float verticalOffset = 0.0f;
	};
	std::vector<FishCatchAnimation> fishCatchAnimations_;
	struct PendingFishCatchEffect {
		uint64_t groupId = 0;
		uint64_t sourceFishEntityId = 0;
		Transform sourceWorldTransform{};
		float screenHorizontalOffset = 0.0f;
		float depthOffset = 0.0f;
		float verticalOffset = 0.0f;
	};
	std::vector<PendingFishCatchEffect> pendingFishCatchEffects_;
	std::vector<uint64_t> pendingFishCatchEffectRemovals_;
	uint64_t nextFishCatchEffectGroupId_ = 1;
	struct FishCatchEffectPoolSlot {
		uint64_t groupId = 0;
		uint64_t leaderAttractorEntityId = 0;
		uint64_t effectHookEntityId = 0;
		std::vector<uint64_t> fishEntityIds;
		std::vector<uint64_t> followerAttractorEntityIds;
		float lastUsedSeconds = 0.0f;
	};
	std::vector<FishCatchEffectPoolSlot> fishCatchEffectPool_;
	float fishCatchEffectPoolElapsedSeconds_ = 0.0f;
	Transform initialPlayerTransform_{};
	std::vector<uint64_t> initialFishEntityIds_;
	std::vector<Transform> initialFishTransforms_;
	enum class SharkNavigationState {
		Patrol,
		Alert,
		Chase,
		Lost
	};
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
		SharkNavigationState navigationState = SharkNavigationState::Patrol;
		std::vector<Vector3> navigationRoute;
		size_t navigationRouteIndex = 0;
		float patrolRouteRemainingSeconds = 0.0f;
		float chaseRouteRemainingSeconds = 0.0f;
		float alertElapsedSeconds = 0.0f;
		float lostElapsedSeconds = 0.0f;
		float reacquireCooldownRemainingSeconds = 0.0f;
		float alertPulseElapsedSeconds = 0.0f;
		Vector3 lastSeenPlayerPosition{};
		bool hasLastSeenPlayerPosition = false;
		int navigationRejectedFrames = 0;
		std::vector<int> patrolVisitCounts;
		int patrolGridWidth = 0;
		int patrolGridHeight = 0;
		float patrolGridOriginX = 0.0f;
		float patrolGridOriginZ = 0.0f;
		float patrolGridCellSize = 0.0f;
	};
	std::unordered_map<uint64_t, SharkRuntime> sharkRuntimes_;
	std::string fishingTeamName_;
	SceneFishingScoreAttackPlayerWaterBounds playerWaterBounds_{};
	bool hasInitialPlayerTransform_ = false;
	bool hasPlayerWaterBounds_ = false;
	float playerPlanarColliderRadius_ = 0.0f;
	Vector3 lastSafePlayerPlanarPosition_{};
	float lastSafePlayerYaw_ = 0.0f;
	bool hasLastSafePlayerPlanarPosition_ = false;
	struct FormationRecoveryPose {
		Vector3 planarPosition{};
		float yaw = 0.0f;
	};
	std::vector<FormationRecoveryPose> formationRecoveryPoses_;
	Vector3 formationNoProgressReferencePosition_{};
	float formationNoProgressReferenceYaw_ = 0.0f;
	float formationNoProgressSeconds_ = 0.0f;
	bool hasFormationNoProgressReference_ = false;
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
	float currentPositionMultiplier_ = 0.0f;
	bool hasCurrentPositionMultiplier_ = false;
	double elapsedSeconds_ = 0.0;
	long long totalScore_ = 0;
	bool timerRunning_ = false;
	bool hasDirector_ = false;
	bool resultTrackingEnabled_ = false;
	std::string resultChannelId_;
	std::string resultTieBreakMode_ = "HigherRank";
	std::vector<SceneFishingResultRankRecord> resultRankRecords_;
	uint64_t sharkHitCount_ = 0;
	uint64_t sharkFishWeightedCount_ = 0;
	bool resultSessionBeginRequested_ = false;
	SceneFishingScoreAttackSessionBeginRequest resultSessionBeginRequest_{};
	bool resultSessionPublishRequested_ = false;
	SceneFishingScoreAttackSessionPublishRequest resultSessionPublishRequest_{};
	std::mt19937 random_{};
	std::string diagnostic_;
	std::vector<SceneFishingScoreAttackTextRequest> textRequests_;
	std::vector<SceneFishingScoreAttackIconRequest> iconRequests_;
	std::vector<SceneFishingScoreAttackHookBubbleRequest> hookBubbleRequests_;
	SceneFishingScoreAttackScorePopup scorePopup_{};
	float formationParticleEmissionAccumulator_ = 0.0f;
	size_t formationParticlePointCursor_ = 0;
	bool formationParticleActive_ = false;
	std::string formationParticlePauseOwnerKey_;
	int formationParticlePointCount_ = 0;
	float formationParticleStartSize_ = 0.26f;
	float formationParticleEndSize_ = 0.43f;
	uint32_t formationParticleCountPerEmission_ = 1;
	float formationParticleEmitterSpread_ = 0.0f;
	float formationParticleLifetime_ = 0.8f;
	Vector4 formationParticleStartColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
	Vector4 formationParticleEndColor_ = { 0.1f, 0.9f, 1.0f, 0.65f };
	float formationParticleEmissiveIntensity_ = 1.0f;
	bool formationParticleTuningDirty_ = false;
	bool formationParticleSaveRequested_ = false;
	SceneFishingScoreAttackFormationParticleSaveRequest formationParticleSaveRequest_{};
	std::string formationParticleSaveStatus_;
	bool formationParticleSaveStatusIsError_ = false;
};
