// 役割: TextMotionをauthoring Text配置へ加えるRuntime deltaとして評価する。
#include "SceneTextMotionSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
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

	bool IsValidClip(const SceneTextMotionClip& clip) {
		if (clip.id.empty() || clip.keyframes.size() < 2) {
			return false;
		}
		float previousTime = -1.0f;
		for (const SceneTextMotionKeyframe& keyframe : clip.keyframes) {
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
			clip.keyframes.back().timeSeconds > 0.0f;
	}

	const SceneTextMotionClip* FindClip(
		const SceneComponent& component,
		const std::string& clipId
	) {
		const auto found = std::find_if(
			component.textMotionClips.begin(),
			component.textMotionClips.end(),
			[&clipId](const SceneTextMotionClip& candidate) {
				return candidate.id == clipId;
			}
		);
		return found == component.textMotionClips.end() ? nullptr : &(*found);
	}

	SceneTextMotionPresentation SampleClip(
		uint64_t entityId,
		const SceneTextMotionClip& clip,
		float elapsedSeconds
	) {
		SceneTextMotionPresentation result{};
		result.entityId = entityId;
		const float elapsed = std::clamp(
			elapsedSeconds,
			0.0f,
			clip.keyframes.back().timeSeconds
		);
		const SceneTextMotionKeyframe* previous = &clip.keyframes.front();
		const SceneTextMotionKeyframe* next = previous;
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

bool SceneTextMotionSystem::Play(
	const SceneDocument& document,
	uint64_t entityId,
	const std::string& clipId
) {
	const SceneEntity* entity = document.FindEntity(entityId);
	const SceneComponent* text = entity
		? FindEnabledComponent(*entity, "TextRenderer")
		: nullptr;
	const SceneComponent* motion = entity
		? FindEnabledComponent(*entity, "TextMotion")
		: nullptr;
	const SceneTextMotionClip* clip = motion
		? FindClip(*motion, clipId)
		: nullptr;
	if (
		!entity || !text || !motion || !clip || !IsValidClip(*clip) ||
		!IsEntityActiveInHierarchy(document, *entity)
	) {
		return false;
	}
	Runtime& runtime = runtimes_[entityId];
	++runtime.generation;
	runtime.clipId = clipId;
	runtime.elapsedSeconds = 0.0f;
	runtime.clearAfterPresent = false;
	presentationOverrides_[entityId] = SampleClip(entityId, *clip, 0.0f);
	return true;
}

void SceneTextMotionSystem::Stop(uint64_t entityId) {
	runtimes_.erase(entityId);
	presentationOverrides_.erase(entityId);
}

void SceneTextMotionSystem::Reset(uint64_t entityId) {
	Stop(entityId);
}

void SceneTextMotionSystem::Update(
	const SceneDocument& document,
	float deltaTime,
	const std::function<bool(uint64_t)>& shouldProcess
) {
	completions_.clear();
	const float elapsedDelta = (std::max)(deltaTime, 0.0f);
	std::unordered_set<uint64_t> requiredEntityIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* text = FindEnabledComponent(entity, "TextRenderer");
		const SceneComponent* motion = FindEnabledComponent(entity, "TextMotion");
		if (!text || !motion) {
			continue;
		}
		requiredEntityIds.insert(entity.id);
		if (shouldProcess && !shouldProcess(entity.id)) {
			continue;
		}
		const auto runtimeIt = runtimes_.find(entity.id);
		if (runtimeIt == runtimes_.end()) {
			continue;
		}
		Runtime& runtime = runtimeIt->second;
		const SceneTextMotionClip* clip = FindClip(*motion, runtime.clipId);
		if (!clip || !IsValidClip(*clip)) {
			Stop(entity.id);
			continue;
		}
		if (runtime.clearAfterPresent) {
			Stop(entity.id);
			continue;
		}
		const float duration = clip->keyframes.back().timeSeconds;
		const float previousElapsed = runtime.elapsedSeconds;
		runtime.elapsedSeconds = (std::min)(
			runtime.elapsedSeconds + elapsedDelta,
			duration
		);
		presentationOverrides_[entity.id] = SampleClip(
			entity.id,
			*clip,
			runtime.elapsedSeconds
		);
		if (previousElapsed < duration && runtime.elapsedSeconds >= duration) {
			completions_.push_back({
				entity.id,
				runtime.clipId,
				runtime.generation
			});
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
}

const std::unordered_map<uint64_t, SceneTextMotionPresentation>&
SceneTextMotionSystem::GetPresentationOverrides() const {
	return presentationOverrides_;
}

std::vector<SceneTextMotionCompletion>
SceneTextMotionSystem::ConsumeCompletions() {
	return std::move(completions_);
}

void SceneTextMotionSystem::Clear() {
	runtimes_.clear();
	presentationOverrides_.clear();
	completions_.clear();
}
