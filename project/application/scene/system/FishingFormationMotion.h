// 役割: プレイヤー群れのXZカプセルを静的障害物と水境界に沿って移動させる純粋計算を提供する。
#pragma once

#include "../../../engine/math/Vector2.h"

#include <cstdint>
#include <vector>

namespace FishingFormationMotion {

struct Obstacle {
	uint64_t entityId = 0;
	std::vector<Vector2> hull;
};

struct CenterBounds {
	bool enabled = false;
	Vector2 center{};
	float yaw = 0.0f;
	float halfSizeX = 0.0f;
	float halfSizeZ = 0.0f;
};

struct Request {
	Vector2 startCenter{};
	Vector2 desiredCenter{};
	Vector2 desiredVelocity{};
	float startYaw = 0.0f;
	float desiredYaw = 0.0f;
	float radius = 0.0f;
	float halfSegmentLength = 0.0f;
	CenterBounds bounds{};
};

struct Result {
	Vector2 center{};
	Vector2 velocity{};
	float yaw = 0.0f;
	bool translationBlocked = false;
	bool rotationBlocked = false;
	bool iterationLimited = false;
};

bool Solve(
	const Request& request,
	const std::vector<Obstacle>& obstacles,
	Result& result
);

} // namespace FishingFormationMotion
