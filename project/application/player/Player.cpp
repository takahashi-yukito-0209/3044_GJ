// 役割: プレイヤーの入力移動と物理状態の同期を実装する。
#include "Player.h"

#include "../../engine/3d/Object3d.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/io/Input.h"
#include "../../engine/math/Math.h"

#include <algorithm>
#include <cmath>

void Player::Initialize(Object3dCommon* object3dCommon, const char* modelName) {
	object_ = new Object3d();
	ownsObject_ = true;
	object_->Initialize(object3dCommon);
	object_->SetModel(modelName);
	object_->SetScale({ 1.0f, 1.0f, 1.0f });
	ApplyPosition();
	object_->Update();

	physicsBody_.type = PhysicsBodyType::Dynamic;
	physicsBody_.transform = &object_->GetTransform();
	physicsBody_.collider = collider_;
	physicsBody_.useGravity = true;
	physicsBody_.gravityScale = 8.0f;
	physicsBody_.maxFallSpeed = 60.0f;
	physicsBody_.friction = 0.0f;
}

void Player::Initialize(Object3d* object) {
	object_ = object;
	ownsObject_ = false;
	if (!object_) {
		return;
	}
	position_ = object_->GetTransform().translate;
	targetYaw_ = GetYaw();
	physicsBody_.type = PhysicsBodyType::Dynamic;
	physicsBody_.transform = &object_->GetTransform();
	physicsBody_.collider = collider_;
	physicsBody_.useGravity = true;
	physicsBody_.gravityScale = 8.0f;
	physicsBody_.maxFallSpeed = 60.0f;
	physicsBody_.friction = 0.0f;
}

void Player::SetCollider(Collider* collider) {
	collider_ = collider;
	physicsBody_.collider = collider_;
}

void Player::Update(
	const Camera*,
	bool acceptGameplayInput,
	float deltaTime
) {
	if (!object_) {
		return;
	}
	const float dt = std::clamp(deltaTime, 0.0f, 0.1f);
	Input* input = acceptGameplayInput ? Input::GetInstance() : nullptr;
	const bool allowKeyboardMouse = inputMode_ != "Gamepad";
	const bool allowGamepad = inputMode_ == "Gamepad" || inputMode_ == "Both";
	Vector3 keyboardMove{};
	if (input && allowKeyboardMouse && input->PushKey(DIK_W)) {
		keyboardMove.x += 1.0f;
	}
	if (input && allowKeyboardMouse && input->PushKey(DIK_S)) {
		keyboardMove.x -= 1.0f;
	}
	if (input && allowKeyboardMouse && input->PushKey(DIK_A)) {
		keyboardMove.z += 1.0f;
	}
	if (input && allowKeyboardMouse && input->PushKey(DIK_D)) {
		keyboardMove.z -= 1.0f;
	}

	Vector3 gamepadMove{};
	if (input && allowGamepad) {
		const Vector2 stick = input->GetGamepadLeftStick(gamepadDeadzone_);
		// Existing Player axes are W/S = +X/-X and A/D = +Z/-Z.
		gamepadMove.x = stick.y;
		gamepadMove.z = -stick.x;
	}

	Vector3 inputMove{};
	const float keyboardLength = Math::Length(keyboardMove);
	const float gamepadLength = Math::Length(gamepadMove);
	if (keyboardLength > 0.000001f && gamepadLength > 0.000001f) {
		inputMove = input->GetLastActiveInputDevice() ==
			Input::InputDeviceKind::Gamepad
			? gamepadMove
			: keyboardMove;
	} else if (keyboardLength > 0.000001f) {
		inputMove = keyboardMove;
	} else if (gamepadLength > 0.000001f) {
		inputMove = gamepadMove;
	}

	Vector3 desiredVelocity = physicsBody_.velocity;
	desiredVelocity.x = 0.0f;
	desiredVelocity.z = 0.0f;

	const Vector3 movementRight = { 1.0f, 0.0f, 0.0f };
	const Vector3 movementForward = { 0.0f, 0.0f, 1.0f };

	const bool dash =
		(input && allowKeyboardMouse &&
			(input->PushKey(DIK_LSHIFT) || input->PushKey(DIK_RSHIFT))) ||
		(input && allowGamepad &&
			input->PushGamepad(Input::GamepadButton::LeftShoulder));
	const float speedMultiplier = dash ? dashMultiplier_ : 1.0f;

	const float inputLength = Math::Length(inputMove);
	if (autoForward_ && acceptGameplayInput) {
		const float currentYaw = GetYaw();
		if (inputLength > 0.000001f) {
			const Vector3 inputDirection = Math::Normalize(
				Math::Add(
					Math::Multiply(movementRight, inputMove.x),
					Math::Multiply(movementForward, inputMove.z)
				)
			);
			targetYaw_ = std::atan2(inputDirection.x, inputDirection.z);
		} else {
			// 方向入力を離したら旋回目標を現在向きへ戻し、前進だけを継続する。
			targetYaw_ = currentYaw;
		}
		const float responsiveness = std::clamp(
			turnResponsiveness_,
			0.0f,
			1.0f
		);
		const float turnAlpha = responsiveness <= 0.0f
			? 0.0f
			: responsiveness >= 1.0f
				? 1.0f
				: 1.0f - std::pow(1.0f - responsiveness, dt * 60.0f);
		const float yaw = inputLength > 0.000001f
			? currentYaw + std::atan2(
				std::sin(targetYaw_ - currentYaw),
				std::cos(targetYaw_ - currentYaw)
			) * turnAlpha
			: currentYaw;
		const float activeMoveSpeed =
			moveSpeed_ * speedMultiplier *
			(inWater_ ? waterMoveSpeedMultiplier_ : 1.0f);
		const Vector3 move = {
			std::sin(yaw) * activeMoveSpeed,
			0.0f,
			std::cos(yaw) * activeMoveSpeed
		};
		desiredVelocity.x = move.x;
		desiredVelocity.z = move.z;
		object_->SetRotate({ 0.0f, yaw, 0.0f });
		if (inWater_) {
			desiredVelocity.y = move.y;
		}
	} else if (inputLength > 0.000001f) {
		Vector3 move = Math::Add(
			Math::Multiply(movementRight, inputMove.x),
			Math::Multiply(movementForward, inputMove.z)
		);
		const float activeMoveSpeed =
			moveSpeed_ *
			speedMultiplier *
			(inWater_ ? waterMoveSpeedMultiplier_ : 1.0f);
		move = Math::Multiply(Math::Normalize(move), activeMoveSpeed);
		desiredVelocity.x = move.x;
		desiredVelocity.z = move.z;
		object_->SetRotate({ 0.0f, std::atan2(move.x, move.z), 0.0f });
		if (inWater_) {
			desiredVelocity.y = move.y;
		}
		targetYaw_ = std::atan2(move.x, move.z);
	} else {
		targetYaw_ = GetYaw();
	}

	if (acceptGameplayInput && inWater_) {
		const bool swimUp = input && allowKeyboardMouse && input->PushKey(DIK_SPACE);
		const bool swimDown = input && allowKeyboardMouse && input->PushKey(DIK_LCONTROL);
		if (swimUp) {
			desiredVelocity.y = waterSwimUpSpeed_ * speedMultiplier;
			physicsBody_.isGrounded = false;
		} else if (swimDown) {
			desiredVelocity.y = -waterSwimUpSpeed_ * 10.6f * speedMultiplier;
			physicsBody_.isGrounded = false;
		} else {
			desiredVelocity.y = std::clamp(
				desiredVelocity.y,
				-waterSwimUpSpeed_ * 0.35f * speedMultiplier,
				waterSwimUpSpeed_ * 0.35f * speedMultiplier
			);
		}
	} else if (input && allowKeyboardMouse && allowJump_ &&
		physicsBody_.isGrounded && input->TriggerKey(DIK_SPACE)) {
		desiredVelocity.y = jumpVelocity_;
		physicsBody_.isGrounded = false;
	}

	physicsBody_.velocity = desiredVelocity;
	object_->Update();
}

void Player::PostPhysicsUpdate() {
	if (!object_) {
		return;
	}
	position_ = object_->GetTransform().translate;
	object_->Update();
}

void Player::Draw() {
	if (object_) {
		object_->Draw();
	}
}

void Player::DrawShadow(const Matrix4x4& lightViewProjection) {
	if (object_) {
		object_->DrawShadow(lightViewProjection);
	}
}

void Player::Finalize() {
	if (ownsObject_) {
		delete object_;
	}
	object_ = nullptr;
	ownsObject_ = false;
}

void Player::SetTransform(const Transform& transform) {
	if (!object_) {
		return;
	}
	position_ = transform.translate;
	physicsBody_.velocity = {};
	physicsBody_.isGrounded = false;
	object_->GetTransform() = transform;
	targetYaw_ = GetYaw();
	ApplyPosition();
	object_->Update();
}

bool Player::RestorePlanarPosition(const Vector3& position) {
	if (!object_ || !std::isfinite(position.x) || !std::isfinite(position.z)) {
		return false;
	}
	position_.x = position.x;
	position_.z = position.z;
	physicsBody_.velocity.x = 0.0f;
	physicsBody_.velocity.z = 0.0f;
	ApplyPosition();
	object_->Update();
	return true;
}

bool Player::RestorePlanarPose(const Vector3& position, float yaw) {
	if (!object_ || !std::isfinite(position.x) ||
		!std::isfinite(position.z) || !std::isfinite(yaw)) {
		return false;
	}
	position_.x = position.x;
	position_.z = position.z;
	physicsBody_.velocity.x = 0.0f;
	physicsBody_.velocity.z = 0.0f;
	object_->SetRotate({ 0.0f, yaw, 0.0f });
	targetYaw_ = yaw;
	ApplyPosition();
	object_->Update();
	return true;
}

bool Player::ApplyPlanarMotionConstraint(
	const Vector3& position,
	float yaw,
	const Vector3& velocity
) {
	if (!object_ ||
		!std::isfinite(position.x) || !std::isfinite(position.z) ||
		!std::isfinite(yaw) ||
		!std::isfinite(velocity.x) || !std::isfinite(velocity.z)) {
		return false;
	}
	position_.x = position.x;
	position_.z = position.z;
	physicsBody_.velocity.x = velocity.x;
	physicsBody_.velocity.z = velocity.z;
	object_->SetRotate({ 0.0f, yaw, 0.0f });
	targetYaw_ = yaw;
	ApplyPosition();
	object_->Update();
	return true;
}

bool Player::ClampToWaterBounds(
	const Vector3& center,
	float yaw,
	float halfSizeX,
	float halfSizeZ
) {
	if (!object_ || !std::isfinite(yaw) ||
		!std::isfinite(halfSizeX) || !std::isfinite(halfSizeZ)) {
		return false;
	}
	const float cosine = std::cos(yaw);
	const float sine = std::sin(yaw);
	const Vector3 worldDelta = {
		position_.x - center.x,
		0.0f,
		position_.z - center.z
	};
	const float localX = worldDelta.x * cosine - worldDelta.z * sine;
	const float localZ = worldDelta.x * sine + worldDelta.z * cosine;
	const float clampedX = std::clamp(localX, -halfSizeX, halfSizeX);
	const float clampedZ = std::clamp(localZ, -halfSizeZ, halfSizeZ);
	if (clampedX == localX && clampedZ == localZ) {
		return false;
	}
	position_.x = center.x + clampedX * cosine + clampedZ * sine;
	position_.z = center.z - clampedX * sine + clampedZ * cosine;
	if (autoForward_) {
		const float localVelocityX =
			physicsBody_.velocity.x * cosine - physicsBody_.velocity.z * sine;
		const float localVelocityZ =
			physicsBody_.velocity.x * sine + physicsBody_.velocity.z * cosine;
		float correctedVelocityX = localVelocityX;
		float correctedVelocityZ = localVelocityZ;
		if (
			(clampedX != localX) &&
			((localX > halfSizeX && correctedVelocityX > 0.0f) ||
				(localX < -halfSizeX && correctedVelocityX < 0.0f))
		) {
			correctedVelocityX = 0.0f;
		}
		if (
			(clampedZ != localZ) &&
			((localZ > halfSizeZ && correctedVelocityZ > 0.0f) ||
				(localZ < -halfSizeZ && correctedVelocityZ < 0.0f))
		) {
			correctedVelocityZ = 0.0f;
		}
		physicsBody_.velocity.x =
			correctedVelocityX * cosine + correctedVelocityZ * sine;
		physicsBody_.velocity.z =
			-correctedVelocityX * sine + correctedVelocityZ * cosine;
	} else {
		physicsBody_.velocity.x = 0.0f;
		physicsBody_.velocity.z = 0.0f;
	}
	ApplyPosition();
	object_->Update();
	return true;
}

void Player::SetBehaviorSettings(
	float moveSpeed,
	float jumpVelocity,
	float turnResponsiveness,
	float dashMultiplier,
	bool cameraRelativeMove,
	bool allowJump,
	bool autoForward
) {
	moveSpeed_ = (std::max)(moveSpeed, 0.0f);
	jumpVelocity_ = (std::max)(jumpVelocity, 0.0f);
	turnResponsiveness_ = std::clamp(turnResponsiveness, 0.0f, 1.0f);
	dashMultiplier_ = (std::max)(dashMultiplier, 1.0f);
	cameraRelativeMove_ = cameraRelativeMove;
	allowJump_ = allowJump;
	if (autoForward && !autoForward_ && object_) {
		targetYaw_ = GetYaw();
	}
	autoForward_ = autoForward;
}

void Player::SetInputDeviceSettings(
	const std::string& inputMode,
	float gamepadDeadzone
) {
	if (
		inputMode == "KeyboardMouse" ||
		inputMode == "Gamepad" ||
		inputMode == "Both"
	) {
		inputMode_ = inputMode;
	} else {
		inputMode_ = "KeyboardMouse";
	}
	gamepadDeadzone_ = std::isfinite(gamepadDeadzone)
		? std::clamp(gamepadDeadzone, 0.0f, 0.95f)
		: 0.20f;
}

void Player::SetWaterState(
	bool inWater,
	float moveSpeedMultiplier,
	float swimUpSpeed
) {
	inWater_ = inWater;
	waterMoveSpeedMultiplier_ = std::clamp(moveSpeedMultiplier, 0.0f, 1.0f);
	waterSwimUpSpeed_ = (std::max)(swimUpSpeed, 0.0f);
}

void Player::ApplyPosition() {
	if (object_) {
		object_->SetTranslate(position_);
	}
}

float Player::GetYaw() const {
	if (!object_) {
		return targetYaw_;
	}
	const Transform& transform = object_->GetTransform();
	if (transform.useQuaternionRotation) {
		const Matrix4x4 rotationMatrix = MakeRotateMatrix(
			transform.quaternionRotate
		);
		const float yaw = std::atan2(
			rotationMatrix.m[2][0],
			rotationMatrix.m[2][2]
		);
		return std::isfinite(yaw) ? yaw : targetYaw_;
	}
	return std::isfinite(transform.rotate.y)
		? transform.rotate.y
		: targetYaw_;
}
