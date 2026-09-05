// 役割: PhysicsBodyとColliderを使った重力、移動、衝突解決を管理する。
#pragma once

#include <vector>

#include "PhysicsBody.h"
#include "../math/Vector3.h"

class Collider;

class PhysicsWorld {
public:
	void Clear();
	void AddBody(PhysicsBody* body);
	void AddStaticCollider(Collider* collider);
	void AddIgnoredCollision(PhysicsBody* body, Collider* collider);
	void Step(float deltaTime);

	void SetGravity(const Vector3& gravity) { gravity_ = gravity; }
	const Vector3& GetGravity() const { return gravity_; }

private:
	struct IgnoredCollisionPair {
		PhysicsBody* body = nullptr;
		Collider* collider = nullptr;
	};

	bool IsCollisionIgnored(
		const PhysicsBody& body,
		const Collider& collider
	) const;
	bool CollidesWithStatic(const PhysicsBody& body) const;
	bool SnapToGround(PhysicsBody& body, float probeDistance) const;
	bool ResolveStaticPenetration(PhysicsBody& body) const;
	bool IntegrateAxis(
		PhysicsBody& body,
		float delta,
		uint32_t axis
	) const;

	Vector3 gravity_ = { 0.0f, -9.8f, 0.0f };
	std::vector<PhysicsBody*> bodies_;
	std::vector<Collider*> staticColliders_;
	std::vector<IgnoredCollisionPair> ignoredCollisions_;
};
