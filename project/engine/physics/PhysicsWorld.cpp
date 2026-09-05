// 役割: 物理ボディの積分、地面判定、静的Colliderとの押し戻しを実装する。
#include "PhysicsWorld.h"

#include "../collision/Collider.h"
#include "../collision/OBBCollider.h"
#include "../collision/SphereCollider.h"
#include "../math/Math.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace {
	constexpr float kBounceVelocityThreshold = 0.01f;
	constexpr float kGroundProbeDistance = 0.05f;
	constexpr uint32_t kAxisSweepIterations = 10;
	constexpr uint32_t kPenetrationResolveIterations = 4;
	constexpr float kPenetrationEpsilon = 0.00001f;
	constexpr float kPenetrationSlop = 0.001f;
	constexpr float kPenetrationSkin = 0.002f;

	float AbsDot(const Vector3& a, const Vector3& b) {
		return std::fabs(Math::Dot(a, b));
	}

	float ProjectOBB(const OBBCollider::OBB& obb, const Vector3& axis) {
		return
			obb.halfSize.x * AbsDot(obb.axis[0], axis) +
			obb.halfSize.y * AbsDot(obb.axis[1], axis) +
			obb.halfSize.z * AbsDot(obb.axis[2], axis);
	}

	bool TestPenetrationAxis(
		const OBBCollider::OBB& moving,
		const OBBCollider::OBB& obstacle,
		Vector3 axis,
		float& bestOverlap,
		Vector3& bestPushAxis
	) {
		if (Math::Length(axis) < kPenetrationEpsilon) {
			return true;
		}

		axis = Math::Normalize(axis);
		const Vector3 centerDelta =
			Math::Subtract(obstacle.center, moving.center);
		const float signedDistance = Math::Dot(centerDelta, axis);
		const float distance = std::fabs(signedDistance);
		const float overlap =
			ProjectOBB(moving, axis) + ProjectOBB(obstacle, axis) - distance;
		if (overlap <= 0.0f) {
			return false;
		}

		if (overlap < bestOverlap) {
			bestOverlap = overlap;
			bestPushAxis = signedDistance < 0.0f
				? axis
				: Vector3{ -axis.x, -axis.y, -axis.z };
		}
		return true;
	}

	bool TryComputePushOut(
		const OBBCollider::OBB& moving,
		const OBBCollider::OBB& obstacle,
		Vector3& pushOut
	) {
		float bestOverlap = FLT_MAX;
		Vector3 bestPushAxis{};

		for (uint32_t i = 0; i < 3; ++i) {
			if (!TestPenetrationAxis(
				moving,
				obstacle,
				moving.axis[i],
				bestOverlap,
				bestPushAxis
			)) {
				return false;
			}
			if (!TestPenetrationAxis(
				moving,
				obstacle,
				obstacle.axis[i],
				bestOverlap,
				bestPushAxis
			)) {
				return false;
			}
		}

		for (uint32_t movingAxis = 0; movingAxis < 3; ++movingAxis) {
			for (uint32_t obstacleAxis = 0; obstacleAxis < 3; ++obstacleAxis) {
				if (!TestPenetrationAxis(
					moving,
					obstacle,
					Math::Cross(
						moving.axis[movingAxis],
						obstacle.axis[obstacleAxis]
					),
					bestOverlap,
					bestPushAxis
				)) {
					return false;
				}
			}
		}

		if (
			bestOverlap <= kPenetrationSlop ||
			Math::Length(bestPushAxis) < kPenetrationEpsilon
		) {
			return false;
		}

		pushOut = Math::Multiply(
			bestPushAxis,
			bestOverlap + kPenetrationSkin
		);
		return true;
	}

	void ApplyFreezeAxes(const PhysicsBody& body, Vector3& pushOut) {
		if (body.freezePositionX) {
			pushOut.x = 0.0f;
		}
		if (body.freezePositionY) {
			pushOut.y = 0.0f;
		}
		if (body.freezePositionZ) {
			pushOut.z = 0.0f;
		}
	}

	Vector3 ClosestPointOnOBB(
		const OBBCollider::OBB& obb,
		const Vector3& point,
		std::array<float, 3>& localCoordinates
	) {
		const Vector3 delta = Math::Subtract(point, obb.center);
		Vector3 closestPoint = obb.center;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const float distance = Math::Dot(delta, obb.axis[axis]);
			localCoordinates[axis] = distance;
			const float halfSize = axis == 0
				? obb.halfSize.x
				: (axis == 1 ? obb.halfSize.y : obb.halfSize.z);
			closestPoint = Math::Add(
				closestPoint,
				Math::Multiply(
					obb.axis[axis],
					std::clamp(distance, -halfSize, halfSize)
				)
			);
		}
		return closestPoint;
	}

	bool TryComputeSphereSpherePushOut(
		const SphereCollider& moving,
		const SphereCollider& obstacle,
		Vector3& pushOut
	) {
		const Vector3 delta = Math::Subtract(
			moving.GetWorldCenter(),
			obstacle.GetWorldCenter()
		);
		const float distance = Math::Length(delta);
		const float overlap = moving.GetRadius() + obstacle.GetRadius() - distance;
		if (overlap <= kPenetrationSlop) {
			return false;
		}
		const Vector3 direction = distance > kPenetrationEpsilon
			? Math::Multiply(delta, 1.0f / distance)
			: Vector3{ 0.0f, 1.0f, 0.0f };
		pushOut = Math::Multiply(direction, overlap + kPenetrationSkin);
		return true;
	}

	bool TryComputeSphereObbPushOut(
		const SphereCollider& moving,
		const OBBCollider::OBB& obstacle,
		Vector3& pushOut
	) {
		std::array<float, 3> localCoordinates{};
		const Vector3 center = moving.GetWorldCenter();
		const Vector3 closestPoint =
			ClosestPointOnOBB(obstacle, center, localCoordinates);
		const Vector3 delta = Math::Subtract(center, closestPoint);
		const float distance = Math::Length(delta);
		if (distance > kPenetrationEpsilon) {
			const float overlap = moving.GetRadius() - distance;
			if (overlap <= kPenetrationSlop) {
				return false;
			}
			pushOut = Math::Multiply(
				delta,
				(overlap + kPenetrationSkin) / distance
			);
			return true;
		}

		float nearestFaceDistance = FLT_MAX;
		Vector3 outwardAxis{};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const float halfSize = axis == 0
				? obstacle.halfSize.x
				: (axis == 1 ? obstacle.halfSize.y : obstacle.halfSize.z);
			const float faceDistance =
				halfSize - std::fabs(localCoordinates[axis]);
			if (faceDistance < nearestFaceDistance) {
				nearestFaceDistance = faceDistance;
				outwardAxis = localCoordinates[axis] < 0.0f
					? Math::Multiply(obstacle.axis[axis], -1.0f)
					: obstacle.axis[axis];
			}
		}
		if (nearestFaceDistance == FLT_MAX) {
			return false;
		}
		pushOut = Math::Multiply(
			outwardAxis,
			moving.GetRadius() + nearestFaceDistance + kPenetrationSkin
		);
		return true;
	}

	bool TryComputeObbSpherePushOut(
		const OBBCollider::OBB& moving,
		const SphereCollider& obstacle,
		Vector3& pushOut
	) {
		std::array<float, 3> localCoordinates{};
		const Vector3 closestPoint = ClosestPointOnOBB(
			moving,
			obstacle.GetWorldCenter(),
			localCoordinates
		);
		const Vector3 delta = Math::Subtract(
			closestPoint,
			obstacle.GetWorldCenter()
		);
		const float distance = Math::Length(delta);
		if (distance > kPenetrationEpsilon) {
			const float overlap = obstacle.GetRadius() - distance;
			if (overlap <= kPenetrationSlop) {
				return false;
			}
			pushOut = Math::Multiply(
				delta,
				(overlap + kPenetrationSkin) / distance
			);
			return true;
		}

		float nearestFaceDistance = FLT_MAX;
		Vector3 outwardAxis{};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const float halfSize = axis == 0
				? moving.halfSize.x
				: (axis == 1 ? moving.halfSize.y : moving.halfSize.z);
			const float faceDistance =
				halfSize - std::fabs(localCoordinates[axis]);
			if (faceDistance < nearestFaceDistance) {
				nearestFaceDistance = faceDistance;
				outwardAxis = localCoordinates[axis] < 0.0f
					? Math::Multiply(moving.axis[axis], -1.0f)
					: moving.axis[axis];
			}
		}
		if (nearestFaceDistance == FLT_MAX) {
			return false;
		}
		pushOut = Math::Multiply(
			outwardAxis,
			-(obstacle.GetRadius() + nearestFaceDistance + kPenetrationSkin)
		);
		return true;
	}

	// 接触面を床として維持しつつ横移動できるよう、PhysicsのSweepでは
	// 「接触」ではなく押し戻しを要する量の貫通だけを停止条件にする。
	// Trigger CombatはCollider::Intersectsを使うため、この判定変更の対象外。
	bool HasBlockingOverlap(const Collider& moving, const Collider& obstacle) {
		if (!moving.CanCollideWith(obstacle)) {
			return false;
		}
		Vector3 pushOut{};
		if (moving.GetType() == Collider::Type::Sphere) {
			const auto& sphere = static_cast<const SphereCollider&>(moving);
			return obstacle.GetType() == Collider::Type::Sphere
				? TryComputeSphereSpherePushOut(
					sphere, static_cast<const SphereCollider&>(obstacle), pushOut
				)
				: TryComputeSphereObbPushOut(
					sphere, static_cast<const OBBCollider&>(obstacle).GetOBB(), pushOut
				);
		}
		const auto& box = static_cast<const OBBCollider&>(moving);
		return obstacle.GetType() == Collider::Type::Sphere
			? TryComputeObbSpherePushOut(
				box.GetOBB(), static_cast<const SphereCollider&>(obstacle), pushOut
			)
			: TryComputePushOut(
				box.GetOBB(), static_cast<const OBBCollider&>(obstacle).GetOBB(), pushOut
			);
	}
}

void PhysicsWorld::Clear() {
	bodies_.clear();
	staticColliders_.clear();
	ignoredCollisions_.clear();
}

void PhysicsWorld::AddBody(PhysicsBody* body) {
	if (!body) {
		return;
	}
	bodies_.push_back(body);
}

void PhysicsWorld::AddStaticCollider(Collider* collider) {
	if (!collider) {
		return;
	}
	staticColliders_.push_back(collider);
}

void PhysicsWorld::AddIgnoredCollision(
	PhysicsBody* body,
	Collider* collider
) {
	if (!body || !collider) {
		return;
	}
	const auto duplicate = std::find_if(
		ignoredCollisions_.begin(),
		ignoredCollisions_.end(),
		[body, collider](const IgnoredCollisionPair& pair) {
			return pair.body == body && pair.collider == collider;
		}
	);
	if (duplicate == ignoredCollisions_.end()) {
		ignoredCollisions_.push_back({ body, collider });
	}
}

bool PhysicsWorld::IsCollisionIgnored(
	const PhysicsBody& body,
	const Collider& collider
) const {
	return std::find_if(
		ignoredCollisions_.begin(),
		ignoredCollisions_.end(),
		[&body, &collider](const IgnoredCollisionPair& pair) {
			return pair.body == &body && pair.collider == &collider;
		}
	) != ignoredCollisions_.end();
}

void PhysicsWorld::Step(float deltaTime) {
	if (deltaTime <= 0.0f) {
		return;
	}
	deltaTime = (std::min)(deltaTime, 1.0f / 15.0f);

	for (PhysicsBody* body : bodies_) {
		if (
			!body ||
			body->type != PhysicsBodyType::Dynamic ||
			!body->transform
		) {
			continue;
		}
		// Colliderは常にObject3d更新後のワールド行列を使って判定する。
		body->SynchronizeTransform();

		if (body->useGravity) {
			body->velocity = Math::Add(
				body->velocity,
				Math::Multiply(gravity_, body->gravityScale * deltaTime)
			);
		}
		body->isGrounded = false;
		ResolveStaticPenetration(*body);

		const float dragFactor = std::clamp(
			1.0f - body->drag * deltaTime,
			0.0f,
			1.0f
		);
		body->velocity = Math::Multiply(body->velocity, dragFactor);
		if (body->maxFallSpeed > 0.0f) {
			body->velocity.y = (std::max)(
				body->velocity.y,
				-body->maxFallSpeed
			);
		}

		Vector3 delta = Math::Multiply(body->velocity, deltaTime);
		if (body->freezePositionX) {
			delta.x = 0.0f;
			body->velocity.x = 0.0f;
		}
		if (body->freezePositionY) {
			delta.y = 0.0f;
			body->velocity.y = 0.0f;
		}
		if (body->freezePositionZ) {
			delta.z = 0.0f;
			body->velocity.z = 0.0f;
		}

		IntegrateAxis(*body, delta.x, 0);
		const bool collidedY = IntegrateAxis(*body, delta.y, 1);
		if (collidedY && delta.y < 0.0f) {
			body->isGrounded = true;
			const float frictionFactor = std::clamp(
				1.0f - body->friction,
				0.0f,
				1.0f
			);
			body->velocity.x *= frictionFactor;
			body->velocity.z *= frictionFactor;
		}
		IntegrateAxis(*body, delta.z, 2);
		if (
			!body->isGrounded &&
			body->velocity.y <= 0.0f &&
			SnapToGround(*body, kGroundProbeDistance)
		) {
			body->isGrounded = true;
			body->velocity.y = 0.0f;
		}
		ResolveStaticPenetration(*body);
	}
}

bool PhysicsWorld::CollidesWithStatic(const PhysicsBody& body) const {
	if (!body.collider || body.collider->IsTrigger()) {
		return false;
	}

	for (const PhysicsBody* other : bodies_) {
		if (
			!other ||
			other == &body ||
			!other->collider ||
			other->type == PhysicsBodyType::Dynamic ||
			other->collider->IsTrigger() ||
			IsCollisionIgnored(body, *other->collider)
		) {
			continue;
		}
		if (HasBlockingOverlap(*body.collider, *other->collider)) {
			return true;
		}
	}

	for (const Collider* collider : staticColliders_) {
		if (
			collider &&
			collider != body.collider &&
			!collider->IsTrigger() &&
			!IsCollisionIgnored(body, *collider) &&
			HasBlockingOverlap(*body.collider, *collider)
		) {
			return true;
		}
	}

	return false;
}

bool PhysicsWorld::SnapToGround(
	PhysicsBody& body,
	float probeDistance
) const {
	if (
		!body.transform ||
		!body.collider ||
		probeDistance <= 0.0f
	) {
		return false;
	}

	const float startY = body.transform->translate.y;
	const float delta = -probeDistance;
	body.transform->translate.y = startY + delta;
	body.SynchronizeTransform();
	if (!CollidesWithStatic(body)) {
		body.transform->translate.y = startY;
		body.SynchronizeTransform();
		return false;
	}

	float safeRate = 0.0f;
	float hitRate = 1.0f;
	for (uint32_t i = 0; i < kAxisSweepIterations; ++i) {
		const float testRate = (safeRate + hitRate) * 0.5f;
		body.transform->translate.y = startY + delta * testRate;
		body.SynchronizeTransform();
		if (CollidesWithStatic(body)) {
			hitRate = testRate;
		} else {
			safeRate = testRate;
		}
	}
	body.transform->translate.y = startY + delta * safeRate;
	body.SynchronizeTransform();
	return true;
}

bool PhysicsWorld::ResolveStaticPenetration(PhysicsBody& body) const {
	if (
		!body.transform ||
		!body.collider ||
		body.collider->IsTrigger()
	) {
		return false;
	}

	bool resolvedAny = false;
	std::vector<const Collider*> testedColliders;

	auto resolveAgainst = [&](const Collider* candidate) {
		if (
			!candidate ||
			candidate == body.collider ||
			candidate->IsTrigger() ||
			IsCollisionIgnored(body, *candidate) ||
			!body.collider->CanCollideWith(*candidate) ||
			std::find(
				testedColliders.begin(),
				testedColliders.end(),
				candidate
			) != testedColliders.end()
		) {
			return;
		}
		testedColliders.push_back(candidate);

		for (uint32_t iteration = 0;
			iteration < kPenetrationResolveIterations;
			++iteration) {
			Vector3 pushOut{};
			bool intersects = false;
			if (body.collider->GetType() == Collider::Type::Sphere) {
				const auto& moving =
					static_cast<const SphereCollider&>(*body.collider);
				if (candidate->GetType() == Collider::Type::Sphere) {
					intersects = TryComputeSphereSpherePushOut(
						moving,
						static_cast<const SphereCollider&>(*candidate),
						pushOut
					);
				} else {
					intersects = TryComputeSphereObbPushOut(
						moving,
						static_cast<const OBBCollider&>(*candidate).GetOBB(),
						pushOut
					);
				}
			} else {
				const auto& moving =
					static_cast<const OBBCollider&>(*body.collider);
				if (candidate->GetType() == Collider::Type::Sphere) {
					intersects = TryComputeObbSpherePushOut(
						moving.GetOBB(),
						static_cast<const SphereCollider&>(*candidate),
						pushOut
					);
				} else {
					intersects = TryComputePushOut(
						moving.GetOBB(),
						static_cast<const OBBCollider&>(*candidate).GetOBB(),
						pushOut
					);
				}
			}
			if (!intersects) {
				return;
			}

			ApplyFreezeAxes(body, pushOut);
			if (Math::Length(pushOut) < kPenetrationEpsilon) {
				return;
			}

			body.transform->translate = Math::Add(
				body.transform->translate,
				pushOut
			);
			body.SynchronizeTransform();
			if (std::abs(pushOut.x) > kPenetrationEpsilon) {
				body.velocity.x = 0.0f;
			}
			if (std::abs(pushOut.y) > kPenetrationEpsilon) {
				body.velocity.y = 0.0f;
				if (pushOut.y > 0.0f) {
					body.isGrounded = true;
				}
			}
			if (std::abs(pushOut.z) > kPenetrationEpsilon) {
				body.velocity.z = 0.0f;
			}
			resolvedAny = true;
		}
	};

	for (const PhysicsBody* other : bodies_) {
		if (
			!other ||
			other == &body ||
			!other->collider ||
			other->type == PhysicsBodyType::Dynamic
		) {
			continue;
		}
		resolveAgainst(other->collider);
	}

	for (const Collider* collider : staticColliders_) {
		resolveAgainst(collider);
	}

	return resolvedAny;
}

bool PhysicsWorld::IntegrateAxis(
	PhysicsBody& body,
	float delta,
	uint32_t axis
) const {
	if (std::abs(delta) < 0.000001f || !body.transform) {
		return false;
	}

	float* components[] = {
		&body.transform->translate.x,
		&body.transform->translate.y,
		&body.transform->translate.z
	};
	float* velocityComponents[] = {
		&body.velocity.x,
		&body.velocity.y,
		&body.velocity.z
	};

	const float startPosition = *components[axis];
	*components[axis] += delta;
	body.SynchronizeTransform();
	if (CollidesWithStatic(body)) {
		*components[axis] = startPosition;
		body.SynchronizeTransform();
		float safeRate = 0.0f;
		float hitRate = 1.0f;
		for (uint32_t i = 0; i < kAxisSweepIterations; ++i) {
			const float testRate = (safeRate + hitRate) * 0.5f;
			*components[axis] = startPosition + delta * testRate;
			body.SynchronizeTransform();
			if (CollidesWithStatic(body)) {
				hitRate = testRate;
			} else {
				safeRate = testRate;
			}
		}
		*components[axis] = startPosition + delta * safeRate;
		body.SynchronizeTransform();
		const float velocity = *velocityComponents[axis];
		if (
			body.restitution > 0.0f &&
			std::abs(velocity) > kBounceVelocityThreshold
		) {
			*velocityComponents[axis] =
				-velocity * std::clamp(body.restitution, 0.0f, 1.0f);
		} else {
			*velocityComponents[axis] = 0.0f;
		}
		return true;
	}
	return false;
}
