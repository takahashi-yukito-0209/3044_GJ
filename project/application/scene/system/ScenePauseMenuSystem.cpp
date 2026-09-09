// 役割: Gameplay Sceneのポーズメニュー操作とTextRenderer表示を実装する。
#include "ScenePauseMenuSystem.h"

#include "SceneOptionMenuSystem.h"
#include "ScenePauseSystem.h"
#include "SceneTextRenderSystem.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace {
	constexpr const char* kPauseControllerName = "Pause Menu Controller";
	constexpr const char* kPauseProfileId = "PauseMenu";
	constexpr const char* kPauseRequestId = "PauseMenu";
	constexpr std::array<const char*, 3> kMainMenuEntityNames = { {
		"PauseTitleText", "PauseRestartText", "PauseOptionText"
	} };
	constexpr std::array<const char*, 3> kOptionMenuEntityNames = { {
		"PauseOptionBgmText", "PauseOptionSeText", "PauseOptionBackText"
	} };
	constexpr Vector4 kSelectedColor = { 1.0f, 0.92f, 0.55f, 1.0f };
	constexpr Vector4 kNormalColor = { 0.78f, 0.86f, 0.94f, 1.0f };
	constexpr float kVisibilityAnimationSeconds = 0.36f;
	constexpr float kVisibilityStartOffsetY = 24.0f;
	constexpr float kVisibilityStagger = 0.30f;

	bool TriggerAnyKey(Input* input, std::initializer_list<BYTE> keyCodes) {
		if (!input) {
			return false;
		}
		for (BYTE keyCode : keyCodes) {
			if (input->TriggerKey(keyCode)) {
				return true;
			}
		}
		return false;
	}

	float EaseInOutCubic(float value) {
		const float clamped = std::clamp(value, 0.0f, 1.0f);
		return clamped < 0.5f
			? 4.0f * clamped * clamped * clamped
			: 1.0f - std::pow(-2.0f * clamped + 2.0f, 3.0f) * 0.5f;
	}
}

ScenePauseMenuResult ScenePauseMenuSystem::Update(
	const SceneDocument& document,
	const ScenePauseSystem& pauseSystem,
	SceneOptionMenuSystem& optionMenuSystem,
	float deltaTime
) {
	ScenePauseMenuResult result{};
	const uint64_t controllerEntityId = FindControllerEntityId(document);
	if (controllerEntityId == 0) {
		UpdateVisibility(false, deltaTime);
		return result;
	}

	Input* input = Input::GetInstance();
	const bool pauseActive = pauseSystem.IsPauseActive(
		controllerEntityId, kPauseProfileId, kPauseRequestId
	);
	bool showHud = pauseActive;
	if (TriggerAnyKey(input, { DIK_ESCAPE })) {
		if (pauseActive) {
			if (optionOpen_) {
				optionOpen_ = false;
				selectedIndex_ = 0;
			} else {
				result.resumeRequested = true;
				showHud = false;
			}
		} else {
			optionOpen_ = false;
			selectedIndex_ = 0;
			result.pauseRequested = true;
			showHud = true;
		}
		UpdateVisibility(showHud, deltaTime);
		return result;
	}
	UpdateVisibility(showHud, deltaTime);
	if (!pauseActive) {
		return result;
	}

	const std::vector<MenuItem> menuItems = CollectMenuItems(document);
	if (menuItems.empty()) {
		selectedIndex_ = 0;
		return result;
	}
	const int itemCount = static_cast<int>(menuItems.size());
	selectedIndex_ = std::clamp(selectedIndex_, 0, itemCount - 1);
	if (TriggerAnyKey(input, { DIK_UP, DIK_W })) {
		selectedIndex_ = (selectedIndex_ + itemCount - 1) % itemCount;
	} else if (TriggerAnyKey(input, { DIK_DOWN, DIK_S })) {
		selectedIndex_ = (selectedIndex_ + 1) % itemCount;
	}
	const MenuItem& selectedItem = menuItems[selectedIndex_];
	if (optionOpen_) {
		if (TriggerAnyKey(input, { DIK_LEFT, DIK_A })) {
			if (selectedIndex_ == 0) {
				optionMenuSystem.AdjustBgmVolume(-10);
			} else if (selectedIndex_ == 1) {
				optionMenuSystem.AdjustSeVolume(-10);
			}
		} else if (TriggerAnyKey(input, { DIK_RIGHT, DIK_D })) {
			if (selectedIndex_ == 0) {
				optionMenuSystem.AdjustBgmVolume(10);
			} else if (selectedIndex_ == 1) {
				optionMenuSystem.AdjustSeVolume(10);
			}
		}
		if (selectedIndex_ == 2 &&
			TriggerAnyKey(input, { DIK_RETURN, DIK_SPACE })) {
			optionOpen_ = false;
			selectedIndex_ = 0;
		}
	} else if (TriggerAnyKey(input, { DIK_RETURN, DIK_SPACE })) {
		if (selectedIndex_ == 2) {
			optionOpen_ = true;
			selectedIndex_ = 0;
		} else {
			result.requestedSceneId = selectedItem.targetSceneId;
		}
	}
	return result;
}

void ScenePauseMenuSystem::ApplyTextOverrides(
	const SceneDocument& document,
	SceneTextRenderSystem& textRenderSystem
) const {
	const auto applyVisibility = [this, &document, &textRenderSystem](
		const char* name,
		bool visible,
		int animationOrder
	) {
		const SceneEntity* entity = document.FindEntityByName(name);
		if (!entity || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) ||
			!SceneEntityQuery::FindEnabledComponent(*entity, "TextRenderer")) {
			return;
		}
		const float presentationProgress =
			GetPresentationProgress(animationOrder);
		const float presentationScale = EaseInOutCubic(presentationProgress);
		const Vector2 presentationOffset = {
			0.0f,
			(1.0f - presentationScale) * kVisibilityStartOffsetY
		};
		textRenderSystem.SetPresentationOverride(
			entity->id, presentationOffset, 0.0f,
			{ presentationScale, presentationScale },
			visible ? presentationProgress : 0.0f
		);
	};
	if (optionOpen_) {
		const SceneEntity* header = document.FindEntityByName("PauseMenuHeaderText");
		if (header && SceneEntityQuery::IsEntityActiveInHierarchy(document, *header) &&
			SceneEntityQuery::FindEnabledComponent(*header, "TextRenderer")) {
			textRenderSystem.SetTextOverride(header->id, "おぷしょん");
		}
	}
	applyVisibility("PauseMenuHeaderText", true, 0);
	applyVisibility("PauseMenuGuideText", true, 4);
	for (size_t index = 0; index < kMainMenuEntityNames.size(); ++index) {
		applyVisibility(kMainMenuEntityNames[index], !optionOpen_,
			static_cast<int>(index) + 1);
	}
	for (size_t index = 0; index < kOptionMenuEntityNames.size(); ++index) {
		applyVisibility(kOptionMenuEntityNames[index], optionOpen_,
			static_cast<int>(index) + 1);
	}

	const std::vector<MenuItem> menuItems = CollectMenuItems(document);
	const int itemCount = static_cast<int>(menuItems.size());
	const int selectedIndex = itemCount > 0
		? std::clamp(selectedIndex_, 0, itemCount - 1)
		: 0;
	for (int index = 0; index < itemCount; ++index) {
		const bool selected = index == selectedIndex;
		const float presentationProgress =
			GetPresentationProgress(index + 1);
		const float presentationScale = EaseInOutCubic(presentationProgress);
		const Vector2 presentationOffset = {
			0.0f,
			(1.0f - presentationScale) * kVisibilityStartOffsetY
		};
		textRenderSystem.SetPresentationOverride(
			menuItems[index].entityId, presentationOffset, 0.0f,
			{ presentationScale, presentationScale },
			presentationProgress
		);
		textRenderSystem.SetTextColorOverride(
			menuItems[index].entityId, selected ? kSelectedColor : kNormalColor
		);
	}
}

void ScenePauseMenuSystem::Clear() {
	optionOpen_ = false;
	selectedIndex_ = 0;
	visibilityProgress_ = 0.0f;
	visibilityTargetVisible_ = false;
}

void ScenePauseMenuSystem::UpdateVisibility(bool visible, float deltaTime) {
	visibilityTargetVisible_ = visible;
	const float progressDelta = (std::max)(deltaTime, 0.0f) /
		kVisibilityAnimationSeconds;
	visibilityProgress_ = std::clamp(
		visibilityProgress_ + (visible ? progressDelta : -progressDelta),
		0.0f,
		1.0f
	);
}

float ScenePauseMenuSystem::GetPresentationProgress(int animationOrder) const {
	const float stagger = std::clamp(
		static_cast<float>(animationOrder) * kVisibilityStagger,
		0.0f,
		0.9f
	);
	if (visibilityTargetVisible_) {
		return std::clamp(
			(visibilityProgress_ - stagger) / (1.0f - stagger),
			0.0f,
			1.0f
		);
	}
	return std::clamp(
		visibilityProgress_ / (1.0f - stagger),
		0.0f,
		1.0f
	);
}

std::vector<ScenePauseMenuSystem::MenuItem>
ScenePauseMenuSystem::CollectMenuItems(const SceneDocument& document) const {
	constexpr std::array<const char*, 3> kTargetSceneIds = { {
		"title", "gameplay", "option"
	} };
	std::vector<MenuItem> items;
	const auto& names = optionOpen_ ? kOptionMenuEntityNames : kMainMenuEntityNames;
	items.reserve(names.size());
	for (size_t index = 0; index < names.size(); ++index) {
		const SceneEntity* entity = document.FindEntityByName(names[index]);
		if (!entity || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) ||
			!SceneEntityQuery::FindEnabledComponent(*entity, "TextRenderer")) {
			continue;
		}
		items.push_back({ entity->id, kTargetSceneIds[index] });
	}
	return items;
}

uint64_t ScenePauseMenuSystem::FindControllerEntityId(
	const SceneDocument& document
) const {
	const SceneEntity* controller = document.FindEntityByName(kPauseControllerName);
	return controller && SceneEntityQuery::IsEntityActiveInHierarchy(document, *controller) &&
		SceneEntityQuery::FindEnabledComponent(*controller, "PauseController")
		? controller->id
		: 0;
}
