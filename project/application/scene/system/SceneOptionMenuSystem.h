// 役割: Option Scene専用の音量メニュー入力と表示更新を管理する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;
class SceneTextRenderSystem;

struct SceneOptionMenuResult {
	std::string requestedSceneId; // 決定操作で要求された遷移先Scene ID。
};

// Option Sceneの音量変更と戻る項目の選択状態を担当する。
class SceneOptionMenuSystem {
public:
	/// <summary>
	/// オプションメニュー入力を更新し、戻る決定時の遷移先Scene IDを返します。
	/// </summary>
	SceneOptionMenuResult Update(const SceneDocument& document);

	/// <summary>
	/// 現在の設定値と選択状態をTextRendererへ反映します。
	/// </summary>
	void ApplyTextOverrides(
		const SceneDocument& document,
		SceneTextRenderSystem& textRenderSystem
	) const;

	/// <summary>
	/// オプションシーン外へ出たときに選択状態を初期化します。
	/// </summary>
	void Clear();

	// ポーズメニューからも同じ音量設定を操作できるように公開する。
	void AdjustBgmVolume(int deltaPercent);
	void AdjustSeVolume(int deltaPercent);
	int GetBgmVolumePercent() const;
	int GetSeVolumePercent() const;

private:
	struct MenuItem {
		uint64_t entityId = 0; // 表示を上書きするTextRenderer Entity ID。
		std::string actionId; // 入力処理で使う項目種別。
	};

	/// <summary>
	/// オプションシーン上のメニューEntityを表示順に収集します。
	/// </summary>
	std::vector<MenuItem> CollectMenuItems(
		const SceneDocument& document
	) const;

	/// <summary>
	/// 指定項目の音量を増減し、Audio Busへ反映します。
	/// </summary>
	void AdjustVolume(const std::string& actionId, int deltaPercent);

	/// <summary>
	/// 現在の音量設定をAudio Busへ反映します。
	/// </summary>
	void ApplyAudioVolumes() const;

	int selectedIndex_ = 0; // 現在選択中のメニュー項目Index。
};
