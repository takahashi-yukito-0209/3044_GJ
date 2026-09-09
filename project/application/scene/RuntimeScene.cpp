// 役割: SceneDocumentと各Runtime Systemを連携し、更新と描画を実行する。
#include "RuntimeScene.h"

#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneExecutionContext.h"
#include "../../engine/scene/EditorSession.h"
#include "../../engine/scene/SceneDocument.h"
#include "../../engine/scene/SceneEntityQuery.h"
#include "../../engine/scene/SceneTransformResolver.h"
#include "../../engine/3d/SrvManager.h"
#include "../../engine/base/DirectXCommon.h"

#include "../../engine/3d/Camera.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/3d/Object3d.h"
#include "../../engine/math/Math.h"
#include "../../engine/particle/ParticleManager.h"
#include "../../engine/utility/Logger.h"
#include "../player/Player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
	constexpr const char* kTitleSceneId = "title";
	constexpr const char* kTitleStartTargetSceneId = "gameplay";
	constexpr float kTitleStartTransitionSeconds = 0.85f;
	constexpr float kTitleStartTextFadeSeconds = 0.25f;

	/// <summary>
	/// 0から1の範囲へ値を制限します。
	/// </summary>
	float Clamp01(float value) {
		return std::clamp(value, 0.0f, 1.0f);
	}

	/// <summary>
	/// タイトル退出中に全TextRendererの透明度をまとめて上書きします。
	/// </summary>
	void ApplyTitleTextOpacityOverride(
		const SceneDocument& document,
		SceneTextRenderSystem& textRenderSystem,
		float opacityMultiplier
	) {
		for (const SceneEntity& entity : document.GetEntities()) { // Scene上のText候補。
			const SceneComponent* textRenderer =
				SceneEntityQuery::FindEnabledComponent(entity, "TextRenderer"); // 表示対象Text。
			if (
				!textRenderer ||
				!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)
			) {
				continue;
			}
			textRenderSystem.SetPresentationOverride(
				entity.id,
				{},
				0.0f,
				{ 1.0f, 1.0f },
				opacityMultiplier
			);
		}
	}

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

	void ProcessFormationParticleSaveRequest(
		SceneFishingScoreAttackSystem& system,
		SceneExecutionContext* executionContext,
		const std::string& runtimeSceneId
	) {
		SceneFishingScoreAttackFormationParticleSaveRequest request{};
		if (!system.ConsumeFormationParticleSaveRequest(request)) {
			return;
		}
		auto fail = [&system](std::string message) {
			system.SetFormationParticleSaveResult(false, std::move(message));
		};
		EditorSession* editorSession = dynamic_cast<EditorSession*>(executionContext);
		if (
			!editorSession ||
			(!editorSession->IsPlaying() && !editorSession->IsPaused())
		) {
			fail("Save failed: editor runtime session is unavailable.");
			return;
		}
		if (
			runtimeSceneId.empty() ||
			runtimeSceneId != editorSession->GetRuntimeSceneId() ||
			runtimeSceneId != editorSession->GetEditSceneId()
		) {
			fail("Save failed: runtime and edit scene IDs do not match.");
			return;
		}
		const SceneDocument& runtimeDocument = editorSession->GetActiveDocument();
		const SceneEntity* runtimeEntity = runtimeDocument.FindEntity(
			request.directorEntityId
		);
		if (!runtimeEntity) {
			fail("Save failed: runtime Director entity was not found.");
			return;
		}
		const SceneComponent* runtimeDirector = nullptr;
		for (const SceneComponent& component : runtimeEntity->components) {
			if (!component.enabled || component.type != "FishingScoreAttackDirector") {
				continue;
			}
			if (runtimeDirector) {
				fail("Save failed: runtime Director is not unique.");
				return;
			}
			runtimeDirector = &component;
		}
		if (!runtimeDirector) {
			fail("Save failed: runtime Director is not enabled.");
			return;
		}

		SceneDocument& editDocument = editorSession->GetEditDocument();
		SceneEntity* editEntity = editDocument.FindEntity(request.directorEntityId);
		if (!editEntity) {
			fail("Save failed: edit Director entity was not found.");
			return;
		}
		SceneComponent* editDirector = nullptr;
		for (SceneComponent& component : editEntity->components) {
			if (!component.enabled || component.type != "FishingScoreAttackDirector") {
				continue;
			}
			if (editDirector) {
				fail("Save failed: edit Director is not unique.");
				return;
			}
			editDirector = &component;
		}
		if (!editDirector) {
			fail("Save failed: edit Director is not enabled.");
			return;
		}

		const auto clampFinite = [](float value, float fallback, float minimum, float maximum) {
			return std::isfinite(value)
				? std::clamp(value, minimum, maximum)
				: fallback;
		};
		const auto sanitizeColor = [](const Vector4& color) {
			return Vector4{
				std::isfinite(color.x) ? std::clamp(color.x, 0.0f, 1.0f) : 0.1f,
				std::isfinite(color.y) ? std::clamp(color.y, 0.0f, 1.0f) : 0.9f,
				std::isfinite(color.z) ? std::clamp(color.z, 0.0f, 1.0f) : 1.0f,
				std::isfinite(color.w) ? std::clamp(color.w, 0.0f, 1.0f) : 0.65f
			};
		};
		const int pointCount = std::clamp(request.pointCount, 12, 128);
		const float startSize = clampFinite(request.startSize, 0.26f, 0.01f, 5.0f);
		const float endSize = clampFinite(request.endSize, 0.43f, 0.01f, 5.0f);
		const int countPerEmission = static_cast<int>(std::clamp(
			request.countPerEmission,
			1u,
			16u
		));
		const float emitterSpread = clampFinite(
			request.emitterSpread,
			0.0f,
			0.0f,
			0.5f
		);
		const float lifetime = clampFinite(request.lifetime, 0.8f, 0.1f, 3.0f);
		const Vector4 startColor = sanitizeColor(request.startColor);
		const Vector4 endColor = sanitizeColor(request.endColor);
		const float emissiveIntensity = clampFinite(
			request.emissiveIntensity,
			1.0f,
			0.0f,
			8.0f
		);
		const auto sameColor = [](const Vector4& left, const Vector4& right) {
			return left.x == right.x && left.y == right.y &&
				left.z == right.z && left.w == right.w;
		};
		const bool changed =
			editDirector->fishingFormationParticlePointCount != pointCount ||
			editDirector->fishingFormationParticleStartSize != startSize ||
			editDirector->fishingFormationParticleEndSize != endSize ||
			editDirector->fishingFormationParticleCountPerEmission != countPerEmission ||
			editDirector->fishingFormationParticleEmitterSpread != emitterSpread ||
			editDirector->fishingFormationParticleLifetime != lifetime ||
			!sameColor(editDirector->fishingFormationParticleStartColor, startColor) ||
			!sameColor(editDirector->fishingFormationParticleEndColor, endColor) ||
			editDirector->fishingFormationParticleEmissiveIntensity != emissiveIntensity;
		if (changed) {
			const SceneDocument beforeSnapshot = editDocument;
			editDirector->fishingFormationParticlePointCount = pointCount;
			editDirector->fishingFormationParticleStartSize = startSize;
			editDirector->fishingFormationParticleEndSize = endSize;
			editDirector->fishingFormationParticleCountPerEmission = countPerEmission;
			editDirector->fishingFormationParticleEmitterSpread = emitterSpread;
			editDirector->fishingFormationParticleLifetime = lifetime;
			editDirector->fishingFormationParticleStartColor = startColor;
			editDirector->fishingFormationParticleEndColor = endColor;
			editDirector->fishingFormationParticleEmissiveIntensity = emissiveIntensity;
			editDocument.MarkDirty();
			if (!editorSession->CommitRuntimeEditAndSave(beforeSnapshot)) {
				const std::string& saveError = editDocument.GetLastSaveError();
				fail(saveError.empty()
					? std::string("Save failed.")
					: std::string("Save failed: ") + saveError);
				return;
			}
		} else if (
			editDocument.IsDirty() &&
			!editorSession->CommitRuntimeEditAndSave(editDocument)
		) {
			const std::string& saveError = editDocument.GetLastSaveError();
			fail(saveError.empty()
				? std::string("Save failed.")
				: std::string("Save failed: ") + saveError);
			return;
		}
		system.SetFormationParticleSaveResult(
			true,
			"Formation particle settings saved to Scene."
		);
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

	bool TryProjectWorldPositionToViewport(
		const Camera& camera,
		const Vector3& worldPosition,
		Vector2& viewportPosition
	) {
		const Matrix4x4& viewProjection = camera.GetViewProjectionMatrix();
		const float x =
			worldPosition.x * viewProjection.m[0][0] +
			worldPosition.y * viewProjection.m[1][0] +
			worldPosition.z * viewProjection.m[2][0] +
			viewProjection.m[3][0];
		const float y =
			worldPosition.x * viewProjection.m[0][1] +
			worldPosition.y * viewProjection.m[1][1] +
			worldPosition.z * viewProjection.m[2][1] +
			viewProjection.m[3][1];
		const float z =
			worldPosition.x * viewProjection.m[0][2] +
			worldPosition.y * viewProjection.m[1][2] +
			worldPosition.z * viewProjection.m[2][2] +
			viewProjection.m[3][2];
		const float w =
			worldPosition.x * viewProjection.m[0][3] +
			worldPosition.y * viewProjection.m[1][3] +
			worldPosition.z * viewProjection.m[2][3] +
			viewProjection.m[3][3];
		if (!std::isfinite(w) || w <= 0.000001f) {
			return false;
		}
		const float inverseW = 1.0f / w;
		const float normalizedDepth = z * inverseW;
		if (!std::isfinite(normalizedDepth) ||
			normalizedDepth < 0.0f || normalizedDepth > 1.0f) {
			return false;
		}
		const float normalizedX = x * inverseW;
		const float normalizedY = y * inverseW;
		if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY)) {
			return false;
		}
		viewportPosition = {
			normalizedX * 0.5f + 0.5f,
			0.5f - normalizedY * 0.5f
		};
		return true;
	}

	void ApplyHookBubbleSpriteOverrides(
		SceneObjectSystem& objectSystem,
		const SceneFishingScoreAttackSystem& fishingScoreAttackSystem,
		Camera* camera
	) {
		for (const SceneFishingScoreAttackHookBubbleRequest& request :
			fishingScoreAttackSystem.GetHookBubbleRequests()) {
			Vector2 viewportPosition{};
			const bool projected = camera &&
			TryProjectWorldPositionToViewport(
				*camera,
				request.worldAnchor,
				viewportPosition
			) &&
			viewportPosition.x >= 0.0f && viewportPosition.x <= 1.0f &&
			viewportPosition.y >= 0.0f && viewportPosition.y <= 1.0f;
			const auto apply = [
				&objectSystem,
				&viewportPosition,
				projected
			](
				uint64_t entityId,
				const std::string& texturePath,
				const Vector2& size,
				const Vector2& screenOffset,
				bool requestedVisible
			) {
				if (entityId == 0) {
					return;
				}
				SceneSpriteRuntimeOverride overrideValue{};
				overrideValue.entityId = entityId;
				overrideValue.texturePath = texturePath;
				overrideValue.size = size;
				overrideValue.color = { 1.0f, 1.0f, 1.0f, 1.0f };
				overrideValue.visible = requestedVisible && projected;
				if (overrideValue.visible) {
					overrideValue.hasViewportPositionOverride = true;
					overrideValue.viewportPosition = viewportPosition;
					overrideValue.positionOffsetPixels = screenOffset;
				}
				objectSystem.SetSpriteRuntimeOverride(overrideValue);
			};
			apply(
				request.bubbleSpriteEntityId,
				request.bubbleTexturePath,
				request.bubbleSize,
				request.bubbleScreenOffset,
				request.bubbleVisible
			);
			apply(
				request.rankIconSpriteEntityId,
				request.rankIconTexturePath,
				request.rankIconSize,
				request.rankIconScreenOffset,
				request.rankIconVisible
			);
		}
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

/// <summary>
/// タイトルのSTART決定後に退出演出を開始します。
/// </summary>
void RuntimeScene::BeginTitleStartTransition() {
	titleStartTransitionActive_ = true;
	titleStartTransitionElapsedSeconds_ = 0.0f;
}

/// <summary>
/// タイトル退出演出を進め、遷移可能になったかを返します。
/// </summary>
bool RuntimeScene::UpdateTitleStartTransition(float deltaTime) {
	if (!titleStartTransitionActive_) {
		return false;
	}
	titleStartTransitionElapsedSeconds_ += (std::max)(deltaTime, 0.0f);
	return titleStartTransitionElapsedSeconds_ >= kTitleStartTransitionSeconds;
}

/// <summary>
/// タイトル退出演出の進行度を0から1で返します。
/// </summary>
float RuntimeScene::GetTitleStartTransitionProgress() const {
	if (!titleStartTransitionActive_) {
		return 0.0f;
	}
	return Clamp01(
		titleStartTransitionElapsedSeconds_ / kTitleStartTransitionSeconds
	);
}

/// <summary>
/// タイトル退出演出の状態を初期化します。
/// </summary>
void RuntimeScene::ClearTitleStartTransition() {
	titleStartTransitionActive_ = false;
	titleStartTransitionElapsedSeconds_ = 0.0f;
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
		// フェード中にPlayerの設定や水域状態が未同期にならないよう、
		// 開始位置を設定した直後に物理の初期状態まで確定する。
		physicsSystem_.SyncSceneSettings(
			*initialDocument,
			player_,
			runtimeObjectBindings_,
			initialEditing
		);
		// フェード遷移中はUpdateを止めたまま描画へ入るため、ここで
		// Scene定義の開始Cameraを反映して、生成直後のOrbit Cameraを出さない。
		cameraSystem_.UpdateBeforeSimulation(
			*initialDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			0.0f,
			initialPlaying,
			initialPlaying,
			false,
			false
		);
		cameraSystem_.UpdateAfterSimulation(
			*initialDocument,
			camera_,
			player_,
			runtimeObjectBindings_,
			0.0f,
			initialPlaying,
			initialPlaying
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
	if (initialDocument) {
		fishingResultPresentationSystem_.Update(
			*initialDocument,
			initialExecutionContext && initialExecutionContext->IsPlaying()
				? &initialExecutionContext->GetRuntimeSessionState()
				: nullptr,
			0.0f,
			initialPlaying
		);
		if (!fishingResultPresentationSystem_.GetSpriteRequests().empty()) {
			objectSystem_.ClearSpriteOverrides();
			for (const SceneFishingResultPresentationSpriteRequest& request :
				fishingResultPresentationSystem_.GetSpriteRequests()) {
				objectSystem_.SetSpriteRuntimeOverride(SceneSpriteRuntimeOverride{
					request.entityId,
					request.texturePath,
					request.size,
					request.color,
					request.visible
				});
			}
			objectSystem_.SyncSprites(initialDocument);
		}
		if (!fishingResultPresentationSystem_.GetTextRequests().empty()) {
			textRenderSystem_.ClearTextOverrides();
			for (const SceneFishingResultPresentationTextRequest& request :
				fishingResultPresentationSystem_.GetTextRequests()) {
				textRenderSystem_.SetTextOverride(request.entityId, request.text);
			}
			textRenderSystem_.Sync(initialDocument);
		}
	}

}

void RuntimeScene::Update(float deltaTime)
{
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	const bool editing = executionContext && executionContext->IsEditing();
	const bool playing = !executionContext || executionContext->IsPlaying();
	SceneDocument* activeDocument = GetSceneDocument();
	const float realDeltaTime = (std::max)(deltaTime, 0.0f);
	if (activeDocument && playing) {
		pauseSystem_.BeginFrame(*activeDocument);
		if (GetSceneAssetId() == "gameplay") {
			const ScenePauseMenuResult pauseMenuResult = pauseMenuSystem_.Update(
				*activeDocument, pauseSystem_, optionMenuSystem_, realDeltaTime
			);
			const SceneEntity* pauseController =
				activeDocument->FindEntityByName("Pause Menu Controller");
			if (pauseController && (pauseMenuResult.pauseRequested ||
				pauseMenuResult.resumeRequested)) {
				pauseSystem_.CommitRequests(*activeDocument, { {
					pauseController->id,
					"PauseMenu",
					"PauseMenu",
					pauseMenuResult.pauseRequested
						? ScenePauseOperation::Pause
						: ScenePauseOperation::Resume
				} });
			}
			if (!pauseMenuResult.requestedSceneId.empty()) {
				sceneManager_->RequestSceneTransition(
					pauseMenuResult.requestedSceneId
				);
				return;
			}
		} else {
			pauseMenuSystem_.Clear();
		}
	} else {
		pauseSystem_.Clear();
		pauseMenuSystem_.Clear();
	}
	const bool gameplayPaused =
		activeDocument && playing &&
		pauseSystem_.IsDomainPaused(ScenePauseDomain::Gameplay);
	const bool physicsPaused =
		activeDocument && playing &&
		pauseSystem_.IsDomainPaused(ScenePauseDomain::Physics);
	const bool gameplayInputPaused =
		activeDocument && playing &&
		pauseSystem_.IsDomainPaused(ScenePauseDomain::GameplayInput);
	const bool worldAnimationPaused =
		activeDocument && playing &&
		pauseSystem_.IsDomainPaused(ScenePauseDomain::WorldAnimation);
	const bool worldEffectsPaused =
		activeDocument && playing &&
		pauseSystem_.IsDomainPaused(ScenePauseDomain::WorldEffects);
	if (activeDocument) {
		postProcessProfileSystem_.Sync(*activeDocument);
		// 2Dの先読みはTransform確定を待たないため、Eventより前に完了させる。
		audioSystem_.Sync(
			*activeDocument,
			playing,
			GetSceneInstanceId(),
			sceneManager_ && sceneManager_->GetActiveSceneInstanceId() == GetSceneInstanceId()
		);
		audioSystem_.ApplyProcessPolicy(
			*activeDocument,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument, entityId, ScenePauseDomain::Audio
				);
			}
		);
	} else {
		postProcessProfileSystem_.Reset();
	}
	std::vector<uint64_t> spawnerResetEntityIds;
	const float hitStopDeltaTime = playing && !gameplayPaused
		? hitStopSystem_.Advance(realDeltaTime)
		: 0.0f;
	const float gameplayDeltaTime = gameplayPaused
		? 0.0f
		: (playing ? hitStopDeltaTime : realDeltaTime);
	const float physicsDeltaTime = physicsPaused
		? 0.0f
		: (gameplayPaused ? realDeltaTime : gameplayDeltaTime);

	// 遷移が成立したフレームは旧Sceneの状態をこれ以上変更しない。
	if (playing && !gameplayPaused && gameplayDeltaTime > 0.0f && activeDocument) {
		if (GetSceneAssetId() == kTitleSceneId) {
			if (titleStartTransitionActive_) {
				if (UpdateTitleStartTransition(realDeltaTime)) {
					sceneManager_->RequestSceneTransition(
						kTitleStartTargetSceneId
					);
					return;
				}
			} else {
				const SceneTitleMenuResult titleMenuResult =
					titleMenuSystem_.Update(*activeDocument);
				if (titleMenuResult.exitRequested) {
					exitRequested_ = true;
					return;
				}
				if (!titleMenuResult.requestedSceneId.empty()) {
					if (
						titleMenuResult.requestedSceneId ==
						kTitleStartTargetSceneId
					) {
						BeginTitleStartTransition();
					} else {
						sceneManager_->RequestSceneTransition(
							titleMenuResult.requestedSceneId
						);
						return;
					}
				}
			}
			optionMenuSystem_.Clear();
		} else if (GetSceneAssetId() == "option") {
			ClearTitleStartTransition();
			const SceneOptionMenuResult optionMenuResult =
				optionMenuSystem_.Update(*activeDocument);
			if (!optionMenuResult.requestedSceneId.empty()) {
				sceneManager_->RequestSceneTransition(
					optionMenuResult.requestedSceneId
				);
				return;
			}
			titleMenuSystem_.Clear();
		} else {
			ClearTitleStartTransition();
			titleMenuSystem_.Clear();
			optionMenuSystem_.Clear();
		}
		const SceneTransitionRequest transitionRequest =
			transitionSystem_.Update(*activeDocument);
		if (!transitionRequest.targetSceneId.empty()) {
			sceneManager_->RequestSceneTransition(
				transitionRequest.targetSceneId,
				transitionRequest.useEffect
			);
			return;
		}
	}
	SceneGameFlowResult gameFlowResult{};
	if (activeDocument && playing) {
		gameFlowResult = gameFlowSystem_.Update(
			*activeDocument,
			enemySpawnerSystem_,
			realDeltaTime,
			!gameplayPaused
		);
		if (!gameplayPaused) {
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
		}
	} else {
		gameFlowSystem_.Clear();
	}
	if (activeDocument && playing) {
		if (!gameplayPaused) {
			// Fish選択はObject同期前に確定し、同FrameのCollider生成へ反映する。
			fishingScoreAttackSystem_.UpdateBeforeSimulation(
				*activeDocument,
				GetSceneAssetId(),
				deltaTime,
				true
			);
		}
	} else {
		fishingScoreAttackSystem_.Clear();
	}
	if (activeDocument && playing && executionContext) {
		SceneFishingScoreAttackSessionBeginRequest beginRequest{};
		while (fishingScoreAttackSystem_.ConsumeResultSessionBeginRequest(
			beginRequest
		)) {
			executionContext->GetRuntimeSessionState().BeginFishingRun(
				beginRequest.channelId
			);
		}
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
		editing,
		!playing || !worldEffectsPaused
	);
	runtimeEffectSystem_.SetWorldEffectsPaused(runtimeSceneId, worldEffectsPaused);
	effectRenderSystem_.Update(worldEffectsPaused ? 0.0f : realDeltaTime);
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
	if (activeDocument) {
		fishingScoreAttackSystem_.DrawFormationParticleTuningImGui(
			*activeDocument,
			playing
		);
	}
#endif
	ProcessFormationParticleSaveRequest(
		fishingScoreAttackSystem_,
		executionContext,
		runtimeSceneId
	);
	lightingSystem_.Sync(activeDocument);
	if (activeDocument && playing) {
		// 前フレームで寿命切れ/HitしたRuntime Entityをbinding再構築前に破棄する。
		combatSystem_.FlushRemovals(*activeDocument);
		projectileSystem_.FlushRemovals(*activeDocument);
		// 保存値を実行時状態へ展開し、Transform AnimationをObject同期前に反映する。
		statSystem_.Update(*activeDocument);
		if (!gameplayPaused && gameFlowResult.gameplayAllowed) {
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
		if (!gameplayPaused && gameFlowResult.gameplayAllowed) {
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
			runtimeEffectSystem_.SetWorldEffectsPaused(
				runtimeSceneId, worldEffectsPaused
			);
			runtimeEffectSystem_.Advance(
				*activeDocument, worldEffectsPaused ? 0.0f : realDeltaTime
			);
			prefabAnimationSystem_.Update(
				*activeDocument,
				worldAnimationPaused ? 0.0f : realDeltaTime,
				[this, activeDocument](uint64_t entityId) {
					return pauseSystem_.ShouldProcess(
						*activeDocument, entityId, ScenePauseDomain::WorldAnimation
					);
				}
			);
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
	bool runtimeBindingsValid = true;
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
		std::string bindingDiagnostic;
		if (!objectSystem_.ValidateBindings(
			*activeDocument,
			runtimeObjectBindings_,
			bindingDiagnostic
		)) {
			Logger::Log(
				"Runtime binding validation failed after BuildBindings: " +
				bindingDiagnostic + "\n"
			);
			objectSystem_.BuildBindings(
				*activeDocument,
				runtimeObjectBindings_
			);
			bindingDiagnostic.clear();
			if (!objectSystem_.ValidateBindings(
				*activeDocument,
				runtimeObjectBindings_,
				bindingDiagnostic
			)) {
				Logger::Log(
					"Runtime binding rebuild failed after BuildBindings: " +
					bindingDiagnostic + "\n"
				);
				runtimeBindingsValid = false;
			}
		}
		if (playing && GetSceneAssetId() == "title") {
			titleBoatMotionSystem_.Update(
				*activeDocument,
				runtimeObjectBindings_,
				deltaTime,
				GetTitleStartTransitionProgress()
			);
		} else {
			titleBoatMotionSystem_.Clear();
		}
		fishingScoreAttackSystem_.ApplyHookVisualOverrides(
			*activeDocument,
			runtimeObjectBindings_
		);
		fishingScoreAttackSystem_.ApplySharkVisualOverrides(
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
		if (playing && !gameplayPaused && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
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
			!gameplayInputPaused &&
				fishingScoreAttackSystem_.IsCameraControlAllowed(),
			fishingScoreAttackSystem_.AcceptWheelZoom(),
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument, entityId, ScenePauseDomain::WorldAnimation
				);
			}
		);
	}
	Vector3 playerAttackInputDirection{};
	if (player_ && playing && !gameplayPaused) {
		player_->Update(
			camera_,
			gameFlowResult.gameplayAllowed &&
				fishingScoreAttackSystem_.IsPlayerMovementAllowed() &&
				!gameplayInputPaused,
			gameplayDeltaTime
		);
		const Vector3& playerVelocity = player_->GetPhysicsBody().velocity;
		playerAttackInputDirection = { playerVelocity.x, 0.0f, playerVelocity.z };
		if (Math::Length(playerAttackInputDirection) > 0.0001f) {
			playerAttackInputDirection = Math::Normalize(playerAttackInputDirection);
		}
	}
	const float stateMachineDeltaTime = gameplayPaused
		? realDeltaTime
		: gameplayDeltaTime;
	if (
		activeDocument && playing && gameFlowResult.gameplayAllowed &&
		stateMachineDeltaTime > 0.0f
	) {
		// State行動は入力取得後、Physics確定前に速度・攻撃判定を更新する。
		stateMachineSystem_.Update(
			*activeDocument,
			runtimeObjectBindings_,
			player_,
			attackRunnerSystem_,
			prefabAnimationSystem_,
			stateMachineDeltaTime,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument,
					entityId,
					ScenePauseDomain::Gameplay
				);
			}
		);
	}
	if (activeDocument && playing && !gameplayPaused &&
		gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
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
	if (activeDocument && (!playing || physicsDeltaTime > 0.0f)) {
		bool physicsBindingsValid = runtimeBindingsValid;
		std::string bindingDiagnostic;
		if (!objectSystem_.ValidateBindings(
			*activeDocument,
			runtimeObjectBindings_,
			bindingDiagnostic
		)) {
			Logger::Log(
				"Runtime binding validation failed before Physics Step: " +
				bindingDiagnostic + "\n"
			);
			objectSystem_.BuildBindings(
				*activeDocument,
				runtimeObjectBindings_
			);
			bindingDiagnostic.clear();
			physicsBindingsValid = objectSystem_.ValidateBindings(
				*activeDocument,
				runtimeObjectBindings_,
				bindingDiagnostic
			);
			if (!physicsBindingsValid) {
				Logger::Log(
					"Runtime binding rebuild failed before Physics Step: " +
					bindingDiagnostic + "\n"
				);
			}
		}
		if (physicsBindingsValid) {
			physicsSystem_.Step(
				player_,
				runtimeObjectBindings_,
				physicsDeltaTime,
				playing
			);
		}
	}
	if (player_ && playing && physicsDeltaTime > 0.0f) {
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
	if (activeDocument && playing && !gameplayPaused) {
		// Player Physics後のCollider world transformで釣り針Triggerを判定する。
		fishingScoreAttackSystem_.UpdateAfterSimulation(
			*activeDocument,
			GetSceneAssetId(),
			runtimeObjectBindings_,
			agentSystem_,
			true,
			gameplayDeltaTime,
			player_ ? player_->GetPhysicsBody().velocity : Vector3{}
		);
		if (executionContext) {
			SceneFishingScoreAttackSessionPublishRequest publishRequest{};
			while (fishingScoreAttackSystem_.ConsumeResultSessionPublishRequest(
				publishRequest
			)) {
				publishRequest.record.sourceSceneId = GetSceneAssetId().empty()
					? "runtime"
					: GetSceneAssetId();
				publishRequest.record.sourceSceneInstanceId = GetSceneInstanceId();
				executionContext->GetRuntimeSessionState().PublishFishingResult(
					std::move(publishRequest.record)
				);
			}
		}
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
	if (activeDocument && playing && !gameplayPaused && gameFlowResult.gameplayAllowed && gameplayDeltaTime > 0.0f) {
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
	if (activeDocument) {
		// Physics／AI反映後の最終Hook位置から、bubble requestを再構築する。
		fishingScoreAttackSystem_.ApplyHookVisualOverrides(
			*activeDocument,
			runtimeObjectBindings_
		);
		fishingScoreAttackSystem_.ApplySharkVisualOverrides(
			*activeDocument,
			runtimeObjectBindings_
		);
	}
	if (activeDocument) {
		fishingResultPresentationSystem_.Update(
			*activeDocument,
			executionContext
				? &executionContext->GetRuntimeSessionState()
				: nullptr,
			realDeltaTime,
			playing
		);
	} else {
		fishingResultPresentationSystem_.Clear();
	}
	runtimeEffectSystem_.SetWorldEffectsPaused(runtimeSceneId, worldEffectsPaused);
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
		if (GetSceneAssetId() == "gameplay") {
			if (const SceneEntity* pauseOverlay =
				activeDocument->FindEntityByName("Pause Dim Overlay")) {
				objectSystem_.SetSpriteRuntimeOverride(SceneSpriteRuntimeOverride{
					pauseOverlay->id,
					"human/white.png",
					{ 4096.0f, 4096.0f },
					{ 0.0f, 0.0f, 0.0f, 0.58f },
					gameplayPaused
				});
			}
		}
	}
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
	if (activeDocument) {
		ApplyHookBubbleSpriteOverrides(
			objectSystem_,
			fishingScoreAttackSystem_,
			GetSceneViewCamera()
		);
		for (const SceneFishingResultPresentationSpriteRequest& request :
			fishingResultPresentationSystem_.GetSpriteRequests()) {
			objectSystem_.SetSpriteRuntimeOverride(SceneSpriteRuntimeOverride{
				request.entityId,
				request.texturePath,
				request.size,
				request.color,
				request.visible
			});
		}
	}
	objectSystem_.SyncSprites(activeDocument);
	// Transform確定後に環境設定とDebug形状を登録し、描画時の状態を揃える。
	environmentSystem_.Sync(activeDocument, runtimeObjectBindings_);
	if (activeDocument) {
		fishingScoreAttackSystem_.UpdateFormationParticleEffect(
			*activeDocument,
			agentSystem_,
			worldEffectsPaused ? 0.0f : realDeltaTime,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument, entityId, ScenePauseDomain::WorldEffects
				);
			},
			runtimeSceneId
		);
		fishingScoreAttackSystem_.AddFormationOutlineDebugDraw(
			*activeDocument,
			agentSystem_
		);
		fishingScoreAttackSystem_.AddSharkNavigationDebugDraw(*activeDocument);
	}
	if (activeDocument && playing) {
		// Eventは同FrameのTextMotion completionを次Packageで受け取れる位置に置く。
		textMotionSystem_.Update(
			*activeDocument,
			worldAnimationPaused ? 0.0f : realDeltaTime,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument, entityId, ScenePauseDomain::WorldAnimation
				);
			}
		);
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
	if (activeDocument && playing) {
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
			pauseSystem_,
			executionContext
				? &executionContext->GetRuntimeSessionState()
				: nullptr,
			realDeltaTime,
			eventSignals,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument,
					entityId,
					ScenePauseDomain::Gameplay
				);
			}
		);
		if (!eventResult.sceneTransitionId.empty()) {
			postProcessProfileSystem_.Reset(activeDocument);
			sceneManager_->RequestSceneTransition(
				eventResult.sceneTransitionId,
				eventResult.sceneTransitionUseEffect
			);
			return;
		}
		pauseSystem_.CommitRequests(*activeDocument, eventResult.pauseRequests);
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
		audioSystem_.ApplyProcessPolicy(
			*activeDocument,
			[this, activeDocument](uint64_t entityId) {
				return pauseSystem_.ShouldProcess(
					*activeDocument, entityId, ScenePauseDomain::Audio
				);
			}
		);
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
	const uint64_t postProcessManagerEntityId =
		postProcessProfileSystem_.GetActiveManagerEntityId();
	const bool advancePostProcess = activeDocument && playing &&
		(postProcessManagerEntityId == 0 || pauseSystem_.ShouldProcess(
			*activeDocument,
			postProcessManagerEntityId,
			ScenePauseDomain::WorldEffects
		));
	postProcessProfileSystem_.Update(advancePostProcess ? realDeltaTime : 0.0f);
	textRenderSystem_.ClearTextOverrides();
	textRenderSystem_.ClearTextColorOverrides();
	textRenderSystem_.ClearViewportPositionOverrides();
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
		for (const SceneFishingResultPresentationTextRequest& request :
			fishingResultPresentationSystem_.GetTextRequests()) {
			textRenderSystem_.SetTextOverride(request.entityId, request.text);
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
		if (playing && GetSceneAssetId() == kTitleSceneId) {
			titleMenuSystem_.ApplyTextOverrides(
				*activeDocument,
				textRenderSystem_
			);
		} else if (playing && GetSceneAssetId() == "option") {
			optionMenuSystem_.ApplyTextOverrides(
				*activeDocument,
				textRenderSystem_
			);
		} else if (playing && GetSceneAssetId() == "gameplay") {
			const bool pauseActive =
				pauseSystem_.IsDomainPaused(ScenePauseDomain::Gameplay);
			if (pauseActive) {
				for (const SceneEntity& entity : activeDocument->GetEntities()) {
					const SceneComponent* textRenderer =
						SceneEntityQuery::FindEnabledComponent(entity, "TextRenderer");
					if (!textRenderer ||
						!SceneEntityQuery::IsEntityActiveInHierarchy(
							*activeDocument, entity
						) ||
						entity.name.rfind("Pause", 0) == 0) {
						continue;
					}
					Vector4 dimmedColor = textRenderer->textColor;
					dimmedColor.x *= 0.42f;
					dimmedColor.y *= 0.42f;
					dimmedColor.z *= 0.42f;
					textRenderSystem_.SetTextColorOverride(entity.id, dimmedColor);
				}
			}
			pauseMenuSystem_.ApplyTextOverrides(
				*activeDocument,
				textRenderSystem_,
				optionMenuSystem_
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
	const SceneFishingScoreAttackScorePopup& scorePopup =
		fishingScoreAttackSystem_.GetScorePopup();
	Camera* popupCamera = GetSceneViewCamera();
	Vector2 popupViewportPosition{};
	if (
		activeDocument && popupCamera && scorePopup.active &&
		scorePopup.entityId != 0 &&
		TryProjectWorldPositionToViewport(
			*popupCamera,
			{
				scorePopup.worldPosition.x,
				scorePopup.worldPosition.y + scorePopup.elapsedSeconds * 1.5f,
				scorePopup.worldPosition.z
			},
			popupViewportPosition
		)
	) {
		const float progress = std::clamp(
			scorePopup.elapsedSeconds / scorePopup.durationSeconds,
			0.0f,
			1.0f
		);
		textRenderSystem_.SetViewportPositionOverride(
			scorePopup.entityId,
			popupViewportPosition
		);
		textRenderSystem_.SetPresentationOverride(
			scorePopup.entityId,
			{},
			0.0f,
			{ 1.0f, 1.0f },
			1.0f - progress
		);
	}
	if (
		activeDocument &&
		playing &&
		GetSceneAssetId() == kTitleSceneId &&
		titleStartTransitionActive_
	) {
		const float textFadeProgress = Clamp01(
			titleStartTransitionElapsedSeconds_ /
			kTitleStartTextFadeSeconds
		); // タイトル文字のフェード進行度。
		ApplyTitleTextOpacityOverride(
			*activeDocument,
			textRenderSystem_,
			1.0f - textFadeProgress
		);
	}
	textRenderSystem_.Sync(activeDocument);
}

void RuntimeScene::UpdatePaused()
{
	cameraSystem_.UpdatePaused(camera_, debugCamera_);
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	SceneDocument* document = GetSceneDocument();
	if (document) {
		fishingResultPresentationSystem_.Update(
			*document,
			executionContext && executionContext->IsPlaying()
				? &executionContext->GetRuntimeSessionState()
				: nullptr,
			0.0f,
			executionContext && executionContext->IsPlaying()
		);
		fishingScoreAttackSystem_.ApplyHookVisualOverrides(
			*document,
			runtimeObjectBindings_
		);
		fishingScoreAttackSystem_.ApplySharkVisualOverrides(
			*document,
			runtimeObjectBindings_
		);
		fishingScoreAttackSystem_.AddSharkNavigationDebugDraw(*document);
		objectSystem_.ClearSpriteOverrides();
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
		ApplyHookBubbleSpriteOverrides(
			objectSystem_,
			fishingScoreAttackSystem_,
			GetSceneViewCamera()
		);
		for (const SceneFishingResultPresentationSpriteRequest& request :
			fishingResultPresentationSystem_.GetSpriteRequests()) {
			objectSystem_.SetSpriteRuntimeOverride(SceneSpriteRuntimeOverride{
				request.entityId,
				request.texturePath,
				request.size,
				request.color,
				request.visible
			});
		}
		objectSystem_.SyncSprites(document);
	}
	lightingSystem_.Sync(document);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	debugSystem_.DrawEditor(document, objectSystem_, true);
	monitorSystem_.DrawEditor(
		document,
		GetSceneViewCamera()
	);
	if (document) {
		fishingScoreAttackSystem_.DrawFormationParticleTuningImGui(
			*document,
			true
		);
	}
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
	ProcessFormationParticleSaveRequest(
		fishingScoreAttackSystem_,
		executionContext,
		GetSceneAssetId()
	);
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
			// ミニマップ表示を一時停止するため、Overlay判定から外す。
			// miniMapSystem_.HasScreenOverlay(document) ||
			objectSystem_.HasScreenOverlaySprites(*document) ||
			textRenderSystem_.HasScreenOverlay(*document)
		);
}

void RuntimeScene::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	SceneDocument* document = GetSceneDocument();
	if (document) {
		// ミニマップ表示を一時停止するため、描画呼び出しを残して無効化する。
		// miniMapSystem_.DrawScreenOverlay(document, width, height);
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

void RuntimeScene::SetRenderAspectRatio(float aspectRatio)
{
	const float safeAspectRatio = (std::max)(aspectRatio, 0.001f);
	if (camera_) {
		camera_->SetAspectRatio(safeAspectRatio);
		camera_->Update();
	}
	if (debugCamera_) {
		debugCamera_->SetAspectRatio(safeAspectRatio);
		debugCamera_->Update();
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
	pauseSystem_.Clear();
	pauseMenuSystem_.Clear();
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	fishingResultPresentationSystem_.Clear();
	ClearTitleStartTransition();
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

bool RuntimeScene::ConsumeExitRequest()
{
	const bool exitRequested = exitRequested_; // 今回消費する終了要求。
	exitRequested_ = false;
	return exitRequested;
}

void RuntimeScene::PrepareForSceneTransition()
{
	exitRequested_ = false;
	pauseSystem_.Clear();
	pauseMenuSystem_.Clear();
	textMotionSystem_.Clear();
	gameFlowSystem_.Clear();
	fishingResultPresentationSystem_.Clear();
	ClearTitleStartTransition();
	SceneExecutionContext* executionContext = sceneManager_
		? sceneManager_->GetExecutionContext()
		: nullptr;
	if (!executionContext || executionContext->IsPlaying()) {
		audioSystem_.PrepareForSceneTransition();
	}
}
