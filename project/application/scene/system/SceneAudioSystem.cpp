// 役割: AudioSourceの有効状態とEvent要求を2D／3D Audio Voiceへ反映する。
#include "SceneAudioSystem.h"

#include "SceneEventSystem.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "../../../engine/math/Math.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	struct SpatialPose {
		Vector3 position{};
		Vector3 front = { 0.0f, 0.0f, 1.0f };
		Vector3 top = { 0.0f, 1.0f, 0.0f };
	};

	AudioBus ToAudioBus(const std::string& value) {
		if (value == "BGM") return AudioBus::BGM;
		if (value == "UI") return AudioBus::UI;
		if (value == "Ambience") return AudioBus::Ambience;
		return AudioBus::SFX;
	}

	/// <summary>
	/// Scene定義上のAudioSource設定から、実際に使用するAudio Busを決定します。
	/// </summary>
	AudioBus ResolveAudioSourceBus(const SceneComponent& component) {
		const bool looksLikeSceneBgm =
			component.audioBus == "UI" &&
			component.audioSpatialMode == "TwoD" &&
			component.audioPlayOnStart &&
			component.audioLoop; // 既存SceneでUI Busに置かれた開始ループ音源をBGM扱いにする判定。
		if (looksLikeSceneBgm) {
			return AudioBus::BGM;
		}
		return ToAudioBus(component.audioBus);
	}

	Vector3 NormalizeOr(const Vector3& value, const Vector3& fallback) {
		return Math::Length(value) > 0.0001f
			? Math::Normalize(value)
			: fallback;
	}

	SpatialPose MakePose(const Matrix4x4& world) {
		SpatialPose result{};
		result.position = { world.m[3][0], world.m[3][1], world.m[3][2] };
		result.front = NormalizeOr(
			{ world.m[2][0], world.m[2][1], world.m[2][2] },
			{ 0.0f, 0.0f, 1.0f }
		);
		Vector3 top = {
			world.m[1][0], world.m[1][1], world.m[1][2]
		};
		top = Math::Subtract(
			top,
			Math::Multiply(result.front, Math::Dot(top, result.front))
		);
		if (Math::Length(top) <= 0.0001f) {
			const Vector3 fallback = std::abs(result.front.y) < 0.99f
				? Vector3{ 0.0f, 1.0f, 0.0f }
				: Vector3{ 0.0f, 0.0f, 1.0f };
			top = Math::Subtract(
				fallback,
				Math::Multiply(result.front, Math::Dot(fallback, result.front))
			);
		}
		result.top = NormalizeOr(top, { 0.0f, 1.0f, 0.0f });
		return result;
	}

	bool IsActiveListener(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& component
	) {
		return component.type == "AudioListener" && component.enabled &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, entity);
	}

	bool UsesSpatialPlayback(const SceneComponent& component) {
		return component.audioSpatialMode == "ThreeD" ||
			component.audioSpatialMode == "ThreeDPointDownmix" ||
			component.audioSpatialMode == "ThreeDStereoArea";
	}

	bool IsStreamCompatible(const SceneComponent& component) {
		return component.audioStreamFromDisk &&
			component.audioSpatialMode == "TwoD" && component.audioBus == "BGM";
	}

	bool IsPersistentBgm(const SceneComponent& component, bool activeScene) {
		return activeScene && IsStreamCompatible(component) &&
			component.audioPersistAcrossScenes;
	}
}

uint64_t SceneAudioSystem::MakeBindingKey(uint64_t entityId, uint64_t componentLocalId) {
	return (entityId << 32) ^ componentLocalId;
}

void SceneAudioSystem::Preload(
	const SceneComponent& component,
	Binding& binding
) {
	if (
		binding.clipPath != component.audioClipPath ||
		binding.spatialMode != component.audioSpatialMode ||
		binding.streamFromDisk != component.audioStreamFromDisk
	) {
		Audio::GetInstance()->SoundStop(binding.handle);
		binding.clip.reset();
		binding.clipPath = component.audioClipPath;
		binding.spatialMode = component.audioSpatialMode;
		binding.streamFromDisk = component.audioStreamFromDisk;
	}
	if (
		component.audioStreamFromDisk ||
		!component.audioDecompressOnLoad ||
		component.audioClipPath.empty() ||
		binding.clip
	) {
		return;
	}
	PrepareClip(component, binding);
}

AudioClipPtr SceneAudioSystem::PrepareClip(
	const SceneComponent& component,
	Binding& binding
) {
	if (binding.clip) {
		return binding.clip;
	}
	std::string error;
	AudioClipPtr clip = Audio::GetInstance()->LoadAudioFile(
		component.audioClipPath.c_str(),
		&error
	);
	if (!clip) {
		return {};
	}
	if (component.audioSpatialMode == "ThreeDPointDownmix") {
		clip = Audio::GetInstance()->GetOrCreatePointDownmixClip(clip, &error);
	}
	binding.clip = clip;
	return binding.clip;
}

Audio::PlaybackHandle SceneAudioSystem::Play(
	const SceneComponent& component,
	Binding& binding
) {
	if (component.audioClipPath.empty()) {
		return {};
	}
	if (component.audioStreamFromDisk) {
		if (!IsStreamCompatible(component)) {
			return {};
		}
		std::string error;
		return Audio::GetInstance()->PlayAudioStream(
			component.audioClipPath.c_str(),
			{
				ResolveAudioSourceBus(component), component.audioVolume,
				component.audioPitch, component.audioLoop, true
			},
			&error
		);
	}
	Preload(component, binding);
	AudioClipPtr clip = PrepareClip(component, binding);
	if (!clip) {
		return {};
	}
	if (
		(component.audioSpatialMode == "ThreeD" && clip->channelCount != 1) ||
		(component.audioSpatialMode == "ThreeDPointDownmix" && clip->channelCount != 1) ||
		(component.audioSpatialMode == "ThreeDStereoArea" && clip->channelCount != 2)
	) {
		return {};
	}
	return Audio::GetInstance()->PlayAudioClip(clip, {
		ResolveAudioSourceBus(component), component.audioVolume,
		component.audioPitch, component.audioLoop, true
	});
}

void SceneAudioSystem::Sync(
	const SceneDocument& document,
	bool playing,
	uint64_t sceneOwnerId,
	bool activeScene
) {
	if (sceneOwnerId_ != 0 && sceneOwnerId_ != sceneOwnerId) {
		Clear();
	}
	sceneOwnerId_ = sceneOwnerId;
	if (activeScene_ && !activeScene && sceneOwnerId_ != 0) {
		Audio::GetInstance()->ReleasePersistentBgmOwner(sceneOwnerId_);
	}
	if (activeScene_ != activeScene) {
		playSessionStarted_ = false;
	}
	activeScene_ = activeScene;
	if (!playing) {
		Clear();
		return;
	}
	std::unordered_set<uint64_t> requiredBindings;
	const SceneComponent* persistentPlayOnStart = nullptr;
	uint64_t persistentPlayOnStartKey = 0;
	for (const SceneEntity& entity : document.GetEntities()) {
		for (const SceneComponent& component : entity.components) {
			if (component.type != "AudioSource") continue;
			const uint64_t key = MakeBindingKey(entity.id, component.localId);
			requiredBindings.insert(key);
			Binding& binding = bindings_[key];
			binding.entityId = entity.id;
			binding.componentLocalId = component.localId;
			const bool wasPersistent = binding.persistent;
			binding.persistent = IsPersistentBgm(component, activeScene_);
			if (wasPersistent && !binding.persistent) {
				Audio::GetInstance()->StopPersistentBgm(sceneOwnerId_, key);
			}
			const bool active = component.enabled &&
				SceneEntityQuery::IsEntityActiveInHierarchy(document, entity);
			if (active) {
				Preload(component, binding);
			}
			if (!active && binding.wasActive && component.audioStopOnDisable) {
				if (binding.persistent) {
					Audio::GetInstance()->StopPersistentBgm(sceneOwnerId_, key);
				} else {
					Audio::GetInstance()->SoundStop(binding.handle);
				}
				binding.eventPaused = false;
				binding.policyPaused = false;
			}
			if (active && !playSessionStarted_ && component.audioPlayOnStart) {
				if (binding.persistent) {
					if (!persistentPlayOnStart) {
						persistentPlayOnStart = &component;
						persistentPlayOnStartKey = key;
					}
				} else {
					binding.handle = Play(component, binding);
				}
				binding.eventPaused = false;
			}
			binding.wasActive = active;
		}
	}
	for (auto iterator = bindings_.begin(); iterator != bindings_.end();) {
		if (!requiredBindings.contains(iterator->first)) {
			if (iterator->second.persistent) {
				Audio::GetInstance()->StopPersistentBgm(sceneOwnerId_, iterator->first);
			} else {
				Audio::GetInstance()->SoundStop(iterator->second.handle);
			}
			iterator = bindings_.erase(iterator);
		} else {
			++iterator;
		}
	}
	if (!playSessionStarted_ && activeScene_) {
		if (persistentPlayOnStart) {
			Audio::GetInstance()->ResolvePersistentBgm({
				sceneOwnerId_,
				persistentPlayOnStartKey,
				persistentPlayOnStart->audioClipPath,
				{
					ResolveAudioSourceBus(*persistentPlayOnStart),
					persistentPlayOnStart->audioVolume,
					persistentPlayOnStart->audioPitch,
					persistentPlayOnStart->audioLoop,
					true
				},
				persistentPlayOnStart->audioBgmFadeSeconds
			});
		} else {
			Audio::GetInstance()->ResolveNoPersistentBgm(sceneOwnerId_);
		}
	}
	playSessionStarted_ = true;
}

void SceneAudioSystem::UpdateSpatial(
	const SceneDocument& document,
	const Camera* camera
) {
	const SceneEntity* listenerEntity = nullptr;
	const SceneComponent* listenerComponent = nullptr;
	for (const SceneEntity& entity : document.GetEntities()) {
		for (const SceneComponent& component : entity.components) {
			if (!IsActiveListener(document, entity, component)) {
				continue;
			}
			// Validatorは複数をErrorにする。RuntimeはDocument順の最初を選び続ける。
			listenerEntity = &entity;
			listenerComponent = &component;
			break;
		}
		if (listenerComponent) {
			break;
		}
	}

	if (!camera && (!listenerComponent || listenerComponent->audioListenerMode != "Entity")) {
		return;
	}
	SpatialPose listener = camera
		? MakePose(camera->GetWorldMatrix())
		: SpatialPose{};
	if (listenerEntity && listenerComponent) {
		const SpatialPose entityPose = MakePose(
			SceneTransformResolver::ResolveSceneWorldMatrix(document, *listenerEntity)
		);
		if (listenerComponent->audioListenerMode == "Entity") {
			listener = entityPose;
		} else if (listenerComponent->audioListenerMode == "Hybrid") {
			// HybridはPlayer等の位置を保ち、三人称Cameraの最終姿勢だけを借用する。
			listener.position = entityPose.position;
		}
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		const SpatialPose emitter = MakePose(
			SceneTransformResolver::ResolveSceneWorldMatrix(document, entity)
		);
		for (const SceneComponent& component : entity.components) {
			if (
				component.type != "AudioSource" ||
				!UsesSpatialPlayback(component) ||
				!component.enabled ||
				!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)
			) {
				continue;
			}
			const auto found = bindings_.find(MakeBindingKey(entity.id, component.localId));
			if (found == bindings_.end() || !found->second.handle.IsValid()) {
				continue;
			}
			Audio::GetInstance()->SetSpatialParameters(found->second.handle, {
				listener.position,
				listener.front,
				listener.top,
				emitter.position,
				emitter.front,
				emitter.top,
				component.audioMinimumDistance,
				component.audioMaximumDistance,
				component.audioSpatialMode == "ThreeDStereoArea"
					? AudioSpatialLayout::StereoArea
					: AudioSpatialLayout::Point,
				component.audioStereoAreaWidth
			});
		}
	}
}

std::vector<uint64_t> SceneAudioSystem::ConsumeFinishedEntityIds(
	const SceneDocument& document
) {
	std::vector<uint64_t> finishedEntityIds;
	std::unordered_set<uint64_t> emittedEntityIds;
	for (auto& [bindingKey, binding] : bindings_) {
		const SceneEntity* entity = document.FindEntity(binding.entityId);
		const SceneComponent* component = nullptr;
		if (entity) {
			const auto foundComponent = std::find_if(
				entity->components.begin(),
				entity->components.end(),
				[componentLocalId = binding.componentLocalId](
					const SceneComponent& candidate
				) {
					return candidate.type == "AudioSource" &&
						candidate.localId == componentLocalId;
				}
			);
			if (foundComponent != entity->components.end()) {
				component = &*foundComponent;
			}
		}
		const bool activeBinding = component && component->enabled &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity);
		bool finished = false;
		if (binding.persistent) {
			finished = Audio::GetInstance()->ConsumePersistentBgmCompletion(
				sceneOwnerId_, bindingKey
			);
		} else {
			finished = Audio::GetInstance()->ConsumeNaturalCompletion(binding.handle);
			if (finished) {
				binding.handle = {};
			}
		}
		if (finished && activeBinding && emittedEntityIds.insert(entity->id).second) {
			finishedEntityIds.push_back(entity->id);
		}
	}
	return finishedEntityIds;
}

void SceneAudioSystem::ApplyRequests(const SceneDocument& document, const std::vector<SceneAudioRequest>& requests) {
	for (const SceneAudioRequest& request : requests) {
		const SceneEntity* entity = request.entityId != 0 ? document.FindEntity(request.entityId) : nullptr;
		if (!entity || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)) continue;
		const SceneComponent* component = SceneEntityQuery::FindEnabledComponent(*entity, "AudioSource");
		if (!component) continue;
		Binding& binding = bindings_[MakeBindingKey(entity->id, component->localId)];
		const uint64_t bindingKey = MakeBindingKey(entity->id, component->localId);
		binding.entityId = entity->id;
		binding.componentLocalId = component->localId;
		binding.persistent = IsPersistentBgm(*component, activeScene_);
		if (request.type == SceneAudioRequestType::Play) {
			if (binding.persistent) {
				Audio::GetInstance()->ResolvePersistentBgm({
					sceneOwnerId_, bindingKey, component->audioClipPath,
					{
						ResolveAudioSourceBus(*component), component->audioVolume,
						component->audioPitch, component->audioLoop, true
					},
					component->audioBgmFadeSeconds
				});
			} else {
				Audio::GetInstance()->SoundStop(binding.handle);
				binding.handle = Play(*component, binding);
			}
			binding.eventPaused = false;
		} else if (request.type == SceneAudioRequestType::Stop) {
			if (binding.persistent) Audio::GetInstance()->StopPersistentBgm(sceneOwnerId_, bindingKey);
			else Audio::GetInstance()->SoundStop(binding.handle);
			binding.eventPaused = false;
			binding.policyPaused = false;
		} else if (request.type == SceneAudioRequestType::Pause) {
			if (binding.persistent) Audio::GetInstance()->PausePersistentBgm(sceneOwnerId_, bindingKey);
			else Audio::GetInstance()->Pause(binding.handle);
			binding.eventPaused = true;
		} else if (request.type == SceneAudioRequestType::Resume) {
			binding.eventPaused = false;
			if (!binding.policyPaused) {
				if (binding.persistent) Audio::GetInstance()->ResumePersistentBgm(sceneOwnerId_, bindingKey);
				else Audio::GetInstance()->Resume(binding.handle);
			}
		}
	}
}

void SceneAudioSystem::ApplyProcessPolicy(
	const SceneDocument& document,
	const std::function<bool(uint64_t)>& shouldProcess
) {
	(void)document;
	for (auto& [bindingKey, binding] : bindings_) {
		const bool allowed = !shouldProcess || shouldProcess(binding.entityId);
		if (!allowed && !binding.policyPaused) {
			if (!binding.eventPaused) {
				if (binding.persistent) Audio::GetInstance()->PausePersistentBgm(sceneOwnerId_, bindingKey);
				else Audio::GetInstance()->Pause(binding.handle);
			}
			binding.policyPaused = true;
		} else if (allowed && binding.policyPaused) {
			binding.policyPaused = false;
			if (!binding.eventPaused) {
				if (binding.persistent) Audio::GetInstance()->ResumePersistentBgm(sceneOwnerId_, bindingKey);
				else Audio::GetInstance()->Resume(binding.handle);
			}
		}
	}
}

void SceneAudioSystem::PrepareForSceneTransition() {
	if (sceneOwnerId_ != 0) {
		Audio::GetInstance()->BeginPersistentBgmSceneTransition(sceneOwnerId_);
	}
}

void SceneAudioSystem::Clear() {
	for (auto& [key, binding] : bindings_) {
		Audio::GetInstance()->SoundStop(binding.handle);
	}
	if (sceneOwnerId_ != 0) {
		Audio::GetInstance()->ReleasePersistentBgmOwner(sceneOwnerId_);
	}
	bindings_.clear();
	playSessionStarted_ = false;
	activeScene_ = false;
	sceneOwnerId_ = 0;
}
