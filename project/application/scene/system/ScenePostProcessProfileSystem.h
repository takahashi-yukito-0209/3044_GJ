// 役割: Eventから選ばれたPost Process ProfileのRuntime Copyだけを保持する。
#pragma once

#include <cstdint>

#include "SceneEventSystem.h"
#include "../../../engine/scene/SceneSettings.h"

class SceneDocument;
struct SceneComponent;
struct SceneEntity;
struct ScenePostProcessProfile;

class ScenePostProcessProfileSystem {
public:
	void Reset(const SceneDocument* document = nullptr);
	void Sync(const SceneDocument& document);
	void ApplyEventResult(
		const SceneDocument& document,
		const SceneEventResult& result
	);
	void Update(float deltaTime);
	const ScenePostProcessSettings& GetEffectiveSettings() const {
		return effectiveSettings_;
	}
	uint64_t GetGeneration() const { return generation_; }
	uint64_t GetActiveManagerEntityId() const { return activeManagerEntityId_; }
	const std::string& GetActiveProfileLabel() const {
		return activeProfileLabel_;
	}
	uint64_t GetStatusTextEntityId() const { return statusTextEntityId_; }
	const std::string& GetStatusTextEntityName() const {
		return statusTextEntityName_;
	}
	const std::string& GetStatusTextPrefix() const {
		return statusTextPrefix_;
	}

private:
	void ApplyBaseline(const SceneDocument& document);
	void ApplyStatusBinding(const SceneComponent* component);
	void ApplyProfile(
		const SceneEntity& manager,
		const SceneComponent& component,
		const ScenePostProcessProfile& profile
	);
	void ClearAutomation();

	ScenePostProcessSettings effectiveSettings_{};
	uint64_t activeManagerEntityId_ = 0;
	std::string activeProfileId_;
	std::string activeProfileLabel_ = "None";
	uint64_t statusTextEntityId_ = 0;
	std::string statusTextEntityName_;
	std::string statusTextPrefix_ = "PostEffect: ";
	bool automationActive_ = false;
	float automationStartValue_ = 0.0f;
	float automationEndValue_ = 1.0f;
	float automationDuration_ = 1.0f;
	float automationElapsed_ = 0.0f;
	uint64_t generation_ = 1;
};
