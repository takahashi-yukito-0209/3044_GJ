// 役割: ScenePauseSystemのrequest同期とEntity ProcessPolicy解決を実装する。
#include "ScenePauseSystem.h"

#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace {
	constexpr size_t ToIndex(ScenePauseDomain domain) {
		return static_cast<size_t>(domain);
	}

	const char* ToDomainName(ScenePauseDomain domain) {
		switch (domain) {
		case ScenePauseDomain::Gameplay: return "Gameplay";
		case ScenePauseDomain::Physics: return "Physics";
		case ScenePauseDomain::GameplayInput: return "GameplayInput";
		case ScenePauseDomain::WorldAnimation: return "WorldAnimation";
		case ScenePauseDomain::WorldEffects: return "WorldEffects";
		case ScenePauseDomain::Audio: return "Audio";
		}
		return "";
	}

	bool SameKey(const ScenePauseRequest& left, const ScenePauseRequest& right) {
		return left.controllerEntityId == right.controllerEntityId &&
			left.profileId == right.profileId &&
			left.requestId == right.requestId;
	}

	bool HasDomain(const ScenePauseProfile& profile, const char* domainName) {
		return std::find(
			profile.pausedDomains.begin(),
			profile.pausedDomains.end(),
			domainName
		) != profile.pausedDomains.end();
	}
}

void ScenePauseSystem::BeginFrame(const SceneDocument& document) {
	uint64_t controllerEntityId = 0;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity) ||
			!SceneEntityQuery::FindEnabledComponent(entity, "PauseController")) {
			continue;
		}
		if (controllerEntityId != 0) {
			Clear();
			return;
		}
		controllerEntityId = entity.id;
	}
	if (controllerEntityId == 0) {
		Clear();
		return;
	}
	activeControllerEntityId_ = controllerEntityId;
	activeRequests_.erase(
		std::remove_if(
			activeRequests_.begin(),
			activeRequests_.end(),
			[this, &document](const ScenePauseRequest& request) {
				return !IsValidRequest(document, request);
			}
		),
		activeRequests_.end()
	);
	UpdateSnapshot(document);
}

void ScenePauseSystem::CommitRequests(
	const SceneDocument& document,
	const std::vector<ScenePauseRequest>& requests
) {
	if (activeControllerEntityId_ == 0) {
		return;
	}
	std::vector<ScenePauseRequest> working = activeRequests_;
	for (const ScenePauseRequest& request : requests) {
		if (!IsValidRequest(document, request)) {
			continue;
		}
		const auto found = std::find_if(
			working.begin(),
			working.end(),
			[&request](const ScenePauseRequest& active) {
				return SameKey(active, request);
			}
		);
		if (request.operation == ScenePauseOperation::Pause) {
			if (found == working.end()) {
				working.push_back(request);
			}
		} else if (request.operation == ScenePauseOperation::Resume) {
			if (found != working.end()) {
				working.erase(found);
			}
		} else if (found == working.end()) {
			working.push_back(request);
		} else {
			working.erase(found);
		}
	}
	activeRequests_ = std::move(working);
}

bool ScenePauseSystem::IsDomainPaused(ScenePauseDomain domain) const {
	return pausedDomains_[ToIndex(domain)];
}

bool ScenePauseSystem::IsPauseActive(
	uint64_t controllerEntityId,
	const std::string& profileId,
	const std::string& requestId
) const {
	if (controllerEntityId == 0 ||
		controllerEntityId != activeControllerEntityId_) {
		return false;
	}
	return std::any_of(
		activeRequests_.begin(),
		activeRequests_.end(),
		[&profileId, &requestId](const ScenePauseRequest& request) {
			return (profileId.empty() || request.profileId == profileId) &&
				(requestId.empty() || request.requestId == requestId);
		}
	);
}

bool ScenePauseSystem::ShouldProcess(
	const SceneDocument& document,
	uint64_t entityId,
	ScenePauseDomain domain
) const {
	const SceneEntity* entity = document.FindEntity(entityId);
	if (!entity || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)) {
		return false;
	}
	std::unordered_set<uint64_t> visited;
	const SceneEntity* current = entity;
	std::string mode = "Inherit";
	while (current) {
		if (!visited.insert(current->id).second) {
			mode = "Pausable";
			break;
		}
		if (const SceneComponent* policy =
			SceneEntityQuery::FindEnabledComponent(*current, "ProcessPolicy")) {
			if (policy->processMode != "Inherit") {
				mode = policy->processMode;
				break;
			}
		}
		current = current->parentId == 0
			? nullptr
			: document.FindEntity(current->parentId);
	}
	if (
		mode != "Inherit" && mode != "Pausable" &&
		mode != "WhenPaused" && mode != "Always" &&
		mode != "Disabled"
	) {
		mode = "Pausable";
	}
	if (mode == "Disabled") {
		return false;
	}
	if (IsDomainPaused(domain)) {
		return mode == "WhenPaused" || mode == "Always";
	}
	return mode == "Pausable" || mode == "Always" || mode == "Inherit";
}

void ScenePauseSystem::Clear() {
	activeControllerEntityId_ = 0;
	activeRequests_.clear();
	std::fill(std::begin(pausedDomains_), std::end(pausedDomains_), false);
}

bool ScenePauseSystem::IsValidRequest(
	const SceneDocument& document,
	const ScenePauseRequest& request
) const {
	if (request.controllerEntityId == 0 || request.profileId.empty() ||
		request.requestId.empty() ||
		request.controllerEntityId != activeControllerEntityId_) {
		return false;
	}
	const SceneEntity* controller = document.FindEntity(request.controllerEntityId);
	const SceneComponent* component = controller &&
		SceneEntityQuery::IsEntityActiveInHierarchy(document, *controller)
		? SceneEntityQuery::FindEnabledComponent(*controller, "PauseController")
		: nullptr;
	return component && std::any_of(
		component->pauseProfiles.begin(),
		component->pauseProfiles.end(),
		[&request](const ScenePauseProfile& profile) {
			return profile.id == request.profileId;
		}
	);
}

void ScenePauseSystem::UpdateSnapshot(const SceneDocument& document) {
	std::fill(std::begin(pausedDomains_), std::end(pausedDomains_), false);
	const SceneEntity* controller = document.FindEntity(activeControllerEntityId_);
	const SceneComponent* component = controller
		? SceneEntityQuery::FindEnabledComponent(*controller, "PauseController")
		: nullptr;
	if (!component) {
		return;
	}
	for (const ScenePauseRequest& request : activeRequests_) {
		const auto profile = std::find_if(
			component->pauseProfiles.begin(),
			component->pauseProfiles.end(),
			[&request](const ScenePauseProfile& candidate) {
				return candidate.id == request.profileId;
			}
		);
		if (profile == component->pauseProfiles.end()) {
			continue;
		}
		for (size_t index = 0; index < std::size(pausedDomains_); ++index) {
			const ScenePauseDomain domain = static_cast<ScenePauseDomain>(index);
			pausedDomains_[index] |= HasDomain(*profile, ToDomainName(domain));
		}
	}
}
