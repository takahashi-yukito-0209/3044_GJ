// 役割: シーン生成、更新、描画、遷移の実行を実装する。
#include "SceneManager.h"

#include <cassert>
#include <algorithm>
#include <memory>

#include "AbstractSceneFactory.h"
#include "BaseScene.h"
#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneExecutionContext.h"
#include "../particle/ParticleManager.h"
#include "../utility/Logger.h"

void SceneManager::ChangeScene(const std::string& sceneId) {
	LoadScene(sceneId, SceneLoadMode::Single);
}

void SceneManager::RequestSceneTransition(const std::string& sceneId, bool useEffect)
{
	if (
		sceneId.empty() ||
		sceneTransitionPhase_ != SceneTransitionPhase::None ||
		pendingSceneInstance_
	) {
		return;
	}
	if (!useEffect) {
		LoadScene(sceneId, SceneLoadMode::Single);
		return;
	}
	sceneTransitionTargetId_ = sceneId;
	sceneTransitionElapsedSeconds_ = 0.0f;
	sceneTransitionFadeAmount_ = 0.0f;
	sceneTransitionPhase_ = SceneTransitionPhase::FadeOut;
}

bool SceneManager::IsSceneTransitioning() const
{
	return sceneTransitionPhase_ != SceneTransitionPhase::None;
}

float SceneManager::GetSceneTransitionFadeAmount() const
{
	return sceneTransitionFadeAmount_;
}

SceneInstanceId SceneManager::LoadScene(
	const std::string& sceneId,
	SceneLoadMode loadMode,
	const std::string& instanceKey
) {
	assert(sceneFactory_);
	if (!sceneFactory_ || pendingSceneInstance_) {
		return kInvalidSceneInstanceId;
	}

	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(sceneId)
		: nullptr;
	if (loadMode == SceneLoadMode::Additive) {
		if (!descriptor) {
			Logger::Log("Additive Scene is not registered: " + sceneId + "\n");
			return kInvalidSceneInstanceId;
		}
		if (executionContext_ && executionContext_->IsEditing()) {
			Logger::Log("Additive Scene loading is available in Play/Runtime mode only\n");
			return kInvalidSceneInstanceId;
		}
	}
	const std::string& runtimeProfile = descriptor
		? descriptor->runtimeProfile
		: sceneId;
	BaseScene* newScene = sceneFactory_->CreateScene(runtimeProfile);
	if (!newScene) {
		Logger::Log("Scene runtime profile is not registered: " + runtimeProfile + "\n");
		return kInvalidSceneInstanceId;
	}

	if (descriptor && executionContext_) {
		if (executionContext_->IsEditing()) {
			if (executionContext_->GetActiveSceneId() != descriptor->id) {
				Logger::Log(
					"Edit SceneDocument must be opened before changing Scene: " +
					descriptor->id + "\n"
				);
				delete newScene;
				return kInvalidSceneInstanceId;
			}
		}
	}

	// Documentはまだ切り替えず、次のUpdate先頭でSceneInstanceと同時に確定する。
	const SceneInstanceId instanceId = nextSceneInstanceId_++;
	if (nextSceneInstanceId_ == kInvalidSceneInstanceId) {
		++nextSceneInstanceId_;
	}
	std::string resolvedInstanceKey = instanceKey.empty()
		? (descriptor ? descriptor->id : sceneId)
		: instanceKey;
	if (const SceneInstance* existing = FindSceneInstanceByKey(
		resolvedInstanceKey
	)) {
		if (!instanceKey.empty()) {
			Logger::Log(
				"Scene instance key is already loaded: " +
				resolvedInstanceKey + "\n"
			);
			delete newScene;
			return kInvalidSceneInstanceId;
		}
		(void)existing;
		resolvedInstanceKey += "#" + std::to_string(instanceId);
		while (FindSceneInstanceByKey(resolvedInstanceKey)) {
			resolvedInstanceKey += "#";
		}
	}
	pendingSceneInstance_.reset(new SceneInstance(
		instanceId,
		descriptor ? descriptor->id : sceneId,
		std::move(resolvedInstanceKey),
		std::unique_ptr<BaseScene>(newScene)
	));
	pendingSceneLoadMode_ = loadMode;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	return instanceId;
}

bool SceneManager::UnloadScene(SceneInstanceId instanceId) {
	if (instanceId == kInvalidSceneInstanceId) {
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		DiscardPendingScene();
		return true;
	}
	if (!FindSceneInstance(instanceId)) {
		return false;
	}
	if (
		std::find(
			pendingUnloadSceneInstanceIds_.begin(),
			pendingUnloadSceneInstanceIds_.end(),
			instanceId
		) == pendingUnloadSceneInstanceIds_.end()
	) {
		pendingUnloadSceneInstanceIds_.push_back(instanceId);
	}
	return true;
}

bool SceneManager::SetActiveScene(SceneInstanceId instanceId) {
	const SceneInstance* instance = FindSceneInstance(instanceId);
	if (!instance) {
		return false;
	}
	activeSceneInstanceId_ = instanceId;
	currentSceneName_ = instance->GetSceneId();
	return true;
}

bool SceneManager::SetScenePersistent(
	SceneInstanceId instanceId,
	bool persistent
) {
	if (
		persistent &&
		executionContext_ &&
		executionContext_->IsEditing()
	) {
		Logger::Log("Persistent Scene is available in Play/Runtime mode only\n");
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		pendingSceneInstance_->SetPersistent(persistent);
		return true;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			instance->SetPersistent(persistent);
			return true;
		}
	}
	return false;
}

void SceneManager::ReloadCurrentScene() {
	if (pendingSceneInstance_) {
		if (currentSceneName_.empty()) {
			return;
		}
		DiscardPendingScene();
	}
	const std::string targetSceneId =
		executionContext_ && executionContext_->IsEditing()
			? executionContext_->GetActiveSceneId()
			: currentSceneName_;
	if (targetSceneId.empty()) {
		return;
	}

	if (
		LoadScene(targetSceneId, SceneLoadMode::Single) !=
		kInvalidSceneInstanceId
	) {
		// ReloadだけはPersistent指定に関係なく現在Instanceを置き換える。
		pendingReloadSceneInstanceId_ = activeSceneInstanceId_;
		// Play終了後のEdit ModeにはRuntimeで追加したPersistent Instanceを残さない。
		pendingForceUnloadAllSceneInstances_ =
			executionContext_ && executionContext_->IsEditing();
	}
}

SceneDocument* SceneManager::GetActiveSceneDocument() {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == activeSceneInstanceId_) {
			if (SceneDocument* document = instance->GetDocument()) {
				return document;
			}
			break;
		}
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneDocument* SceneManager::GetActiveSceneDocument() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	if (instance && instance->GetDocument()) {
		return instance->GetDocument();
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneInstance* SceneManager::GetActiveSceneInstance() const {
	return FindSceneInstance(activeSceneInstanceId_);
}

const SceneInstance* SceneManager::FindSceneInstance(SceneInstanceId instanceId) const {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			return instance.get();
		}
	}
	return nullptr;
}

const SceneInstance* SceneManager::FindSceneInstanceByKey(
	const std::string& instanceKey
) const {
	if (instanceKey.empty()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetInstanceKey() == instanceKey) {
			return instance.get();
		}
	}
	return nullptr;
}

std::vector<const SceneInstance*> SceneManager::GetLoadedSceneInstances() const {
	std::vector<const SceneInstance*> result;
	result.reserve(sceneInstances_.size());
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		result.push_back(instance.get());
	}
	return result;
}

SceneEntity* SceneManager::ResolveEntity(const SceneEntityHandle& handle) {
	if (!handle.IsValid()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() != handle.sceneInstanceId) {
			continue;
		}
		SceneDocument* document = instance->GetDocument();
		return document ? document->FindEntity(handle.entityId) : nullptr;
	}
	return nullptr;
}

const SceneEntity* SceneManager::ResolveEntity(
	const SceneEntityHandle& handle
) const {
	if (!handle.IsValid()) {
		return nullptr;
	}
	const SceneInstance* instance = FindSceneInstance(
		handle.sceneInstanceId
	);
	const SceneDocument* document = instance
		? instance->GetDocument()
		: nullptr;
	return document ? document->FindEntity(handle.entityId) : nullptr;
}

SceneEntityHandle SceneManager::ResolveEntityReference(
	SceneInstanceId sourceInstanceId,
	const SceneEntityReference& reference
) const {
	if (reference.entityId == 0) {
		return {};
	}

	const SceneInstance* targetInstance = nullptr;
	if (reference.sceneId.empty()) {
		if (!reference.instanceKey.empty()) {
			return {};
		}
		targetInstance = FindSceneInstance(sourceInstanceId);
	} else if (!reference.instanceKey.empty()) {
		targetInstance = FindSceneInstanceByKey(reference.instanceKey);
		if (
			targetInstance &&
			targetInstance->GetSceneId() != reference.sceneId
		) {
			targetInstance = nullptr;
		}
	} else {
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (instance->GetSceneId() != reference.sceneId) {
				continue;
			}
			if (targetInstance) {
				// 同じSceneアセットが複数ある場合はinstanceKeyが必須。
				return {};
			}
			targetInstance = instance.get();
		}
	}
	if (!targetInstance) {
		return {};
	}

	const SceneEntityHandle handle{
		targetInstance->GetId(),
		reference.entityId
	};
	return ResolveEntity(handle) ? handle : SceneEntityHandle{};
}

BaseScene* SceneManager::GetActiveScene() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	return instance ? instance->GetScene() : nullptr;
}

bool SceneManager::TryGetActiveRuntimePostProcessSettings(
	ScenePostProcessSettings& settings,
	uint64_t& generation
) const {
	BaseScene* scene = GetActiveScene();
	return scene && scene->TryGetRuntimePostProcessSettings(settings, generation);
}

void SceneManager::Update(float deltaTime)
{
	AdvanceSceneTransition(deltaTime);
	ProcessPendingSceneUnloads();
	ActivatePendingScene();
	if (IsSceneTransitioning()) {
		return;
	}

	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->Update(deltaTime);
		}
	}
	if (!sceneInstances_.empty()) {
		if (BaseScene* activeScene = GetActiveScene()) {
			ParticleManager::GetInstance()->SetCamera(
				activeScene->GetRenderCamera()
			);
		}
		ParticleManager::GetInstance()->Update();
	}
}

void SceneManager::AdvanceSceneTransition(float deltaTime)
{
	if (sceneTransitionPhase_ == SceneTransitionPhase::None) {
		return;
	}

	const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
	sceneTransitionElapsedSeconds_ += safeDeltaTime;
	const float progress = (std::min)(
		sceneTransitionElapsedSeconds_ / kSceneTransitionFadeSeconds,
		1.0f
	);

	// イージングの適用
	const float easedProgress = progress * progress * (3.0f - 2.0f * progress);

	if (sceneTransitionPhase_ == SceneTransitionPhase::FadeOut) {
		sceneTransitionFadeAmount_ = easedProgress;
		if (progress < 1.0f) {
			return;
		}

		const std::string targetSceneId = sceneTransitionTargetId_;
		sceneTransitionTargetId_.clear();
		sceneTransitionElapsedSeconds_ = 0.0f;
		if (LoadScene(targetSceneId, SceneLoadMode::Single) ==
			kInvalidSceneInstanceId) {
			sceneTransitionFadeAmount_ = 0.0f;
			sceneTransitionPhase_ = SceneTransitionPhase::None;
			return;
		}
		sceneTransitionPhase_ = SceneTransitionPhase::FadeIn;
		return;
	}

	sceneTransitionFadeAmount_ = 1.0f - easedProgress;
	if (progress >= 1.0f) {
		sceneTransitionFadeAmount_ = 0.0f;
		sceneTransitionElapsedSeconds_ = 0.0f;
		sceneTransitionPhase_ = SceneTransitionPhase::None;
	}
}

void SceneManager::ActivatePendingScene()
{
	if (!pendingSceneInstance_) {
		return;
	}
	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(pendingSceneInstance_->GetSceneId())
		: nullptr;
	// Editor状態が予約後に変わっていても、異なるDocumentとの組み合わせを許可しない。
	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		descriptor &&
		executionContext_ &&
		executionContext_->IsEditing() &&
		executionContext_->GetActiveSceneId() != descriptor->id
	) {
		Logger::Log(
			"Pending Scene does not match the active Edit SceneDocument: " +
			descriptor->id + "\n"
		);
		DiscardPendingScene();
		return;
	}
	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		descriptor &&
		executionContext_ &&
		!executionContext_->IsEditing() &&
		executionContext_->GetActiveSceneId() != descriptor->id &&
		!executionContext_->LoadRuntimeScene(
			descriptor->id,
			descriptor->filePath
		)
	) {
		Logger::Log(
			"Runtime SceneDocument could not be loaded: " +
			descriptor->filePath + "\n"
		);
		DiscardPendingScene();
		return;
	}
	if (pendingSceneLoadMode_ == SceneLoadMode::Additive) {
		if (!descriptor) {
			DiscardPendingScene();
			return;
		}
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"Additive SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene();
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	} else if (executionContext_) {
		pendingSceneInstance_->BindDocument(
			&executionContext_->GetActiveDocument()
		);
	} else if (descriptor) {
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene();
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	}
	if (pendingSceneInstance_->IsPersistent()) {
		// Context所有Documentを次のSingleロードから隔離する。
		pendingSceneInstance_->SetPersistent(true);
	}

	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		!sceneInstances_.empty()
	) {
		// 新Documentの検証に成功してから、現在Sceneの所有物を破棄する。
		const bool persistentInstancesRemain =
			UnloadNonPersistentSceneInstances(
				pendingReloadSceneInstanceId_,
				pendingForceUnloadAllSceneInstances_,
				true
			);

		if (!persistentInstancesRemain) {
			// Persistent Sceneがなければ、従来どおり共有演出も完全に切り替える。
			ParticleManager::GetInstance()->ClearActiveParticles();
			ParticleManager::GetInstance()->ClearGpuParticles();
		}
	}

	const SceneLoadMode loadMode = pendingSceneLoadMode_;
	const SceneInstanceId instanceId = pendingSceneInstance_->GetId();
	sceneInstances_.push_back(std::move(pendingSceneInstance_));
	if (
		loadMode == SceneLoadMode::Single ||
		activeSceneInstanceId_ == kInvalidSceneInstanceId
	) {
		SetActiveScene(instanceId);
	}

	sceneInstances_.back()->Initialize(this);
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
}

void SceneManager::DiscardPendingScene()
{
	pendingSceneInstance_.reset();
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
}

void SceneManager::ProcessPendingSceneUnloads()
{
	for (SceneInstanceId instanceId : pendingUnloadSceneInstanceIds_) {
		auto instance = std::find_if(
			sceneInstances_.begin(),
			sceneInstances_.end(),
			[instanceId](const std::unique_ptr<SceneInstance>& candidate) {
				return candidate->GetId() == instanceId;
			}
		);
		if (instance == sceneInstances_.end()) {
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		(*instance)->Finalize();
		sceneInstances_.erase(instance);
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	pendingUnloadSceneInstanceIds_.clear();

	if (!FindSceneInstance(activeSceneInstanceId_)) {
		if (sceneInstances_.empty()) {
			activeSceneInstanceId_ = kInvalidSceneInstanceId;
			currentSceneName_.clear();
		} else {
			SetActiveScene(sceneInstances_.front()->GetId());
		}
	}
}

void SceneManager::UpdatePaused()
{
	// Pause中は遷移予約も進めず、Resume後の通常Updateで反映する。
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->UpdatePaused();
		}
	}
}

void SceneManager::Draw()
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = activeScene->GetRenderCamera();
	activeScene->DrawEnvironment(renderCamera);
	auto drawSceneContent = [activeScene, renderCamera](BaseScene* scene) {
		scene->PrepareSceneContent(renderCamera);
		// Prepareが設定したRoot SignatureへActive Lightingを再Bindする。
		activeScene->BindLighting();
		scene->DrawPreparedSceneContent(renderCamera);
	};
	drawSceneContent(activeScene);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				drawSceneContent(scene);
			}
		}
	}
}

void SceneManager::DrawForegroundEffects()
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = activeScene->GetRenderCamera();
	activeScene->DrawForegroundEffectsWithCamera(renderCamera);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawForegroundEffectsWithCamera(renderCamera);
			}
		}
	}
}

bool SceneManager::HasScreenOverlay() const
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (const BaseScene* scene = instance->GetScene()) {
			if (scene->HasScreenOverlay()) {
				return true;
			}
		}
	}
	return false;
}

void SceneManager::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	activeScene->DrawScreenOverlay(width, height);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawScreenOverlay(width, height);
			}
		}
	}
}

void SceneManager::DrawShadow()
{
	if (BaseScene* activeScene = GetActiveScene()) {
		std::vector<Object3d*> shadowCasters;
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (BaseScene* scene = instance->GetScene()) {
				scene->CollectShadowCasters(shadowCasters);
			}
		}
		// Shadow設定とMapはActive Scene、Casterは全Loaded Sceneから集約する。
		activeScene->RenderShadowCasters(shadowCasters);
	}
}

void SceneManager::SetDeferForegroundEffects(bool defer)
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->SetDeferForegroundEffects(defer);
		}
	}
}

void SceneManager::DrawOffscreenViews()
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->DrawOffscreenViews();
		}
	}
}

bool SceneManager::UnloadNonPersistentSceneInstances(
	SceneInstanceId forcedUnloadInstanceId,
	bool forceUnloadAll,
	bool prepareForSceneTransition
)
{
	std::vector<std::string> unloadedSceneIds;
	for (auto instance = sceneInstances_.begin();
		instance != sceneInstances_.end();) {
		if (
			!forceUnloadAll &&
			(*instance)->IsPersistent() &&
			(*instance)->GetId() != forcedUnloadInstanceId
		) {
			++instance;
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		if (prepareForSceneTransition) {
			if (BaseScene* scene = (*instance)->GetScene()) {
				scene->PrepareForSceneTransition();
			}
		}
		(*instance)->Finalize();
		instance = sceneInstances_.erase(instance);
		if (
			std::find(
				unloadedSceneIds.begin(),
				unloadedSceneIds.end(),
				sceneId
			) == unloadedSceneIds.end()
		) {
			unloadedSceneIds.push_back(sceneId);
		}
	}
	for (const std::string& sceneId : unloadedSceneIds) {
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	return !sceneInstances_.empty();
}

void SceneManager::ReleaseSceneParticlesIfUnused(const std::string& sceneId)
{
	const bool sameSceneStillLoaded = std::any_of(
		sceneInstances_.begin(),
		sceneInstances_.end(),
		[&sceneId](const std::unique_ptr<SceneInstance>& candidate) {
			return candidate->GetSceneId() == sceneId;
		}
	);
	if (!sameSceneStillLoaded) {
		ParticleManager::GetInstance()->ReleaseSceneParticles(sceneId);
	}
}

void SceneManager::UnloadAllSceneInstances()
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		instance->Finalize();
	}
	sceneInstances_.clear();
	pendingUnloadSceneInstanceIds_.clear();
	activeSceneInstanceId_ = kInvalidSceneInstanceId;
	currentSceneName_.clear();
}

SceneManager::~SceneManager()
{
	UnloadAllSceneInstances();
	DiscardPendingScene();
}
