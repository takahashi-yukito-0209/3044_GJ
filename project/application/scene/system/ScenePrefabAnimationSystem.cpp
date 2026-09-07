// 役割: Prefab内Property Animationの単一Clip評価とTransform Cross Fadeを実行する。
#include "ScenePrefabAnimationSystem.h"

#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/ScenePrefabAnimationEvaluator.h"
#include "../../../engine/math/Math.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	const SceneEntity* ResolveTrackTarget(
		const SceneDocument& document,
		uint64_t ownerEntityId,
		const SceneAnimationTrack& track
	) {
		if (track.targetEntityId != 0) {
			if (const SceneEntity* entity =
				document.FindEntity(track.targetEntityId)) {
				return entity;
			}
		}
		if (!track.targetEntityName.empty()) {
			if (const SceneEntity* entity =
				document.FindEntityByName(track.targetEntityName)) {
				return entity;
			}
		}
		return document.FindEntity(ownerEntityId);
	}

	Vector3 LerpVector(
		const Vector3& start,
		const Vector3& destination,
		float amount
	) {
		return {
			Math::Lerp(start.x, destination.x, amount),
			Math::Lerp(start.y, destination.y, amount),
			Math::Lerp(start.z, destination.z, amount)
		};
	}
}

void ScenePrefabAnimationSystem::Update(
	SceneDocument& document,
	float deltaTime,
	const std::function<bool(uint64_t)>& shouldProcess
) {
	std::unordered_set<uint64_t> requiredEntities;
	for (SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* animator =
			SceneEntityQuery::FindEnabledComponent(entity, "PrefabAnimator");
		if (!animator || animator->prefabAnimationClips.empty()) {
			continue;
		}
		requiredEntities.insert(entity.id);
		if (shouldProcess && !shouldProcess(entity.id)) {
			continue;
		}
		AnimationRuntime& runtime = runtimes_[entity.id];
		if (!runtime.initialized) {
			runtime.initialized = true;
			for (size_t index = 0;
				index < animator->prefabAnimationClips.size();
				++index) {
				if (animator->prefabAnimationClips[index].playOnStart) {
					runtime.clipIndex = index;
					runtime.playing = true;
					break;
				}
			}
		}
		if (runtime.clipIndex >= animator->prefabAnimationClips.size()) {
			continue;
		}
		if (!runtime.playing && runtime.transitionPoses.empty()) {
			continue;
		}
		const ScenePrefabAnimationClip& clip =
			animator->prefabAnimationClips[runtime.clipIndex];
		const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
		if (runtime.playing) {
			runtime.time += safeDeltaTime;
		}
		const float duration = (std::max)(clip.duration, 0.001f);
		if (runtime.playing && clip.loop) {
			runtime.time = std::fmod(runtime.time, duration);
		} else if (runtime.playing && runtime.time >= duration) {
			runtime.time = duration;
			runtime.playing = false;
		}

		ScenePrefabAnimationEvaluator::ApplyClip(
			document,
			entity.id,
			clip,
			runtime.time
		);

		if (!runtime.transitionPoses.empty()) {
			runtime.transitionTime += safeDeltaTime;
			const float transitionAmount = Math::SmoothStep(std::clamp(
				runtime.transitionTime /
					(std::max)(runtime.transitionDuration, 0.0001f),
				0.0f,
				1.0f
			));
			for (const TransitionPose& startPose : runtime.transitionPoses) {
				SceneEntity* target = document.FindEntity(startPose.entityId);
				if (!target) {
					continue;
				}
				if ((startPose.propertyMask & 0x1u) != 0) {
					target->transform.translate = LerpVector(
						startPose.transform.translate,
						target->transform.translate,
						transitionAmount
					);
				}
				if ((startPose.propertyMask & 0x2u) != 0) {
					target->transform.rotate = Slerp(
						startPose.transform.rotate,
						target->transform.rotate,
						transitionAmount
					);
				}
				if ((startPose.propertyMask & 0x4u) != 0) {
					target->transform.scale = LerpVector(
						startPose.transform.scale,
						target->transform.scale,
						transitionAmount
					);
				}
			}
			if (transitionAmount >= 1.0f) {
				runtime.transitionPoses.clear();
				runtime.transitionTime = 0.0f;
				runtime.transitionDuration = 0.0f;
			}
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

bool ScenePrefabAnimationSystem::Play(
	const SceneDocument& document,
	uint64_t entityId,
	const std::string& clipName,
	bool restart,
	float transitionDuration
) {
	const SceneEntity* entity = document.FindEntity(entityId);
	const SceneComponent* animator = entity
		? SceneEntityQuery::FindEnabledComponent(*entity, "PrefabAnimator")
		: nullptr;
	if (!animator) {
		return false;
	}
	for (size_t index = 0; index < animator->prefabAnimationClips.size(); ++index) {
		if (animator->prefabAnimationClips[index].name != clipName) {
			continue;
		}
		AnimationRuntime& runtime = runtimes_[entityId];
		runtime.transitionPoses.clear();
		runtime.transitionTime = 0.0f;
		runtime.transitionDuration = (std::max)(transitionDuration, 0.0f);
		if (runtime.transitionDuration > 0.0f) {
			const ScenePrefabAnimationClip& destinationClip =
				animator->prefabAnimationClips[index];
			for (const SceneAnimationTrack& track : destinationClip.tracks) {
				uint8_t propertyMask = 0;
				if (track.property == "LocalPosition") {
					propertyMask = 0x1u;
				} else if (track.property == "LocalRotation") {
					propertyMask = 0x2u;
				} else if (track.property == "LocalScale") {
					propertyMask = 0x4u;
				} else {
					continue;
				}
				const SceneEntity* target = ResolveTrackTarget(
					document,
					entityId,
					track
				);
				if (!target) {
					continue;
				}
				auto found = std::find_if(
					runtime.transitionPoses.begin(),
					runtime.transitionPoses.end(),
					[target](const TransitionPose& pose) {
						return pose.entityId == target->id;
					}
				);
				if (found == runtime.transitionPoses.end()) {
					runtime.transitionPoses.push_back({
						target->id,
						target->transform,
						propertyMask
					});
				} else {
					found->propertyMask |= propertyMask;
				}
			}
		}
		if (restart || runtime.clipIndex != index) {
			runtime.time = 0.0f;
		}
		runtime.clipIndex = index;
		runtime.initialized = true;
		runtime.playing = true;
		return true;
	}
	return false;
}

void ScenePrefabAnimationSystem::ResetEntity(uint64_t entityId) {
	runtimes_.erase(entityId);
}

void ScenePrefabAnimationSystem::Clear() {
	runtimes_.clear();
}
