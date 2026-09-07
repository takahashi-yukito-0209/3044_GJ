// 役割: AudioSourceのSceneライフサイクルとAudio Playback Handleの対応を所有する。
#pragma once

#include "../../../engine/Audio/Audio.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class SceneDocument;
struct SceneComponent;
struct SceneAudioRequest;
class Camera;

class SceneAudioSystem {
public:
	void Sync(
		const SceneDocument& document,
		bool playing,
		uint64_t sceneOwnerId,
		bool activeScene
	);
	// Camera AfterSimulation後に、3D Sourceへ最終World Transformを反映する。
	void UpdateSpatial(const SceneDocument& document, const Camera* camera);
	// Event評価直前に、現行Bindingへ対応する自然終了Entityを一回だけ回収する。
	std::vector<uint64_t> ConsumeFinishedEntityIds(const SceneDocument& document);
	void ApplyRequests(const SceneDocument& document, const std::vector<SceneAudioRequest>& requests);
	void ApplyProcessPolicy(
		const SceneDocument& document,
		const std::function<bool(uint64_t)>& shouldProcess
	);
	void PrepareForSceneTransition();
	void Clear();

private:
	struct Binding {
		Audio::PlaybackHandle handle;
		AudioClipPtr clip;
		uint64_t entityId = 0;
		uint64_t componentLocalId = 0;
		std::string clipPath;
		std::string spatialMode;
		bool wasActive = false;
		bool streamFromDisk = false;
		bool persistent = false;
		bool eventPaused = false;
		bool policyPaused = false;
	};

	static uint64_t MakeBindingKey(uint64_t entityId, uint64_t componentLocalId);
	void Preload(const SceneComponent& component, Binding& binding);
	AudioClipPtr PrepareClip(const SceneComponent& component, Binding& binding);
	Audio::PlaybackHandle Play(const SceneComponent& component, Binding& binding);
	std::unordered_map<uint64_t, Binding> bindings_;
	bool playSessionStarted_ = false;
	bool activeScene_ = false;
	uint64_t sceneOwnerId_ = 0;
};
