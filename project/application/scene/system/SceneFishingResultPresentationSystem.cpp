// 役割: 釣り結果のvariant選択と装飾pulseを、Sceneを変更しないrequestとして生成する。
#include "SceneFishingResultPresentationSystem.h"

#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneRuntimeSessionState.h"

#include <cmath>

namespace {
	const SceneFishingResultVisualVariant* FindVariant(
		const SceneComponent& presenter,
		const std::string& variantId
	) {
		for (const SceneFishingResultVisualVariant& variant :
			presenter.fishingResultPresentationVariants) {
			if (variant.id == variantId) {
				return &variant;
			}
		}
		return nullptr;
	}
}

void SceneFishingResultPresentationSystem::Update(
	const SceneDocument& document,
	const SceneRuntimeSessionState* sessionState,
	float realDeltaTime,
	bool playing
) {
	if (!sessionState || !playing) {
		Clear();
		return;
	}

	const SceneEntity* presenterEntity = nullptr;
	const SceneComponent* presenter = nullptr;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* candidate =
			SceneEntityQuery::FindEnabledComponent(entity, "FishingResultPresenter");
		if (candidate) {
			presenterEntity = &entity;
			presenter = candidate;
			break;
		}
	}
	if (!presenterEntity || !presenter) {
		Clear();
		return;
	}

	spriteRequests_.clear();
	textRequests_.clear();
	const SceneFishingResultRecord* record = sessionState->FindFishingResult(
		presenter->fishingResultPresentationChannelId
	);
	std::string selectedVariantId =
		presenter->fishingResultPresentationFallbackVariantId;
	if (record &&
		(record->winningFishWeightedCount != 0 ||
			record->sharkFishWeightedCount != 0)) {
		if (presenter->fishingResultPresentationIncludeSharkInWinnerSelection &&
			record->sharkFishWeightedCount > record->winningFishWeightedCount) {
			selectedVariantId = presenter->fishingResultPresentationSharkVariantId;
		} else {
			selectedVariantId = record->winningRankId;
		}
	}
	const SceneFishingResultVisualVariant* selectedVariant = FindVariant(
		*presenter, selectedVariantId
	);
	if (!selectedVariant) {
		selectedVariantId = presenter->fishingResultPresentationFallbackVariantId;
		selectedVariant = FindVariant(*presenter, selectedVariantId);
	}

	const uint64_t recordGeneration = record ? record->generation : 0;
	const bool presentationChanged = !hasPresentationIdentity_ ||
		presenterEntityId_ != presenterEntity->id ||
		recordGeneration_ != recordGeneration ||
		variantId_ != selectedVariantId;
	if (presentationChanged) {
		presenterEntityId_ = presenterEntity->id;
		recordGeneration_ = recordGeneration;
		variantId_ = selectedVariantId;
		elapsedSeconds_ = 0.0f;
		hasPresentationIdentity_ = true;
	} else if (std::isfinite(realDeltaTime) && realDeltaTime > 0.0f) {
		elapsedSeconds_ += realDeltaTime;
	}

	const SceneEntity* scoreEntity = document.FindEntity(
		presenter->fishingResultPresentationScoreTextEntityId
	);
	if (scoreEntity && SceneEntityQuery::FindEnabledComponent(
		*scoreEntity, "TextRenderer"
	)) {
		textRequests_.push_back({
			scoreEntity->id,
			presenter->fishingResultPresentationScorePrefix +
				std::to_string(record ? record->totalScore : 0)
		});
	}
	if (!selectedVariant) {
		return;
	}

	auto appendSpriteRequest = [
		&document,
		this
	](uint64_t entityId, const std::string& texturePath, float scaleMultiplier) {
		if (texturePath.empty() || !std::isfinite(scaleMultiplier) ||
			scaleMultiplier <= 0.0f) {
			return;
		}
		const SceneEntity* entity = document.FindEntity(entityId);
		const SceneComponent* spriteRenderer = entity
			? SceneEntityQuery::FindEnabledComponent(*entity, "SpriteRenderer")
			: nullptr;
		if (!entity || !spriteRenderer) {
			return;
		}
		spriteRequests_.push_back({
			entity->id,
			texturePath,
			{
				spriteRenderer->spriteSize.x * scaleMultiplier,
				spriteRenderer->spriteSize.y * scaleMultiplier
			},
			spriteRenderer->spriteColor,
			true
		});
	};
	appendSpriteRequest(
		presenter->fishingResultPresentationBackgroundEntityId,
		selectedVariant->backgroundTexturePath,
		1.0f
	);
	constexpr float kTwoPi = 6.28318530717958647692f;
	for (const SceneFishingResultDecorationEntry& decoration :
		presenter->fishingResultPresentationDecorations) {
		if (!std::isfinite(decoration.periodSeconds) ||
			decoration.periodSeconds <= 0.0f ||
			!std::isfinite(decoration.minScaleMultiplier) ||
			!std::isfinite(decoration.maxScaleMultiplier) ||
			!std::isfinite(decoration.phaseOffset)) {
			continue;
		}
		const float wave = 0.5f - 0.5f * std::cos(
			kTwoPi * (elapsedSeconds_ / decoration.periodSeconds +
				decoration.phaseOffset)
		);
		const float scaleMultiplier = decoration.minScaleMultiplier +
			(decoration.maxScaleMultiplier - decoration.minScaleMultiplier) * wave;
		appendSpriteRequest(
			decoration.spriteEntityId,
			selectedVariant->decorationTexturePath,
			scaleMultiplier
		);
	}
}

void SceneFishingResultPresentationSystem::Clear() {
	spriteRequests_.clear();
	textRequests_.clear();
	presenterEntityId_ = 0;
	recordGeneration_ = 0;
	variantId_.clear();
	elapsedSeconds_ = 0.0f;
	hasPresentationIdentity_ = false;
}

const std::vector<SceneFishingResultPresentationSpriteRequest>&
SceneFishingResultPresentationSystem::GetSpriteRequests() const {
	return spriteRequests_;
}

const std::vector<SceneFishingResultPresentationTextRequest>&
SceneFishingResultPresentationSystem::GetTextRequests() const {
	return textRequests_;
}
