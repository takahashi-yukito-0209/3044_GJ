// 役割: SceneDocumentと各Runtime Systemを連携し、更新と描画を実行する。
#include "RuntimeScene.h"

#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneExecutionContext.h"
#include "../../engine/scene/SceneDocument.h"
#include "../../engine/scene/SceneTransformResolver.h"
#include "../../engine/3d/SrvManager.h"
#include "../../engine/base/DirectXCommon.h"

#include "../../engine/3d/Camera.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/3d/Object3d.h"
#include "../../engine/math/Math.h"
#include "../../engine/particle/ParticleManager.h"
#include "../player/Player.h"

namespace {
	Transform MakeRuntimeTransform(const QuaternionTransform& source) {
		Transform result{};
		result.scale = source.scale;
		result.rotate = MakeEulerFromQuaternion(source.rotate);
		result.translate = source.translate;
		result.useQuaternionRotation = true;
		result.quaternionRotate = source.rotate;
		return result;
	}

	void SynchronizeSceneTransform(
		const SceneDocument& document,
		SceneEntity& entity,
		Object3d& object,
		const Transform& source
	) {
		Transform localTransform{};
		if (!SceneTransformResolver::TryConvertSceneWorldTransformToLocal(
			document,
			entity,
			source,
			localTransform
		)) {
			return;
		}
		entity.transform.scale = localTransform.scale;
		entity.transform.rotate = localTransform.quaternionRotate;
		entity.transform.translate = localTransform.translate;
		object.GetTransform() = localTransform;
		object.Update();
	}

	Transform GetSceneTransform(
		const SceneDocument* document,
		const char* name,
		const Transform& fallback
	) {
		const SceneEntity* entity = document
			? document->FindEntityByName(name)
			: nullptr;
		return entity ? MakeRuntimeTransform(entity->transform) : fallback;
	}

	Camera* CreateOrbitCamera() {
		Camera* camera = new Camera();
		camera->SetOrbitMode(true);
		camera->SetOrbitTarget({ 0.0f, 0.0f, 0.0f });
		camera->SetOrbitDistance(10.0f);
		camera->SetOrbitAngle(0.0f, 0.0f);
		camera->Update();
		return camera;
	}
}

void RuntimeScene::ApplyRenderCamera(Camera* viewCamera) {
	objectSystem_.ApplyRenderCamera(viewCamera);
	environmentSystem_.ApplyRenderCamera(viewCamera);
	ParticleManager::GetInstance()->SetCamera(viewCamera);
}

Camera* RuntimeScene::GetSceneViewCamera() const {
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	return cameraSystem_.SelectSceneViewCamera(
		camera_,
		debugCamera_,
		executionContext && executionContext->IsPaused()
	);
}

bool RuntimeScene::TryGetRuntimePostProcessSettings(
	ScenePostProcessSettings& settings,
	uint64_t& generation
) const {
	settings = postProcessProfileSystem_.GetEffectiveSettings();
	generation = postProcessProfileSystem_.GetGeneration();
	return true;
}

void RuntimeScene::DrawSceneView(Camera* viewCamera, uint64_t skipEntityId) {
	DrawEnvironment(viewCamera);
	PrepareSceneContent(viewCamera);
	BindLighting();
	DrawPreparedSceneContentForView(viewCamera, skipEntityId);
}

void RuntimeScene::DrawPreparedSceneContentForView(
	Camera* viewCamera,
	uint64_t skipEntityId
) {
	SceneDocument* document = GetSceneDocument();
	if (document) {
		const bool hidePlayerModel =
			ShouldHidePlayerModelForCamera(viewCamera);
		objectSystem_.DrawModels(
			*document,
			skipEntityId,
			hidePlayerModel
		);
	}
	effectRenderSystem_.DrawScenePass(
		document,
		viewCamera,
		skipEntityId,
		environmentSystem_,
		objectSystem_
	);
	if (document) {
		DirectXCommon* dxCommon = Object3dCommon::GetInstance()->GetDxCommon();
		textRenderSystem_.DrawScene2D(
			*document,
			dxCommon->GetClientWidth(),
			dxCommon->GetClientHeight()
		);
	}
}

bool RuntimeScene::ShouldHidePlayerModelForCamera(Camera* viewCamera) const {
	return
		viewCamera == camera_ &&
		cameraSystem_.IsFirstPersonMode();
}

void RuntimeScene::Initialize()
{
	// PlayerはObjectSystemのObjectを借用するため、Objectを最初に構築する。
	cameraSystem_.Reset();

	camera_ = CreateOrbitCamera();
	debugCamera_ = CreateOrbitCamera();

	Object3dCommon::GetInstance()->SetDefaultCamera(camera_);
	particleSystem_.Initialize(camera_);
	SceneDocument* initialDocument = GetSceneDocument();
	postProcessProfileSystem_.Reset(initialDocument);
	debugSystem_.LoadSettings(initialDocument);
	SceneExecutionContext* initialExecutionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool initialEditing =
		initialExecutionContext && initialExecutionContext->IsEditing();
	const bool initialPlaying =
		!initialExecutionContext || initialExecutionContext->IsPlaying();
	objectSystem_.SyncModels(
		initialDocument,
		physicsSystem_,
		0.0f,
		initialPlaying,
		initialEditing
	);

	Vector3 target = GetSceneTransform(
		initialDocument,
		"Human",
		Transform{
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
			{ -2.0f, 0.0f, -2.0f }
		}
	).translate;
	camera_->SetOrbitTarget(target);

	player_ = new Player();
	player_->Initialize(
		objectSystem_.FindObjectByName(initialDocument, "Player")
	);
	player_->SetTransform(GetSceneTransform(
		initialDocument,
		"Player",
		Transform{
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, -4.0f }
		}
	));

	environmentSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon()
	);
	if (initialDocument) {
		objectSystem_.BuildBindings(
			*initialDocument,
			runtimeObjectBindings_
		);
		environmentSystem_.Sync(
			initialDocument,
			runtimeObjectBindings_
		);
	}

	lightingSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		SrvManager::GetInstance()
	);
	lightingSystem_.Sync(initialDocument);
	monitorSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		SrvManager::GetInstance()
	);
	miniMapSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		SrvManager::GetInstance()
	);

	effectRenderSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon()
	);
	textRenderSystem_.Initialize(
		Object3dCommon::GetInstance()->GetDxCommon(),
		GetSceneAssetId() + "_" + std::to_string(GetSceneInstanceId())
	);

}

void RuntimeScene::Update(float deltaTime)
{
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool editing = executionContext && executionContext->IsEditing();
	const bool playing = !executionContext || executionContext->IsPlaying();
	SceneDocument* activeDocument = GetSceneDocument();
	if (activeDocument) {
		postProcessProfileSystem_.Sync(*activeDocument);
		// 2Dの先読みはTransform確定を待たないため、Eventより前に完了させる。
		audioSystem_.Sync(
			*activeDocument,
			playing,
			GetSceneInstanceId(),
			sceneManager_ && sceneManager_->GetActiveSceneInstanceId() == GetSceneInstanceId()
		);
	} else {
		postProcessProfileSystem_.Reset();
	}
	std::vector<uint64_t> spawnerResetEntityIds;
	const float gameplayDeltaTime = playing
		? hitStopSystem_.Advance(deltaTime)
		: deltaTime;

	// 遷移が成立したフレームは旧Sceneの状態をこれ以上変更しない。
	if (playing && gameplayDeltaTime > 0.0f && activeDocument) {
		const std::string targetSceneId =
			transitionSystem_.Update(*activeDocument);
		if (!targetSceneId.empty()) {
			sceneManager_->RequestSceneTransition(targetSceneId);
			return;
		}
	}
	SceneGameFlowResult gameFlowResult{};
	if (activeDocument && playing) {
		gameFlowResult = gameFlowSystem_.Update(
			*activeDocument,
			enemySpawnerSystem_,
			deltaTime
		);
		for (const SceneGameFlowEntityRequest& request : gameFlowResult.entityRequests) {
			if (SceneEntity* entity = activeDocument->FindEntity(request.entityId)) {
				entity->active = request.active;
			}
		}
		for (const SceneGameFlowWaveRequest& request : gameFlowResult.waveRequests) {
			enemySpawnerSystem_.BeginFiniteWave(
				request.spawnerEntityId,
				request.generation,
				request.count
			);
		}
		for (const SceneGameFlowMotionRequest& request : gameFlowResult.motionRequests) {
			textMotionSystem_.Play(*activeDocument, request.entityId, request.clipId);
		}
	} else {
		gameFlowSystem_.Clear();
	}
	if (activeDocument && playing) {
		// Fish選択はObject同期前に確定し、同FrameのCollider生成へ反映する。
		fishingScoreAttackSystem_.UpdateBeforeSimulation(
			*activeDocument,
			deltaTime,
			true
		);
	} else {
		fishingScoreAttackSystem_.Clear();
	}
	const std::string runtimeSceneId = GetSceneAssetId().empty()
		? "runtime"
		: GetSceneAssetId();
	if (editing && player_) {
		const SceneEntity* playerEntity = activeDocument
			? activeDocument->FindEntityByName("Player")
			: nullptr;
		if (playerEntity) {
			player_->SetTransform(MakeRuntimeTransform(playerEntity->transform));
		}
	}

	particleSystem_.Update(
		runtimeSceneId,
		editing
	);
	effectRenderSystem_.Update(deltaTime);
	environmentSystem_.Update(deltaTime);

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editing) {
		// Editor操作を先に受け取り、変更されたDocumentを直後の同期へ反映する。
		debugSystem_.DrawEditor(
			activeDocument,
			objectSystem_,
			false
		);

		monitorSystem_.DrawEditor(
			activeDocument,
			GetSceneViewCamera()
		);

		if (activeDocument) {
			environmentSystem_.DrawEditor(*activeDocument);
		}

		particleSystem_.DrawEditor(runtimeSceneId);
	}
#endif
	lightingSystem_.Sync(activeDocument);
	if (activeDocument && playing) {
		// 前フレームで寿命切れ/HitしたRuntime Entityをbinding再構築前に破棄する。
		combatSystem_.FlushRemovals(*activeDocument);
		projectileSystem_.FlushRemovals(*activeDocument);
		// 保存値を実行時状態へ展開し、Transform AnimationをObject同期前に反映する。
		statSystem_.Update(*activeDocument);
		if (gameFlowResult.gameplayAllowed) {
			enemySpawnerSystem_.Update(*activeDocument, gameplayDeltaTime);
		}
		spawnerResetEntityIds = enemySpawnerSystem_.ConsumeResetEntityIds();
		for (uint64_t entityId : spawnerResetEntityIds) {
			attackRunnerSystem_.ResetEntity(*activeDocument, entityId);
			stateMachineSystem_.ResetEntity(entityId);
			prefabAnimationSystem_.ResetEntity(entityId);
			enemySystem_.ResetEntity(entityId);
			hitReactionSystem_.ResetEntity(entityId);
		}
		if (gameFlowResult.gameplayAllowed) {
			hitReactionSystem_.AdvanceRecoveries(statSystem_, gameplayDeltaTime);
			attackRunnerSystem_.Advance(
				*activeDocument,
				prefabAnimationSystem_,
				gameplayDeltaTime
			);
			runtimeEffectSystem_.Spawn(
				*activeDocument,
				physicsSystem_,
				attackRunnerSystem_.ConsumeEffectRequests()
			);
			effectRenderSystem_.SpawnGroundCracks(
				runtimeEffectSystem_.ConsumeGroundCrackRequests()
			);
			runtimeEffectSystem_.Advance(*activeDocument, deltaTime);
			prefabAnimationSystem_.Update(*activeDocument, gameplayDeltaTime);
		}
	} else {
		audioSystem_.Clear();
		statSystem_.Clear();
		attackRunnerSystem_.Clear(activeDocument);
		runtimeEffectSystem_.Clear(activeDocument);
		prefabAnimationSystem_.Clear();
		eventSystem_.Clear();
		textMotionSystem_.Clear();
		gameFlowSystem_.Clear();
		postProcessProfileSystem_.Reset(activeDocument);
		stateMachineSystem_.Clear();
		combatSystem_.Clear();
		hitReactionSystem_.Clear();
		hitStopSystem_.Clear();
		enemySystem_.Clear();
		enemySpawnerSystem_.Clear();
		projectileSystem_.Clear();
		attachmentSystem_.Clear(&objectSystem_);
	}

	// Objectが実体を所有し、以降のSystemは再構築したbindingsだけを借用する。
	objectSystem_.SyncModels(
		activeDocument,
		physicsSystem_,
		gameplayDeltaTime,
		playing,
		editing
	);
	if (activeDocument) {
		objectSystem_.BuildBindings(
			*activeDocument,
			runtimeObjectBindings_
		);
		fishingScoreAttackSystem_.ApplyHookVisualOverrides(
			*activeDocument,
			runtimeObjectBindings_
		);
		physicsSystem_.SyncSceneSettings(
			*activeDocument,
			player_,
			runtimeObjectBindings_,
			editing
		);
		physicsSystem_.ResetBodies(
			runtimeObjectBindings_,
			spawnerResetEntityIds
		);
		if (playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
			enemySystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				statSystem_,
				prefabAnimationSystem_,
				hitReactionSystem_,
				stateMachineSystem_,
				gameplayDeltaTime
			);
			// GroundXZ Agentは敵AIが決めた速度へ離隔補正だけを加える。
			agentSystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				gameplayDeltaTime
			);
			enemySystem_.ApplyMovementStops(runtimeObjectBindings_);
			projectileSystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				gameplayDeltaTime
			);
		}
	} else {
		audioSystem_.Clear();
		runtimeObjectBindings_.clear();
		agentSystem_.Clear();
		attachmentSystem_.Clear(&objectSystem_);
		combatSystem_.Clear();
		hitReactionSystem_.Clear();
		hitStopSystem_.Clear();
		enemySystem_.Clear();
		eventSystem_.Clear();
		gameFlowSystem_.Clear();
		postProcessProfileSystem_.Reset();
		stateMachineSystem_.Clear();
		attackRunnerSystem_.Clear();
		prefabAnimationSystem_.Clear();
		projectileSystem_.Clear();
		statSystem_.Clear();
		physicsSystem_.Clear();
		cameraSystem_.Reset();
	}

	// Camera入力、Player移動、Physics、追従Cameraの順序は相互依存を持つ。
	if (activeDocument) {
		cameraSystem_.UpdateBeforeSimulation(
			*activeDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			deltaTime,
			playing,
			playing,
			fishingScoreAttackSystem_.AcceptWheelZoom()
		);
	}
	Vector3 playerAttackInputDirection{};
	if (player_ && playing) {
		player_->Update(
			camera_,
			gameFlowResult.gameplayAllowed &&
				fishingScoreAttackSystem_.IsPlayerMovementAllowed(),
			gameplayDeltaTime
		);
		const Vector3& playerVelocity = player_->GetPhysicsBody().velocity;
		playerAttackInputDirection = { playerVelocity.x, 0.0f, playerVelocity.z };
		if (Math::Length(playerAttackInputDirection) > 0.0001f) {
			playerAttackInputDirection = Math::Normalize(playerAttackInputDirection);
		}
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		// State行動は入力取得後、Physics確定前に速度・攻撃判定を更新する。
		stateMachineSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			player_,
			attackRunnerSystem_,
			prefabAnimationSystem_,
			gameplayDeltaTime
		);
		attackRunnerSystem_.ApplyMotion(
			*activeDocument,
			runtimeObjectBindings_,
			player_,
			playerAttackInputDirection,
			gameplayDeltaTime
		);
		// Combat後に予約した被弾速度を、AI/Agent/Stateの書込み後に上書きする。
		hitReactionSystem_.ApplyMotionOverrides(
			*activeDocument,
			runtimeObjectBindings_,
			gameplayDeltaTime
		);
	}
	if (activeDocument && (!playing || gameplayDeltaTime > 0.0f)) {
		physicsSystem_.Step(
			player_,
			runtimeObjectBindings_,
			gameplayDeltaTime,
			playing
		);
	}
	if (player_ && playing && gameplayDeltaTime > 0.0f) {
		player_->PostPhysicsUpdate();
		SceneEntity* playerEntity = activeDocument
			? activeDocument->FindEntityByName("Player")
			: nullptr;
		SceneFishingScoreAttackPlayerWaterBounds waterBounds{};
		if (playerEntity &&
			fishingScoreAttackSystem_.TryGetPlayerWaterBounds(waterBounds) &&
			waterBounds.playerEntityId == playerEntity->id) {
			player_->ClampToWaterBounds(
				waterBounds.center,
				waterBounds.yaw,
				waterBounds.halfSizeX,
				waterBounds.halfSizeZ
			);
		}
		if (playerEntity && player_->GetObject()) {
			SynchronizeSceneTransform(
				*activeDocument,
				*playerEntity,
				*player_->GetObject(),
				player_->GetObject()->GetTransform()
			);
		}
	}
	if (activeDocument && playing) {
		// Player Physics後のCollider world transformで釣り針Triggerを判定する。
		fishingScoreAttackSystem_.UpdateAfterSimulation(
			*activeDocument,
			runtimeObjectBindings_,
			agentSystem_,
			true,
			gameplayDeltaTime,
			player_ ? player_->GetPhysicsBody().velocity : Vector3{}
		);
		SceneFishingScoreAttackPlayerConstraintRequest constraintRequest{};
		if (
			player_ &&
			fishingScoreAttackSystem_.ConsumePlayerConstraintRequest(constraintRequest)
		) {
			SceneEntity* playerEntity = activeDocument->FindEntity(
				constraintRequest.playerEntityId
			);
			if (
				playerEntity &&
				player_->ApplyPlanarMotionConstraint(
					constraintRequest.planarPosition,
					constraintRequest.yaw,
					constraintRequest.planarVelocity
				)
			) {
				SynchronizeSceneTransform(
					*activeDocument,
					*playerEntity,
					*player_->GetObject(),
					player_->GetObject()->GetTransform()
				);
			}
		}
		SceneFishingScoreAttackPlayerResetRequest resetRequest{};
		if (player_ &&
			fishingScoreAttackSystem_.ConsumePlayerResetRequest(resetRequest)) {
			SceneEntity* playerEntity = activeDocument->FindEntity(
				resetRequest.playerEntityId
			);
			if (playerEntity && player_->GetObject()) {
				player_->SetTransform(resetRequest.transform);
				SynchronizeSceneTransform(
					*activeDocument,
					*playerEntity,
					*player_->GetObject(),
					player_->GetObject()->GetTransform()
				);
			}
			for (const SceneFishingScoreAttackPlayerResetRequest::EntityReset& entityReset :
				resetRequest.entityResets) {
				for (const SceneRuntimeObjectBinding& binding : runtimeObjectBindings_) {
					if (
						!binding.entity ||
						binding.entity->id != entityReset.entityId ||
						!binding.object
					) {
						continue;
					}
					SynchronizeSceneTransform(
						*activeDocument,
						*binding.entity,
						*binding.object,
						entityReset.transform
					);
					break;
				}
			}
			agentSystem_.ResetTeam(
				*activeDocument,
				resetRequest.teamName
			);
		}
	}
	if (activeDocument && playing && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
		// Bone追従はAnimation/Physics後、当たり判定とEventは最終Transform後に評価する。
		attachmentSystem_.Update(
			*activeDocument,
			objectSystem_,
			runtimeObjectBindings_
		);
		combatSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			statSystem_,
			gameplayDeltaTime
		);
		std::vector<SceneCombatHitEvent> hitEvents = combatSystem_.ConsumeHitEvents();
		for (const SceneCombatHitEvent& hitEvent : hitEvents) {
			hitStopSystem_.Request(hitEvent.hitStopDuration);
		}
		runtimeEffectSystem_.SpawnHitEffects(hitEvents);
		hitReactionSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			statSystem_,
			stateMachineSystem_,
			hitEvents,
			gameplayDeltaTime
		);
		runtimeEffectSystem_.SpawnDeathEffects(
			*activeDocument,
			hitReactionSystem_.ConsumeDeathEffectRequests()
		);
	}
	objectSystem_.ClearSpriteOverrides();
	if (activeDocument) {
		for (const SceneFishingScoreAttackIconRequest& request :
			fishingScoreAttackSystem_.GetIconRequests()) {
			objectSystem_.SetSpriteRuntimeOverride(SceneSpriteRuntimeOverride{
				request.entityId,
				request.texturePath,
				request.size,
				{ 1.0f, 1.0f, 1.0f, 1.0f },
				request.visible
			});
		}
	}
	objectSystem_.SyncSprites(activeDocument);
	if (activeDocument) {
		cameraSystem_.UpdateAfterSimulation(
			*activeDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			deltaTime,
			playing,
			playing
		);
	} else if (camera_) {
		camera_->Update();
	}
	// Transform確定後に環境設定とDebug形状を登録し、描画時の状態を揃える。
	environmentSystem_.Sync(activeDocument, runtimeObjectBindings_);
	if (activeDocument) {
		fishingScoreAttackSystem_.AddFormationOutlineDebugDraw(
			*activeDocument,
			agentSystem_
		);
	}
	if (activeDocument && playing) {
		// Eventは同FrameのTextMotion completionを次Packageで受け取れる位置に置く。
		textMotionSystem_.Update(*activeDocument, deltaTime);
	} else {
		textMotionSystem_.Clear();
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	debugSystem_.AddDebugDraw(
		activeDocument,
		objectSystem_,
		cameraSystem_,
		camera_,
		playing,
		playing,
		false
	);
#endif
	if (activeDocument && playing && gameplayDeltaTime > 0.0f) {
		// Prefab生成はEntity配列を再配置し得るため、bindingを使い終えた最後に行う。
		const SceneEventRuntimeSignals eventSignals{
			cameraSystem_.ConsumeCompletedCameraPathEntityId(),
			audioSystem_.ConsumeFinishedEntityIds(*activeDocument),
			textMotionSystem_.ConsumeCompletions(),
			fishingScoreAttackSystem_.GetResultInputReadyDirectorEntityId()
		};
		const SceneEventResult eventResult = eventSystem_.Update(
			*activeDocument,
			statSystem_,
			stateMachineSystem_,
			gameplayDeltaTime,
			eventSignals
		);
		if (!eventResult.sceneTransitionId.empty()) {
			postProcessProfileSystem_.Reset(activeDocument);
			sceneManager_->RequestSceneTransition(
				eventResult.sceneTransitionId
			);
			return;
		}
		for (const SceneFishingFishCountRequest& request :
			eventResult.fishingFishCountRequests) {
			fishingScoreAttackSystem_.QueueFishCountAdjustment(
				request.directorEntityId,
				request.delta
			);
		}
		for (const SceneTextMotionRequest& request : eventResult.textMotionRequests) {
			if (request.type == SceneTextMotionRequestType::Play) {
				textMotionSystem_.Play(*activeDocument, request.entityId, request.clipId);
			} else if (request.type == SceneTextMotionRequestType::Stop) {
				textMotionSystem_.Stop(request.entityId);
			} else {
				textMotionSystem_.Reset(request.entityId);
			}
		}
		cameraSystem_.ApplyEventRequests(
			*activeDocument,
			camera_,
			player_,
			eventResult.cameraRequests
		);
		audioSystem_.ApplyRequests(*activeDocument, eventResult.audioRequests);
		postProcessProfileSystem_.ApplyEventResult(*activeDocument, eventResult);
	}
	if (activeDocument) {
		audioSystem_.Sync(
			*activeDocument,
			playing,
			GetSceneInstanceId(),
			sceneManager_ && sceneManager_->GetActiveSceneInstanceId() == GetSceneInstanceId()
		);
		if (playing) {
			// Event Play直後のVoiceにも、三人称Cameraを含む最終姿勢を同Frameで適用する。
			audioSystem_.UpdateSpatial(*activeDocument, camera_);
		}
	}
	postProcessProfileSystem_.Update(playing ? gameplayDeltaTime : 0.0f);
	textRenderSystem_.ClearTextOverrides();
	textRenderSystem_.ClearTextColorOverrides();
	textRenderSystem_.ClearPresentationOverrides();
	if (activeDocument) {
		for (const SceneGameFlowTextRequest& request : gameFlowResult.textRequests) {
			textRenderSystem_.SetTextOverride(request.entityId, request.text);
		}
		for (const SceneFishingScoreAttackTextRequest& request :
			fishingScoreAttackSystem_.GetTextRequests()) {
			textRenderSystem_.SetTextOverride(request.entityId, request.text);
			if (request.hasColor) {
				textRenderSystem_.SetTextColorOverride(request.entityId, request.color);
			}
		}
		SceneEntity* statusText = postProcessProfileSystem_.GetStatusTextEntityId() != 0
			? activeDocument->FindEntity(
				postProcessProfileSystem_.GetStatusTextEntityId()
			)
			: nullptr;
		if (!statusText &&
			!postProcessProfileSystem_.GetStatusTextEntityName().empty()) {
			statusText = activeDocument->FindEntityByName(
				postProcessProfileSystem_.GetStatusTextEntityName()
			);
		}
		if (statusText) {
			textRenderSystem_.SetTextOverride(
				statusText->id,
				postProcessProfileSystem_.GetStatusTextPrefix() +
					postProcessProfileSystem_.GetActiveProfileLabel()
			);
		}
	}
	for (const auto& [entityId, presentation] :
		textMotionSystem_.GetPresentationOverrides()) {
		textRenderSystem_.SetPresentationOverride(
			entityId,
			presentation.positionOffset,
			presentation.rotationOffset,
			presentation.scaleMultiplier,
			presentation.opacityMultiplier
		);
	}
	textRenderSystem_.Sync(activeDocument);
}

void RuntimeScene::UpdatePaused()
{
	cameraSystem_.UpdatePaused(camera_, debugCamera_);
	SceneDocument* document = GetSceneDocument();
	lightingSystem_.Sync(document);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	debugSystem_.DrawEditor(document, objectSystem_, true);
	monitorSystem_.DrawEditor(
		document,
		GetSceneViewCamera()
	);
	debugSystem_.AddDebugDraw(
		document,
		objectSystem_,
		cameraSystem_,
		camera_,
		true,
		false,
		true
	);
#endif
}

void RuntimeScene::Draw()
{
	DrawWithCamera(GetSceneViewCamera());
}

Camera* RuntimeScene::GetRenderCamera() const
{
	return GetSceneViewCamera();
}

void RuntimeScene::DrawWithCamera(Camera* viewCamera)
{
	DrawSceneView(viewCamera ? viewCamera : GetSceneViewCamera());
}

void RuntimeScene::DrawEnvironment(Camera* viewCamera)
{
	ApplyRenderCamera(viewCamera ? viewCamera : GetSceneViewCamera());
	environmentSystem_.DrawSkybox();
}

void RuntimeScene::BindLighting()
{
	lightingSystem_.Bind();
}

void RuntimeScene::DrawSceneContent(Camera* viewCamera)
{
	viewCamera = viewCamera ? viewCamera : GetSceneViewCamera();
	PrepareSceneContent(viewCamera);
	BindLighting();
	DrawPreparedSceneContentForView(viewCamera, 0);
}

void RuntimeScene::PrepareSceneContent(Camera* viewCamera)
{
	ApplyRenderCamera(viewCamera ? viewCamera : GetSceneViewCamera());
	objectSystem_.PrepareModelDraw();
}

void RuntimeScene::DrawPreparedSceneContent(Camera* viewCamera)
{
	DrawPreparedSceneContentForView(
		viewCamera ? viewCamera : GetSceneViewCamera(),
		0
	);
}

void RuntimeScene::DrawForegroundEffects()
{
	DrawForegroundEffectsWithCamera(GetSceneViewCamera());
}

void RuntimeScene::DrawForegroundEffectsWithCamera(Camera* viewCamera)
{
	viewCamera = viewCamera ? viewCamera : GetSceneViewCamera();
	ApplyRenderCamera(viewCamera);
	effectRenderSystem_.DrawForegroundPass(
		GetSceneDocument(),
		viewCamera,
		0,
		environmentSystem_,
		objectSystem_
	);
}

bool RuntimeScene::HasScreenOverlay() const
{
	const SceneDocument* document = GetSceneDocument();
	return
		document &&
		(
			miniMapSystem_.HasScreenOverlay(document) ||
			objectSystem_.HasScreenOverlaySprites(*document) ||
			textRenderSystem_.HasScreenOverlay(*document)
		);
}

void RuntimeScene::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	SceneDocument* document = GetSceneDocument();
	if (document) {
		miniMapSystem_.DrawScreenOverlay(document, width, height);
		objectSystem_.DrawScreenOverlaySprites(*document, width, height);
		textRenderSystem_.DrawScreenOverlay(*document, width, height);
	}
}

void RuntimeScene::DrawOffscreenViews()
{
	SceneDocument* document = GetSceneDocument();
	monitorSystem_.DrawOffscreen(
		document,
		runtimeObjectBindings_,
		cameraSystem_,
		[this](Camera* monitorCamera, uint64_t skipEntityId) {
			DrawSceneView(monitorCamera, skipEntityId);
		}
	);
	miniMapSystem_.DrawOffscreen(
		document,
		[this](Camera* miniMapCamera, uint64_t skipEntityId) {
			DrawSceneView(miniMapCamera, skipEntityId);
		}
	);
	if (document) {
		// Offscreen描画が差し替えたCameraを、通常Scene View用へ戻す。
		ApplyRenderCamera(GetSceneViewCamera());
	}
}

void RuntimeScene::DrawShadow()
{
	std::vector<Object3d*> shadowCasters;
	CollectShadowCasters(shadowCasters);
	RenderShadowCasters(shadowCasters);
}

void RuntimeScene::CollectShadowCasters(
	std::vector<Object3d*>& shadowCasters
) {
	const bool hidePlayerModel = ShouldHidePlayerModelForCamera(camera_);
	SceneDocument* document = GetSceneDocument();
	if (document) {
		objectSystem_.CollectShadowCasters(
			*document,
			hidePlayerModel,
			shadowCasters
		);
	}
}

void RuntimeScene::RenderShadowCasters(
	const std::vector<Object3d*>& shadowCasters
) {
	lightingSystem_.RenderShadows(shadowCasters);
}

void RuntimeScene::Finalize()
{
	// 非所有参照を持つSystemから解除し、最後にObjectとCameraを破棄する。
	monitorSystem_.Finalize(&runtimeObjectBindings_);
	miniMapSystem_.Finalize();
	agentSystem_.Clear();
	attachmentSystem_.Clear(&objectSystem_);
	combatSystem_.Clear();
	hitReactionSystem_.Clear();
	hitStopSystem_.Clear();
	enemySystem_.Clear();
	eventSystem_.Clear();
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	audioSystem_.Clear();
	postProcessProfileSystem_.Reset();
	stateMachineSystem_.Clear();
	attackRunnerSystem_.Clear();
	physicsSystem_.Clear();
	prefabAnimationSystem_.Clear();
	projectileSystem_.Clear();
	statSystem_.Clear();

	if (player_) {
		player_->Finalize();
		delete player_;
		player_ = nullptr;
	}

	runtimeObjectBindings_.clear();
	textRenderSystem_.Finalize();
	objectSystem_.Finalize();

	particleSystem_.Finalize();
	lightingSystem_.Finalize();
	effectRenderSystem_.Finalize();
	environmentSystem_.Finalize();
	cameraSystem_.Reset();

	delete camera_;
	camera_ = nullptr;

	delete debugCamera_;
	debugCamera_ = nullptr;
}

void RuntimeScene::PrepareForSceneTransition()
{
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	if (!executionContext || executionContext->IsPlaying()) {
		audioSystem_.PrepareForSceneTransition();
	}
}
