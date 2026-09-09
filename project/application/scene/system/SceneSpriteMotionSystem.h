// 役割: SpriteMotion clipのRuntime再生状態と描画用deltaを所有する。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../../../engine/math/Vector2.h"

class SceneDocument;

struct SceneSpriteMotionPresentation {
	uint64_t entityId = 0;
	Vector2 positionOffset{};
	float rotationOffset = 0.0f;
	Vector2 scaleMultiplier = { 1.0f, 1.0f };
	float opacityMultiplier = 1.0f;
};

class SceneSpriteMotionSystem {
public:
	void Update(
		const SceneDocument& document,
		float deltaTime,
		const std::function<bool(uint64_t)>& shouldProcess = {}
	);
	const std::unordered_map<uint64_t, SceneSpriteMotionPresentation>&
	GetPresentationOverrides() const;
	void Clear();

private:
	struct Runtime {
		std::string clipId;
		float elapsedSeconds = 0.0f;
		bool clearAfterPresent = false;
	};

	std::unordered_map<uint64_t, Runtime> runtimes_;
	std::unordered_set<uint64_t> autoStartedEntityIds_;
	std::unordered_map<uint64_t, SceneSpriteMotionPresentation>
		presentationOverrides_;
};
