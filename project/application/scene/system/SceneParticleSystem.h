// 役割: Runtime Sceneのパーティクル状態、更新、Editor操作を管理する。
#pragma once

#include <string>

#include "../../../engine/particle/ParticleEffectEditor.h"
#include "../../../engine/particle/ParticleEffectResource.h"

class Camera;
class ParticleEmitter;

// Scene固有Emitterを所有し、GPU粒子本体は共有ParticleManagerへ委譲する。
class SceneParticleSystem {
public:
	~SceneParticleSystem();

	void Initialize(Camera* camera);
	void Update(const std::string& sceneId, bool editing, bool advanceWorldEffects);
	void DrawEditor(const std::string& sceneId);
	void Finalize();

private:
	ParticleEffectDesc editingEffect_{};
	ParticleEmitter* editorPreviewEmitter_ = nullptr;
	ParticleEffectEditor particleEffectEditor_;
	ParticleEffectDesc primaryEffect_{};
	ParticleEmitter* primaryEmitter_ = nullptr;
	ParticleEffectDesc secondaryEffect_{};
	ParticleEmitter* secondaryEmitter_ = nullptr;
	std::string activeSceneId_;
};
