// 役割: PrefabAnimatorのClip再生状態とTransform Cross FadeをEntity単位で管理する。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../engine/math/Transform.h"

class SceneDocument;

class ScenePrefabAnimationSystem {
public:
	void Update(SceneDocument& document, float deltaTime,
		const std::function<bool(uint64_t)>& shouldProcess = {});
	bool Play(
		const SceneDocument& document,
		uint64_t entityId,
		const std::string& clipName,
		bool restart = true,
		float transitionDuration = 0.0f
	);
	void ResetEntity(uint64_t entityId);
	void Clear();

private:
	struct TransitionPose {
		uint64_t entityId = 0;
		QuaternionTransform transform{};
		uint8_t propertyMask = 0;
	};

	struct AnimationRuntime {
		size_t clipIndex = 0;
		float time = 0.0f;
		float transitionTime = 0.0f;
		float transitionDuration = 0.0f;
		bool initialized = false;
		bool playing = false;
		std::vector<TransitionPose> transitionPoses;
	};

	std::unordered_map<uint64_t, AnimationRuntime> runtimes_;
};
