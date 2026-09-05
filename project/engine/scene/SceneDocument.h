// 役割: Entity、Component、Hierarchy、チーム設定を保存可能なシーンデータとして所有する。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SceneSettings.h"
#include "SceneEntityReference.h"
#include "../math/Transform.h"
#include "../math/Vector2.h"
#include "../math/Vector4.h"

struct SceneTeamSettings {
	std::string name = "Team";
	bool agentBehaviorOverride = false;
	std::string agentGroupName;
	float agentMinSpeed = 1.0f;
	float agentMaxSpeed = 3.0f;
	float agentTurnSpeed = 2.5f;
	float agentWanderStrength = 0.8f;
	float agentWanderChangeInterval = 4.0f;
	float agentWanderDirectionRange = 1.1f;
	float agentWanderVerticalRange = 0.18f;
	bool agentRandomizeSeedOnPlay = true;
	int agentRandomSeed = 1;
	bool agentUseLeaderStartPosition = false;
	Vector3 agentLeaderStartPosition{};
	float agentFlockDecisionInterval = 0.25f;
	float agentFlockAcceleration = 4.0f;
	float agentFlockTurnRate = 1.5f;
	float agentMemberCenterFollow = 1.5f;
	float agentMemberJitterStrength = 0.35f;
	float agentMemberJitterFrequency = 0.9f;
	float agentMemberJitterUpdateInterval = 0.5f;
	float agentMemberJitterFollowSpeed = 2.0f;
	float agentMemberSpeedVariation = 0.15f;
	float agentMemberLeashDistance = 4.0f;
	float agentMemberLeashStrength = 1.5f;
	float agentMemberCatchupSpeed = 2.0f;
	float agentMemberSeparationUpdateInterval = 0.1f;
	float agentMemberSeparationBlend = 0.5f;
	float agentMemberMinimumDistance = 0.0f;
	bool agentFormationCapsuleEnabled = false;
	bool agentFormationCapsuleScaleWithActiveMembers = false;
	float agentFormationCapsuleRadius = 8.5f;
	float agentFormationCapsuleHalfSegmentLength = 10.5f;
	bool agentUseTeamHeading = false;
	bool agentTeamHeadingFromAverage = true;
	Vector3 agentTeamHeadingDirection = { 0.0f, 0.0f, 1.0f };
	float agentTeamHeadingWeight = 0.75f;
	float agentTeamHeadingFollowSpeed = 2.5f;
	bool agentUseTeamRotation = false;
	float agentTeamRotationWeight = 0.6f;
	float agentTeamRotationFollowSpeed = 4.0f;
	bool agentAlignForwardToVelocity = true;
	std::string agentForwardAxis = "+Z";
	bool agentRotateAxisX = true;
	bool agentRotateAxisY = true;
	bool agentRotateAxisZ = false;
	float agentRotationFollowSpeed = 12.0f;
	float agentPitchFromVerticalVelocity = 1.0f;
	float agentBankingStrength = 0.0f;
	bool agentSchooling = false;
	float agentSchoolingUpdateInterval = 0.0f;
	float agentSchoolingUpdateJitter = 0.0f;
	int agentNeighborLimit = 0;
	float agentSchoolingBlend = 1.0f;
	float agentSeparationRadius = 1.2f;
	float agentAlignmentRadius = 4.0f;
	float agentCohesionRadius = 5.0f;
	float agentSeparationWeight = 1.8f;
	float agentAlignmentWeight = 0.8f;
	float agentCohesionWeight = 0.9f;
	Vector4 agentVisualColor = { 0.25f, 0.75f, 1.0f, 1.0f };
	bool agentEnableLighting = true;
};

// MeshRendererが持つ、モデル標準材質へのEntity単位の上書き設定。
struct SceneMeshMaterialOverride {
	std::string materialName;
	bool enabled = false;
	bool colorOverrideEnabled = false;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	std::string texturePath;
};

struct SceneStatDefinition {
	std::string id = "stat";
	std::string displayName = "Stat";
	float minValue = 0.0f;
	float maxValue = 100.0f;
	float initialValue = 100.0f;
};

struct SceneEventAction {
	std::string type = "SetEntityActive";
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string statId;
	std::string statOperation = "Add";
	float value = 0.0f;
	bool active = true;
	std::string sceneId;
	std::string prefabPath;
	bool prefabParentToTarget = false;
	bool prefabUseTargetTransform = true;
	std::string stateName;
	uint64_t postProcessManagerEntityId = 0;
	std::string postProcessManagerEntityName;
	std::string postProcessProfileId;
	std::string textMotionClipId;
};

struct SceneInputTerm {
	std::string input;
	std::string phase = "Pressed";
};

struct SceneInputGroup {
	std::string mode = "Any";
	std::vector<SceneInputTerm> terms;
};

struct SceneInputExpression {
	std::string mode = "Any";
	std::vector<SceneInputGroup> groups;
};

struct SceneEventBinding {
	std::string triggerType = "OnStart";
	std::string triggerKey;
	std::optional<SceneInputExpression> inputExpression;
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string statId = "hp";
	std::string statComparison = "LessOrEqual";
	float statValue = 0.0f;
	Vector3 targetPosition{};
	float radius = 1.0f;
	bool triggerOnce = true;
	float cooldown = 0.0f;
	std::string textMotionClipId;
	std::vector<SceneEventAction> actions;
};

// 完全なPost Process設定をManager Entity単位で切り替えるAuthoring Profile。
// Runtime中の選択状態は保存せず、SceneEventSystemから要求として渡す。
enum class ScenePostProcessAutomationParameter {
	DissolveThreshold
};

enum class ScenePostProcessAutomationPlayback {
	OneShot
};

enum class ScenePostProcessAutomationEasing {
	Linear
};

struct ScenePostProcessAutomation {
	ScenePostProcessAutomationParameter parameter =
		ScenePostProcessAutomationParameter::DissolveThreshold;
	float startValue = 0.0f;
	float endValue = 1.0f;
	float duration = 2.0f;
	ScenePostProcessAutomationPlayback playback =
		ScenePostProcessAutomationPlayback::OneShot;
	ScenePostProcessAutomationEasing easing =
		ScenePostProcessAutomationEasing::Linear;
};

struct ScenePostProcessProfile {
	std::string id = "Profile1";
	std::string label = "Profile 1";
	ScenePostProcessSettings settings{};
	std::vector<ScenePostProcessAutomation> automations;
};

struct SceneAnimationKeyframe {
	float time = 0.0f;
	Vector3 value{};
	// Empty inherits the owning Track setting. The start Key owns the segment
	// from itself to the next Key.
	std::string easingToNext;
	// LocalPosition-only midpoint offset. Zero keeps the segment straight.
	Vector3 positionBulge{};
};

struct SceneAttackHitWindow {
	float startTime = 0.15f;
	float endTime = 0.35f;
	uint64_t hitBoxEntityId = 0;
	std::string hitBoxEntityName;
	// HitBoxは新規専用EntityのPayloadを使い、WindowLegacyは旧Window Payloadを適用する。
	std::string payloadSource = "HitBox";
	float damage = 10.0f;
	float poiseDamage = 0.0f;
	float knockback = 0.0f;
	// 水平Knockbackとは独立した、被弾時に一度だけ適用する上向き初速。
	float verticalKnockback = 0.0f;
	// 有効Window中だけBox ColliderのHalf Sizeを置き換えるAuthoring値。
	bool overrideHitBoxHalfSize = false;
	Vector3 hitBoxHalfSize = { 1.0f, 1.0f, 1.0f };
	float hitStopDuration = 0.0f;
	std::string reactionTag = "Light";
	std::string knockbackDirectionMode = "RadialFromAttacker";
	Vector3 knockbackLocalDirection = { 0.0f, 0.0f, 1.0f };
	std::string hitPolicy = "OncePerActivation";
	float targetCooldown = 0.15f;
};

// AttackRunner時刻から一発Particleを発火するためのAuthoringデータ。
struct SceneAttackEffectEvent {
	float time = 0.15f;
	std::string particleEffectPath;
	uint64_t spawnEntityId = 0;
	std::string spawnEntityName;
	Vector3 localOffset{};
	// 空でない場合、Spawn Entity下方のStatic Colliderへ設置する短命Prefab。
	std::string groundPrefabPath;
	float groundProbeDistance = 6.0f;
	float groundPrefabLifetime = 1.2f;
	// None / Prefab / ProceduralCrack。旧Prefab Pathだけのデータは読込時にPrefabへ正規化する。
	std::string groundEffectType;
	float groundCrackRadius = 3.2f;
	uint32_t groundCrackPrimaryBranchCount = 10;
	uint32_t groundCrackSegmentsPerBranch = 6;
	float groundCrackBranchProbability = 0.25f;
	float groundCrackWidth = 0.06f;
	float groundCrackLifetime = 1.2f;
	float groundCrackSurfaceOffset = 0.03f;
};

// AttackDefinitionはStateMachineから再生する攻撃時系列の共有データ。
struct SceneAttackDefinition {
	std::string name = "Attack";
	std::string animation;
	uint64_t animationTargetEntityId = 0;
	std::string animationTargetEntityName;
	uint64_t hitBoxEntityId = 0;
	std::string hitBoxEntityName;
	float windup = 0.15f;
	float activeTime = 0.2f;
	float recovery = 0.35f;
	float forwardDistance = 0.0f;
	float sideDistance = 0.0f;
	std::string motionEasing = "SmoothStep";
	// FixedAtStartは既存互換。InputDirectionはRuntimeSceneがPlayer入力から渡すXZ方向を使う。
	std::string facingMode = "FixedAtStart";
	uint64_t facingTargetEntityId = 0;
	std::string facingTargetEntityName;
	float facingRotateAngle = 0.0f;
	bool loopEnabled = false;
	int loopMaxCount = 0;
	float loopSafetyTimeout = 0.0f;
	std::vector<SceneAttackHitWindow> hitWindows;
	std::vector<SceneAttackEffectEvent> effectEvents;
};

struct SceneAnimationTrack {
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string property = "LocalPosition";
	std::string easing = "SmoothStep";
	std::vector<SceneAnimationKeyframe> keyframes;
};

struct ScenePrefabAnimationClip {
	std::string name = "Animation";
	float duration = 1.0f;
	bool loop = false;
	bool playOnStart = true;
	std::vector<SceneAnimationTrack> tracks;
};

struct SceneCameraSwitchEntry {
	uint64_t cameraEntityId = 0;
	std::string cameraEntityName;
};

struct SceneStateParameter {
	std::string name = "Parameter";
	std::string type = "Float";
	float floatValue = 0.0f;
	int intValue = 0;
	bool boolValue = false;
	std::string stringValue;
	uint64_t entityId = 0;
	std::string entityName;
};

struct SceneStateDefinition {
	std::string name = "State";
	std::string actionId = "Builtin.Idle";
	std::vector<SceneStateParameter> parameters;
};

struct Text2DPlacement {
	Vector2 position = { 0.0f, 0.0f };
	float rotation = 0.0f;
	Vector2 scale = { 1.0f, 1.0f };
	Vector2 pivot = { 0.0f, 0.0f };
	Vector2 viewportAnchor = { 0.0f, 0.0f };
	int sortingOrder = 0;
	bool clipEnabled = false;
};

struct SceneTextMotionKeyframe {
	float timeSeconds = 0.0f;
	Vector2 positionOffset = { 0.0f, 0.0f };
	float rotationOffset = 0.0f;
	Vector2 scaleMultiplier = { 1.0f, 1.0f };
	float opacityMultiplier = 1.0f;
	std::string easingToNext = "SmoothStep";
};

struct SceneTextMotionClip {
	std::string id;
	bool holdFinalPose = false;
	std::vector<SceneTextMotionKeyframe> keyframes;
};

struct SceneGameFlowWave {
	uint64_t spawnerEntityId = 0;
	int count = 1;
};

struct SceneGameFlowPhase {
	std::string id;
	std::string label;
	std::vector<SceneGameFlowWave> waves;
};

struct SceneFishingHookPoolEntry {
	uint64_t hookEntityId = 0;
	std::vector<float> weightsByDistanceBand;
};

// 釣り針の抽選ランクに紐づく、外部参照可能な安定IDと表示／表示モデル設定。
struct SceneFishingHookRankDefinition {
	std::string id;
	std::string displayName;
	std::string modelPath;
	std::string iconTexturePath;
	float scoreMultiplier = 1.0f;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

struct SceneFishingHookBandSettings {
	float distanceMultiplier = 1.0f;
	int hookCount = 0;
	std::vector<float> hookMultiplierWeights;
};

struct SceneComponent {
	SceneComponent() = default;
	SceneComponent(const char* componentType) : type(componentType ? componentType : "") {}
	SceneComponent(std::string componentType, bool componentEnabled = true)
		: type(std::move(componentType)), enabled(componentEnabled) {}

	// Prefab Property OverrideがComponentの並び順ではなく安定IDで対応付ける。
	uint64_t localId = 0;
	std::string type;
	bool enabled = true;
	std::string modelPath;
	std::string meshCullMode = "Back";
	Vector3 meshVisualRotation{};
	bool meshEnvironmentReflectionOverride = false;
	float meshEnvironmentReflectionIntensity = 0.3f;
	std::vector<SceneMeshMaterialOverride> meshMaterialOverrides;
	std::string texturePath;
	bool environmentSkyboxEnabled = true;
	std::string environmentSkyboxPath;
	float environmentSkyboxIntensity = 1.0f;
	float environmentReflectionIntensity = 0.3f;
	Vector2 spriteSize = { 100.0f, 100.0f };
	Vector2 spriteAnchor = { 0.5f, 0.5f };
	std::string spriteRenderSpace = "Scene2D";
	Vector2 spriteViewportAnchor = { 0.0f, 0.0f };
	Vector4 spriteColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool spriteFlipX = false;
	bool spriteFlipY = false;
	std::string textValue = "Text";
	std::string textRenderSpace = "ScreenOverlay";
	std::string textFontSource = "System";
	std::string textFontResourcePath;
	std::string textFontFamily = "Yu Gothic UI";
	float textFontSize = 32.0f;
	std::string textFontWeight = "Regular";
	std::string textFontStyle = "Normal";
	Vector4 textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	float textOpacity = 1.0f;
	std::string textHorizontalAlignment = "Left";
	std::string textVerticalAlignment = "Top";
	std::string textWrapMode = "NoWrap";
	std::string textOverflowMode = "Overflow";
	Vector2 textLayoutSize = { 0.0f, 0.0f };
	float textCharacterSpacing = 0.0f;
	float textLineSpacing = 1.0f;
	bool textOutlineEnabled = false;
	Vector4 textOutlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	float textOutlineWidth = 2.0f;
	bool textShadowEnabled = false;
	Vector4 textShadowColor = { 0.0f, 0.0f, 0.0f, 0.5f };
	Vector2 textShadowOffset = { 2.0f, 2.0f };
	Vector2 textViewportAnchor = { 0.0f, 0.0f };
	Vector2 textPivot = { 0.0f, 0.0f };
	int textSortingOrder = 0;
	bool textClipEnabled = false;
	bool textHasPlacementProfiles = false;
	Text2DPlacement textOverlayPlacement{};
	Text2DPlacement textScene2DPlacement{};
	std::vector<SceneTextMotionClip> textMotionClips;
	bool gameFlowAutoStart = true;
	int gameFlowCountdownStart = 3;
	float gameFlowCountdownStepSeconds = 1.0f;
	std::string gameFlowStartCueText = "START!";
	float gameFlowStartCueSeconds = 0.75f;
	float gameFlowInterPhaseDelaySeconds = 1.0f;
	float gameFlowResultRevealDelaySeconds = 0.75f;
	float gameFlowTimerDisplayStepSeconds = 0.1f;
	std::string gameFlowTimerPrefix = "TIME ";
	std::string gameFlowResultPrefix = "CLEAR TIME ";
	uint64_t gameFlowCountdownTextEntityId = 0;
	std::string gameFlowCountdownMotionClipId;
	uint64_t gameFlowPhaseTextEntityId = 0;
	std::string gameFlowPhaseMotionClipId;
	uint64_t gameFlowTimerTextEntityId = 0;
	uint64_t gameFlowRemainingTextEntityId = 0;
	std::string gameFlowRemainingPrefix = "ENEMIES ";
	uint64_t gameFlowResultRootEntityId = 0;
	uint64_t gameFlowResultTimeTextEntityId = 0;
	std::string gameFlowResultMotionClipId;
	std::vector<SceneGameFlowPhase> gameFlowPhases;
	uint64_t fishingPlayerEntityId = 0;
	std::vector<uint64_t> fishingFishEntityIds;
	uint64_t fishingHookSpawnAreaEntityId = 0;
	uint64_t fishingHookPoolEntityId = 0;
	uint64_t fishingWaterVolumeEntityId = 0;
	float fishingDurationSeconds = 60.0f;
	int fishingMaxSelectableFishCount = 5;
	std::string fishingConfirmInput = "ENTER";
	std::optional<SceneInputExpression> fishingConfirmInputExpression;
	int fishingDistanceBandCount = 5;
	int fishingHooksPerDistanceBand = 2;
	float fishingDistanceMultiplierBase = 1.0f;
	float fishingDistanceMultiplierStep = 0.2f;
	bool fishingUseHookBandSettings = false;
	std::vector<SceneFishingHookBandSettings> fishingHookBands;
	float fishingHookScoreUnit = 100.0f;
	float fishingFishMultiplierBase = 1.0f;
	float fishingFishMultiplierPerAdditionalFish = 1.0f;
	std::vector<SceneFishingHookRankDefinition> fishingHookRanks;
	std::vector<float> fishingHookTierScoreMultipliers = {
		1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
		6.0f, 7.0f, 8.0f, 9.0f, 10.0f
	};
	std::vector<Vector4> fishingHookMultiplierColors;
	float fishingHookColorEmissiveIntensity = 0.35f;
	bool fishingHookLegendVisible = false;
	uint64_t fishingHookLegendTitleTextEntityId = 0;
	std::vector<uint64_t> fishingHookLegendTextEntityIds;
	std::string fishingHookLegendTitle = "HOOK BONUS";
	std::string fishingHookLegendPrefix = "x";
	std::vector<uint64_t> fishingHookLegendIconEntityIds;
	Vector2 fishingHookLegendIconSize = { 32.0f, 32.0f };
	bool fishingRandomizeSeedOnPlay = true;
	int fishingRandomSeed = 1;
	uint64_t fishingFishCountTextEntityId = 0;
	uint64_t fishingTimerTextEntityId = 0;
	uint64_t fishingScoreTextEntityId = 0;
	uint64_t fishingMultiplierTextEntityId = 0;
	uint64_t fishingResultTextEntityId = 0;
	std::string fishingFishCountPrefix = "FISH ";
	std::string fishingTimerPrefix = "TIME ";
	std::string fishingScorePrefix = "SCORE ";
	std::string fishingMultiplierPrefix = "MULTIPLIER ";
	std::string fishingResultPrefix = "RESULT ";
	bool fishingUseFormationCapsuleCollision = false;
	bool fishingFormationOutlineVisible = false;
	Vector4 fishingFormationOutlineColor = { 0.1f, 0.9f, 1.0f, 1.0f };
	float fishingFormationOutlineBloomIntensity = 1.0f;
	float fishingFormationOutlineYOffset = 0.25f;
	int fishingFormationOutlineSegments = 48;
	float fishingSpawnHalfSizeX = 10.0f;
	float fishingSpawnHalfSizeZ = 10.0f;
	float fishingSpawnMinimumDistance = 0.0f;
	int fishingSpawnMaxAttempts = 16;
	std::vector<SceneFishingHookPoolEntry> fishingHookPoolEntries;
	int fishingHookBaseScore = 0;
	float fishingSharkRadiusX = 12.0f;
	float fishingSharkRadiusZ = 18.0f;
	float fishingSharkAngularSpeed = 0.35f;
	float fishingSharkInitialPhase = 0.0f;
	int fishingSharkPenaltyScore = 300;
	float fishingSharkHitCooldownSeconds = 2.0f;
	float fishingSharkPathRandomness = 0.2f;
	float fishingSharkWanderMoveSpeed = 0.0f;
	float fishingSharkWanderMaximumTurnRate = 1.2f;
	float fishingSharkObstacleAvoidanceDistance = 8.0f;
	float fishingSharkObstacleAvoidanceStrength = 0.65f;
	float fishingSharkObstacleAvoidanceResponse = 4.0f;
	bool cameraIsMain = false;
	float cameraFovY = 0.45f;
	float cameraNearClip = 0.1f;
	float cameraFarClip = 1000.0f;
	bool cameraInvertYaw = false;
	bool cameraInvertPitch = false;
	std::string lightType = "Point";
	Vector4 lightColor = { 1.0f, 0.85f, 0.65f, 1.0f };
	float lightIntensity = 2.0f;
	float lightRange = 8.0f;
	float lightDecay = 1.0f;
	float lightSpotInnerAngle = 25.0f;
	float lightSpotOuterAngle = 35.0f;
	bool lightCastsShadow = false;
	float lightShadowBias = 0.0025f;
	float lightShadowNormalBias = 0.02f;
	float lightShadowStrength = 0.55f;
	float lightShadowDistance = 45.0f;
	float lightShadowOrthographicSize = 40.0f;
	float lightShadowNearClip = 0.1f;
	float lightShadowFarClip = 120.0f;
	bool lightShadowTexelSnap = true;
	uint64_t monitorCameraEntityId = 0;
	std::string monitorCameraName;
	std::string monitorResolutionPreset = "Square 512";
	uint32_t monitorWidth = 512;
	uint32_t monitorHeight = 512;
	bool monitorHideSelf = true;
	std::string cameraSwitchTriggerKey = "F5";
	bool cameraSwitchWrap = true;
	std::vector<SceneCameraSwitchEntry> cameraSwitchEntries;
	uint64_t thirdPersonTargetEntityId = 0;
	std::string thirdPersonTargetEntityName;
	float thirdPersonDistance = 8.0f;
	float thirdPersonAimDistance = 3.0f;
	Vector3 thirdPersonTargetOffset = { 0.0f, 1.35f, 0.0f };
	Vector3 thirdPersonAimTargetOffset = { 0.0f, 1.55f, 0.0f };
	float thirdPersonMouseSensitivity = 0.005f;
	float thirdPersonMinPitch = -1.45f;
	float thirdPersonMaxPitch = 1.35f;
	float thirdPersonOcclusionMargin = 0.45f;
	uint32_t thirdPersonOcclusionMask = 0xffffffffu;
	float thirdPersonOcclusionPullInSmoothTime = 0.04f;
	float thirdPersonOcclusionRecoverySmoothTime = 0.18f;
	float thirdPersonPositionSmoothTime = 0.12f;
	float thirdPersonRotationSmoothTime = 0.08f;
	std::string thirdPersonYawReference = "World";
	bool thirdPersonAllowMouseInput = true;
	bool thirdPersonOcclusionEnabled = true;
	bool thirdPersonAimModeEnabled = true;
	bool thirdPersonInvertYaw = false;
	bool thirdPersonInvertPitch = false;
	bool animatorPlayOnStart = true;
	bool animatorLoop = true;
	float animatorSpeed = 1.0f;
	int animatorDefaultClip = 0;
	float animatorTransitionDuration = 0.2f;
	std::string animatorBlendCurve = "SmoothStep";
	std::string audioClipPath;
	std::string audioSpatialMode = "TwoD";
	float audioMinimumDistance = 1.4f;
	float audioMaximumDistance = 30.0f;
	float audioStereoAreaWidth = 1.0f;
	std::string audioBus = "SFX";
	float audioVolume = 1.0f;
	float audioPitch = 1.0f;
	bool audioLoop = false;
	bool audioPlayOnStart = false;
	bool audioStopOnDisable = true;
	bool audioDecompressOnLoad = true;
	bool audioStreamFromDisk = false;
	bool audioPersistAcrossScenes = false;
	float audioBgmFadeSeconds = 0.5f;
	std::string audioListenerMode = "Hybrid";
	std::string physicsBodyType = "Static";
	float physicsMass = 1.0f;
	bool physicsUseGravity = true;
	float physicsGravityScale = 1.0f;
	float physicsDrag = 0.0f;
	float physicsRestitution = 0.0f;
	float physicsFriction = 0.0f;
	float physicsMaxFallSpeed = 100.0f;
	Vector3 physicsVelocity = { 0.0f, 0.0f, 0.0f };
	bool physicsFreezePositionX = false;
	bool physicsFreezePositionY = false;
	bool physicsFreezePositionZ = false;
	Vector3 colliderOffset = { 0.0f, 0.0f, 0.0f };
	Vector3 colliderSizeMultiplier = { 1.0f, 1.0f, 1.0f };
	Vector4 colliderDebugColor = { 0.2f, 0.95f, 0.7f, 1.0f };
	std::string colliderShape = "Box";
	float colliderSphereRadius = 0.5f;
	bool colliderDebugVisible = true;
	std::string colliderDebugDrawMode = "Wireframe";
	int colliderDebugSegments = 16;
	bool colliderIsTrigger = false;
	bool colliderActive = true;
	uint32_t colliderLayer = 0xffffffffu;
	uint32_t colliderMask = 0xffffffffu;
	float playerMoveSpeed = 10.8f;
	float playerJumpVelocity = 37.2f;
	float playerTurnResponsiveness = 0.018f;
	float playerDashMultiplier = 1.65f;
	bool playerCameraRelativeMove = true;
	bool playerAllowJump = true;
	bool playerAutoForward = false;
	std::string playerInputMode = "KeyboardMouse";
	float playerGamepadDeadzone = 0.20f;
	std::string agentBehaviorName = "Fish";
	// Free3Dは既存の遊泳群制御、GroundXZはEnemyBehaviorの速度へ離隔だけを加える。
	std::string agentMovementMode = "Free3D";
	std::string agentProfileName = "Default";
	std::string agentGroupName;
	uint64_t agentBoundsEntityId = 0;
	std::string agentBoundsName;
	uint64_t agentAttractorEntityId = 0;
	std::string agentAttractorTag;
	bool agentUseWaterBounds = true;
	float agentMinSpeed = 1.0f;
	float agentMaxSpeed = 3.0f;
	float agentTurnSpeed = 2.5f;
	float agentWanderStrength = 0.8f;
	float agentWanderChangeInterval = 4.0f;
	float agentWanderDirectionRange = 1.1f;
	float agentWanderVerticalRange = 0.18f;
	bool agentRandomizeSeedOnPlay = true;
	int agentRandomSeed = 1;
	float agentFlockDecisionInterval = 0.25f;
	float agentFlockAcceleration = 4.0f;
	float agentFlockTurnRate = 1.5f;
	float agentMemberCenterFollow = 1.5f;
	float agentMemberJitterStrength = 0.35f;
	float agentMemberJitterFrequency = 0.9f;
	float agentMemberJitterUpdateInterval = 0.5f;
	float agentMemberJitterFollowSpeed = 2.0f;
	float agentMemberSpeedVariation = 0.15f;
	float agentMemberLeashDistance = 4.0f;
	float agentMemberLeashStrength = 1.5f;
	float agentMemberCatchupSpeed = 2.0f;
	float agentMemberSeparationUpdateInterval = 0.1f;
	float agentMemberSeparationBlend = 0.5f;
	float agentMemberMinimumDistance = 0.0f;
	float agentBoundsWeight = 3.0f;
	bool agentUseTeamHeading = false;
	bool agentTeamHeadingFromAverage = true;
	Vector3 agentTeamHeadingDirection = { 0.0f, 0.0f, 1.0f };
	float agentTeamHeadingWeight = 0.75f;
	float agentTeamHeadingFollowSpeed = 2.5f;
	bool agentUseTeamRotation = false;
	float agentTeamRotationWeight = 0.6f;
	float agentTeamRotationFollowSpeed = 4.0f;
	bool agentAlignForwardToVelocity = true;
	std::string agentForwardAxis = "+Z";
	bool agentRotateAxisX = true;
	bool agentRotateAxisY = true;
	bool agentRotateAxisZ = false;
	float agentRotationFollowSpeed = 12.0f;
	float agentPitchFromVerticalVelocity = 1.0f;
	float agentBankingStrength = 0.0f;
	bool agentSchooling = false;
	float agentSchoolingUpdateInterval = 0.0f;
	float agentSchoolingUpdateJitter = 0.0f;
	int agentNeighborLimit = 0;
	float agentSchoolingBlend = 1.0f;
	float agentSeparationRadius = 1.2f;
	float agentAlignmentRadius = 4.0f;
	float agentCohesionRadius = 5.0f;
	float agentSeparationWeight = 1.8f;
	float agentAlignmentWeight = 0.8f;
	float agentCohesionWeight = 0.9f;
	float agentAttractorWeight = 0.0f;
	bool agentTeamSettingsOverride = false;
	Vector4 agentVisualColor = { 0.25f, 0.75f, 1.0f, 1.0f };
	bool agentEnableLighting = true;
	std::string attractorTag = "Default";
	std::string attractorTargetBehaviorName;
	std::string attractorTargetProfileName;
	float attractorRadius = 6.0f;
	float attractorStrength = 1.0f;
	Vector4 attractorVisualColor = { 1.0f, 0.35f, 0.45f, 1.0f };
	Vector3 waterHalfSize = { 10.0f, 4.0f, 10.0f };
	Vector3 waterOffset = { 0.0f, 0.0f, 0.0f };
	bool waterSurfaceEnabled = true;
	Vector4 waterSurfaceBaseColor = { 0.04f, 0.55f, 0.78f, 1.0f };
	Vector4 waterSurfaceHighlightColor = { 0.42f, 0.95f, 1.20f, 1.0f };
	float waterSurfaceAlpha = 0.36f;
	float waterSurfaceWaveScale = 1.0f;
	float waterSurfaceNormalStrength = 0.75f;
	float waterSurfaceFresnelPower = 3.0f;
	bool waterLightShaftEnabled = true;
	Vector4 waterLightColor = { 0.55f, 0.90f, 1.15f, 1.0f };
	Vector3 waterLightDirection = { -0.25f, -1.0f, 0.18f };
	float waterLightIntensity = 0.55f;
	float waterLightDensity = 0.045f;
	float waterLightCausticsIntensity = 0.35f;
	float waterLightCausticsScale = 0.08f;
	float waterLightCausticsSpeed = 1.0f;
	float waterLightBreakupStrength = 1.0f;
	float waterLightWarpStrength = 1.0f;
	float waterLightNoiseScale = 1.0f;
	int waterLightSampleCount = 16;
	float waterMoveSpeedMultiplier = 0.45f;
	float waterGravityScale = 0.55f;
	float waterDrag = 4.0f;
	float waterMaxFallSpeed = 5.0f;
	float waterSwimUpSpeed = 12.0f;
	std::string entityReferenceName = "Target";
	SceneEntityReference entityReferenceTarget{};
	std::string sceneTransitionTargetSceneId = "gameplay";
	std::string sceneTransitionTriggerType = "Key";
	std::string sceneTransitionTriggerKey = "ENTER";
	std::string cameraPathTargetCameraName;
	std::string cameraPathTriggerType = "Key";
	std::string cameraPathTriggerKey = "C";
	float cameraPathEnterDuration = 0.5f;
	float cameraPathExitDuration = 0.5f;
	std::string cameraPathInterpolation = "Linear";
	std::string cameraPathDefaultEasing = "SmoothStep";
	bool cameraPathReturnToPreviousCamera = true;
	bool cameraPathStartFromCurrentCamera = true;
	bool cameraPathAutoCollectChildPoints = true;
	float cameraPathPointDurationToNext = 1.0f;
	std::string cameraPathPointEasingToNext = "SmoothStep";
	std::vector<SceneStatDefinition> stats;
	std::vector<SceneAttackDefinition> attackDefinitions;
	std::vector<SceneEventBinding> eventBindings;
	std::vector<ScenePrefabAnimationClip> prefabAnimationClips;
	std::string stateMachineInitialState = "Idle";
	bool stateMachineResetOnDisable = true;
	std::vector<SceneStateDefinition> stateMachineStates;
	std::string factionName = "Neutral";
	float hitBoxDamage = 10.0f;
	float hitBoxPoiseDamage = 0.0f;
	float hitBoxKnockback = 0.0f;
	float hitBoxVerticalKnockback = 0.0f;
	std::string hitBoxKnockbackDirectionMode = "RadialFromAttacker";
	Vector3 hitBoxKnockbackLocalDirection = { 0.0f, 0.0f, 1.0f };
	// Runtime専用。Attack Windowが切り替わった接触を別Hitとして扱うための世代番号。
	uint64_t hitBoxAttackWindowSerial = 0;
	// Runtime専用。同一Attack内で同Frameに重なったReactionの優先判定に使う。
	uint64_t hitBoxAttackExecutionId = 0;
	uint32_t hitBoxReactionPriority = 0xffffffffu;
	std::string hitBoxHitPolicy = "OncePerActivation";
	float hitBoxTargetCooldown = 0.15f;
	float hitBoxHitStopDuration = 0.0f;
	std::string hitBoxReactionTag = "Light";
	std::string hitBoxDamageStatId = "hp";
	std::string hitBoxPoiseStatId = "poise";
	uint64_t hitBoxOwnerEntityId = 0;
	std::string hitBoxOwnerEntityName;
	bool hitBoxIgnoreSameFaction = true;
	float hurtBoxDamageMultiplier = 1.0f;
	std::string hurtBoxHealthStatId = "hp";
	uint64_t hurtBoxStatsEntityId = 0;
	std::string hurtBoxStatsEntityName;
	float hitReactionKnockbackMultiplier = 1.0f;
	std::string hitReactionTriggerMode = "MinimumDamage";
	float hitReactionMinimumPoiseDamage = 0.0f;
	std::string hitReactionPoiseStatId = "poise";
	float hitReactionPoiseRecoveryDelay = 1.0f;
	std::string hitReactionStateName = "Hit";
	float hitReactionStateDuration = 0.2f;
	std::string deathPresentationStateName = "Dead";
	float deathPresentationDeactivateDelay = 2.0f;
	std::string deathPresentationEffectPath;
	uint64_t boneAttachmentTargetEntityId = 0;
	std::string boneAttachmentTargetEntityName;
	std::string boneAttachmentJointName;
	// ManualOffsetはEntity Transformをオフセットとして使う。
	// MatchSourceBoneは武器側の指定ボーンを対象ボーンへ一致させる。
	std::string boneAttachmentAlignmentMode = "ManualOffset";
	std::string boneAttachmentSourceJointName;
	bool boneAttachmentInheritScale = true;
	uint64_t enemyTargetEntityId = 0;
	std::string enemyTargetEntityName = "Player";
	std::string enemySpawnerPrefabPath;
	int enemySpawnerInitialCount = 0;
	int enemySpawnerMaxAlive = 10;
	float enemySpawnerInterval = 1.0f;
	float enemySpawnerRadius = 3.0f;
	bool enemySpawnerAutoStart = true;
	std::string enemyHealthStatId = "hp";
	float enemyDetectionRange = 12.0f;
	float enemyLoseRange = 18.0f;
	float enemyAttackRange = 2.0f;
	float enemyMoveSpeed = 3.0f;
	float enemyTurnSpeed = 8.0f;
	float enemyAttackCooldown = 1.5f;
	float enemyAttackWindup = 0.2f;
	float enemyAttackActiveTime = 0.25f;
	float enemyAttackRecovery = 0.55f;
	int enemyAttackAnimationClip = 0;
	std::string enemyAttackPrefabAnimationClip;
	uint64_t enemyAttackHitBoxEntityId = 0;
	std::string enemyAttackHitBoxEntityName;
	Vector3 projectileDirection = { 0.0f, 0.0f, 1.0f };
	float projectileSpeed = 12.0f;
	float projectileGravity = 0.0f;
	float projectileLifetime = 5.0f;
	bool projectileDestroyOnHit = true;
	uint64_t projectileHomingTargetEntityId = 0;
	std::string projectileHomingTargetEntityName;
	float projectileHomingStrength = 0.0f;
	std::vector<ScenePostProcessProfile> postProcessProfiles;
	uint64_t postProcessStatusTextEntityId = 0;
	std::string postProcessStatusTextEntityName;
	std::string postProcessStatusTextPrefix = "PostEffect: ";
};

struct ScenePrefabLink {
	std::string assetId;
	std::string sourcePath;
	uint64_t instanceRootId = 0;
	uint64_t localId = 0;
};

struct SceneEntity {
	uint64_t id = 0;
	uint64_t parentId = 0;
	std::string name;
	bool folder = false;
	bool folderTeamEnabled = false;
	bool active = true;
	bool locked = false;
	bool runtimeOnly = false;
	// 最外層Prefab Linkの互換表示。実体はprefabLinksへ保持する。
	// 元アセットとの対応がない通常Entityは全て既定値のままにする。
	std::string prefabAssetId;
	std::string prefabSourcePath;
	uint64_t prefabInstanceRootId = 0;
	uint64_t prefabLocalId = 0;
	// 外側から内側の順にPrefab Instance境界を保持する。
	std::vector<ScenePrefabLink> prefabLinks;
	std::string teamName;
	QuaternionTransform transform{};
	std::string modelPath;
	std::string spriteTexturePath;
	Vector2 spriteSize = { 100.0f, 100.0f };
	Vector2 spriteAnchor = { 0.5f, 0.5f };
	Vector4 spriteColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool spriteFlipX = false;
	bool spriteFlipY = false;
	std::vector<SceneComponent> components;
};

enum class ScenePrefabOverrideKind {
	EntityProperty,
	ComponentProperty,
	AddedComponent,
	RemovedComponent,
	AddedEntity,
	RemovedEntity,
	StaleEntity
};

struct ScenePrefabPropertyOverride {
	ScenePrefabOverrideKind kind = ScenePrefabOverrideKind::EntityProperty;
	uint64_t entityLocalId = 0;
	uint64_t instanceEntityId = 0;
	uint64_t componentLocalId = 0;
	std::string entityName;
	std::string componentType;
	std::string propertyPath;
	std::string label;
};

class SceneDocument {
public:
	void Clear(const std::string& sceneName = {});

	bool Load(const std::string& filePath);
	bool Save(const std::string& filePath);

	SceneEntity& CreateEntity(const std::string& name, uint64_t parentId = 0);
	bool RemoveEntity(uint64_t id);
	uint64_t DuplicateEntity(uint64_t id);
	bool SaveEntityBranchAsPrefab(
		uint64_t id,
		const std::string& filePath,
		uint64_t sourceInstanceRootId = 0
	) const;
	bool SaveAsPrefabVariant(
		const std::string& filePath,
		const std::string& basePrefabPath
	) const;
	bool RevertPrefabVariantToBase();
	std::vector<ScenePrefabPropertyOverride> CollectPrefabVariantOverrides()
		const;
	bool ApplyPrefabVariantOverrideToBase(
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool RevertPrefabVariantOverride(
		const ScenePrefabPropertyOverride& overrideValue
	);
	uint64_t InstantiatePrefab(
		const std::string& filePath,
		uint64_t parentId = 0,
		bool runtimeOnly = false
	);
	bool ApplyPrefabInstance(uint64_t rootId);
	bool RevertPrefabInstance(uint64_t rootId);
	bool UnpackPrefabInstance(uint64_t rootId);
	uint64_t FindPrefabInstanceRoot(uint64_t entityId) const;
	std::vector<uint64_t> CollectPrefabInstanceRoots(uint64_t entityId) const;
	std::vector<std::string> CollectPrefabInstanceOverrides(uint64_t rootId) const;
	std::vector<ScenePrefabPropertyOverride> CollectPrefabPropertyOverrides(
		uint64_t rootId
	) const;
	bool ApplyPrefabPropertyOverride(
		uint64_t rootId,
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool RevertPrefabPropertyOverride(
		uint64_t rootId,
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool SetParent(uint64_t id, uint64_t parentId);
	bool MoveEntity(uint64_t id, int direction);
	bool MoveEntityToParent(uint64_t id, uint64_t parentId);
	bool MoveEntityToSibling(uint64_t id, uint64_t siblingId, bool after);
	bool AddComponent(uint64_t id, const std::string& type);
	bool RemoveComponent(uint64_t id, const std::string& type);
	bool IsDescendantOf(uint64_t id, uint64_t potentialAncestorId) const;
	SceneTeamSettings& CreateTeam(const std::string& name);
	bool RenameTeam(const std::string& oldName, const std::string& newName);
	bool RemoveTeam(const std::string& name);
	SceneTeamSettings* FindTeam(const std::string& name);
	const SceneTeamSettings* FindTeam(const std::string& name) const;
	std::string ResolveInheritedFolderTeamName(uint64_t entityId) const;
	const SceneTeamSettings* ResolveEntityTeam(const SceneEntity& entity) const;
	SceneEntity* FindEntity(uint64_t id);
	const SceneEntity* FindEntity(uint64_t id) const;
	SceneEntity* FindEntityByName(const std::string& name);
	const SceneEntity* FindEntityByName(const std::string& name) const;

	const std::string& GetSceneName() const { return sceneName_; }
	void SetSceneName(const std::string& sceneName) {
		sceneName_ = sceneName;
		MarkDirty();
	}
	std::vector<SceneEntity>& GetEntities() { return entities_; }
	const std::vector<SceneEntity>& GetEntities() const { return entities_; }
	std::vector<SceneTeamSettings>& GetTeams() { return teams_; }
	const std::vector<SceneTeamSettings>& GetTeams() const { return teams_; }
	const SceneLightingSettings& GetLightingSettings() const {
		return lightingSettings_;
	}
	void SetLightingSettings(const SceneLightingSettings& settings) {
		lightingSettings_ = settings;
		MarkDirty();
	}
	ScenePostProcessSettings& GetPostProcessSettings() {
		return postProcessSettings_;
	}
	const ScenePostProcessSettings& GetPostProcessSettings() const {
		return postProcessSettings_;
	}
	void SetPostProcessSettings(const ScenePostProcessSettings& settings) {
		postProcessSettings_ = settings;
		MarkDirty();
	}
	const SceneDebugSettings& GetDebugSettings() const {
		return debugSettings_;
	}
	void SetDebugSettings(const SceneDebugSettings& settings) {
		debugSettings_ = settings;
		MarkDirty();
	}
	bool IsDirty() const { return dirty_; }
	const std::string& GetAssetId() const { return assetId_; }
	bool IsPrefabVariant() const { return !variantBaseAssetId_.empty(); }
	const std::string& GetVariantBaseAssetId() const {
		return variantBaseAssetId_;
	}
	const std::string& GetVariantBasePath() const {
		return variantBasePath_;
	}
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	const std::string& GetLastSaveError() const { return lastSaveError_; }
	uint64_t GetRevision() const { return revision_; }
	void MarkDirty() {
		dirty_ = true;
		++revision_;
	}
	void MarkClean() { dirty_ = false; }

private:
	bool LoadInternal(const std::string& filePath);
	void RebuildNextId();
	void ValidateHierarchy();

	std::string sceneName_;
	std::string assetId_;
	std::string variantBaseAssetId_;
	std::string variantBasePath_;
	std::shared_ptr<const SceneDocument> variantBaseSnapshot_;
	std::vector<SceneEntity> entities_;
	std::vector<SceneTeamSettings> teams_;
	SceneLightingSettings lightingSettings_{};
	ScenePostProcessSettings postProcessSettings_{};
	SceneDebugSettings debugSettings_{};
	uint64_t nextId_ = 1;
	bool dirty_ = false;
	uint64_t revision_ = 0;
	std::string lastLoadError_;
	std::string lastSaveError_;
};
