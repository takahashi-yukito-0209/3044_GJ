// 役割: SceneのCamera ComponentとRuntimeカメラ制御の状態を同期する。
#include "SceneCameraSystem.h"

#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/collision/Collider.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/math/Matrix4x4.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "../../player/Player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	constexpr float kMinimumFov = 0.0174532925f;
	constexpr float kMaximumFov = 3.12413936f;

	const SceneEntity* ResolveEntityReference(
		const SceneDocument& document,
		uint64_t entityId,
		const std::string& entityName
	) {
		const SceneEntity* byId = entityId != 0
			? document.FindEntity(entityId)
			: nullptr;
		if (byId && (entityName.empty() || byId->name == entityName)) {
			return byId;
		}
		if (!entityName.empty()) {
			if (const SceneEntity* byName = document.FindEntityByName(entityName)) {
				return byName;
			}
		}
		return byId;
	}

	BYTE ResolveSwitchKey(const std::string& keyName) {
		std::string key = keyName;
		std::transform(
			key.begin(),
			key.end(),
			key.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::toupper(character));
			}
		);
		if (key == "F1") return DIK_F1;
		if (key == "F2") return DIK_F2;
		if (key == "F3") return DIK_F3;
		if (key == "F4") return DIK_F4;
		if (key.empty() || key == "F5") return DIK_F5;
		if (key == "F6") return DIK_F6;
		if (key == "F7") return DIK_F7;
		if (key == "F8") return DIK_F8;
		if (key == "F9") return DIK_F9;
		if (key == "F10") return DIK_F10;
		if (key == "F11") return DIK_F11;
		if (key == "F12") return DIK_F12;
		return 0;
	}
}

void SceneCameraSystem::Reset() {
	cameraPathRuntime_.Stop();
	playerCameraController_ = ThirdPersonCameraController{};
	playerCameraInitialized_ = false;
	debugCameraInitialized_ = false;
	wasPlaying_ = false;
	activeCameraEntityId_ = 0;
	thirdPersonCameraEntityId_ = 0;
	activeCameraPathEntityId_ = 0;
	completedCameraPathEntityId_ = 0;
}

uint64_t SceneCameraSystem::ConsumeCompletedCameraPathEntityId() {
	const uint64_t completed = completedCameraPathEntityId_;
	completedCameraPathEntityId_ = 0;
	return completed;
}

void SceneCameraSystem::UpdateBeforeSimulation(
	SceneDocument& document,
	Camera* camera,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	bool runtimeActive,
	bool playing,
	bool acceptWheelZoom
) {
	if (!playing) {
		if (wasPlaying_) {
			cameraPathRuntime_.Stop();
			playerCameraController_ = ThirdPersonCameraController{};
			playerCameraInitialized_ = false;
			activeCameraEntityId_ = 0;
			thirdPersonCameraEntityId_ = 0;
		}
		activeCameraPathEntityId_ = 0;
		completedCameraPathEntityId_ = 0;
		wasPlaying_ = false;
	} else if (!wasPlaying_) {
		playerCameraController_ = ThirdPersonCameraController{};
		playerCameraInitialized_ = false;
		activeCameraEntityId_ = 0;
		thirdPersonCameraEntityId_ = 0;
		wasPlaying_ = true;
	}
	if (!camera || !runtimeActive) {
		return;
	}
	debugCameraInitialized_ = false;

	// CameraPathを優先し、終了したフレームだけPlayer追従へ制御を戻す。
	if (cameraPathRuntime_.IsPlaying()) {
		cameraPathRuntime_.Update(deltaTime, *camera);
		if (cameraPathRuntime_.ConsumeFinishedThisFrame()) {
			HandlePathFinished(document, camera, player);
		}
	} else {
		UpdateCameraSwitch(document, playing);
		ApplyActiveCamera(document, camera);
		UpdateThirdPersonCamera(
			document,
			camera,
			player,
			bindings,
			deltaTime * 0.5f,
			playing,
			true,
			acceptWheelZoom
		);
		TryStartCameraPath(document, camera);
		if (cameraPathRuntime_.IsPlaying()) {
			cameraPathRuntime_.Update(deltaTime, *camera);
			if (cameraPathRuntime_.ConsumeFinishedThisFrame()) {
				HandlePathFinished(document, camera, player);
			}
		}
	}
	camera->Update();
}

void SceneCameraSystem::UpdateAfterSimulation(
	SceneDocument& document,
	Camera* camera,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	bool runtimeActive,
	bool playing
) {
	if (!camera) {
		return;
	}
	if (runtimeActive && !cameraPathRuntime_.IsPlaying()) {
		ApplyActiveCamera(document, camera);
		UpdateThirdPersonCamera(
			document,
			camera,
			player,
			bindings,
			deltaTime * 0.5f,
			playing,
			false,
			false
		);
	}
	camera->Update();
}

void SceneCameraSystem::UpdatePaused(Camera* camera, Camera* debugCamera) {
	InitializePauseDebugCamera(camera, debugCamera);
	if (debugCamera) {
		debugCamera->Update();
	}
}

Camera* SceneCameraSystem::SelectSceneViewCamera(
	Camera* camera,
	Camera* debugCamera,
	bool paused
) const {
	return paused && debugCamera ? debugCamera : camera;
}

bool SceneCameraSystem::ApplyComponentToCamera(
	const SceneDocument& document,
	const SceneEntity& cameraEntity,
	const SceneComponent& cameraComponent,
	Camera* camera,
	float aspectRatio
) const {
	if (!camera || !IsEntityActiveInHierarchy(document, cameraEntity)) {
		return false;
	}
	const Transform transform =
		SceneTransformResolver::ResolveScene3DTransform(document, cameraEntity);
	camera->SetOrbitMode(false);
	camera->SetTranslate(transform.translate);
	camera->SetRotateQuaternion(transform.quaternionRotate);
	camera->SetFovY(std::clamp(
		cameraComponent.cameraFovY,
		kMinimumFov,
		kMaximumFov
	));
	camera->SetAspectRatio((std::max)(aspectRatio, 0.001f));
	camera->SetNearClip((std::max)(cameraComponent.cameraNearClip, 0.001f));
	camera->SetFarClip((std::max)(
		cameraComponent.cameraFarClip,
		cameraComponent.cameraNearClip + 0.001f
	));
	camera->Update();
	return true;
}

const SceneEntity* SceneCameraSystem::ResolveActiveCameraEntity(
	const SceneDocument& document
) const {
	if (activeCameraEntityId_ != 0) {
		if (const SceneEntity* selected = document.FindEntity(activeCameraEntityId_);
			selected &&
			IsEntityActiveInHierarchy(document, *selected) &&
			FindEnabledComponent(*selected, "Camera")) {
			return selected;
		}
	}

	const SceneEntity* fallback = nullptr;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			!IsEntityActiveInHierarchy(document, entity) ||
			!FindEnabledComponent(entity, "Camera")
		) {
			continue;
		}
		if (!fallback) {
			fallback = &entity;
		}
		if (const SceneComponent* component =
			FindEnabledComponent(entity, "Camera");
			component && component->cameraIsMain) {
			return &entity;
		}
	}
	return fallback;
}

void SceneCameraSystem::UpdateCameraSwitch(
	const SceneDocument& document,
	bool playing
) {
	if (!playing) {
		return;
	}
	const SceneComponent* switcher = nullptr;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		switcher = FindEnabledComponent(entity, "CameraSwitcher");
		if (switcher) {
			break;
		}
	}
	if (!switcher) {
		return;
	}
	Input* input = Input::GetInstance();
	const BYTE switchKey = ResolveSwitchKey(switcher->cameraSwitchTriggerKey);
	if (!input || switchKey == 0 || !input->TriggerKey(switchKey)) {
		return;
	}

	std::vector<const SceneEntity*> cameras;
	for (const SceneCameraSwitchEntry& entry : switcher->cameraSwitchEntries) {
		const SceneEntity* entity = ResolveEntityReference(
			document,
			entry.cameraEntityId,
			entry.cameraEntityName
		);
		if (
			!entity ||
			!IsEntityActiveInHierarchy(document, *entity) ||
			!FindEnabledComponent(*entity, "Camera") ||
			std::find(cameras.begin(), cameras.end(), entity) != cameras.end()
		) {
			continue;
		}
		cameras.push_back(entity);
	}
	if (cameras.empty()) {
		return;
	}

	const SceneEntity* current = ResolveActiveCameraEntity(document);
	auto found = std::find(cameras.begin(), cameras.end(), current);
	size_t nextIndex = 0;
	if (found != cameras.end()) {
		nextIndex = static_cast<size_t>(std::distance(cameras.begin(), found)) + 1;
		if (nextIndex >= cameras.size()) {
			if (!switcher->cameraSwitchWrap) {
				return;
			}
			nextIndex = 0;
		}
	}
	activeCameraEntityId_ = cameras[nextIndex]->id;
	playerCameraInitialized_ = false;
	thirdPersonCameraEntityId_ = 0;
}

void SceneCameraSystem::ApplyActiveCamera(
	const SceneDocument& document,
	Camera* camera
) const {
	if (!camera) {
		return;
	}
	const SceneEntity* entity = ResolveActiveCameraEntity(document);
	const SceneComponent* component = entity
		? FindEnabledComponent(*entity, "Camera")
		: nullptr;
	if (!entity || !component) {
		return;
	}

	camera->SetOrbitMode(false);
	const Transform transform =
		SceneTransformResolver::ResolveScene3DTransform(document, *entity);
	camera->SetTranslate(transform.translate);
	camera->SetRotateQuaternion(transform.quaternionRotate);
	camera->SetFovY(std::clamp(
		component->cameraFovY,
		kMinimumFov,
		kMaximumFov
	));
	camera->SetNearClip((std::max)(component->cameraNearClip, 0.001f));
	camera->SetFarClip((std::max)(
		component->cameraFarClip,
		component->cameraNearClip + 0.001f
	));
}

const SceneEntity* SceneCameraSystem::ResolveThirdPersonTarget(
	const SceneDocument& document,
	const SceneEntity& cameraEntity,
	const SceneComponent& thirdPerson
) const {
	if (
		const SceneEntity* configured = ResolveEntityReference(
			document,
			thirdPerson.thirdPersonTargetEntityId,
			thirdPerson.thirdPersonTargetEntityName
		);
		configured && IsEntityActiveInHierarchy(document, *configured)
	) {
		return configured;
	}
	if (
		HasComponent(cameraEntity, "PlayerBehavior") ||
		HasComponent(cameraEntity, "AgentBehavior") ||
		HasComponent(cameraEntity, "MeshRenderer")
	) {
		return &cameraEntity;
	}
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			IsEntityActiveInHierarchy(document, entity) &&
			HasComponent(entity, "PlayerBehavior")
		) {
			return &entity;
		}
	}
	return nullptr;
}

bool SceneCameraSystem::UpdateThirdPersonCamera(
	SceneDocument& document,
	Camera* camera,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	bool playing,
	bool acceptMouseInput,
	bool acceptWheelZoom
) {
	(void)player;
	if (!playing || !camera) {
		playerCameraInitialized_ = false;
		thirdPersonCameraEntityId_ = 0;
		ApplyPlayerDissolve(bindings, false);
		return false;
	}

	const SceneEntity* cameraEntity = ResolveActiveCameraEntity(document);
	const SceneComponent* cameraComponent = cameraEntity
		? FindEnabledComponent(*cameraEntity, "Camera")
		: nullptr;
	const SceneComponent* thirdPerson = cameraEntity
		? FindEnabledComponent(*cameraEntity, "ThirdPersonCamera")
		: nullptr;
	if (
		!cameraEntity ||
		!cameraComponent ||
		!thirdPerson
	) {
		playerCameraInitialized_ = false;
		thirdPersonCameraEntityId_ = 0;
		ApplyPlayerDissolve(bindings, false);
		return false;
	}
	const SceneEntity* targetEntity = ResolveThirdPersonTarget(
		document,
		*cameraEntity,
		*thirdPerson
	);
	if (!targetEntity) {
		playerCameraInitialized_ = false;
		thirdPersonCameraEntityId_ = 0;
		ApplyPlayerDissolve(bindings, false);
		return false;
	}

	if (
		!playerCameraInitialized_ ||
		thirdPersonCameraEntityId_ != cameraEntity->id
	) {
		playerCameraController_.Initialize(camera);
		const Transform initialTransform =
			SceneTransformResolver::ResolveScene3DTransform(
				document,
				*cameraEntity
			);
		playerCameraController_.SetYawPitch(
			initialTransform.rotate.y,
			initialTransform.rotate.x
		);
		playerCameraController_.SetDistance(thirdPerson->thirdPersonDistance);
		playerCameraController_.SetAimDistance(
			thirdPerson->thirdPersonAimDistance
		);
		playerCameraInitialized_ = true;
		thirdPersonCameraEntityId_ = cameraEntity->id;
	}

	Input* input = Input::GetInstance();
	const bool gameplayMouseActive =
		!input || input->IsCursorCaptured();
	if (!gameplayMouseActive && acceptMouseInput) {
		return true;
	}

	camera->SetOrbitMode(false);
	camera->SetFovY(std::clamp(
		cameraComponent->cameraFovY,
		kMinimumFov,
		kMaximumFov
	));
	camera->SetNearClip((std::max)(cameraComponent->cameraNearClip, 0.001f));
	camera->SetFarClip((std::max)(
		cameraComponent->cameraFarClip,
		cameraComponent->cameraNearClip + 0.001f
	));
	playerCameraController_.SetTargetOffset(
		thirdPerson->thirdPersonTargetOffset
	);
	playerCameraController_.SetAimTargetOffset(
		thirdPerson->thirdPersonAimTargetOffset
	);
	playerCameraController_.SetMouseSensitivity(
		thirdPerson->thirdPersonMouseSensitivity
	);
	playerCameraController_.SetPitchLimit(
		thirdPerson->thirdPersonMinPitch,
		thirdPerson->thirdPersonMaxPitch
	);
	playerCameraController_.SetOcclusionMargin(
		thirdPerson->thirdPersonOcclusionMargin
	);
	playerCameraController_.SetOcclusionSmoothTimes(
		thirdPerson->thirdPersonOcclusionPullInSmoothTime,
		thirdPerson->thirdPersonOcclusionRecoverySmoothTime
	);
	playerCameraController_.SetPositionSmoothTime(
		thirdPerson->thirdPersonPositionSmoothTime
	);
	playerCameraController_.SetRotationSmoothTime(
		thirdPerson->thirdPersonRotationSmoothTime
	);
	playerCameraController_.SetFollowTargetYaw(
		thirdPerson->thirdPersonYawReference == "Target"
	);
	playerCameraController_.SetOcclusionEnabled(
		thirdPerson->thirdPersonOcclusionEnabled
	);
	playerCameraController_.SetAimModeEnabled(
		thirdPerson->thirdPersonAimModeEnabled &&
			thirdPerson->thirdPersonAllowMouseInput
	);
	playerCameraController_.SetMouseInvert(
		thirdPerson->thirdPersonInvertYaw,
		thirdPerson->thirdPersonInvertPitch
	);

	std::vector<OBBCollider*> obstacles;
	obstacles.reserve(bindings.size());
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		// 以前はTarget自身だけを除外していたため、子のHurtBoxや武器HitBoxが
		// Camera Rayを遮っていた。TriggerとTarget階層は遮蔽物に含めない。
		if (
			!binding.entity ||
			!binding.collider ||
			!binding.collider->IsActive() ||
			binding.collider->IsTrigger() ||
			binding.entity->id == targetEntity->id ||
			document.IsDescendantOf(
				binding.entity->id,
				targetEntity->id
			) ||
			(
				binding.collider->GetCollisionAttribute() &
				thirdPerson->thirdPersonOcclusionMask
			) == 0 ||
			!IsEntityActiveInHierarchy(document, *binding.entity)
		) {
			continue;
		}
		std::string modelPath = binding.entity->modelPath;
		if (const SceneComponent* meshRenderer =
			FindEnabledComponent(*binding.entity, "MeshRenderer")) {
			modelPath = meshRenderer->modelPath;
		}
		std::transform(
			modelPath.begin(),
			modelPath.end(),
			modelPath.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			}
		);
		if (
			modelPath.find("terrain") == std::string::npos &&
			binding.collider->GetType() == Collider::Type::OBB
		) {
			obstacles.push_back(static_cast<OBBCollider*>(binding.collider));
		}
	}

	const Transform targetTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *targetEntity);
	playerCameraController_.Update(
		targetTransform.translate,
		targetTransform.rotate.y,
		obstacles,
		deltaTime,
		acceptMouseInput &&
			thirdPerson->thirdPersonAllowMouseInput &&
			gameplayMouseActive,
		acceptWheelZoom
	);
	ApplyPlayerDissolve(
		bindings,
		HasComponent(*targetEntity, "PlayerBehavior")
	);
	return true;
}

void SceneCameraSystem::ApplyPlayerDissolve(
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	bool enabled
) const {
	constexpr float kStartPitch = -0.35f;
	constexpr float kFullPitch = -0.85f;
	const float rawAmount = std::clamp(
		(kStartPitch - playerCameraController_.GetPitch()) /
			(kStartPitch - kFullPitch),
		0.0f,
		1.0f
	);
	const float amount = enabled
		? rawAmount * rawAmount * (3.0f - 2.0f * rawAmount)
		: 0.0f;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			binding.entity &&
			binding.object &&
			HasComponent(*binding.entity, "PlayerBehavior")
		) {
			binding.object->SetDissolve(amount, 0.08f, 6.0f);
		}
	}
}

bool SceneCameraSystem::TryStartCameraPath(
	SceneDocument& document,
	Camera* camera
) {
	if (!camera || cameraPathRuntime_.IsPlaying()) {
		return false;
	}
	Input* input = Input::GetInstance();
	if (!input) {
		return false;
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* path = FindEnabledComponent(entity, "CameraPath");
		if (!path || path->cameraPathTriggerType != "Key") {
			continue;
		}
		std::string triggerKey = path->cameraPathTriggerKey;
		std::transform(
			triggerKey.begin(),
			triggerKey.end(),
			triggerKey.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::toupper(character));
			}
		);
		const bool triggered =
			(triggerKey.empty() || triggerKey == "C") &&
			input->TriggerKey(DIK_C);
		if (!triggered) {
			continue;
		}

		if (StartCameraPath(document, camera, entity, *path)) {
			return true;
		}
	}
	return false;
}

bool SceneCameraSystem::StartCameraPath(
	SceneDocument& document,
	Camera* camera,
	const SceneEntity& pathEntity,
	const SceneComponent& path
) {
	if (!camera || cameraPathRuntime_.IsPlaying()) {
		return false;
	}
	const SceneEntity* targetEntity = nullptr;
	const SceneComponent* targetCamera = nullptr;
	if (!path.cameraPathTargetCameraName.empty()) {
		targetEntity = document.FindEntityByName(path.cameraPathTargetCameraName);
		targetCamera = targetEntity
			? FindEnabledComponent(*targetEntity, "Camera")
			: nullptr;
	} else {
		targetEntity = ResolveActiveCameraEntity(document);
		targetCamera = targetEntity
			? FindEnabledComponent(*targetEntity, "Camera")
			: nullptr;
	}
	if (!targetEntity || !targetCamera) {
		return false;
	}

	camera->SetFovY(std::clamp(
		targetCamera->cameraFovY,
		kMinimumFov,
		kMaximumFov
	));
	camera->SetNearClip((std::max)(targetCamera->cameraNearClip, 0.001f));
	camera->SetFarClip((std::max)(
		targetCamera->cameraFarClip,
		targetCamera->cameraNearClip + 0.001f
	));
	camera->Update();
	cameraPathRuntime_.Play(document, pathEntity, path, *camera);
	if (cameraPathRuntime_.IsPlaying()) {
		activeCameraPathEntityId_ = pathEntity.id;
		return true;
	}
	return false;
}

void SceneCameraSystem::HandlePathFinished(
	const SceneDocument& document,
	Camera* camera,
	Player* player
) {
	completedCameraPathEntityId_ = activeCameraPathEntityId_;
	activeCameraPathEntityId_ = 0;
	SyncPlayerController(document, camera, player);
}

void SceneCameraSystem::ApplyEventRequests(
	SceneDocument& document,
	Camera* camera,
	Player* player,
	const std::vector<SceneCameraRequest>& requests
) {
	for (const SceneCameraRequest& request : requests) {
		const SceneEntity* target = ResolveEntityReference(
			document, request.entityId, request.entityName
		);
		if (!target || !IsEntityActiveInHierarchy(document, *target)) {
			continue;
		}
		if (request.type == SceneCameraRequestType::PlayPath) {
			const SceneComponent* path =
				FindEnabledComponent(*target, "CameraPath");
			if (path) {
				StartCameraPath(document, camera, *target, *path);
			}
		} else if (request.type == SceneCameraRequestType::StopPath) {
			if (
				target->id == activeCameraPathEntityId_ &&
				FindEnabledComponent(*target, "CameraPath")
			) {
				cameraPathRuntime_.Stop();
				activeCameraPathEntityId_ = 0;
				ApplyActiveCamera(document, camera);
				SyncPlayerController(document, camera, player);
			}
		} else if (request.type == SceneCameraRequestType::SelectCamera) {
			if (!FindEnabledComponent(*target, "Camera")) {
				continue;
			}
			activeCameraEntityId_ = target->id;
			playerCameraInitialized_ = false;
			thirdPersonCameraEntityId_ = 0;
			if (!cameraPathRuntime_.IsPlaying()) {
				ApplyActiveCamera(document, camera);
				SyncPlayerController(document, camera, player);
			}
		}
	}
}

void SceneCameraSystem::SyncPlayerController(
	const SceneDocument& document,
	Camera* camera,
	Player* player
) {
	(void)player;
	if (!camera) {
		return;
	}
	const SceneEntity* cameraEntity = ResolveActiveCameraEntity(document);
	const SceneComponent* thirdPerson = cameraEntity
		? FindEnabledComponent(*cameraEntity, "ThirdPersonCamera")
		: nullptr;
	const SceneEntity* targetEntity = cameraEntity && thirdPerson
		? ResolveThirdPersonTarget(document, *cameraEntity, *thirdPerson)
		: nullptr;
	if (!cameraEntity || !thirdPerson || !targetEntity) {
		return;
	}
	const Transform targetTransform =
		SceneTransformResolver::ResolveScene3DTransform(document, *targetEntity);
	const Vector3 focus = Math::Add(
		targetTransform.translate,
		playerCameraController_.IsAimMode()
			? thirdPerson->thirdPersonAimTargetOffset
			: thirdPerson->thirdPersonTargetOffset
	);
	playerCameraController_.SyncFromCameraPose(camera->GetTranslate(), focus);
	playerCameraInitialized_ = true;
	thirdPersonCameraEntityId_ = cameraEntity->id;
}

void SceneCameraSystem::InitializePauseDebugCamera(
	Camera* camera,
	Camera* debugCamera
) {
	if (!camera || !debugCamera || debugCameraInitialized_) {
		return;
	}
	debugCamera->SetFovY(camera->GetFovY());
	debugCamera->SetAspectRatio(camera->GetAspectRatio());
	debugCamera->SetNearClip(camera->GetNearClip());
	debugCamera->SetFarClip(camera->GetFarClip());

	const Matrix4x4& cameraWorld = camera->GetWorldMatrix();
	Vector3 forward = {
		cameraWorld.m[2][0],
		cameraWorld.m[2][1],
		cameraWorld.m[2][2]
	};
	const float length = Math::Length(forward);
	forward = length > 0.000001f
		? Math::Multiply(forward, 1.0f / length)
		: Vector3{ 0.0f, 0.0f, 1.0f };

	constexpr float kOrbitDistance = 10.0f;
	debugCamera->SetOrbitMode(true);
	debugCamera->SetOrbitDistance(kOrbitDistance);
	debugCamera->SetOrbitTarget(Math::Add(
		camera->GetTranslate(),
		Math::Multiply(forward, kOrbitDistance)
	));
	debugCamera->SetOrbitAngle(
		std::atan2(-forward.x, forward.z),
		std::asin(std::clamp(-forward.y, -1.0f, 1.0f))
	);
	debugCamera->UpdatePreviewMatrices();
	debugCameraInitialized_ = true;
}
