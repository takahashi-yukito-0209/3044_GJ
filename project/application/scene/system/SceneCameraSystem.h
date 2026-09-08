// 役割: Scene内で使用するカメラを選択し、追従・経路・Pause状態を更新する。
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/3d/CameraPathRuntime.h"
#include "../../camera/ThirdPersonCameraController.h"
#include "SceneEventSystem.h"

class Camera;
class Player;
class SceneDocument;
struct SceneComponent;
struct SceneEntity;

// Camera・Player・Objectは所有しない。Player/Physics更新の前後で追従処理を分ける。
class SceneCameraSystem {
public:
	void Reset();
	// 入力とCameraPathを先に評価し、Playerが参照する向きを確定する。
	void UpdateBeforeSimulation(
		SceneDocument& document,
		Camera* camera,
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime,
		bool runtimeActive,
		bool playing,
		bool acceptGameplayInput,
		bool acceptWheelZoom,
		const std::function<bool(uint64_t)>& shouldProcessCameraPath = {}
	);
	// 移動後のPlayer座標へ追従Cameraを合わせ、最終行列を更新する。
	void UpdateAfterSimulation(
		SceneDocument& document,
		Camera* camera,
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime,
		bool runtimeActive,
		bool playing
	);
	void UpdatePaused(Camera* camera, Camera* debugCamera);

	Camera* SelectSceneViewCamera(
		Camera* camera,
		Camera* debugCamera,
		bool paused
	) const;
	bool ApplyComponentToCamera(
		const SceneDocument& document,
		const SceneEntity& cameraEntity,
		const SceneComponent& cameraComponent,
		Camera* camera,
		float aspectRatio
	) const;

	bool IsFirstPersonMode() const {
		return thirdPersonCameraEntityId_ != 0 &&
			playerCameraController_.IsFirstPersonMode();
	}
	bool IsPathPlaying() const { return cameraPathRuntime_.IsPlaying(); }
	bool HasCurrentPathTransform() const {
		return cameraPathRuntime_.HasCurrentTransform();
	}
	const Transform& GetCurrentPathTransform() const {
		return cameraPathRuntime_.GetCurrentTransform();
	}
	uint64_t ConsumeCompletedCameraPathEntityId();
	void ApplyEventRequests(
		SceneDocument& document,
		Camera* camera,
		Player* player,
		const std::vector<SceneCameraRequest>& requests
	);

private:
	void ApplyActiveCamera(
		const SceneDocument& document,
		Camera* camera
	) const;
	void UpdateCameraSwitch(
		const SceneDocument& document,
		bool playing
	);
	const SceneEntity* ResolveActiveCameraEntity(
		const SceneDocument& document
	) const;
	const SceneEntity* ResolveThirdPersonTarget(
		const SceneDocument& document,
		const SceneEntity& cameraEntity,
		const SceneComponent& thirdPerson
	) const;
	bool UpdateThirdPersonCamera(
		SceneDocument& document,
		Camera* camera,
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime,
		bool playing,
		bool acceptMouseInput,
		bool acceptWheelZoom
	);
	void ApplyPlayerDissolve(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		uint64_t targetEntityId,
		bool enabled
	) const;
	bool TryStartCameraPath(
		SceneDocument& document,
		Camera* camera
	);
	bool StartCameraPath(
		SceneDocument& document,
		Camera* camera,
		const SceneEntity& pathEntity,
		const SceneComponent& path
	);
	void HandlePathFinished(
		const SceneDocument& document,
		Camera* camera,
		Player* player
	);
	void SyncPlayerController(
		const SceneDocument& document,
		Camera* camera,
		Player* player
	);
	void InitializePauseDebugCamera(Camera* camera, Camera* debugCamera);

	ThirdPersonCameraController playerCameraController_;
	CameraPathRuntime cameraPathRuntime_;
	bool playerCameraInitialized_ = false;
	bool debugCameraInitialized_ = false;
	bool wasPlaying_ = false;
	uint64_t activeCameraEntityId_ = 0;
	uint64_t thirdPersonCameraEntityId_ = 0;
	uint64_t activeCameraPathEntityId_ = 0;
	uint64_t completedCameraPathEntityId_ = 0;
};
