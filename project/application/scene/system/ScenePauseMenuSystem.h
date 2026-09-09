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
	bool respawnRequested = false;
	std::string requestedSceneId;
};

// Gameplay中にだけ使う、Escで開閉するポーズメニュー。
class ScenePauseMenuSystem {
public:
	ScenePauseMenuResult Update(
		const SceneDocument& document,
		const ScenePauseSystem& pauseSystem,
		SceneOptionMenuSystem& optionMenuSystem,
		float deltaTime
	);
	void ApplyTextOverrides(
		const SceneDocument& document,
		SceneTextRenderSystem& textRenderSystem
	) const;
	void Clear();

private:
	struct MenuItem {
		enum class Action {
			SceneTransition,
			Respawn,
			OpenOptions
		};

		uint64_t entityId = 0;
		Action action = Action::SceneTransition;
		std::string targetSceneId;
	};

	std::vector<MenuItem> CollectMenuItems(const SceneDocument& document) const;
	uint64_t FindControllerEntityId(const SceneDocument& document) const;
	void UpdateVisibility(bool visible, float deltaTime);
	float GetPresentationProgress(int animationOrder) const;
	bool optionOpen_ = false;
	int selectedIndex_ = 0;
	float visibilityProgress_ = 0.0f;
	bool visibilityTargetVisible_ = false;
};
