// 役割: Scene固有パーティクルの初期化、更新、Editor操作、破棄を実装する。
#include "SceneParticleSystem.h"

#include "../../../engine/base/ImGuiManager.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/particle/ParticleEmitter.h"
#include "../../../engine/particle/ParticleManager.h"

namespace {
	constexpr const char* kPrimaryEffectPath =
		"resources/particles/core_burst.json";
	constexpr const char* kSecondaryEffectPath =
		"resources/particles/ring_burst.json";
}

SceneParticleSystem::~SceneParticleSystem() {
	Finalize();
}

void SceneParticleSystem::Initialize(Camera* camera) {
	Finalize();
	ParticleManager* particleManager = ParticleManager::GetInstance();
	particleManager->SetCamera(camera);
	particleManager->SetGpuParticleEnabled(true);

	if (ParticleEffectResource::Load(kPrimaryEffectPath, primaryEffect_)) {
		primaryEmitter_ = ParticleEffectResource::CreateEmitter(primaryEffect_);
		editingEffect_ = primaryEffect_;
		particleEffectEditor_.Initialize(editingEffect_, kPrimaryEffectPath);
		editorPreviewEmitter_ =
			ParticleEffectResource::CreateEmitter(editingEffect_);
	}
	if (ParticleEffectResource::Load(kSecondaryEffectPath, secondaryEffect_)) {
		secondaryEmitter_ =
			ParticleEffectResource::CreateEmitter(secondaryEffect_);
	}
}

void SceneParticleSystem::Update(
	const std::string& sceneId,
	bool editing,
	bool advanceWorldEffects
) {
	ParticleManager* particleManager = ParticleManager::GetInstance();
	if (!activeSceneId_.empty() && activeSceneId_ != sceneId) {
		particleManager->SetSceneParticleSimulationPaused(activeSceneId_, false);
	}
	activeSceneId_ = sceneId;
	const std::string groupPauseOwnerKey = "scene-particle:" + sceneId;
	// Editor previewはRuntimeのPause対象ではない。
	particleManager->SetSceneParticleSimulationPaused(sceneId, !editing && !advanceWorldEffects);
	if (!editing) {
		if (!primaryEffect_.name.empty()) {
			particleManager->SetParticleGroupSimulationPaused(
				primaryEffect_.name, groupPauseOwnerKey, !advanceWorldEffects
			);
		}
		if (!secondaryEffect_.name.empty()) {
			particleManager->SetParticleGroupSimulationPaused(
				secondaryEffect_.name, groupPauseOwnerKey, !advanceWorldEffects
			);
		}
	}
	// Editor PreviewとRuntime Emitterを同じManager更新へ集約し、生成順を揃える。
	std::string particlePath;
	ImGuiManager* imguiManager = ImGuiManager::GetInstance();
	if (imguiManager && imguiManager->GetRequestLoadParticle(particlePath)) {
		ParticleEffectDesc loadedEffect{};
		if (ParticleEffectResource::Load(particlePath, loadedEffect)) {
			editingEffect_ = loadedEffect;
			particleEffectEditor_.Initialize(editingEffect_, particlePath);
			delete editorPreviewEmitter_;
			editorPreviewEmitter_ =
				ParticleEffectResource::CreateEmitter(editingEffect_);
		}
	}

	Input* input = Input::GetInstance();
	if (editing && input && input->TriggerKey(DIK_SPACE)) {
		particleManager->CycleSceneParticleAssets(sceneId);
	}
	if (editorPreviewEmitter_) {
		editorPreviewEmitter_->Update();
	}
	if (primaryEmitter_ && (editing || advanceWorldEffects)) {
		primaryEmitter_->Update();
	}
	if (secondaryEmitter_ && (editing || advanceWorldEffects)) {
		secondaryEmitter_->Update();
	}
	if (editing || advanceWorldEffects) {
		particleManager->UpdateSceneParticles(sceneId);
	}
}

void SceneParticleSystem::DrawEditor(const std::string& sceneId) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	particleEffectEditor_.DrawImGui(
		editingEffect_,
		editorPreviewEmitter_,
		"Particle Effect Editor"
	);
	ParticleManager::GetInstance()->DrawSceneParticleImGui(
		sceneId,
		"Scene Particles"
	);

	if (editingEffect_.name == primaryEffect_.name && primaryEmitter_) {
		primaryEffect_ = editingEffect_;
		ParticleEffectResource::PrepareParticleGroup(primaryEffect_, false);
		ParticleEffectResource::ApplyToEmitter(
			*primaryEmitter_,
			primaryEffect_
		);
	} else if (
		editingEffect_.name == secondaryEffect_.name && secondaryEmitter_
	) {
		secondaryEffect_ = editingEffect_;
		ParticleEffectResource::PrepareParticleGroup(secondaryEffect_, false);
		ParticleEffectResource::ApplyToEmitter(
			*secondaryEmitter_,
			secondaryEffect_
		);
	}
#else
	(void)sceneId;
#endif
}

void SceneParticleSystem::Finalize() {
	if (!activeSceneId_.empty()) {
		const std::string groupPauseOwnerKey = "scene-particle:" + activeSceneId_;
		ParticleManager::GetInstance()->SetSceneParticleSimulationPaused(activeSceneId_, false);
		if (!primaryEffect_.name.empty()) {
			ParticleManager::GetInstance()->SetParticleGroupSimulationPaused(
				primaryEffect_.name, groupPauseOwnerKey, false
			);
		}
		if (!secondaryEffect_.name.empty()) {
			ParticleManager::GetInstance()->SetParticleGroupSimulationPaused(
				secondaryEffect_.name, groupPauseOwnerKey, false
			);
		}
	}
	activeSceneId_.clear();
	delete editorPreviewEmitter_;
	editorPreviewEmitter_ = nullptr;
	delete primaryEmitter_;
	primaryEmitter_ = nullptr;
	delete secondaryEmitter_;
	secondaryEmitter_ = nullptr;
}
