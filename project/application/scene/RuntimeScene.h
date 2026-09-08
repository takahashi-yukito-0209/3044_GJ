// 役割: SceneDocumentを実行時オブジェクトへ反映し、更新と描画を管理する。
#pragma once
#include "../../engine/scene/BaseScene.h"
#include "SceneRuntimeObjectBinding.h"
#include "system/SceneAgentSystem.h"
#include "system/SceneAudioSystem.h"
#include "system/SceneAttackRunnerSystem.h"
#include "system/SceneAttachmentSystem.h"
#include "system/SceneCameraSystem.h"
#include "system/SceneCombatSystem.h"
#include "system/SceneDebugSystem.h"
#include "system/SceneEnemySystem.h"
#include "system/SceneEnemySpawnerSystem.h"
#include "system/SceneEventSystem.h"
#include "system/SceneHitReactionSystem.h"
#include "system/SceneHitStopSystem.h"
#include "system/SceneGameFlowSystem.h"
#include "system/SceneFishingScoreAttackSystem.h"
#include "system/SceneEffectRenderSystem.h"
#include "system/SceneEnvironmentSystem.h"
#include "system/SceneLightingSystem.h"
#include "system/SceneMiniMapSystem.h"
#include "system/SceneMonitorSystem.h"
#include "system/SceneObjectSystem.h"
#include "system/SceneParticleSystem.h"
#include "system/SceneOptionMenuSystem.h"
#include "system/ScenePauseSystem.h"
#include "system/ScenePostProcessProfileSystem.h"
#include "system/ScenePhysicsSystem.h"
#include "system/ScenePrefabAnimationSystem.h"
#include "system/SceneProjectileSystem.h"
#include "system/SceneRuntimeEffectSystem.h"
#include "system/SceneStatSystem.h"
#include "system/SceneStateMachineSystem.h"
#include "system/SceneTextRenderSystem.h"
#include "system/SceneTextMotionSystem.h"
#include "system/SceneTitleBoatMotionSystem.h"
#include "system/SceneTitleMenuSystem.h"
#include "system/SceneTransitionSystem.h"

#include <cstdint>
#include <vector>

class Camera;
class Object3d;
class Player;

// 各SystemとCameraを所有し、Component同期から描画までの実行順だけを決定する。
class RuntimeScene : public BaseScene
{
public:
	// BaseSceneのライフサイクル入口。個別機能は対応するSystemへ委譲する。
	void Initialize() override;
	void Finalize() override;
	void PrepareForSceneTransition() override;
	void Update(float deltaTime) override;
	void UpdatePaused() override;
	void Draw() override;
	Camera* GetRenderCamera() const override;
	void DrawWithCamera(Camera* viewCamera) override;
	void DrawEnvironment(Camera* viewCamera) override;
	void BindLighting() override;
	void DrawSceneContent(Camera* viewCamera) override;
	void PrepareSceneContent(Camera* viewCamera) override;
	void DrawPreparedSceneContent(Camera* viewCamera) override;
	void DrawForegroundEffects() override;
	void DrawForegroundEffectsWithCamera(Camera* viewCamera) override;
	bool HasScreenOverlay() const override;
	void DrawScreenOverlay(uint32_t width, uint32_t height) override;
	void DrawOffscreenViews() override;
	void SetRenderAspectRatio(float aspectRatio) override;
	void DrawShadow() override;
	void CollectShadowCasters(std::vector<Object3d*>& shadowCasters) override;
	void RenderShadowCasters(
		const std::vector<Object3d*>& shadowCasters
	) override;
	void SetDeferForegroundEffects(bool defer) override {
		effectRenderSystem_.SetDeferForegroundEffects(defer);
	}
	bool TryGetRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const override;

private:
	void DrawSceneView(Camera* viewCamera, uint64_t skipEntityId = 0);
	void DrawPreparedSceneContentForView(
		Camera* viewCamera,
		uint64_t skipEntityId
	);
	/// <summary>
	/// タイトルのSTART決定後に退出演出を開始します。
	/// </summary>
	void BeginTitleStartTransition();

	/// <summary>
	/// タイトル退出演出を進め、遷移可能になったかを返します。
	/// </summary>
	bool UpdateTitleStartTransition(float deltaTime);

	/// <summary>
	/// タイトル退出演出の進行度を0から1で返します。
	/// </summary>
	float GetTitleStartTransitionProgress() const;

	/// <summary>
	/// タイトル退出演出の状態を初期化します。
	/// </summary>
	void ClearTitleStartTransition();
	bool ShouldHidePlayerModelForCamera(Camera* viewCamera) const;
	void ApplyRenderCamera(Camera* viewCamera);
	Camera* GetSceneViewCamera() const;

	Camera* camera_ = nullptr;
	Camera* debugCamera_ = nullptr;
	Player* player_ = nullptr;
	std::vector<SceneRuntimeObjectBinding> runtimeObjectBindings_;
	bool titleStartTransitionActive_ = false; // タイトルSTART後の退出演出中か。
	float titleStartTransitionElapsedSeconds_ = 0.0f; // タイトル退出演出の経過時間。

	SceneAgentSystem agentSystem_;
	SceneAudioSystem audioSystem_;
	SceneAttackRunnerSystem attackRunnerSystem_;
	SceneAttachmentSystem attachmentSystem_;
	SceneCameraSystem cameraSystem_;
	SceneCombatSystem combatSystem_;
	SceneDebugSystem debugSystem_;
	SceneEnemySystem enemySystem_;
	SceneEnemySpawnerSystem enemySpawnerSystem_;
	SceneGameFlowSystem gameFlowSystem_;
	SceneFishingScoreAttackSystem fishingScoreAttackSystem_;
	SceneEventSystem eventSystem_;
	SceneHitReactionSystem hitReactionSystem_;
	SceneHitStopSystem hitStopSystem_;
	SceneEffectRenderSystem effectRenderSystem_;
	SceneEnvironmentSystem environmentSystem_;
	SceneLightingSystem lightingSystem_;
	SceneMiniMapSystem miniMapSystem_;
	SceneMonitorSystem monitorSystem_;
	SceneObjectSystem objectSystem_;
	SceneOptionMenuSystem optionMenuSystem_;
	SceneTransitionSystem transitionSystem_;
	SceneParticleSystem particleSystem_;
	ScenePauseSystem pauseSystem_;
	ScenePostProcessProfileSystem postProcessProfileSystem_;
	ScenePhysicsSystem physicsSystem_;
	ScenePrefabAnimationSystem prefabAnimationSystem_;
	SceneProjectileSystem projectileSystem_;
	SceneRuntimeEffectSystem runtimeEffectSystem_;
	SceneStatSystem statSystem_;
	SceneStateMachineSystem stateMachineSystem_;
	SceneTextMotionSystem textMotionSystem_;
	SceneTextRenderSystem textRenderSystem_;
	SceneTitleBoatMotionSystem titleBoatMotionSystem_;
	SceneTitleMenuSystem titleMenuSystem_;
};

