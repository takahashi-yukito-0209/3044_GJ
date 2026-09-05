// 役割: EventTriggerの条件を評価し、Stat変更・Entity制御・Scene遷移を実行する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "SceneTextMotionSystem.h"

class SceneDocument;
class SceneStatSystem;
class SceneStateMachineSystem;

enum class ScenePostProcessRequestType {
	None,
	SetProfile,
	NextProfile,
	ResetToSceneDefault
};

struct ScenePostProcessRequest {
	ScenePostProcessRequestType type = ScenePostProcessRequestType::None;
	uint64_t managerEntityId = 0;
	std::string managerEntityName;
	std::string profileId;
};

enum class SceneCameraRequestType {
	PlayPath,
	StopPath,
	SelectCamera
};

struct SceneCameraRequest {
	SceneCameraRequestType type = SceneCameraRequestType::PlayPath;
	uint64_t entityId = 0;
	std::string entityName;
};

enum class SceneAudioRequestType {
	Play,
	Stop,
	Pause,
	Resume
};

struct SceneAudioRequest {
	SceneAudioRequestType type = SceneAudioRequestType::Play;
	uint64_t entityId = 0;
};

enum class SceneTextMotionRequestType {
	Play,
	Stop,
	Reset
};

struct SceneTextMotionRequest {
	SceneTextMotionRequestType type = SceneTextMotionRequestType::Play;
	uint64_t entityId = 0;
	std::string clipId;
};

struct SceneFishingFishCountRequest {
	uint64_t directorEntityId = 0;
	int delta = 0;
};

struct SceneEventRuntimeSignals {
	uint64_t completedCameraPathEntityId = 0;
	std::vector<uint64_t> finishedAudioEntityIds;
	std::vector<SceneTextMotionCompletion> textMotionCompletions;
	uint64_t fishingResultInputReadyDirectorEntityId = 0;
};

struct SceneEventResult {
	std::string sceneTransitionId;
	ScenePostProcessRequest postProcessRequest;
	std::vector<SceneCameraRequest> cameraRequests;
	std::vector<SceneAudioRequest> audioRequests;
	std::vector<SceneTextMotionRequest> textMotionRequests;
	std::vector<SceneFishingFishCountRequest> fishingFishCountRequests;
};

class SceneEventSystem {
public:
	SceneEventResult Update(
		SceneDocument& document,
		SceneStatSystem& statSystem,
		SceneStateMachineSystem& stateMachineSystem,
		float deltaTime,
		const SceneEventRuntimeSignals& signals
	);
	void Clear();

private:
	struct BindingRuntime {
		float cooldown = 0.0f;
		bool initialized = false;
		bool wasConditionTrue = false;
		bool fired = false;
	};

	std::unordered_map<uint64_t, std::vector<BindingRuntime>> runtimes_;
};
