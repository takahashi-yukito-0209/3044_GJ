// 役割: プレイヤーの移動状態、物理ボディ、表示オブジェクトをまとめる。
#pragma once

#include "../../engine/collision/Collider.h"
#include "../../engine/physics/PhysicsBody.h"
#include "../../engine/math/Vector3.h"
#include "../../engine/math/Transform.h"

#include <string>

class Object3d;
class Object3dCommon;
class Camera;
struct Matrix4x4;

class Player {
public:
	void Initialize(Object3dCommon* object3dCommon, const char* modelName);
	void Initialize(Object3d* object);
	void Update(
		const Camera* camera,
		bool acceptGameplayInput,
		float deltaTime
	);
	void PostPhysicsUpdate();
	void Draw();
	void DrawShadow(const Matrix4x4& lightViewProjection);
	void Finalize();

	const Vector3& GetPosition() const { return position_; }
	Collider* GetCollider() const { return collider_; }
	PhysicsBody& GetPhysicsBody() { return physicsBody_; }
	Object3d* GetObject() const { return object_; }
	void SetCollider(Collider* collider);
	void SetTransform(const Transform& transform);
	bool RestorePlanarPosition(const Vector3& position);
	bool RestorePlanarPose(const Vector3& position, float yaw);
	bool ApplyPlanarMotionConstraint(
		const Vector3& position,
		float yaw,
		const Vector3& velocity
	);
	bool ClampToWaterBounds(
		const Vector3& center,
		float yaw,
		float halfSizeX,
		float halfSizeZ
	);
	void SetBehaviorSettings(
		float moveSpeed,
		float jumpVelocity,
		float turnResponsiveness,
		float dashMultiplier,
		bool cameraRelativeMove,
		bool allowJump,
		bool autoForward
	);
	void SetInputDeviceSettings(
		const std::string& inputMode,
		float gamepadDeadzone
	);
	void SetWaterState(
		bool inWater,
		float moveSpeedMultiplier,
		float swimUpSpeed
	);

private:
	void ApplyPosition();
	float GetYaw() const;

private:
	Object3d* object_ = nullptr;
	bool ownsObject_ = false;
	Collider* collider_ = nullptr;
	PhysicsBody physicsBody_;
	Vector3 position_ = { 0.0f, 1.0f, -4.0f };
	float moveSpeed_ = 10.8f;
	float turnResponsiveness_ = 0.018f;
	float jumpVelocity_ = 37.2f;
	float dashMultiplier_ = 1.65f;
	bool cameraRelativeMove_ = true;
	bool allowJump_ = true;
	bool inWater_ = false;
	float waterMoveSpeedMultiplier_ = 0.45f;
	float waterSwimUpSpeed_ = 12.0f;
	float targetYaw_ = 0.0f;
	bool autoForward_ = false;
	std::string inputMode_ = "KeyboardMouse";
	float gamepadDeadzone_ = 0.20f;
};
