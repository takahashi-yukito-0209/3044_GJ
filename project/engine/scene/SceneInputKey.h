// 役割: Scene用に保存された入力名をKeyboard／Mouseの入力種別へ正規化する。
#pragma once

#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

struct SceneInputKeyDefinition {
	const char* name;
	BYTE code;
};

enum class SceneInputMouseButton {
	None,
	Left,
	Right,
	Middle
};

struct SceneInputMouseDefinition {
	const char* name;
	SceneInputMouseButton button;
};

enum class SceneInputGamepadButton : uint16_t {
	None = 0,
	DPadUp = 0x0001,
	DPadDown = 0x0002,
	DPadLeft = 0x0004,
	DPadRight = 0x0008,
	Start = 0x0010,
	Back = 0x0020,
	LeftThumb = 0x0040,
	RightThumb = 0x0080,
	LeftShoulder = 0x0100,
	RightShoulder = 0x0200,
	A = 0x1000,
	B = 0x2000,
	X = 0x4000,
	Y = 0x8000
};

struct SceneInputGamepadDefinition {
	const char* name;
	SceneInputGamepadButton button;
};

inline constexpr SceneInputMouseDefinition kSceneInputMouseDefinitions[] = {
	{ "Mouse Left", SceneInputMouseButton::Left },
	{ "Mouse Right", SceneInputMouseButton::Right },
	{ "Mouse Middle", SceneInputMouseButton::Middle }
};

inline constexpr SceneInputGamepadDefinition kSceneInputGamepadDefinitions[] = {
	{ "Gamepad A", SceneInputGamepadButton::A },
	{ "Gamepad B", SceneInputGamepadButton::B },
	{ "Gamepad X", SceneInputGamepadButton::X },
	{ "Gamepad Y", SceneInputGamepadButton::Y },
	{ "Gamepad DPad Up", SceneInputGamepadButton::DPadUp },
	{ "Gamepad DPad Down", SceneInputGamepadButton::DPadDown },
	{ "Gamepad DPad Left", SceneInputGamepadButton::DPadLeft },
	{ "Gamepad DPad Right", SceneInputGamepadButton::DPadRight },
	{ "Gamepad Start", SceneInputGamepadButton::Start },
	{ "Gamepad Back", SceneInputGamepadButton::Back },
	{ "Gamepad Left Shoulder", SceneInputGamepadButton::LeftShoulder },
	{ "Gamepad Right Shoulder", SceneInputGamepadButton::RightShoulder },
	{ "Gamepad Left Thumb", SceneInputGamepadButton::LeftThumb },
	{ "Gamepad Right Thumb", SceneInputGamepadButton::RightThumb }
};

inline constexpr SceneInputKeyDefinition kSceneInputKeyDefinitions[] = {
	{ "1", DIK_1 }, { "2", DIK_2 }, { "3", DIK_3 }, { "4", DIK_4 },
	{ "5", DIK_5 }, { "6", DIK_6 }, { "7", DIK_7 }, { "8", DIK_8 },
	{ "9", DIK_9 }, { "0", DIK_0 },
	{ "F3", DIK_F3 }, { "F4", DIK_F4 }, { "F5", DIK_F5 },
	{ "F6", DIK_F6 }, { "F7", DIK_F7 }, { "F8", DIK_F8 },
	{ "F9", DIK_F9 }, { "F10", DIK_F10 },
	{ "ENTER", DIK_RETURN },
	{ "SPACE", DIK_SPACE }, { "ESCAPE", DIK_ESCAPE }, { "TAB", DIK_TAB },
	{ "A", DIK_A }, { "B", DIK_B }, { "C", DIK_C }, { "D", DIK_D },
	{ "E", DIK_E }, { "F", DIK_F }, { "G", DIK_G }, { "H", DIK_H },
	{ "I", DIK_I }, { "J", DIK_J }, { "K", DIK_K }, { "L", DIK_L },
	{ "M", DIK_M }, { "N", DIK_N }, { "O", DIK_O }, { "P", DIK_P },
	{ "Q", DIK_Q }, { "R", DIK_R }, { "S", DIK_S }, { "T", DIK_T },
	{ "U", DIK_U }, { "V", DIK_V }, { "W", DIK_W }, { "X", DIK_X },
	{ "Y", DIK_Y }, { "Z", DIK_Z }
};

inline BYTE ResolveSceneInputKey(const std::string& keyName) {
	std::string key = keyName;
	std::transform(
		key.begin(), key.end(), key.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::toupper(character));
		}
	);
	for (const SceneInputKeyDefinition& definition :
		kSceneInputKeyDefinitions) {
		if (key == definition.name) {
			return definition.code;
		}
	}
	return 0;
}

inline SceneInputMouseButton ResolveSceneInputMouseButton(
	const std::string& inputName
) {
	std::string normalized;
	normalized.reserve(inputName.size());
	for (const unsigned char character : inputName) {
		if (std::isalnum(character)) {
			normalized.push_back(static_cast<char>(std::toupper(character)));
		}
	}
	if (
		normalized == "MOUSELEFT" ||
		normalized == "LEFTMOUSE" ||
		normalized == "LMB"
	) {
		return SceneInputMouseButton::Left;
	}
	if (
		normalized == "MOUSERIGHT" ||
		normalized == "RIGHTMOUSE" ||
		normalized == "RMB"
	) {
		return SceneInputMouseButton::Right;
	}
	if (
		normalized == "MOUSEMIDDLE" ||
		normalized == "MIDDLEMOUSE" ||
		normalized == "MMB"
	) {
		return SceneInputMouseButton::Middle;
	}
	return SceneInputMouseButton::None;
}

inline SceneInputGamepadButton ResolveSceneInputGamepadButton(
	const std::string& inputName
) {
	std::string normalized;
	normalized.reserve(inputName.size());
	for (const unsigned char character : inputName) {
		if (std::isalnum(character)) {
			normalized.push_back(static_cast<char>(std::toupper(character)));
		}
	}
	for (const SceneInputGamepadDefinition& definition :
		kSceneInputGamepadDefinitions) {
		std::string definitionName;
		for (const unsigned char character : std::string(definition.name)) {
			if (std::isalnum(character)) {
				definitionName.push_back(
					static_cast<char>(std::toupper(character))
				);
			}
		}
		if (normalized == definitionName) {
			return definition.button;
		}
	}
	return SceneInputGamepadButton::None;
}

inline bool IsSupportedSceneInput(const std::string& inputName) {
	return ResolveSceneInputKey(inputName) != 0 ||
		ResolveSceneInputMouseButton(inputName) != SceneInputMouseButton::None ||
		ResolveSceneInputGamepadButton(inputName) !=
			SceneInputGamepadButton::None;
}
