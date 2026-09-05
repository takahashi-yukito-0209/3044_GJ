// 役割: DirectInputとXInputから入力状態を更新する。
#include "Input.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <Xinput.h>

#pragma comment(lib,"dinput8.lib")
#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"xinput.lib")

Input* Input::instance_ = nullptr;

namespace {
	uint16_t ToGamepadMask(Input::GamepadButton button) {
		return static_cast<uint16_t>(button);
	}

	float NormalizeStickAxis(SHORT value) {
		return value >= 0
			? static_cast<float>(value) / 32767.0f
			: static_cast<float>(value) / 32768.0f;
	}
}

Input* Input::GetInstance() {
	if (instance_ == nullptr) {
		instance_ = new Input();
	}
	return instance_;
}

void Input::Finalize() {
	gamepadButtonCurrent_ = 0;
	gamepadButtonPrevious_ = 0;
	gamepadButtonSuppressionMask_ = 0;
	gamepadLeftStickX_ = 0;
	gamepadLeftStickY_ = 0;
	gamepadConnected_ = false;
	gamepadSampleValid_ = false;
	gamepadStickArmed_ = false;
	lastActiveInputDevice_ = InputDeviceKind::KeyboardMouse;
	if (mouse_) {
		mouse_->Unacquire();
		mouse_->Release();
		mouse_ = nullptr;
	}

	if (keyboard) {
		keyboard->Unacquire();
		keyboard->Release();
		keyboard = nullptr;
	}

	if (directInput) {
		directInput->Release();
		directInput = nullptr;
	}

	winApp_ = nullptr;
	SetCursorCapture(false);

	delete instance_;
	instance_ = nullptr;
}

void Input::Initialize(WinApp* winApp){

	winApp_ = winApp;
	gamepadButtonCurrent_ = 0;
	gamepadButtonPrevious_ = 0;
	gamepadButtonSuppressionMask_ = 0;
	gamepadLeftStickX_ = 0;
	gamepadLeftStickY_ = 0;
	gamepadConnected_ = false;
	gamepadSampleValid_ = false;
	gamepadStickArmed_ = false;
	lastActiveInputDevice_ = InputDeviceKind::KeyboardMouse;
	HRESULT hr;

	//DirectInputの初期化
	hr = DirectInput8Create(winApp_->GetHInstance(), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&directInput, nullptr);
	assert(SUCCEEDED(hr));
	hr = directInput->CreateDevice(GUID_SysKeyboard, &keyboard, NULL);
	assert(SUCCEEDED(hr));
	//入力データの形式のセット
	hr = keyboard->SetDataFormat(&c_dfDIKeyboard);
	assert(SUCCEEDED(hr));
	hr = keyboard->SetCooperativeLevel(
		winApp_->GetHwnd(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE | DISCL_NOWINKEY);
	assert(SUCCEEDED(hr));

	hr = directInput->CreateDevice(GUID_SysMouse, &mouse_, nullptr);
	assert(SUCCEEDED(hr));
	hr = mouse_->SetDataFormat(&c_dfDIMouse2);
	assert(SUCCEEDED(hr));
	hr = mouse_->SetCooperativeLevel(
		winApp_->GetHwnd(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
	assert(SUCCEEDED(hr));
}

void Input::Update(){

	memcpy(keyPre, key, sizeof(key));
	memset(key, 0, sizeof(key));
	previousMouseState_ = mouseState_;
	mouseState_ = {};

	//キーボード情報の取得開始
	HRESULT hr = keyboard->Acquire();
	//全キーの入力情報を取得する
	if (SUCCEEDED(hr)) {
		hr = keyboard->GetDeviceState(sizeof(key), key);
		if (FAILED(hr)) {
			memset(key, 0, sizeof(key));
		}
	}

	hr = mouse_->Acquire();
	if (SUCCEEDED(hr)) {
		hr = mouse_->GetDeviceState(sizeof(mouseState_), &mouseState_);
		if (FAILED(hr)) {
			mouseState_ = {};
		}
	}

	XINPUT_STATE gamepadState{};
	const DWORD gamepadResult = XInputGetState(0, &gamepadState);
	if (gamepadResult != ERROR_SUCCESS) {
		gamepadConnected_ = false;
		gamepadSampleValid_ = false;
		gamepadButtonCurrent_ = 0;
		gamepadButtonPrevious_ = 0;
		gamepadButtonSuppressionMask_ = 0;
		gamepadLeftStickX_ = 0;
		gamepadLeftStickY_ = 0;
		gamepadStickArmed_ = false;
	} else {
		gamepadConnected_ = true;
		const bool focused =
			winApp_ && GetForegroundWindow() == winApp_->GetHwnd();
		if (!focused) {
			gamepadSampleValid_ = false;
			gamepadButtonCurrent_ = 0;
			gamepadButtonPrevious_ = 0;
			gamepadButtonSuppressionMask_ = 0;
			gamepadLeftStickX_ = 0;
			gamepadLeftStickY_ = 0;
			gamepadStickArmed_ = false;
		} else {
			const uint16_t rawButtons = gamepadState.Gamepad.wButtons;
			if (!gamepadSampleValid_) {
				gamepadButtonCurrent_ = 0;
				gamepadButtonPrevious_ = 0;
				gamepadButtonSuppressionMask_ = rawButtons;
				gamepadSampleValid_ = true;
				gamepadStickArmed_ = false;
			} else {
				gamepadButtonPrevious_ = gamepadButtonCurrent_;
				gamepadButtonSuppressionMask_ &= rawButtons;
				gamepadButtonCurrent_ =
					rawButtons & ~gamepadButtonSuppressionMask_;
			}
			gamepadLeftStickX_ = gamepadState.Gamepad.sThumbLX;
			gamepadLeftStickY_ = gamepadState.Gamepad.sThumbLY;
		}
	}

	POINT mousePoint{};
	if (GetCursorPos(&mousePoint)) {
		ScreenToClient(winApp_->GetHwnd(), &mousePoint);
		mousePosition_ = {
			static_cast<float>(mousePoint.x),
			static_cast<float>(mousePoint.y)
		};
	}

	bool keyboardMouseActivity = false;
	for (size_t index = 0; index < sizeof(key) / sizeof(key[0]); ++index) {
		if (key[index] && !keyPre[index]) {
			keyboardMouseActivity = true;
			break;
		}
	}
	if (!keyboardMouseActivity) {
		for (size_t index = 0; index < ARRAYSIZE(mouseState_.rgbButtons); ++index) {
			if ((mouseState_.rgbButtons[index] & 0x80) != 0 &&
				(previousMouseState_.rgbButtons[index] & 0x80) == 0) {
				keyboardMouseActivity = true;
				break;
			}
		}
	}
	if (!keyboardMouseActivity) {
		keyboardMouseActivity =
			mouseState_.lZ != 0 ||
			std::abs(mouseState_.lX) > 1 ||
			std::abs(mouseState_.lY) > 1;
	}

	bool gamepadActivity = false;
	if (gamepadConnected_ && gamepadSampleValid_) {
		const float stickX = NormalizeStickAxis(gamepadLeftStickX_);
		const float stickY = NormalizeStickAxis(gamepadLeftStickY_);
		const float stickMagnitude = std::sqrt(stickX * stickX + stickY * stickY);
		gamepadActivity =
			(gamepadButtonCurrent_ & ~gamepadButtonPrevious_) != 0 ||
			stickMagnitude > 0.20f;
	}
	if (gamepadActivity && !keyboardMouseActivity) {
		lastActiveInputDevice_ = InputDeviceKind::Gamepad;
	} else if (keyboardMouseActivity && !gamepadActivity) {
		lastActiveInputDevice_ = InputDeviceKind::KeyboardMouse;
	}
	ApplyCursorCapture();
}

bool Input::PushKey(BYTE keyNumber){
	if(key[keyNumber]){
		return true;
	}
	return false;
}

bool Input::TriggerKey(BYTE keyNumber) {
	if (PushKey(keyNumber) && !keyPre[keyNumber]) {
		return true;
	}
	return false;
}

bool Input::PushMouse(MouseButton button) const {
	const uint8_t index = static_cast<uint8_t>(button);
	return (mouseState_.rgbButtons[index] & 0x80) != 0;
}

bool Input::TriggerMouse(MouseButton button) const {
	const uint8_t index = static_cast<uint8_t>(button);
	return
		(mouseState_.rgbButtons[index] & 0x80) != 0 &&
		(previousMouseState_.rgbButtons[index] & 0x80) == 0;
}

bool Input::PushGamepad(GamepadButton button) const {
	return gamepadSampleValid_ &&
		(gamepadButtonCurrent_ & ToGamepadMask(button)) != 0;
}

bool Input::TriggerGamepad(GamepadButton button) const {
	const uint16_t mask = ToGamepadMask(button);
	return gamepadSampleValid_ &&
		(gamepadButtonCurrent_ & mask) != 0 &&
		(gamepadButtonPrevious_ & mask) == 0;
}

Vector2 Input::GetGamepadLeftStick(float deadzone) const {
	if (!gamepadConnected_ || !gamepadSampleValid_) {
		return {};
	}
	if (!std::isfinite(deadzone)) {
		deadzone = 0.20f;
	}
	deadzone = std::clamp(deadzone, 0.0f, 0.95f);
	const Vector2 raw = {
		NormalizeStickAxis(gamepadLeftStickX_),
		NormalizeStickAxis(gamepadLeftStickY_)
	};
	const float magnitude = std::sqrt(raw.x * raw.x + raw.y * raw.y);
	if (!gamepadStickArmed_) {
		if (magnitude <= deadzone) {
			gamepadStickArmed_ = true;
		}
		return {};
	}
	if (magnitude <= deadzone || magnitude <= 0.000001f) {
		return {};
	}
	const float normalizedMagnitude = std::clamp(
		(magnitude - deadzone) / (1.0f - deadzone),
		0.0f,
		1.0f
	);
	return {
		raw.x / magnitude * normalizedMagnitude,
		raw.y / magnitude * normalizedMagnitude
	};
}

void Input::SetCursorCapture(bool enabled) {
	if (cursorCaptured_ == enabled) {
		return;
	}

	cursorCaptured_ = enabled;
	if (!cursorCaptured_) {
		ClipCursor(nullptr);
		hasCursorCaptureRect_ = false;
		hasAppliedCursorCaptureRect_ = false;
		if (cursorHidden_) {
			while (ShowCursor(TRUE) < 0) {
			}
			cursorHidden_ = false;
		}
		return;
	}

	if (!cursorHidden_) {
		while (ShowCursor(FALSE) >= 0) {
		}
		cursorHidden_ = true;
	}
	ApplyCursorCapture();
}

void Input::SetCursorCaptureRect(
	float minX,
	float minY,
	float maxX,
	float maxY
) {
	constexpr LONG kCaptureInset = 2;
	const LONG rawLeft = static_cast<LONG>(std::ceil(minX));
	const LONG rawTop = static_cast<LONG>(std::ceil(minY));
	const LONG rawRight = static_cast<LONG>(std::floor(maxX));
	const LONG rawBottom = static_cast<LONG>(std::floor(maxY));
	const LONG horizontalInset =
		rawRight - rawLeft > kCaptureInset * 2 ? kCaptureInset : 0;
	const LONG verticalInset =
		rawBottom - rawTop > kCaptureInset * 2 ? kCaptureInset : 0;
	const RECT newRect = {
		rawLeft + horizontalInset,
		rawTop + verticalInset,
		rawRight - horizontalInset,
		rawBottom - verticalInset
	};
	const bool sameRect =
		hasCursorCaptureRect_ &&
		cursorCaptureRect_.left == newRect.left &&
		cursorCaptureRect_.top == newRect.top &&
		cursorCaptureRect_.right == newRect.right &&
		cursorCaptureRect_.bottom == newRect.bottom;
	cursorCaptureRect_ = newRect;
	hasCursorCaptureRect_ = true;
	if (cursorCaptured_ && !sameRect) {
		ApplyCursorCapture();
	}
}

void Input::ApplyCursorCapture() {
	if (!cursorCaptured_ || !winApp_ || !winApp_->GetHwnd()) {
		return;
	}

	RECT clientRect = cursorCaptureRect_;
	if (!hasCursorCaptureRect_) {
		GetClientRect(winApp_->GetHwnd(), &clientRect);
	}
	POINT topLeft{ clientRect.left, clientRect.top };
	POINT bottomRight{ clientRect.right, clientRect.bottom };
	ClientToScreen(winApp_->GetHwnd(), &topLeft);
	ClientToScreen(winApp_->GetHwnd(), &bottomRight);
	RECT screenRect{
		topLeft.x,
		topLeft.y,
		bottomRight.x,
		bottomRight.y
	};
	if (
		hasAppliedCursorCaptureRect_ &&
		appliedCursorCaptureRect_.left == screenRect.left &&
		appliedCursorCaptureRect_.top == screenRect.top &&
		appliedCursorCaptureRect_.right == screenRect.right &&
		appliedCursorCaptureRect_.bottom == screenRect.bottom
	) {
		return;
	}
	ClipCursor(&screenRect);
	appliedCursorCaptureRect_ = screenRect;
	hasAppliedCursorCaptureRect_ = true;
}
