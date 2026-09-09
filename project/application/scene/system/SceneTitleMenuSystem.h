// 役割: Title Scene専用の初期メニュー選択と表示強調を管理する。
#pragma once

#include "../../../engine/math/Vector4.h"

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;
class SceneTextRenderSystem;

struct SceneTitleMenuResult {
	std::string requestedSceneId; // 決定操作で要求された遷移先Scene ID。
	bool useSceneTransitionEffect = true; // 通常のScene切り替え演出を使うか。
	bool exitRequested = false; // 決定操作で要求されたゲーム終了。
};

// Title Sceneのメニュー入力とTextRendererの選択表示だけを担当する。
class SceneTitleMenuSystem {
public:
	/// <summary>
	/// タイトルメニュー入力を更新し、遷移または終了要求を返します。
	/// </summary>
	SceneTitleMenuResult Update(const SceneDocument& document);

	/// <summary>
	/// 現在の選択状態をTextRendererの表示文字列と色へ反映します。
	/// </summary>
	void ApplyTextOverrides(
		const SceneDocument& document,
		SceneTextRenderSystem& textRenderSystem
	) const;

	/// <summary>
	/// タイトルシーン外へ出たときに選択状態を初期化します。
	/// </summary>
	void Clear();

private:
	struct MenuItem {
		uint64_t entityId = 0; // 表示を上書きするTextRenderer Entity ID。
		std::string label; // メニューに表示する項目名。
		std::string targetSceneId; // 決定時に遷移するScene ID。
		bool useSceneTransitionEffect = true; // 通常のScene切り替え演出を使うか。
		bool exitRequested = false; // 決定時にゲーム終了を要求する項目か。
	};

	/// <summary>
	/// タイトルシーン上のメニューEntityを表示順に収集します。
	/// </summary>
	std::vector<MenuItem> CollectMenuItems(
		const SceneDocument& document
	) const;

	int selectedIndex_ = 0; // 現在選択中のメニュー項目Index。
};
