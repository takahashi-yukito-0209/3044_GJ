// 役割: Scene保存入力式を一つのsnapshotから評価する。
#include "SceneRuntimeInput.h"

#include "../../engine/io/Input.h"
#include "../../engine/scene/SceneInputKey.h"

namespace {

Input::GamepadButton ToInputGamepadButton(SceneInputGamepadButton button) {
	switch (button) {
	case SceneInputGamepadButton::DPadUp:
		return Input::GamepadButton::DPadUp;
	case SceneInputGamepadButton::DPadDown:
		return Input::GamepadButton::DPadDown;
	case SceneInputGamepadButton::DPadLeft:
		return Input::GamepadButton::DPadLeft;
	case SceneInputGamepadButton::DPadRight:
		return Input::GamepadButton::DPadRight;
	case SceneInputGamepadButton::Start:
		return Input::GamepadButton::Start;
	case SceneInputGamepadButton::Back:
		return Input::GamepadButton::Back;
	case SceneInputGamepadButton::LeftThumb:
		return Input::GamepadButton::LeftThumb;
	case SceneInputGamepadButton::RightThumb:
		return Input::GamepadButton::RightThumb;
	case SceneInputGamepadButton::LeftShoulder:
		return Input::GamepadButton::LeftShoulder;
	case SceneInputGamepadButton::RightShoulder:
		return Input::GamepadButton::RightShoulder;
	case SceneInputGamepadButton::A:
		return Input::GamepadButton::A;
	case SceneInputGamepadButton::B:
		return Input::GamepadButton::B;
	case SceneInputGamepadButton::X:
		return Input::GamepadButton::X;
	case SceneInputGamepadButton::Y:
		return Input::GamepadButton::Y;
	case SceneInputGamepadButton::None:
	default:
		return static_cast<Input::GamepadButton>(0);
	}
}

}

namespace SceneRuntimeInput {

bool IsTriggered(const std::string& inputName, const std::string& phase) {
	Input* input = Input::GetInstance();
	if (!input || inputName.empty() || (phase != "Pressed" && phase != "Held")) {
		return false;
	}
	const bool pressed = phase == "Pressed";
	const SceneInputMouseButton mouseButton =
		ResolveSceneInputMouseButton(inputName);
	if (mouseButton != SceneInputMouseButton::None) {
		const Input::MouseButton button = mouseButton == SceneInputMouseButton::Left
			? Input::MouseButton::Left
			: mouseButton == SceneInputMouseButton::Right
				? Input::MouseButton::Right
				: Input::MouseButton::Middle;
		return pressed ? input->TriggerMouse(button) : input->PushMouse(button);
	}
	const SceneInputGamepadButton gamepadButton =
		ResolveSceneInputGamepadButton(inputName);
	if (gamepadButton != SceneInputGamepadButton::None) {
		const Input::GamepadButton button = ToInputGamepadButton(gamepadButton);
		return pressed ? input->TriggerGamepad(button) : input->PushGamepad(button);
	}
	const BYTE key = ResolveSceneInputKey(inputName);
	return key != 0 && (pressed ? input->TriggerKey(key) : input->PushKey(key));
}

bool EvaluateExpression(
	const std::optional<SceneInputExpression>& expression,
	const std::string& legacyInput
) {
	if (!expression) {
		return IsTriggered(legacyInput);
	}
	if (expression->groups.empty() ||
		(expression->mode != "Any" && expression->mode != "All")) {
		return false;
	}
	const auto evaluateGroup = [](const SceneInputGroup& group) {
		if (group.terms.empty() ||
			(group.mode != "Any" && group.mode != "All")) {
			return false;
		}
		if (group.mode == "All") {
			for (const SceneInputTerm& term : group.terms) {
				if (!IsTriggered(term.input, term.phase)) {
					return false;
				}
			}
			return true;
		}
		for (const SceneInputTerm& term : group.terms) {
			if (IsTriggered(term.input, term.phase)) {
				return true;
			}
		}
		return false;
	};
	if (expression->mode == "All") {
		for (const SceneInputGroup& group : expression->groups) {
			if (!evaluateGroup(group)) {
				return false;
			}
		}
		return true;
	}
	for (const SceneInputGroup& group : expression->groups) {
		if (evaluateGroup(group)) {
			return true;
		}
	}
	return false;
}

}
