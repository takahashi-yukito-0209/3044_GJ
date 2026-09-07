// 役割: SceneInstance集合とSceneFactoryを管理し、Active Sceneの遷移を実行する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SceneInstance.h"

class AbstractSceneFactory;
class BaseScene;
class SceneCatalog;
class SceneDocument;
struct SceneEntity;
struct ScenePostProcessSettings;
class SceneExecutionContext;

enum class SceneLoadMode {
	Single,
	Additive
};

// SceneInstanceを所有し、実行ContextのDocumentと同じタイミングで予約Sceneへ切り替える。
class SceneManager
{
public:
	~SceneManager();

	void Update(float deltaTime);
	void UpdatePaused();

	void Draw();
	void DrawForegroundEffects();
	bool HasScreenOverlay() const;
	void DrawScreenOverlay(uint32_t width, uint32_t height);
	void DrawShadow();
	void DrawOffscreenViews();
	void SetDeferForegroundEffects(bool defer);

	// ChangeSceneは予約のみ行い、次のUpdate先頭でDocument読込と初期化を確定する。
	void ChangeScene(const std::string& sceneId);
	// Runtime中のScene遷移を予約する。演出を使わない場合は次のUpdateで即時切り替える。
	// ChangeSceneは起動処理・Editor操作向けの通常切り替えとして残す。
	void RequestSceneTransition(const std::string& sceneId, bool useEffect = true);
	bool IsSceneTransitioning() const;
	float GetSceneTransitionFadeAmount() const;
	SceneInstanceId LoadScene(
		const std::string& sceneId,
		SceneLoadMode loadMode,
		const std::string& instanceKey = {}
	);
	SceneInstanceId LoadSceneAdditive(
		const std::string& sceneId,
		const std::string& instanceKey = {}
	) {
		return LoadScene(sceneId, SceneLoadMode::Additive, instanceKey);
	}
	// Unloadは安全のため予約し、次のUpdate先頭で実行する。
	bool UnloadScene(SceneInstanceId instanceId);
	bool SetActiveScene(SceneInstanceId instanceId);
	// Persistent InstanceはSingleロードでも破棄しない。明示Unloadは可能。
	bool SetScenePersistent(SceneInstanceId instanceId, bool persistent);
	void ReloadCurrentScene();
	void SetSceneFactory(AbstractSceneFactory* sceneFactory) {
		sceneFactory_ = sceneFactory;
	}
	void SetSceneCatalog(SceneCatalog* sceneCatalog) {
		sceneCatalog_ = sceneCatalog;
	}
	const std::string& GetCurrentSceneName() const {
		return currentSceneName_;
	}
	const std::string& GetCurrentSceneId() const {
		return currentSceneName_;
	}
	void SetExecutionContext(SceneExecutionContext* executionContext) {
		executionContext_ = executionContext;
	}
	SceneExecutionContext* GetExecutionContext() const {
		return executionContext_;
	}
	SceneDocument* GetActiveSceneDocument();
	const SceneDocument* GetActiveSceneDocument() const;
	bool TryGetActiveRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const;
	SceneInstanceId GetActiveSceneInstanceId() const {
		return activeSceneInstanceId_;
	}
	const SceneInstance* GetActiveSceneInstance() const;
	const SceneInstance* FindSceneInstance(SceneInstanceId instanceId) const;
	const SceneInstance* FindSceneInstanceByKey(
		const std::string& instanceKey
	) const;
	std::vector<const SceneInstance*> GetLoadedSceneInstances() const;
	SceneEntity* ResolveEntity(const SceneEntityHandle& handle);
	const SceneEntity* ResolveEntity(const SceneEntityHandle& handle) const;
	SceneEntityHandle ResolveEntityReference(
		SceneInstanceId sourceInstanceId,
		const SceneEntityReference& reference
	) const;

private:
	enum class SceneTransitionPhase {
		None,
		FadeOut,
		FadeIn
	};

	void AdvanceSceneTransition(float deltaTime);
	void ActivatePendingScene();
	void DiscardPendingScene();
	void ProcessPendingSceneUnloads();
	BaseScene* GetActiveScene() const;
	bool UnloadNonPersistentSceneInstances(
		SceneInstanceId forcedUnloadInstanceId = kInvalidSceneInstanceId,
		bool forceUnloadAll = false,
		bool prepareForSceneTransition = false
	);
	void ReleaseSceneParticlesIfUnused(const std::string& sceneId);
	void UnloadAllSceneInstances();

	std::vector<std::unique_ptr<SceneInstance>> sceneInstances_;
	std::unique_ptr<SceneInstance> pendingSceneInstance_;
	SceneLoadMode pendingSceneLoadMode_ = SceneLoadMode::Single;
	SceneInstanceId pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	bool pendingForceUnloadAllSceneInstances_ = false;
	std::vector<SceneInstanceId> pendingUnloadSceneInstanceIds_;
	SceneInstanceId activeSceneInstanceId_ = kInvalidSceneInstanceId;
	SceneInstanceId nextSceneInstanceId_ = 1;
	AbstractSceneFactory* sceneFactory_ = nullptr;
	SceneCatalog* sceneCatalog_ = nullptr;
	std::string currentSceneName_;
	SceneExecutionContext* executionContext_ = nullptr;
	SceneTransitionPhase sceneTransitionPhase_ = SceneTransitionPhase::None;
	std::string sceneTransitionTargetId_;
	float sceneTransitionElapsedSeconds_ = 0.0f;
	float sceneTransitionFadeAmount_ = 0.0f;
	static constexpr float kSceneTransitionFadeSeconds = 0.5f;
};

