// 役割: Scene System内の瞬間的なSEを既存Audio APIへ接続する。
#pragma once

#include "../../../engine/Audio/Audio.h"

#include <string>
#include <unordered_map>

namespace SceneSoundEffectPlayer {
namespace Detail {
	/// <summary>
	/// 指定したAudio Clipを短いSEとして一度だけ再生します。
	/// </summary>
	inline void PlayOneShot(const char* clipPath, AudioBus bus) {
		Audio* audio = Audio::GetInstance(); // Audio再生を管理する共有インスタンス。
		if (!audio || !clipPath || clipPath[0] == '\0') {
			return;
		}

		static std::unordered_map<std::string, AudioClipPtr> clipCache; // Decode済みSEを保持するCache。
		const std::string cacheKey = clipPath; // Clip Cache検索用のResource Path。
		AudioClipPtr clip; // 今回再生する共有Audio Clip。

		const auto found = clipCache.find(cacheKey); // Cache上の既存Clip。
		if (found != clipCache.end()) {
			clip = found->second;
		}
		if (!clip) {
			std::string error; // 読み込み失敗時の診断文字列。
			clip = audio->LoadAudioFile(clipPath, &error);
			if (!clip) {
				return;
			}
			clipCache[cacheKey] = clip;
		}

		AudioPlaybackOptions options{}; // One Shot再生用の設定。
		options.bus = bus;
		options.loop = false;
		audio->PlayAudioClip(clip, options);
	}
}

/// <summary>
/// 決定操作のSEを再生します。
/// </summary>
inline void PlayDecision() {
	Detail::PlayOneShot("resources/decision.mp3", AudioBus::UI);
}

/// <summary>
/// UI選択項目を移動した時のSEを再生します。
/// </summary>
inline void PlaySelect() {
	Detail::PlayOneShot("resources/ui_se.mp3", AudioBus::UI);
}

/// <summary>
/// 釣りの魚数増減SEを再生します。
/// </summary>
inline void PlayFishSelect() {
	Detail::PlayOneShot("resources/fish_select.mp3", AudioBus::UI);
}

/// <summary>
/// 釣り針に当たった時のSEを再生します。
/// </summary>
inline void PlayFishingScore() {
	Detail::PlayOneShot("resources/fishing_se.mp3", AudioBus::SFX);
}

/// <summary>
/// サメに当たった時のSEを再生します。
/// </summary>
inline void PlaySharkEat() {
	Detail::PlayOneShot("resources/eat.mp3", AudioBus::SFX);
}
}
