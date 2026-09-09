// 役割: Option Sceneの音量調整、戻る遷移要求、選択中表示を処理する。
#include "SceneOptionMenuSystem.h"

#include "SceneSoundEffectPlayer.h"
#include "SceneTextRenderSystem.h"
#include "../../../engine/Audio/Audio.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <array>
#include <initializer_list>

namespace {
	struct OptionMenuDefinition {
		const char* entityName; // Scene上で対応するTextRenderer Entity名。
		const char* actionId; // 入力処理で使う項目種別。
	};

	constexpr std::array<OptionMenuDefinition, 3> kOptionMenuDefinitions = { {
		{ "OptionBgmVolumeText", "BGM" },
		{ "OptionSeVolumeText", "SE" },
		{ "OptionBackText", "Back" },
	} };

	constexpr int kVolumeStepPercent = 10;
	constexpr int kMinimumVolumePercent = 0;
	constexpr int kMaximumVolumePercent = 100;
	constexpr int kDefaultVolumePercent = 60;
	constexpr Vector4 kSelectedColor = { 1.0f, 0.92f, 0.55f, 1.0f };
	constexpr Vector4 kNormalColor = { 0.78f, 0.86f, 0.94f, 1.0f };

	int gBgmVolumePercent = kDefaultVolumePercent; // BGM Busへ反映する音量値。
	int gSeVolumePercent = kDefaultVolumePercent; // SFX Busへ反映する音量値。

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

	/// <summary>
	/// 0から100の音量値をAudio Bus用の0.0から1.0へ変換します。
	/// </summary>
	float ToVolumeGain(int volumePercent) {
		const int clampedPercent = std::clamp(
			volumePercent,
			kMinimumVolumePercent,
			kMaximumVolumePercent
		); // Audioへ渡す前に制限した音量値。
		return static_cast<float>(clampedPercent) / 100.0f;
	}

	/// <summary>
	/// 音量メニュー用の表示文字列を作成します。
	/// </summary>
	std::string BuildVolumeText(const char* label, int volumePercent) {
		const int clampedPercent = std::clamp(
			volumePercent,
			kMinimumVolumePercent,
			kMaximumVolumePercent
		); // 表示に使う0から100の音量値。
		return std::string(label) + " 音量 < " +
			std::to_string(clampedPercent) + "% >";
	}

}

SceneOptionMenuResult SceneOptionMenuSystem::Update(
	const SceneDocument& document
) {
	SceneOptionMenuResult result{}; // 呼び出し元へ返す遷移要求。
	const std::vector<MenuItem> menuItems = CollectMenuItems(document); // 有効なメニュー項目。
	if (menuItems.empty()) {
		selectedIndex_ = 0;
		return result;
	}

	const int itemCount = static_cast<int>(menuItems.size()); // 選択可能な項目数。
	selectedIndex_ = std::clamp(selectedIndex_, 0, itemCount - 1);

	Input* input = Input::GetInstance(); // 入力状態の参照。
	const int previousSelectedIndex = selectedIndex_; // 入力前の選択項目Index。
	if (TriggerAnyKey(input, { DIK_UP, DIK_W })) {
		selectedIndex_ = (selectedIndex_ + itemCount - 1) % itemCount;
	} else if (TriggerAnyKey(input, { DIK_DOWN, DIK_S })) {
		selectedIndex_ = (selectedIndex_ + 1) % itemCount;
	}
	if (selectedIndex_ != previousSelectedIndex) {
		SceneSoundEffectPlayer::PlaySelect();
	}

	const MenuItem& selectedItem = menuItems[selectedIndex_]; // 現在操作対象の項目。
	if (TriggerAnyKey(input, { DIK_LEFT, DIK_A })) {
		AdjustVolume(selectedItem.actionId, -kVolumeStepPercent);
	} else if (TriggerAnyKey(input, { DIK_RIGHT, DIK_D })) {
		AdjustVolume(selectedItem.actionId, kVolumeStepPercent);
	}

	ApplyAudioVolumes();

	if (
		selectedItem.actionId == "Back" &&
		TriggerAnyKey(input, { DIK_RETURN, DIK_SPACE })
	) {
		SceneSoundEffectPlayer::PlayDecision();
		result.requestedSceneId = "title";
		result.useSceneTransitionEffect = false;
	}
	if (TriggerAnyKey(input, { DIK_ESCAPE })) {
		result.requestedSceneId = "title";
		result.useSceneTransitionEffect = false;
	}
	return result;
}

void SceneOptionMenuSystem::ApplyTextOverrides(
	const SceneDocument& document,
	SceneTextRenderSystem& textRenderSystem
) const {
	const std::vector<MenuItem> menuItems = CollectMenuItems(document); // 有効なメニュー項目。
	if (menuItems.empty()) {
		return;
	}

	const int itemCount = static_cast<int>(menuItems.size()); // 選択可能な項目数。
	const int selectedIndex = std::clamp(selectedIndex_, 0, itemCount - 1); // 表示用の選択Index。
	for (int index = 0; index < itemCount; ++index) { // 選択色を更新する項目Index。
		const MenuItem& item = menuItems[index]; // 表示対象のメニュー項目。
		const bool selected = index == selectedIndex; // この項目が選択中か。
		if (item.actionId == "BGM") {
			textRenderSystem.SetTextOverride(
				item.entityId,
				BuildVolumeText("BGM", gBgmVolumePercent)
			);
		} else if (item.actionId == "SE") {
			textRenderSystem.SetTextOverride(
				item.entityId,
				BuildVolumeText("SE ", gSeVolumePercent)
			);
		}
		textRenderSystem.SetTextColorOverride(
			item.entityId,
			selected ? kSelectedColor : kNormalColor
		);
	}
}

void SceneOptionMenuSystem::Clear() {
	selectedIndex_ = 0;
}

void SceneOptionMenuSystem::AdjustBgmVolume(int deltaPercent) {
	AdjustVolume("BGM", deltaPercent);
}

void SceneOptionMenuSystem::AdjustSeVolume(int deltaPercent) {
	AdjustVolume("SE", deltaPercent);
}

int SceneOptionMenuSystem::GetBgmVolumePercent() const {
	return gBgmVolumePercent;
}

int SceneOptionMenuSystem::GetSeVolumePercent() const {
	return gSeVolumePercent;
}

std::vector<SceneOptionMenuSystem::MenuItem>
SceneOptionMenuSystem::CollectMenuItems(const SceneDocument& document) const {
	std::vector<MenuItem> menuItems; // Sceneから見つかったメニュー項目。
	menuItems.reserve(kOptionMenuDefinitions.size());

	for (const OptionMenuDefinition& definition : kOptionMenuDefinitions) { // 登録済み項目定義。
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
			definition.actionId
		});
	}
	return menuItems;
}

void SceneOptionMenuSystem::AdjustVolume(
	const std::string& actionId,
	int deltaPercent
) {
	int* targetVolume = nullptr; // 変更対象の音量値。
	if (actionId == "BGM") {
		targetVolume = &gBgmVolumePercent;
	} else if (actionId == "SE") {
		targetVolume = &gSeVolumePercent;
	}
	if (!targetVolume) {
		return;
	}

	*targetVolume = std::clamp(
		*targetVolume + deltaPercent,
		kMinimumVolumePercent,
		kMaximumVolumePercent
	);
	ApplyAudioVolumes();
}

void SceneOptionMenuSystem::ApplyAudioVolumes() const {
	Audio* audio = Audio::GetInstance(); // Audio Busへ反映するAudio管理インスタンス。
	if (!audio) {
		return;
	}
	audio->SetBusVolume(AudioBus::BGM, ToVolumeGain(gBgmVolumePercent));
	audio->SetBusVolume(AudioBus::SFX, ToVolumeGain(gSeVolumePercent));
	audio->SetBusVolume(AudioBus::UI, ToVolumeGain(gSeVolumePercent));
}
