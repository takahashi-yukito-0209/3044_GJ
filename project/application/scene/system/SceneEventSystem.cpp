// 役割: 汎用Trigger/Actionをゲーム進行要求へ変換する。
#include "SceneEventSystem.h"

#include "SceneStatSystem.h"
#include "SceneStateMachineSystem.h"
#include "../SceneRuntimeInput.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	SceneEntity* ResolveEntity(
		SceneDocument& document,
		uint64_t id,
		const std::string& name,
		uint64_t fallbackId
	) {
		if (id != 0) {
			if (SceneEntity* entity = document.FindEntity(id)) {
				return entity;
			}
		}
		if (!name.empty()) {
			if (SceneEntity* entity = document.FindEntityByName(name)) {
				return entity;
			}
		}
		// 明示参照が壊れたActionをOwnerへ誤適用しない。
		return id == 0 && name.empty()
			? document.FindEntity(fallbackId)
			: nullptr;
	}

	bool CompareStat(float value, const SceneEventBinding& binding) {
		if (binding.statComparison == "GreaterOrEqual") {
			return value >= binding.statValue;
		}
		if (binding.statComparison == "Greater") {
			return value > binding.statValue;
		}
		if (binding.statComparison == "Less") {
			return value < binding.statValue;
		}
		if (binding.statComparison == "Equal") {
			return std::abs(value - binding.statValue) <= 0.0001f;
		}
		return value <= binding.statValue;
	}

	bool IsValidPostProcessProfileAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* manager = action.postProcessManagerEntityId != 0
			? document.FindEntity(action.postProcessManagerEntityId)
			: nullptr;
		if (!manager && !action.postProcessManagerEntityName.empty()) {
			manager = document.FindEntityByName(
				action.postProcessManagerEntityName
			);
		}
		if (!manager || action.postProcessProfileId.empty() ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *manager)) {
			return false;
		}
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(
				*manager,
				"PostProcessProfileManager"
			);
		if (!component) {
			return false;
		}
		return std::any_of(
			component->postProcessProfiles.begin(),
			component->postProcessProfiles.end(),
			[&action](const ScenePostProcessProfile& profile) {
				return profile.id == action.postProcessProfileId;
			}
		);
	}

	bool IsValidPostProcessManagerAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* manager = action.postProcessManagerEntityId != 0
			? document.FindEntity(action.postProcessManagerEntityId)
			: nullptr;
		if (!manager && !action.postProcessManagerEntityName.empty()) {
			manager = document.FindEntityByName(
				action.postProcessManagerEntityName
			);
		}
		const SceneComponent* component = manager &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *manager)
			? SceneEntityQuery::FindEnabledComponent(
				*manager, "PostProcessProfileManager"
			)
			: nullptr;
		return component && !component->postProcessProfiles.empty();
	}

	bool IsValidCameraAction(
		const SceneDocument& document,
		const SceneEventAction& action,
		const char* componentName
	) {
		if (action.targetEntityId == 0 && action.targetEntityName.empty()) {
			return false;
		}
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId)
			: nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		return entity &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) &&
			SceneEntityQuery::FindEnabledComponent(*entity, componentName);
	}

	bool IsValidAudioAction(const SceneDocument& document, const SceneEventAction& action) {
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId) : nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		return entity && SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) &&
			SceneEntityQuery::FindEnabledComponent(*entity, "AudioSource");
	}

	bool IsValidTextMotionAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId) : nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		const SceneComponent* component = entity &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)
			? SceneEntityQuery::FindEnabledComponent(*entity, "TextMotion")
			: nullptr;
		if (!component) {
			return false;
		}
		return action.type != "PlayTextMotion" || std::any_of(
			component->textMotionClips.begin(),
			component->textMotionClips.end(),
			[&action](const SceneTextMotionClip& clip) {
				return clip.id == action.textMotionClipId;
			}
		);
	}
}

SceneEventResult SceneEventSystem::Update(
	SceneDocument& document,
	SceneStatSystem& statSystem,
	SceneStateMachineSystem& stateMachineSystem,
	float deltaTime,
	const SceneEventRuntimeSignals& signals
) {
	SceneEventResult result{};
	struct QueuedAction {
		uint64_t ownerEntityId = 0;
		SceneEventAction action{};
	};
	std::vector<QueuedAction> queuedActions;
	std::unordered_set<uint64_t> requiredEntities;

	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* eventComponent =
			SceneEntityQuery::FindEnabledComponent(entity, "EventTrigger");
		if (!eventComponent) {
			continue;
		}
		requiredEntities.insert(entity.id);
		auto& states = runtimes_[entity.id];
		states.resize(eventComponent->eventBindings.size());
		for (size_t index = 0;
			index < eventComponent->eventBindings.size();
			++index) {
			const SceneEventBinding& binding =
				eventComponent->eventBindings[index];
			BindingRuntime& state = states[index];
			state.cooldown = (std::max)(state.cooldown - deltaTime, 0.0f);
			SceneEntity* target = ResolveEntity(
				document,
				binding.targetEntityId,
				binding.targetEntityName,
				entity.id
			);
			bool condition = false;
			bool shouldFire = false;
			if (binding.triggerType == "OnStart") {
				shouldFire = !state.initialized;
				condition = true;
			} else if (binding.triggerType == "OnInterval") {
				condition = true;
				shouldFire = !state.initialized || state.cooldown <= 0.0f;
			} else if (binding.triggerType == "OnStatReachedMin") {
				condition = target && statSystem.IsAtMin(target->id, binding.statId);
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnStatCompare") {
				float value = 0.0f;
				condition = target &&
					statSystem.TryGet(target->id, binding.statId, value) &&
					CompareStat(value, binding);
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnPositionReached") {
				if (target) {
					const Transform transform =
						SceneTransformResolver::ResolveScene3DTransform(
							document,
							*target
						);
					condition = Math::Length(Math::Subtract(
						transform.translate,
						binding.targetPosition
					)) <= (std::max)(binding.radius, 0.0f);
				}
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnKeyPressed") {
				condition = SceneRuntimeInput::EvaluateExpression(
					binding.inputExpression,
					binding.triggerKey
				);
				shouldFire = condition;
			} else if (binding.triggerType == "OnFishingScoreAttackResultInput") {
				condition =
					(binding.targetEntityId != 0 || !binding.targetEntityName.empty()) &&
					target &&
					SceneEntityQuery::FindEnabledComponent(
						*target, "FishingScoreAttackDirector"
					) &&
					target->id == signals.fishingResultInputReadyDirectorEntityId &&
					SceneRuntimeInput::EvaluateExpression(
						binding.inputExpression,
						binding.triggerKey
					);
				shouldFire = condition;
			} else if (binding.triggerType == "OnCameraPathCompleted") {
				condition =
					(binding.targetEntityId != 0 ||
						!binding.targetEntityName.empty()) &&
					target &&
					target->id == signals.completedCameraPathEntityId;
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnAudioFinished") {
				condition =
					(binding.targetEntityId != 0 ||
						!binding.targetEntityName.empty()) &&
					target &&
					SceneEntityQuery::FindEnabledComponent(*target, "AudioSource") &&
					std::find(
						signals.finishedAudioEntityIds.begin(),
						signals.finishedAudioEntityIds.end(),
						target->id
					) != signals.finishedAudioEntityIds.end();
				// 完了は状態ではなくgenerationごとのpulseなので、連続Frameも取りこぼさない。
				shouldFire = condition;
			} else if (binding.triggerType == "OnTextMotionCompleted") {
				condition = target &&
					SceneEntityQuery::FindEnabledComponent(*target, "TextMotion") &&
					std::any_of(
						signals.textMotionCompletions.begin(),
						signals.textMotionCompletions.end(),
						[&binding, target](const SceneTextMotionCompletion& completion) {
							return completion.entityId == target->id &&
								(binding.textMotionClipId.empty() ||
									completion.clipId == binding.textMotionClipId);
						}
					);
				shouldFire = condition;
			}

			if (
				shouldFire &&
				state.cooldown <= 0.0f &&
				(!binding.triggerOnce || !state.fired)
			) {
				for (const SceneEventAction& action : binding.actions) {
					queuedActions.push_back({ entity.id, action });
				}
				state.fired = true;
				state.cooldown = (std::max)(binding.cooldown, 0.0f);
			}
			state.wasConditionTrue = condition;
			state.initialized = true;
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}

	for (const QueuedAction& queued : queuedActions) {
		const SceneEventAction& action = queued.action;
		SceneEntity* target = ResolveEntity(
			document,
			action.targetEntityId,
			action.targetEntityName,
			queued.ownerEntityId
		);
		if (action.type == "ModifyStat") {
			if (target) {
				statSystem.Modify(
					target->id,
					action.statId,
					action.statOperation,
					action.value
				);
			}
		} else if (action.type == "SetEntityActive") {
			if (target) {
				target->active = action.active;
			}
		} else if (action.type == "InstantiatePrefab") {
			const uint64_t targetId = target ? target->id : 0;
			Transform spawnTransform{};
			const bool hasSpawnTransform =
				target && action.prefabUseTargetTransform;
			if (hasSpawnTransform) {
				spawnTransform = SceneTransformResolver::ResolveScene3DTransform(
					document,
					*target
				);
			}
			const uint64_t instanceId = document.InstantiatePrefab(
				action.prefabPath,
				action.prefabParentToTarget ? targetId : 0,
				true
			);
			if (
				instanceId != 0 &&
				hasSpawnTransform &&
				!action.prefabParentToTarget
			) {
				if (SceneEntity* instance = document.FindEntity(instanceId)) {
					instance->transform.translate = spawnTransform.translate;
					instance->transform.rotate = spawnTransform.quaternionRotate;
				}
			}
		} else if (action.type == "ChangeState") {
			if (target) {
				stateMachineSystem.RequestState(target->id, action.stateName);
			}
		} else if (action.type == "AdjustFishingFishCount") {
			const SceneComponent* director = target &&
				SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)
				? SceneEntityQuery::FindEnabledComponent(
					*target,
					"FishingScoreAttackDirector"
				)
				: nullptr;
			if (director && std::isfinite(action.value) &&
				(action.value == 1.0f || action.value == -1.0f)) {
				result.fishingFishCountRequests.push_back({
					target->id,
					static_cast<int>(action.value)
				});
			}
		} else if (
			action.type == "SceneTransition" &&
			result.sceneTransitionId.empty()
		) {
			result.sceneTransitionId = action.sceneId;
		} else if (
			action.type == "SetPostProcessProfile" &&
			IsValidPostProcessProfileAction(document, action)
		) {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::SetProfile;
			result.postProcessRequest.managerEntityId =
				action.postProcessManagerEntityId;
			result.postProcessRequest.managerEntityName =
				action.postProcessManagerEntityName;
			result.postProcessRequest.profileId = action.postProcessProfileId;
		} else if (
			action.type == "NextPostProcessProfile" &&
			IsValidPostProcessManagerAction(document, action)
		) {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::NextProfile;
			result.postProcessRequest.managerEntityId =
				action.postProcessManagerEntityId;
			result.postProcessRequest.managerEntityName =
				action.postProcessManagerEntityName;
			result.postProcessRequest.profileId.clear();
		} else if (action.type == "ResetPostProcessProfile") {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::ResetToSceneDefault;
			result.postProcessRequest.managerEntityId = 0;
			result.postProcessRequest.managerEntityName.clear();
			result.postProcessRequest.profileId.clear();
		} else if (
			(action.type == "PlayAudio" || action.type == "StopAudio" ||
				action.type == "PauseAudio" || action.type == "ResumeAudio") &&
			IsValidAudioAction(document, action)
		) {
			SceneAudioRequestType type = SceneAudioRequestType::Play;
			if (action.type == "StopAudio") type = SceneAudioRequestType::Stop;
			else if (action.type == "PauseAudio") type = SceneAudioRequestType::Pause;
			else if (action.type == "ResumeAudio") type = SceneAudioRequestType::Resume;
			result.audioRequests.push_back({ type, target->id });
		} else if (
			(action.type == "PlayTextMotion" || action.type == "StopTextMotion" ||
				action.type == "ResetTextMotion") &&
			IsValidTextMotionAction(document, action)
		) {
			SceneTextMotionRequestType type = SceneTextMotionRequestType::Play;
			if (action.type == "StopTextMotion") type = SceneTextMotionRequestType::Stop;
			else if (action.type == "ResetTextMotion") type = SceneTextMotionRequestType::Reset;
			result.textMotionRequests.push_back({
				type,
				target->id,
				action.textMotionClipId
			});
		} else if (
			action.type == "PlayCameraPath" &&
			IsValidCameraAction(document, action, "CameraPath")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::PlayPath,
				action.targetEntityId,
				action.targetEntityName
			});
		} else if (
			action.type == "StopCameraPath" &&
			IsValidCameraAction(document, action, "CameraPath")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::StopPath,
				action.targetEntityId,
				action.targetEntityName
			});
		} else if (
			action.type == "SelectCamera" &&
			IsValidCameraAction(document, action, "Camera")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::SelectCamera,
				action.targetEntityId,
				action.targetEntityName
			});
		}
	}
	if (!result.sceneTransitionId.empty()) {
		result.postProcessRequest.type = ScenePostProcessRequestType::None;
		result.cameraRequests.clear();
		result.audioRequests.clear();
		result.textMotionRequests.clear();
	}
	return result;
}

void SceneEventSystem::Clear() {
	runtimes_.clear();
}
