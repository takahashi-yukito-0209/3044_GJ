// 役割: SpriteMotionをauthoring Sprite配置へ加えるRuntime deltaとして評価する。
#include "SceneSpriteMotionSystem.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "../../../engine/math/Math.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	bool IsFiniteVector(const Vector2& value) {
		return std::isfinite(value.x) && std::isfinite(value.y);
	}

	bool IsKnownEasing(const std::string& easing) {
		return easing == "Linear" || easing == "EaseIn" ||
			easing == "EaseOut" || easing == "EaseInOut" ||
			easing == "SmoothStep";
	}

	float ApplyEasing(float value, const std::string& easing) {
		const float t = Math::Clamp01(value);
		if (easing == "Linear") {
			return t;
		}
		if (easing == "EaseIn") {
			return t * t * t;
		}
		if (easing == "EaseOut") {
			return Math::EaseOutCubic(t);
		}
		if (easing == "EaseInOut") {
			return t < 0.5f
				? 4.0f * t * t * t
				: 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
		}
		return Math::SmoothStep(t);
	}

	bool IsValidClip(const SceneSpriteMotionClip& clip) {
		if (clip.id.empty() || clip.keyframes.size() < 2) {
			return false;
		}
		float previousTime = -1.0f;
		for (const SceneSpriteMotionKeyframe& keyframe : clip.keyframes) {
			if (
				!std::isfinite(keyframe.timeSeconds) ||
				!std::isfinite(keyframe.rotationOffset) ||
				!std::isfinite(keyframe.opacityMultiplier) ||
				!IsFiniteVector(keyframe.positionOffset) ||
				!IsFiniteVector(keyframe.scaleMultiplier) ||
				keyframe.timeSeconds <= previousTime ||
				keyframe.scaleMultiplier.x <= 0.0f ||
				keyframe.scaleMultiplier.y <= 0.0f ||
				keyframe.opacityMultiplier < 0.0f ||
				keyframe.opacityMultiplier > 1.0f ||
				!IsKnownEasing(keyframe.easingToNext)
			) {
				return false;
			}
			previousTime = keyframe.timeSeconds;
		}
		return clip.keyframes.front().timeSeconds == 0.0f &&
			clip.keyframes.back().timeSeconds > 0.0f &&
			(!clip.loop ||
				(std::isfinite(clip.loopStartTimeSeconds) &&
					clip.loopStartTimeSeconds >= 0.0f &&
					clip.loopStartTimeSeconds < clip.keyframes.back().timeSeconds));
	}

	const SceneSpriteMotionClip* FindClip(
		const SceneComponent& component,
		const std::string& clipId
	) {
		const auto found = std::find_if(
			component.spriteMotionClips.begin(),
			component.spriteMotionClips.end(),
			[&clipId](const SceneSpriteMotionClip& candidate) {
				return candidate.id == clipId;
			}
		);
		return found == component.spriteMotionClips.end() ? nullptr : &(*found);
	}

	SceneSpriteMotionPresentation SampleClip(
		uint64_t entityId,
		const SceneSpriteMotionClip& clip,
		float elapsedSeconds
	) {
		SceneSpriteMotionPresentation result{};
		result.entityId = entityId;
		const float elapsed = std::clamp(
			elapsedSeconds,
			0.0f,
			clip.keyframes.back().timeSeconds
		);
		const SceneSpriteMotionKeyframe* previous = &clip.keyframes.front();
		const SceneSpriteMotionKeyframe* next = previous;
		for (size_t index = 1; index < clip.keyframes.size(); ++index) {
			next = &clip.keyframes[index];
			if (elapsed <= next->timeSeconds) {
				break;
			}
			previous = next;
		}
		if (previous == next) {
			result.positionOffset = previous->positionOffset;
			result.rotationOffset = previous->rotationOffset;
			result.scaleMultiplier = previous->scaleMultiplier;
			result.opacityMultiplier = previous->opacityMultiplier;
			return result;
		}
		const float duration = next->timeSeconds - previous->timeSeconds;
		const float amount = ApplyEasing(
			(elapsed - previous->timeSeconds) / duration,
			previous->easingToNext
		);
		result.positionOffset = {
			previous->positionOffset.x +
			(next->positionOffset.x - previous->positionOffset.x) * amount,
			previous->positionOffset.y +
			(next->positionOffset.y - previous->positionOffset.y) * amount
		};
		result.rotationOffset = previous->rotationOffset +
			(next->rotationOffset - previous->rotationOffset) * amount;
		result.scaleMultiplier = {
			previous->scaleMultiplier.x +
			(next->scaleMultiplier.x - previous->scaleMultiplier.x) * amount,
			previous->scaleMultiplier.y +
			(next->scaleMultiplier.y - previous->scaleMultiplier.y) * amount
		};
		result.opacityMultiplier = previous->opacityMultiplier +
			(next->opacityMultiplier - previous->opacityMultiplier) * amount;
		return result;
	}
}

void SceneSpriteMotionSystem::Update(
	const SceneDocument& document,
	float deltaTime,
	const std::function<bool(uint64_t)>& shouldProcess
) {
	const float elapsedDelta = (std::max)(deltaTime, 0.0f);
	std::unordered_set<uint64_t> requiredEntityIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* spriteRenderer =
			FindEnabledComponent(entity, "SpriteRenderer");
		const SceneComponent* motion =
			FindEnabledComponent(entity, "SpriteMotion");
		if (!spriteRenderer || !motion) {
			continue;
		}
		requiredEntityIds.insert(entity.id);
		if (shouldProcess && !shouldProcess(entity.id)) {
			continue;
		}

		const SceneSpriteMotionClip* clip = nullptr;
		auto runtimeIt = runtimes_.find(entity.id);
		if (runtimeIt == runtimes_.end()) {
			if (!motion->spriteMotionPlayOnStart ||
				autoStartedEntityIds_.contains(entity.id)) {
				continue;
			}
			clip = FindClip(*motion, motion->spriteMotionStartClipId);
			if (!clip || !IsValidClip(*clip)) {
				autoStartedEntityIds_.insert(entity.id);
				continue;
			}
			autoStartedEntityIds_.insert(entity.id);
			Runtime runtime{};
			runtime.clipId = clip->id;
			runtime.elapsedSeconds = 0.0f;
			runtimeIt = runtimes_.emplace(entity.id, std::move(runtime)).first;
		}

		Runtime& runtime = runtimeIt->second;
		clip = FindClip(*motion, runtime.clipId);
		if (!clip || !IsValidClip(*clip)) {
			runtimes_.erase(runtimeIt);
			presentationOverrides_.erase(entity.id);
			continue;
		}
		if (runtime.clearAfterPresent) {
			runtimes_.erase(runtimeIt);
			presentationOverrides_.erase(entity.id);
			continue;
		}

		const float duration = clip->keyframes.back().timeSeconds;
		if (clip->loop) {
			runtime.elapsedSeconds += elapsedDelta;
			const float loopDuration = duration - clip->loopStartTimeSeconds;
			if (runtime.elapsedSeconds > duration) {
				runtime.elapsedSeconds = clip->loopStartTimeSeconds + std::fmod(
					runtime.elapsedSeconds - clip->loopStartTimeSeconds,
					loopDuration
				);
			}
		} else {
			runtime.elapsedSeconds = (std::min)(
				runtime.elapsedSeconds + elapsedDelta,
				duration
			);
		}
		presentationOverrides_[entity.id] = SampleClip(
			entity.id,
			*clip,
			runtime.elapsedSeconds
		);
		if (!clip->loop && runtime.elapsedSeconds >= duration) {
			runtime.clearAfterPresent = !clip->holdFinalPose;
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntityIds.contains(iterator->first)) {
			presentationOverrides_.erase(iterator->first);
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
	for (auto iterator = autoStartedEntityIds_.begin();
		iterator != autoStartedEntityIds_.end();) {
		if (!requiredEntityIds.contains(*iterator)) {
			iterator = autoStartedEntityIds_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

const std::unordered_map<uint64_t, SceneSpriteMotionPresentation>&
SceneSpriteMotionSystem::GetPresentationOverrides() const {
	return presentationOverrides_;
}

void SceneSpriteMotionSystem::Clear() {
	runtimes_.clear();
	autoStartedEntityIds_.clear();
	presentationOverrides_.clear();
}
