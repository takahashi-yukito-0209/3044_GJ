// 役割: Title Sceneのメニュー入力、遷移要求、選択中表示を処理する。
#include "SceneTitleMenuSystem.h"

#include "SceneTextRenderSystem.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <array>
#include <initializer_list>

namespace {
	struct MenuDefinition {
		const char* entityName; // Scene上で対応するTextRenderer Entity名。
		const char* targetSceneId; // 決定時に要求するScene ID。
		bool exitRequested; // 決定時にゲーム終了を要求する項目か。
	};

	constexpr std::array<MenuDefinition, 5> kMenuDefinitions = { {
		{ "TitleMenuStartText", "gameplay", false },
		{ "TitleMenuTutorialText", "tutorial", false },
		{ "TitleMenuOptionText", "option", false },
		{ "TitleMenuCreditText", "credit", false },
		{ "TitleMenuExitText", "", true },
	} };

	constexpr Vector4 kSelectedColor = { 1.0f, 0.92f, 0.55f, 1.0f };
	constexpr Vector4 kNormalColor = { 0.78f, 0.86f, 0.94f, 1.0f };

	/// <summary>
	/// 複数候補キーのいずれかが押された瞬間かを判定します。
	/// </summary>
	bool TriggerAnyKey(Input* input, std::initializer_list<BYTE> keyCodes) {
		if (!input) {
			return false;
		}
		for (BYTE keyCode : keyCodes) { // 判定するDirectInputキーコード。
			if (input->TriggerKey(keyCode)) {
				return true;
			}
		}
		return false;
	}
}

SceneTitleMenuResult SceneTitleMenuSystem::Update(
	const SceneDocument& document
) {
	SceneTitleMenuResult result{}; // 呼び出し元へ返す遷移要求。
	const std::vector<MenuItem> menuItems = CollectMenuItems(document); // 有効なメニュー項目。
	if (menuItems.empty()) {
		selectedIndex_ = 0;
		return result;
	}

	const int itemCount = static_cast<int>(menuItems.size()); // 選択可能な項目数。
	selectedIndex_ = std::clamp(selectedIndex_, 0, itemCount - 1);

	Input* input = Input::GetInstance(); // 入力状態の参照。
	if (TriggerAnyKey(input, { DIK_UP, DIK_W })) {
		selectedIndex_ = (selectedIndex_ + itemCount - 1) % itemCount;
	} else if (TriggerAnyKey(input, { DIK_DOWN, DIK_S })) {
		selectedIndex_ = (selectedIndex_ + 1) % itemCount;
	}

	if (TriggerAnyKey(input, { DIK_RETURN, DIK_SPACE })) {
		const MenuItem& selectedItem = menuItems[selectedIndex_]; // 決定されたメニュー項目。
		if (selectedItem.exitRequested) {
			result.exitRequested = true;
		} else {
			result.requestedSceneId = selectedItem.targetSceneId;
		}
	}
	return result;
}

void SceneTitleMenuSystem::ApplyTextOverrides(
	const SceneDocument& document,
	SceneTextRenderSystem& textRenderSystem
) const {
	const std::vector<MenuItem> menuItems = CollectMenuItems(document); // 有効なメニュー項目。
	if (menuItems.empty()) {
		return;
	}

	const int itemCount = static_cast<int>(menuItems.size()); // 選択可能な項目数。
	const int selectedIndex = std::clamp(selectedIndex_, 0, itemCount - 1); // 表示用の選択Index。
	for (int index = 0; index < itemCount; ++index) { // 表示を更新する項目Index。
		const MenuItem& item = menuItems[index]; // 表示対象のメニュー項目。
		const bool selected = index == selectedIndex; // この項目が選択中か。
		textRenderSystem.SetTextOverride(
			item.entityId,
			selected ? "> " + item.label : "  " + item.label
		);
		textRenderSystem.SetTextColorOverride(
			item.entityId,
			selected ? kSelectedColor : kNormalColor
		);
	}
}

void SceneTitleMenuSystem::Clear() {
	selectedIndex_ = 0;
}

std::vector<SceneTitleMenuSystem::MenuItem>
SceneTitleMenuSystem::CollectMenuItems(const SceneDocument& document) const {
	std::vector<MenuItem> menuItems; // Sceneから見つかったメニュー項目。
	menuItems.reserve(kMenuDefinitions.size());

	for (const MenuDefinition& definition : kMenuDefinitions) { // 登録済み項目定義。
		const SceneEntity* entity =
			document.FindEntityByName(definition.entityName); // 対応Entity。
		const SceneComponent* textRenderer =
			entity && SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)
			? SceneEntityQuery::FindEnabledComponent(*entity, "TextRenderer")
			: nullptr; // 表示用TextRenderer。
		if (!entity || !textRenderer) {
			continue;
		}
		menuItems.push_back({
			entity->id,
			textRenderer->textValue,
			definition.targetSceneId,
			definition.exitRequested
		});
	}
	return menuItems;
}
