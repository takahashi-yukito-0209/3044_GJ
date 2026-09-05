// 役割: キーボード、マウス、ゲームパッドの入力状態を取得する。
#pragma once
#include <cassert>
#include <Windows.h>
#include <cstdint>
#include "../base/WinApp.h"
#include "../math/Vector2.h"
//入力
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
class Input {
private:
	static Input* instance_;

	Input() = default;
	~Input() = default;

	Input(const Input&) = delete;
	Input& operator=(const Input&) = delete;

public:
	enum class InputDeviceKind : uint8_t {
		KeyboardMouse,
		Gamepad,
	};
	enum class MouseButton : uint8_t {
		Left = 0,
		Right = 1,
		Middle = 2,
	};
	enum class GamepadButton : uint16_t {
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

	// シングルトンインスタンスの取得
	static Input* GetInstance();

	// 終了
	void Finalize();

	//初期化
	void Initialize(WinApp* winApp);
	//更新
	void Update();

	bool PushKey(BYTE keyNumber);
	bool TriggerKey(BYTE keyNumber);

	bool PushMouse(MouseButton button) const;
	bool TriggerMouse(MouseButton button) const;
	bool IsGamepadConnected() const { return gamepadConnected_; }
	InputDeviceKind GetLastActiveInputDevice() const { return lastActiveInputDevice_; }
	bool PushGamepad(GamepadButton button) const;
	bool TriggerGamepad(GamepadButton button) const;
	Vector2 GetGamepadLeftStick(float deadzone = 0.20f) const;

	const Vector2& GetMousePosition() const { return mousePosition_; }
	Vector2 GetMouseMove() const {
		return {
			static_cast<float>(mouseState_.lX),
			static_cast<float>(mouseState_.lY)
		};
	}
	float GetMouseWheel() const {
		return static_cast<float>(mouseState_.lZ) / static_cast<float>(WHEEL_DELTA);
	}
	void SetCursorCapture(bool enabled);
	void SetCursorCaptureRect(float minX, float minY, float maxX, float maxY);
	bool IsCursorCaptured() const { return cursorCaptured_; }

private:
	void ApplyCursorCapture();

	//キーボードデバイスの生成
	IDirectInputDevice8* keyboard = nullptr;
	IDirectInputDevice8* mouse_ = nullptr;
	BYTE key[256] = {};
	BYTE keyPre[256] = {};
	DIMOUSESTATE2 mouseState_ = {};
	DIMOUSESTATE2 previousMouseState_ = {};
	Vector2 mousePosition_ = {};
	bool cursorCaptured_ = false;
	bool cursorHidden_ = false;
	RECT cursorCaptureRect_ = {};
	bool hasCursorCaptureRect_ = false;
	RECT appliedCursorCaptureRect_ = {};
	bool hasAppliedCursorCaptureRect_ = false;
	uint16_t gamepadButtonCurrent_ = 0;
	uint16_t gamepadButtonPrevious_ = 0;
	uint16_t gamepadButtonSuppressionMask_ = 0;
	int16_t gamepadLeftStickX_ = 0;
	int16_t gamepadLeftStickY_ = 0;
	bool gamepadConnected_ = false;
	bool gamepadSampleValid_ = false;
	mutable bool gamepadStickArmed_ = false;
	InputDeviceKind lastActiveInputDevice_ = InputDeviceKind::KeyboardMouse;

	IDirectInput8* directInput = nullptr;
	//WindowAPI
	WinApp* winApp_ = nullptr;
};
