// 役割: TextMotion clipのRuntime再生状態と描画用deltaを所有する。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../engine/math/Vector2.h"

class SceneDocument;

struct SceneTextMotionPresentation {
	uint64_t entityId = 0;
	Vector2 positionOffset{};
	float rotationOffset = 0.0f;
	Vector2 scaleMultiplier = { 1.0f, 1.0f };
	float opacityMultiplier = 1.0f;
};

struct SceneTextMotionCompletion {
	uint64_t entityId = 0;
	std::string clipId;
	uint64_t generation = 0;
};

class SceneTextMotionSystem {
public:
	bool Play(
		const SceneDocument& document,
		uint64_t entityId,
		const std::string& clipId
	);
	void Stop(uint64_t entityId);
	void Reset(uint64_t entityId);
	void Update(const SceneDocument& document, float deltaTime,
		const std::function<bool(uint64_t)>& shouldProcess = {});
	const std::unordered_map<uint64_t, SceneTextMotionPresentation>&
	GetPresentationOverrides() const;
	std::vector<SceneTextMotionCompletion> ConsumeCompletions();
	void Clear();

private:
	struct Runtime {
		std::string clipId;
		float elapsedSeconds = 0.0f;
		uint64_t generation = 0;
		bool clearAfterPresent = false;
	};

	std::unordered_map<uint64_t, Runtime> runtimes_;
	std::unordered_map<uint64_t, SceneTextMotionPresentation>
		presentationOverrides_;
	std::vector<SceneTextMotionCompletion> completions_;
};
