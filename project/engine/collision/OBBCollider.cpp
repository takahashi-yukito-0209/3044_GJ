// 役割: ワールド行列からOBBの軸と半サイズを計算する。
#include "OBBCollider.h"

#include "SphereCollider.h"
#include "../math/Matrix4x4.h"
#include "../math/Math.h"
namespace {
	Vector3 GetMatrixAxis(const Matrix4x4& matrix, uint32_t index) {
		Vector3 axis = {
			matrix.m[index][0],
			matrix.m[index][1],
			matrix.m[index][2]
		};

		axis = Math::Normalize(axis);
		if (Math::Length(axis) < 0.000001f) {
			if (index == 0) {
				return { 1.0f, 0.0f, 0.0f };
			}
			if (index == 1) {
				return { 0.0f, 1.0f, 0.0f };
			}
			return { 0.0f, 0.0f, 1.0f };
		}

		return axis;
	}
}

bool OBBCollider::Intersects(const Collider& other) const {
	if (!CanCollideWith(other)) {
		return false;
	}

	switch (other.GetType()) {
	case Type::Sphere:
		return CheckCollision(static_cast<const SphereCollider&>(other), *this);
	case Type::OBB:
		return CheckCollision(*this, static_cast<const OBBCollider&>(other));
	default:
		return false;
	}
}

OBBCollider::OBB OBBCollider::GetOBB() const {
	OBB obb{};
	obb.center = GetWorldCenter();

	const Matrix4x4 fallbackMatrix = MakeIdentity4x4();
	const Matrix4x4& worldMatrix =
		GetWorldMatrix() ? *GetWorldMatrix() : fallbackMatrix;
	const Matrix4x4 localRotation = MakeAffineMatrix(
		{ 1.0f, 1.0f, 1.0f }, localRotation_, {}
	);
	const float halfSizes[] = { halfSize_.x, halfSize_.y, halfSize_.z };
	for (uint32_t axisIndex = 0; axisIndex < 3; ++axisIndex) {
		const Vector3 localAxis = {
			localRotation.m[axisIndex][0],
			localRotation.m[axisIndex][1],
			localRotation.m[axisIndex][2]
		};
		const Vector3 transformedAxis = {
			localAxis.x * worldMatrix.m[0][0] +
				localAxis.y * worldMatrix.m[1][0] +
				localAxis.z * worldMatrix.m[2][0],
			localAxis.x * worldMatrix.m[0][1] +
				localAxis.y * worldMatrix.m[1][1] +
				localAxis.z * worldMatrix.m[2][1],
			localAxis.x * worldMatrix.m[0][2] +
				localAxis.y * worldMatrix.m[1][2] +
				localAxis.z * worldMatrix.m[2][2]
		};
		const float worldScale = Math::Length(transformedAxis);
		obb.axis[axisIndex] = worldScale > 0.000001f
			? Math::Normalize(transformedAxis)
			: GetMatrixAxis(worldMatrix, axisIndex);
		if (axisIndex == 0) {
			obb.halfSize.x = halfSizes[axisIndex] * worldScale;
		} else if (axisIndex == 1) {
			obb.halfSize.y = halfSizes[axisIndex] * worldScale;
		} else {
			obb.halfSize.z = halfSizes[axisIndex] * worldScale;
		}
	}

	return obb;
}
