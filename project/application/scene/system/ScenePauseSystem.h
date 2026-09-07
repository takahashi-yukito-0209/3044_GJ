// 役割: Scene内Pause requestとframe単位のProcessPolicy判定を所有する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;

enum class ScenePauseDomain {
	Gameplay,
	Physics,
	GameplayInput,
	WorldAnimation,
	WorldEffects,
	Audio
};

enum class ScenePauseOperation {
	Pause,
	Resume,
	Toggle
};

struct ScenePauseRequest {
	uint64_t controllerEntityId = 0;
	std::string profileId;
	std::string requestId;
	ScenePauseOperation operation = ScenePauseOperation::Pause;
};

class ScenePauseSystem {
public:
	void BeginFrame(const SceneDocument& document);
	void CommitRequests(
		const SceneDocument& document,
		const std::vector<ScenePauseRequest>& requests
	);
	bool IsDomainPaused(ScenePauseDomain domain) const;
	bool IsPauseActive(
		uint64_t controllerEntityId,
		const std::string& profileId,
		const std::string& requestId
	) const;
	bool ShouldProcess(
		const SceneDocument& document,
		uint64_t entityId,
		ScenePauseDomain domain
	) const;
	void Clear();

private:
	bool IsValidRequest(
		const SceneDocument& document,
		const ScenePauseRequest& request
	) const;
	void UpdateSnapshot(const SceneDocument& document);

	uint64_t activeControllerEntityId_ = 0;
	std::vector<ScenePauseRequest> activeRequests_;
	bool pausedDomains_[6]{};
};
