// 役割: 共有Audio Clip、Clip cache、XAudio2 Source Voiceの寿命を管理する。
#pragma once
#include "AudioClip.h"
#include "../math/Vector3.h"

#include <xaudio2.h>
#include <x3daudio.h>
#pragma comment(lib, "xaudio2.lib")

#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl.h>

class MediaFoundationRuntime;
class AudioDirector;
class AudioStreamDecoder;

enum class AudioBus : uint8_t {
	Master,
	BGM,
	SFX,
	UI,
	Ambience
};

struct AudioPlaybackOptions {
	AudioBus bus = AudioBus::SFX;
	float volume = 1.0f;
	float pitch = 1.0f;
	bool loop = false;
	// Scene Runtimeだけが自然終了をEventへ接続するための内部opt-in。
	bool trackNaturalCompletion = false;
};

struct PersistentBgmRequest {
	uint64_t sceneOwnerId = 0;
	uint64_t componentKey = 0;
	std::string clipPath;
	AudioPlaybackOptions options{ AudioBus::BGM, 1.0f, 1.0f, true };
	float fadeSeconds = 0.5f;
};

enum class AudioSpatialLayout : uint8_t {
	Point,
	StereoArea
};

struct AudioSpatialParameters {
	Vector3 listenerPosition{};
	Vector3 listenerFront = { 0.0f, 0.0f, 1.0f };
	Vector3 listenerTop = { 0.0f, 1.0f, 0.0f };
	Vector3 emitterPosition{};
	Vector3 emitterFront = { 0.0f, 0.0f, 1.0f };
	Vector3 emitterTop = { 0.0f, 1.0f, 0.0f };
	float minimumDistance = 1.4f;
	float maximumDistance = 30.0f;
	AudioSpatialLayout layout = AudioSpatialLayout::Point;
	float stereoAreaWidth = 1.0f;
};

class Audio{
public:
	// 旧WAV Preview APIの型名は既存呼び出しとの互換用に維持する。
	using SoundData = AudioClip;
	using SoundDataPtr = AudioClipPtr;

	class PlaybackHandle{
	public:
		bool IsValid() const { return id_ != 0; }

	private:
		friend class Audio;
		uint64_t id_ = 0;
	};

	Audio();
	~Audio();
	Audio(const Audio&) = delete;
	Audio& operator=(const Audio&) = delete;

	// 初期化済みMF Runtimeを参照し、XAudio2とMastering Voiceを生成する。
	void Initialize(const MediaFoundationRuntime* mediaFoundationRuntime);

	// 全Source Voiceを停止・破棄してからXAudio2を終了する。
	void Finalize();

	// callback threadが通知した自然終了VoiceをMain Threadで破棄する。
	void Update(float realDeltaTime = 0.0f);

	// 対応音声を共有Audio ClipへFull Decodeする。失敗内容はErrorへ返す。
	AudioClipPtr LoadAudioFile(
		const char* filename,
		std::string* error = nullptr
	);

	// Media Foundationが利用可能な場合だけ、PCMを展開せず実際の出力Formatを返す。
	bool CanProbeAudioFileMetadata() const;
	bool TryGetAudioFileMetadata(
		const char* filename,
		AudioFileMetadata& metadata,
		std::string* error = nullptr
	);

	// stereo整数PCMから一点定位用の共有mono Clipを作成または再利用する。
	// 元Clipは変更せず、返値の寿命は呼び出し側とPlaybackStateが保持する。
	AudioClipPtr GetOrCreatePointDownmixClip(
		const AudioClipPtr& audioClip,
		std::string* error = nullptr
	);

	// 共有Clipの寿命を保持して再生を開始する。失敗時は無効Handleを返す。
	PlaybackHandle PlayAudioClip(
		const AudioClipPtr& audioClip,
		const AudioPlaybackOptions& options = {}
	);

	// local Audio FileをFull Decodeせず、Workerと固定slotで逐次再生する。
	PlaybackHandle PlayAudioStream(
		const char* filename,
		const AudioPlaybackOptions& options = {},
		std::string* error = nullptr
	);

	// Persistent BGMの選曲とScene遷移中の所有をProcess lifetimeのDirectorへ委譲する。
	void ResolvePersistentBgm(const PersistentBgmRequest& request);
	void ResolveNoPersistentBgm(uint64_t sceneOwnerId);
	void BeginPersistentBgmSceneTransition(uint64_t sceneOwnerId);
	void ReleasePersistentBgmOwner(uint64_t sceneOwnerId);
	void StopPersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey);
	void PausePersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey);
	void ResumePersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey);

	// BusとSourceのgainを乗算して各Voiceへ反映する。Master以外のBusは個別に調整する。
	void SetBusVolume(AudioBus bus, float volume);
	void Pause(PlaybackHandle& handle);
	void Resume(PlaybackHandle& handle);
	void SetPlaybackFadeGain(const PlaybackHandle& handle, float gain);
	void SetPlaybackProperties(
		const PlaybackHandle& handle,
		float volume,
		float pitch,
		bool loop
	);
	bool IsPlaybackStarted(const PlaybackHandle& handle) const;
	// Main Threadで、このHandle generationの自然終了を一回だけ回収する。
	bool ConsumeNaturalCompletion(const PlaybackHandle& handle);
	// Persistent BGMのcurrent Trackに対応する自然終了をScene bindingへ一回だけ渡す。
	bool ConsumePersistentBgmCompletion(
		uint64_t sceneOwnerId,
		uint64_t componentKey
	);

	// Scene worldの左手系座標をそのまま使い、PointまたはStereo Areaの定位と距離減衰を反映する。
	// falseは無効Handle、3D未初期化、またはClip／Layout不一致を表す。
	bool SetSpatialParameters(
		const PlaybackHandle& handle,
		const AudioSpatialParameters& parameters
	);

	// 既存WAV Preview呼び出しを共通Clip経路へ接続する互換API。
	SoundDataPtr SoundLoadWave(const char* filename);

	// 既存WAV Preview呼び出しを共通再生経路へ接続する互換API。
	PlaybackHandle SoundPlayWave(const SoundDataPtr& soundData);

	// 再生を停止してVoiceを破棄する。停止済みHandleへの呼び出しは何もしない。
	void SoundStop(PlaybackHandle& handle);

	bool IsPlaying(const PlaybackHandle& handle) const;

	static Audio* GetInstance();

private:
	class VoiceCallback final : public IXAudio2VoiceCallback {
	public:
		explicit VoiceCallback(Audio* owner) : owner_(owner) {}

		void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
		void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
		void STDMETHODCALLTYPE OnStreamEnd() override {}
		void STDMETHODCALLTYPE OnBufferStart(void*) override {}
		void STDMETHODCALLTYPE OnBufferEnd(void* bufferContext) override;
		void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
		void STDMETHODCALLTYPE OnVoiceError(
			void* bufferContext,
			HRESULT
		) override;

	private:
		Audio* owner_ = nullptr;
	};

	struct BufferCallbackContext {
		uint64_t playbackId = 0;
		uint32_t slotIndex = 0;
		bool streaming = false;
		bool endOfStream = false;
	};

	struct CallbackEvent {
		BufferCallbackContext context{};
		bool error = false;
	};

	struct PlaybackState {
		IXAudio2SourceVoice* voice = nullptr;
		AudioClipPtr audioClip;
		std::unique_ptr<AudioStreamDecoder> streamDecoder;
		std::unique_ptr<BufferCallbackContext> clipContext;
		std::array<std::unique_ptr<BufferCallbackContext>, 4> streamContexts{};
		AudioBus bus = AudioBus::SFX;
		float sourceVolume = 1.0f;
		float fadeGain = 1.0f;
		float pitch = 1.0f;
		bool loop = false;
		bool paused = false;
		bool trackNaturalCompletion = false;
		uint32_t submittedStreamBuffers = 0;
		bool voiceStarted = false;
		bool streaming = false;
	};

	struct AudioMetadataCacheEntry {
		uintmax_t fileSize = 0;
		std::filesystem::file_time_type lastWriteTime{};
		bool succeeded = false;
		AudioFileMetadata metadata{};
		std::string error;
	};

	void QueueCallbackEvent(const BufferCallbackContext& context, bool error);
	void ProcessCallbackEvents();
	void UpdateStreamingPlaybacks();
	void CollectRetiredPlaybacks(bool waitForAll);
	void DestroyPlayback(uint64_t playbackId);
	void DestroyPlaybackVoice(PlaybackState& playback);
	float GetEffectiveVolume(const PlaybackState& playback) const;
	void ApplyVolume(PlaybackState& playback);

	static Audio* instance;
	Microsoft::WRL::ComPtr<IXAudio2> xAudio2_;
	IXAudio2MasteringVoice* masterVoice_ = nullptr;
	X3DAUDIO_HANDLE x3dAudio_{};
	UINT32 masteringChannelCount_ = 0;
	bool x3dAudioInitialized_ = false;
	const MediaFoundationRuntime* mediaFoundationRuntime_ = nullptr;
	VoiceCallback voiceCallback_;
	std::unique_ptr<AudioDirector> audioDirector_;
	uint64_t nextPlaybackId_ = 1;
	float busVolumes_[5] = { 1.0f, 0.6f, 0.6f, 0.6f, 0.6f };
	std::unordered_map<uint64_t, std::unique_ptr<PlaybackState>> playbacks_;
	std::vector<std::unique_ptr<PlaybackState>> retiringPlaybacks_;
	std::unordered_map<std::string, std::weak_ptr<const AudioClip>> clipCache_;
	std::unordered_map<std::string, std::weak_ptr<const AudioClip>> pointDownmixClipCache_;
	std::unordered_map<std::string, AudioMetadataCacheEntry> metadataCache_;
	std::unordered_set<uint64_t> naturallyCompletedPlaybackIds_;
	std::mutex callbackEventMutex_;
	std::vector<CallbackEvent> callbackEvents_;
};
