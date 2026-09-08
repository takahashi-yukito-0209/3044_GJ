// 役割: すべてのシーンに共通する更新と描画のライフサイクルを定義する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SceneEntityReference.h"

class Camera;
class Object3d;
class SceneDocument;
class SceneInstance;
class SceneManager;
struct ScenePostProcessSettings;

// SceneInstanceを介してSceneManagerが所有するSceneの共通契約。派生側は各Passの順序を変えない。
class BaseScene
{
public:
	virtual ~BaseScene() = default;

	// InitializeとFinalizeはScene切り替えごとに一度ずつ対で呼ばれる。
	virtual void Initialize() = 0;
	virtual void Finalize() = 0;
	// Single Scene切替で旧Sceneを破棄する直前だけ呼ばれる任意hook。
	virtual void PrepareForSceneTransition() {}

	virtual void Update(float deltaTime) = 0;
	// Pause中はSimulationを進めず、Debug Cameraなど必要な表示状態だけを更新する。
	virtual void UpdatePaused() {}

	virtual void Draw() = 0;
	// Additive描画ではActive SceneのCameraを全Instanceへ共有する。
	virtual Camera* GetRenderCamera() const { return nullptr; }
	virtual void DrawWithCamera(Camera* viewCamera) {
		(void)viewCamera;
		Draw();
	}
	// Main ViewではActive SceneがEnvironmentとLightingを提供し、全Sceneが内容を描画する。
	virtual void DrawEnvironment(Camera* viewCamera) { (void)viewCamera; }
	virtual void BindLighting() {}
	virtual void DrawSceneContent(Camera* viewCamera) {
		DrawWithCamera(viewCamera);
	}
	virtual void PrepareSceneContent(Camera* viewCamera) { (void)viewCamera; }
	virtual void DrawPreparedSceneContent(Camera* viewCamera) {
		DrawSceneContent(viewCamera);
	}
	// 以下は対応する機能を持つSceneだけが上書きする任意Pass。
	virtual void DrawForegroundEffects() {}
	virtual void DrawForegroundEffectsWithCamera(Camera* viewCamera) {
		(void)viewCamera;
		DrawForegroundEffects();
	}
	virtual bool HasScreenOverlay() const { return false; }
	virtual void DrawScreenOverlay(uint32_t width, uint32_t height) {
		(void)width;
		(void)height;
	}
	virtual void DrawShadow() {}
	virtual void CollectShadowCasters(std::vector<Object3d*>& shadowCasters) {
		(void)shadowCasters;
	}
	virtual void RenderShadowCasters(
		const std::vector<Object3d*>& shadowCasters
	) {
		(void)shadowCasters;
		DrawShadow();
	}
	virtual void DrawOffscreenViews() {}
	// 描画先の比率が決まった時点でCameraへ反映する。
	virtual void SetRenderAspectRatio(float aspectRatio) { (void)aspectRatio; }
	virtual void SetDeferForegroundEffects(bool defer) { (void)defer; }
	// Runtime-only Profileなど、Scene更新後に有効になる描画設定を返す任意口。
	// 非Runtime Sceneはfalseのままで既存のDocument Baselineを使う。
	virtual bool TryGetRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const {
		(void)settings;
		(void)generation;
		return false;
	}

	virtual void SetSceneManager(SceneManager* sceneManager) { sceneManager_ = sceneManager; }
	virtual void SetSceneInstance(SceneInstance* sceneInstance) {
		sceneInstance_ = sceneInstance;
	}


protected:
	SceneDocument* GetSceneDocument() const;
	const std::string& GetSceneAssetId() const;
	SceneInstanceId GetSceneInstanceId() const;

	SceneManager* sceneManager_ = nullptr;
	SceneInstance* sceneInstance_ = nullptr;
};
