// 役割: Runtime入力をSceneTransition Componentの遷移要求へ変換する。
#include "SceneTransitionSystem.h"

#include "../../../engine/io/Input.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <cctype>

namespace {
	bool ResolveTriggerKey(const std::string& keyName, BYTE& keyCode) {
		std::string normalizedKey = keyName;
		std::transform(
			normalizedKey.begin(),
			normalizedKey.end(),
			normalizedKey.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::toupper(character));
			}
		);
		struct KeyBinding {
			const char* name;
			BYTE code;
		};
		const KeyBinding bindings[] = {
			{ "ENTER", DIK_RETURN }, { "SPACE", DIK_SPACE },
			{ "ESCAPE", DIK_ESCAPE }, { "TAB", DIK_TAB },
			{ "A", DIK_A }, { "B", DIK_B }, { "C", DIK_C },
			{ "D", DIK_D }, { "E", DIK_E }, { "F", DIK_F },
			{ "G", DIK_G }, { "H", DIK_H }, { "I", DIK_I },
			{ "J", DIK_J }, { "K", DIK_K }, { "L", DIK_L },
			{ "M", DIK_M }, { "N", DIK_N }, { "O", DIK_O },
			{ "P", DIK_P }, { "Q", DIK_Q }, { "R", DIK_R },
			{ "S", DIK_S }, { "T", DIK_T }, { "U", DIK_U },
			{ "V", DIK_V }, { "W", DIK_W }, { "X", DIK_X },
			{ "Y", DIK_Y }, { "Z", DIK_Z }
		};
		for (const KeyBinding& binding : bindings) {
			if (normalizedKey == binding.name) {
				keyCode = binding.code;
				return true;
			}
		}
		return false;
	}
}

SceneTransitionRequest SceneTransitionSystem::Update(
	const SceneDocument& document
) const {
	Input* input = Input::GetInstance();
	if (!input) {
		return {};
	}

	// 複数の遷移が同時成立しても、Hierarchy順で最初の要求だけを採用する。
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		for (const SceneComponent& transition : entity.components) { // 同一Entity内の遷移候補。
			if (
				!transition.enabled ||
				transition.type != "SceneTransition" ||
				transition.sceneTransitionTriggerType != "Key" ||
				transition.sceneTransitionTargetSceneId.empty()
			) {
				continue;
			}

			BYTE keyCode = 0; // 遷移入力に対応するDirectInputキーコード。
			if (ResolveTriggerKey(
				transition.sceneTransitionTriggerKey,
				keyCode
			) && input->TriggerKey(keyCode)) {
				return {
					transition.sceneTransitionTargetSceneId,
					transition.sceneTransitionUseEffect
				};
			}
		}
	}
	return {};
}
