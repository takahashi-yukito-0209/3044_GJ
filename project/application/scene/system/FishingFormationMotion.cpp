#include "FishingFormationMotion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace FishingFormationMotion {
namespace {

constexpr double kEpsilon = 1.0e-9;
constexpr double kSegmentEpsilon = 1.0e-12;
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr int kRotationIterations = 64;
constexpr int kCastIterations = 32;
constexpr int kSlideIterations = 4;

struct Vec2 {
	double x = 0.0;
	double y = 0.0;
};

struct DistanceInfo {
	double distance = 0.0;
	Vec2 polygonPoint{};
	Vec2 segmentPoint{};
};

struct Contact {
	Vec2 normal{};
};

struct CastHit {
	bool hit = false;
	double time = 1.0;
	std::vector<Contact> contacts;
};

Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
Vec2 Subtract(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
Vec2 Multiply(Vec2 value, double scalar) {
	return { value.x * scalar, value.y * scalar };
}
double Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double LengthSquared(Vec2 value) { return Dot(value, value); }

bool IsFinite(Vec2 value) {
	return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(float value) {
	return std::isfinite(static_cast<double>(value));
}

Vec2 ToVec2(Vector2 value) {
	return { static_cast<double>(value.x), static_cast<double>(value.y) };
}

bool ToFloat(Vec2 value, Vector2& output) {
	if (!IsFinite(value) ||
		std::abs(value.x) > std::numeric_limits<float>::max() ||
		std::abs(value.y) > std::numeric_limits<float>::max()) {
		return false;
	}
	output = {
		static_cast<float>(value.x),
		static_cast<float>(value.y)
	};
	return true;
}

bool Normalize(Vec2 value, Vec2& normalized) {
	const double lengthSquared = LengthSquared(value);
	if (!std::isfinite(lengthSquared) || lengthSquared <= kSegmentEpsilon) {
		return false;
	}
	const double inverseLength = 1.0 / std::sqrt(lengthSquared);
	normalized = Multiply(value, inverseLength);
	return IsFinite(normalized);
}

Vec2 Forward(float yaw) {
	return { std::sin(static_cast<double>(yaw)), std::cos(static_cast<double>(yaw)) };
}

double WrapAngle(double angle) {
	while (angle > kPi) {
		angle -= 2.0 * kPi;
	}
	while (angle < -kPi) {
		angle += 2.0 * kPi;
	}
	return angle;
}

Vec2 CapsuleEndpoint(Vec2 center, double yaw, double halfSegmentLength, double sign) {
	return Add(center, Multiply(Forward(static_cast<float>(yaw)), sign * halfSegmentLength));
}

Vec2 ClosestPointOnSegment(Vec2 point, Vec2 start, Vec2 end) {
	const Vec2 edge = Subtract(end, start);
	const double edgeLengthSquared = LengthSquared(edge);
	if (edgeLengthSquared <= kSegmentEpsilon) {
		return start;
	}
	const double parameter = std::clamp(
		Dot(Subtract(point, start), edge) / edgeLengthSquared,
		0.0,
		1.0
	);
	return Add(start, Multiply(edge, parameter));
}

bool IsPointInsideConvexPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
	for (size_t index = 0; index < polygon.size(); ++index) {
		const Vec2 edge = Subtract(
			polygon[(index + 1) % polygon.size()], polygon[index]
		);
		if (Cross(edge, Subtract(point, polygon[index])) < -kEpsilon) {
			return false;
		}
	}
	return true;
}

int SignWithEpsilon(double value) {
	if (value > kEpsilon) return 1;
	if (value < -kEpsilon) return -1;
	return 0;
}

bool OnSegment(Vec2 point, Vec2 start, Vec2 end) {
	return
		point.x >= (std::min)(start.x, end.x) - kEpsilon &&
		point.x <= (std::max)(start.x, end.x) + kEpsilon &&
		point.y >= (std::min)(start.y, end.y) - kEpsilon &&
		point.y <= (std::max)(start.y, end.y) + kEpsilon;
}

bool SegmentsIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
	const int first = SignWithEpsilon(Cross(Subtract(b, a), Subtract(c, a)));
	const int second = SignWithEpsilon(Cross(Subtract(b, a), Subtract(d, a)));
	const int third = SignWithEpsilon(Cross(Subtract(d, c), Subtract(a, c)));
	const int fourth = SignWithEpsilon(Cross(Subtract(d, c), Subtract(b, c)));
	if (first == 0 && OnSegment(c, a, b)) return true;
	if (second == 0 && OnSegment(d, a, b)) return true;
	if (third == 0 && OnSegment(a, c, d)) return true;
	if (fourth == 0 && OnSegment(b, c, d)) return true;
	return first != second && third != fourth;
}

void ConsiderClosest(
	Vec2 polygonPoint,
	Vec2 segmentPoint,
	DistanceInfo& best
) {
	const double distance = std::sqrt(LengthSquared(
		Subtract(segmentPoint, polygonPoint)
	));
	if (distance < best.distance) {
		best.distance = distance;
		best.polygonPoint = polygonPoint;
		best.segmentPoint = segmentPoint;
	}
}

DistanceInfo SegmentPolygonDistance(
	Vec2 segmentStart,
	Vec2 segmentEnd,
	const std::vector<Vec2>& polygon
) {
	DistanceInfo result{
		std::numeric_limits<double>::max(),
		{},
		{}
	};
	if (
		IsPointInsideConvexPolygon(segmentStart, polygon) ||
		IsPointInsideConvexPolygon(segmentEnd, polygon)
	) {
		result.distance = 0.0;
		result.polygonPoint = segmentStart;
		result.segmentPoint = segmentStart;
		return result;
	}
	for (size_t index = 0; index < polygon.size(); ++index) {
		const Vec2 polygonStart = polygon[index];
		const Vec2 polygonEnd = polygon[(index + 1) % polygon.size()];
		if (SegmentsIntersect(segmentStart, segmentEnd, polygonStart, polygonEnd)) {
			result.distance = 0.0;
			result.polygonPoint = polygonStart;
			result.segmentPoint = polygonStart;
			return result;
		}
		ConsiderClosest(
			polygonStart,
			ClosestPointOnSegment(polygonStart, segmentStart, segmentEnd),
			result
		);
		ConsiderClosest(
			ClosestPointOnSegment(segmentStart, polygonStart, polygonEnd),
			segmentStart,
			result
		);
		ConsiderClosest(
			ClosestPointOnSegment(segmentEnd, polygonStart, polygonEnd),
			segmentEnd,
			result
		);
	}
	return result;
}

DistanceInfo CapsulePolygonDistance(
	Vec2 center,
	double yaw,
	double halfSegmentLength,
	const std::vector<Vec2>& polygon
) {
	const Vec2 start = CapsuleEndpoint(center, static_cast<float>(yaw), halfSegmentLength, -1.0);
	const Vec2 end = CapsuleEndpoint(center, static_cast<float>(yaw), halfSegmentLength, 1.0);
	return SegmentPolygonDistance(start, end, polygon);
}

bool ValidatePolygon(const Obstacle& obstacle, std::vector<Vec2>& polygon) {
	if (obstacle.hull.size() < 3) {
		return false;
	}
	polygon.clear();
	polygon.reserve(obstacle.hull.size());
	for (const Vector2 point : obstacle.hull) {
		const Vec2 converted = ToVec2(point);
		if (!IsFinite(converted)) {
			return false;
		}
		polygon.push_back(converted);
	}
	double areaTwice = 0.0;
	for (size_t index = 0; index < polygon.size(); ++index) {
		const Vec2 current = polygon[index];
		const Vec2 next = polygon[(index + 1) % polygon.size()];
		if (LengthSquared(Subtract(next, current)) <= kSegmentEpsilon) {
			return false;
		}
		areaTwice += Cross(current, next);
	}
	if (areaTwice <= kEpsilon) {
		return false;
	}
	for (size_t index = 0; index < polygon.size(); ++index) {
		const Vec2 a = polygon[index];
		const Vec2 b = polygon[(index + 1) % polygon.size()];
		const Vec2 c = polygon[(index + 2) % polygon.size()];
		if (Cross(Subtract(b, a), Subtract(c, b)) < -kEpsilon) {
			return false;
		}
	}
	return true;
}

Vec2 WorldToLocal(Vec2 world, Vec2 center, double yaw) {
	const Vec2 delta = Subtract(world, center);
	const double cosine = std::cos(yaw);
	const double sine = std::sin(yaw);
	return {
		delta.x * cosine - delta.y * sine,
		delta.x * sine + delta.y * cosine
	};
}

Vec2 LocalNormalToWorld(Vec2 local, double yaw) {
	return {
		local.x * std::cos(yaw) + local.y * std::sin(yaw),
		-local.x * std::sin(yaw) + local.y * std::cos(yaw)
	};
}

bool IsSafePose(
	Vec2 center,
	double yaw,
	double radius,
	double halfSegmentLength,
	const std::vector<std::vector<Vec2>>& polygons,
	const CenterBounds& bounds,
	double epsilon
) {
	if (bounds.enabled) {
		const Vec2 local = WorldToLocal(
			center, ToVec2(bounds.center), static_cast<double>(bounds.yaw)
		);
		if (
			local.x < -static_cast<double>(bounds.halfSizeX) - epsilon ||
			local.x > static_cast<double>(bounds.halfSizeX) + epsilon ||
			local.y < -static_cast<double>(bounds.halfSizeZ) - epsilon ||
			local.y > static_cast<double>(bounds.halfSizeZ) + epsilon
		) {
			return false;
		}
	}
	for (const std::vector<Vec2>& polygon : polygons) {
		const DistanceInfo distance = CapsulePolygonDistance(
			center, yaw, halfSegmentLength, polygon
		);
		if (distance.distance < radius - epsilon) {
			return false;
		}
	}
	return true;
}

void AddContact(std::vector<Contact>& contacts, Vec2 normal) {
	Vec2 normalized{};
	if (Normalize(normal, normalized)) {
		contacts.push_back({ normalized });
	}
}

void AddWaterCast(
	Vec2 center,
	Vec2 delta,
	const CenterBounds& bounds,
	CastHit& hit
) {
	if (!bounds.enabled) {
		return;
	}
	const double yaw = static_cast<double>(bounds.yaw);
	const Vec2 localCenter = WorldToLocal(
		center, ToVec2(bounds.center), yaw
	);
	const Vec2 localDelta = {
		delta.x * std::cos(yaw) - delta.y * std::sin(yaw),
		delta.x * std::sin(yaw) + delta.y * std::cos(yaw)
	};
	const double limits[4] = {
		static_cast<double>(bounds.halfSizeX),
		-static_cast<double>(bounds.halfSizeX),
		static_cast<double>(bounds.halfSizeZ),
		-static_cast<double>(bounds.halfSizeZ)
	};
	const double positions[4] = {
		localCenter.x, localCenter.x, localCenter.y, localCenter.y
	};
	const double directions[4] = {
		localDelta.x, localDelta.x, localDelta.y, localDelta.y
	};
	const Vec2 inwardNormals[4] = {
		{ -1.0, 0.0 }, { 1.0, 0.0 },
		{ 0.0, -1.0 }, { 0.0, 1.0 }
	};
	for (int side = 0; side < 4; ++side) {
		const bool movingOutward = side == 0 || side == 2
			? directions[side] > kEpsilon
			: directions[side] < -kEpsilon;
		if (!movingOutward) {
			continue;
		}
		const double parameter = (limits[side] - positions[side]) / directions[side];
		if (parameter < -kEpsilon || parameter > 1.0 + kEpsilon) {
			continue;
		}
		const double clampedParameter = std::clamp(parameter, 0.0, 1.0);
		const Vec2 normal = LocalNormalToWorld(inwardNormals[side], yaw);
		if (!hit.hit || clampedParameter < hit.time - kEpsilon) {
			hit = {};
			hit.hit = true;
			hit.time = clampedParameter;
			AddContact(hit.contacts, normal);
		} else if (std::abs(clampedParameter - hit.time) <= kEpsilon) {
			AddContact(hit.contacts, normal);
		}
	}
}

bool CastObstacle(
	Vec2 center,
	Vec2 delta,
	double yaw,
	double radius,
	double halfSegmentLength,
	const std::vector<Vec2>& polygon,
	double epsilon,
	CastHit& hit
) {
	double parameter = 0.0;
	double lastSafeParameter = 0.0;
	Vec2 lastNormal{};
	bool hasLastNormal = false;
	for (int iteration = 0; iteration < kCastIterations; ++iteration) {
		const Vec2 current = Add(center, Multiply(delta, parameter));
		const DistanceInfo distance = CapsulePolygonDistance(
			current, yaw, halfSegmentLength, polygon
		);
		if (distance.distance < radius - epsilon) {
			if (parameter <= kEpsilon || !hasLastNormal) {
				return false;
			}
			parameter = lastSafeParameter;
			break;
		}
		Vec2 normal{};
		if (!Normalize(
			Subtract(distance.segmentPoint, distance.polygonPoint), normal
		)) {
			return false;
		}
		lastNormal = normal;
		hasLastNormal = true;
		const double gap = distance.distance - radius -
			std::clamp(radius * 0.01, 0.001, 0.02);
		const double closing = -Dot(delta, normal);
		if (closing <= kEpsilon) {
			return false;
		}
		if (gap <= epsilon) {
			hit = {};
			hit.hit = true;
			hit.time = parameter;
			AddContact(hit.contacts, normal);
			return true;
		}
		const double step = gap / closing;
		if (!std::isfinite(step) || step <= kEpsilon) {
			hit = {};
			hit.hit = true;
			hit.time = parameter;
			AddContact(hit.contacts, lastNormal);
			return true;
		}
		const double nextParameter = parameter + step;
		if (nextParameter > 1.0 + kEpsilon) {
			return false;
		}
		lastSafeParameter = parameter;
		parameter = std::clamp(nextParameter, 0.0, 1.0);
	}
	if (!hasLastNormal) {
		return false;
	}
	hit = {};
	hit.hit = true;
	hit.time = std::clamp(lastSafeParameter, 0.0, 1.0);
	AddContact(hit.contacts, lastNormal);
	return !hit.contacts.empty();
}

CastHit Cast(
	Vec2 center,
	Vec2 delta,
	double yaw,
	double radius,
	double halfSegmentLength,
	const std::vector<std::vector<Vec2>>& polygons,
	const std::vector<size_t>& obstacleOrder,
	const CenterBounds& bounds,
	double epsilon
) {
	CastHit best{};
	AddWaterCast(center, delta, bounds, best);
	for (const size_t index : obstacleOrder) {
		CastHit candidate{};
		if (!CastObstacle(
			center, delta, yaw, radius, halfSegmentLength,
			polygons[index], epsilon, candidate
		)) {
			continue;
		}
		if (!best.hit || candidate.time < best.time - kEpsilon) {
			best = std::move(candidate);
		} else if (std::abs(candidate.time - best.time) <= kEpsilon) {
			best.contacts.insert(
				best.contacts.end(),
				candidate.contacts.begin(),
				candidate.contacts.end()
			);
		}
	}
	return best;
}

bool IsFeasible(Vec2 value, const std::vector<Contact>& contacts) {
	for (const Contact& contact : contacts) {
		if (Dot(value, contact.normal) < -kEpsilon) {
			return false;
		}
	}
	return true;
}

Vec2 ProjectToContactCone(Vec2 value, const std::vector<Contact>& contacts) {
	if (contacts.empty() || IsFeasible(value, contacts)) {
		return value;
	}
	Vec2 best = {};
	double bestDistance = LengthSquared(value);
	for (const Contact& contact : contacts) {
		const Vec2 candidate = Subtract(
			value,
			Multiply(contact.normal, Dot(value, contact.normal))
		);
		if (!IsFeasible(candidate, contacts)) {
			continue;
		}
		const double distance = LengthSquared(Subtract(candidate, value));
		if (distance < bestDistance - kEpsilon) {
			best = candidate;
			bestDistance = distance;
		}
	}
	return best;
}

bool SweepRotation(
	Vec2 center,
	double startYaw,
	double desiredYaw,
	double radius,
	double halfSegmentLength,
	const std::vector<std::vector<Vec2>>& polygons,
	double epsilon,
	double& outputYaw,
	bool& iterationLimited
) {
	const double deltaYaw = WrapAngle(desiredYaw - startYaw);
	outputYaw = startYaw;
	if (halfSegmentLength <= kSegmentEpsilon || std::abs(deltaYaw) <= kEpsilon) {
		outputYaw = desiredYaw;
		return true;
	}
	double progress = 0.0;
	for (int iteration = 0; iteration < kRotationIterations; ++iteration) {
		if (progress >= 1.0 - kEpsilon) {
			outputYaw = desiredYaw;
			return true;
		}
		const double currentYaw = startYaw + deltaYaw * progress;
		double minimumGap = std::numeric_limits<double>::max();
		for (const std::vector<Vec2>& polygon : polygons) {
			const DistanceInfo distance = CapsulePolygonDistance(
				center, currentYaw, halfSegmentLength, polygon
			);
			minimumGap = (std::min)(minimumGap, distance.distance - radius);
		}
		if (minimumGap <= epsilon) {
			return false;
		}
		const double angularTravel = halfSegmentLength * std::abs(deltaYaw);
		const double maximumProgress = angularTravel > kSegmentEpsilon
			? std::clamp((minimumGap - epsilon) * 0.8 / angularTravel, 0.0, 1.0)
			: 1.0;
		const double step = (std::min)(1.0 - progress, maximumProgress);
		if (step <= 1.0e-6) {
			return false;
		}
		const double nextProgress = progress + step;
		const double nextYaw = startYaw + deltaYaw * nextProgress;
		bool safe = true;
		for (const std::vector<Vec2>& polygon : polygons) {
			const DistanceInfo distance = CapsulePolygonDistance(
				center, nextYaw, halfSegmentLength, polygon
			);
			if (distance.distance < radius - epsilon) {
				safe = false;
				break;
			}
		}
		if (!safe) {
			return false;
		}
		progress = nextProgress;
		outputYaw = nextYaw;
	}
	iterationLimited = true;
	return false;
}

bool ValidateRequest(
	const Request& request,
	const std::vector<Obstacle>& obstacles,
	std::vector<std::vector<Vec2>>& polygons,
	std::vector<size_t>& obstacleOrder,
	std::vector<uint64_t>& obstacleIds
) {
	if (
		!IsFinite(request.startYaw) || !IsFinite(request.desiredYaw) ||
		!IsFinite(request.radius) || !IsFinite(request.halfSegmentLength) ||
		!IsFinite(request.startCenter.x) || !IsFinite(request.startCenter.y) ||
		!IsFinite(request.desiredCenter.x) || !IsFinite(request.desiredCenter.y) ||
		!IsFinite(request.desiredVelocity.x) || !IsFinite(request.desiredVelocity.y) ||
		request.radius <= 0.0f || request.halfSegmentLength < 0.0f
	) {
		return false;
	}
	if (request.bounds.enabled && (
		!IsFinite(request.bounds.center.x) || !IsFinite(request.bounds.center.y) ||
		!IsFinite(request.bounds.yaw) || !IsFinite(request.bounds.halfSizeX) ||
		!IsFinite(request.bounds.halfSizeZ) || request.bounds.halfSizeX <= 0.0f ||
		request.bounds.halfSizeZ <= 0.0f
	)) {
		return false;
	}
	polygons.clear();
	obstacleOrder.clear();
	obstacleIds.clear();
	polygons.reserve(obstacles.size());
	for (size_t index = 0; index < obstacles.size(); ++index) {
		std::vector<Vec2> polygon;
		if (!ValidatePolygon(obstacles[index], polygon)) {
			return false;
		}
		polygons.push_back(std::move(polygon));
		obstacleOrder.push_back(index);
		obstacleIds.push_back(obstacles[index].entityId);
	}
	std::sort(
		obstacleOrder.begin(), obstacleOrder.end(),
		[&obstacleIds](size_t left, size_t right) {
			if (obstacleIds[left] != obstacleIds[right]) {
				return obstacleIds[left] < obstacleIds[right];
			}
			return left < right;
		}
	);
	return true;
}

} // namespace

bool Solve(
	const Request& request,
	const std::vector<Obstacle>& obstacles,
	Result& result
) {
	result = {};
	result.center = request.startCenter;
	result.yaw = request.startYaw;
	if (!IsFinite(request.startCenter.x) || !IsFinite(request.startCenter.y) ||
		!IsFinite(request.startYaw)) {
		return false;
	}
	std::vector<std::vector<Vec2>> polygons;
	std::vector<size_t> obstacleOrder;
	std::vector<uint64_t> obstacleIds;
	if (!ValidateRequest(request, obstacles, polygons, obstacleOrder, obstacleIds)) {
		return false;
	}
	const Vec2 startCenter = ToVec2(request.startCenter);
	const Vec2 desiredCenter = ToVec2(request.desiredCenter);
	const double radius = static_cast<double>(request.radius);
	const double halfSegmentLength = static_cast<double>(request.halfSegmentLength);
	const double skin = std::clamp(radius * 0.01, 0.001, 0.02);
	const double epsilon = skin * 0.1;
	if (!IsSafePose(
		startCenter, request.startYaw, radius, halfSegmentLength,
		polygons, request.bounds, epsilon
	)) {
		return false;
	}

	double solvedYaw = request.startYaw;
	bool rotationIterationLimited = false;
	const bool rotationSucceeded = SweepRotation(
		startCenter,
		static_cast<double>(request.startYaw),
		static_cast<double>(request.desiredYaw),
		radius,
		halfSegmentLength,
		polygons,
		epsilon,
		solvedYaw,
		rotationIterationLimited
	);
	result.rotationBlocked = !rotationSucceeded;
	if (!rotationSucceeded) {
		solvedYaw = request.startYaw;
	}
	result.yaw = static_cast<float>(solvedYaw);

	Vec2 currentCenter = startCenter;
	Vec2 remaining = Subtract(desiredCenter, currentCenter);
	Vec2 solvedVelocity = ToVec2(request.desiredVelocity);
	std::vector<Contact> activeContacts;
	bool slideIterationLimited = false;
	Vec2 lastVerifiedCenter = currentCenter;
	for (int iteration = 0; iteration < kSlideIterations; ++iteration) {
		if (LengthSquared(remaining) <= epsilon * epsilon) {
			break;
		}
		const CastHit hit = Cast(
			currentCenter,
			remaining,
			solvedYaw,
			radius,
			halfSegmentLength,
			polygons,
			obstacleOrder,
			request.bounds,
			epsilon
		);
		if (!hit.hit) {
			currentCenter = Add(currentCenter, remaining);
			if (!IsSafePose(
				currentCenter, solvedYaw, radius, halfSegmentLength,
				polygons, request.bounds, epsilon
			)) {
				currentCenter = lastVerifiedCenter;
				slideIterationLimited = true;
				break;
			}
			lastVerifiedCenter = currentCenter;
			remaining = {};
			break;
		}
		const double hitTime = std::clamp(hit.time, 0.0, 1.0);
		currentCenter = Add(currentCenter, Multiply(remaining, hitTime));
		if (!IsSafePose(
			currentCenter, solvedYaw, radius, halfSegmentLength,
			polygons, request.bounds, epsilon
		)) {
			currentCenter = lastVerifiedCenter;
			slideIterationLimited = true;
			break;
		}
		lastVerifiedCenter = currentCenter;
		activeContacts.insert(
			activeContacts.end(), hit.contacts.begin(), hit.contacts.end()
		);
		const Vec2 leftover = Multiply(remaining, 1.0 - hitTime);
		remaining = ProjectToContactCone(leftover, activeContacts);
		solvedVelocity = ProjectToContactCone(solvedVelocity, activeContacts);
		if (hitTime >= 1.0 - epsilon) {
			remaining = {};
			break;
		}
	}
	if (LengthSquared(remaining) > epsilon * epsilon) {
		slideIterationLimited = true;
	}
	if (slideIterationLimited) {
		currentCenter = lastVerifiedCenter;
		solvedVelocity = {};
	}
	if (!IsSafePose(
		currentCenter, solvedYaw, radius, halfSegmentLength,
		polygons, request.bounds, epsilon
	)) {
		currentCenter = lastVerifiedCenter;
		solvedVelocity = {};
		slideIterationLimited = true;
	}
	if (!ToFloat(currentCenter, result.center) || !ToFloat(solvedVelocity, result.velocity)) {
		result = {};
		result.center = request.startCenter;
		result.yaw = request.startYaw;
		return false;
	}
	result.translationBlocked = LengthSquared(
		Subtract(currentCenter, desiredCenter)
	) > epsilon * epsilon;
	result.iterationLimited = rotationIterationLimited || slideIterationLimited;
	return true;
}

} // namespace FishingFormationMotion
