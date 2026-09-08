// 役割: Gameplay Sceneのポーズ入力、選択状態、表示上書きを管理する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;
class SceneOptionMenuSystem;
class ScenePauseSystem;
class SceneTextRenderSystem;

struct ScenePauseMenuResult {
	bool pauseRequested = false;
	bool resumeRequested = false;
	std::string requestedSceneId;
};

// Gameplay中にだけ使う、Escで開閉するポーズメニュー。
class ScenePauseMenuSystem {
public:
	ScenePauseMenuResult Update(
		const SceneDocument& document,
		const ScenePauseSystem& pauseSystem,
		SceneOptionMenuSystem& optionMenuSystem
	);
	void ApplyTextOverrides(
		const SceneDocument& document,
		SceneTextRenderSystem& textRenderSystem,
		bool pauseActive,
		const SceneOptionMenuSystem& optionMenuSystem
	) const;
	void Clear();

private:
	struct MenuItem {
		uint64_t entityId = 0;
		std::string targetSceneId;
	};

	std::vector<MenuItem> CollectMenuItems(const SceneDocument& document) const;
	uint64_t FindControllerEntityId(const SceneDocument& document) const;
	bool optionOpen_ = false;
	int selectedIndex_ = 0;
};
