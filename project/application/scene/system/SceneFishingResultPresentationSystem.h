// 役割: FishingResultPresenterとRuntime Sessionの結果を、描画用override requestへ投影する。
#pragma once

#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector4.h"

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;
class SceneRuntimeSessionState;

struct SceneFishingResultPresentationSpriteRequest {
	uint64_t entityId = 0;
	std::string texturePath;
	Vector2 size{};
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool visible = true;
};

struct SceneFishingResultPresentationTextRequest {
	uint64_t entityId = 0;
	std::string text;
};

class SceneFishingResultPresentationSystem final {
public:
	void Update(
		const SceneDocument& document,
		const SceneRuntimeSessionState* sessionState,
		float realDeltaTime,
		bool playing
	);
	void Clear();

	const std::vector<SceneFishingResultPresentationSpriteRequest>&
	GetSpriteRequests() const;
	const std::vector<SceneFishingResultPresentationTextRequest>&
	GetTextRequests() const;

private:
	std::vector<SceneFishingResultPresentationSpriteRequest> spriteRequests_;
	std::vector<SceneFishingResultPresentationTextRequest> textRequests_;
	uint64_t presenterEntityId_ = 0;
	uint64_t recordGeneration_ = 0;
	std::string variantId_;
	float elapsedSeconds_ = 0.0f;
	bool hasPresentationIdentity_ = false;
};
