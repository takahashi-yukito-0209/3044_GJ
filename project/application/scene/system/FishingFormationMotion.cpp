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
constexpr int kSlideIterations = 32;
constexpr double kMinimumTranslationStep = 0.25;
constexpr double kTranslationStepRadiusScale = 0.5;
constexpr int kRotationEscapeRingCount = 4;
constexpr int kRotationEscapeDirectionCount = 16;

struct Vec2 {
	double x = 0.0;
	double y = 0.0;
};

struct DistanceInfo {
	double distance = 0.0;
	Vec2 polygonPoint{};
	Vec2 segmentPoint{};
};

struct ObstacleGeometry {
	ObstacleShape shape = ObstacleShape::ConvexHull;
	std::vector<Vec2> hull;
	Vec2 center{};
	double radius = 0.0;
};

enum class ContactSource {
	Water,
	Obstacle
};

struct Contact {
	Vec2 normal{};
	ContactSource source = ContactSource::Water;
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

DistanceInfo CapsuleCircleDistance(
	Vec2 center,
	double yaw,
	double halfSegmentLength,
	Vec2 circleCenter,
	double circleRadius
) {
	const Vec2 segmentStart = CapsuleEndpoint(
		center, static_cast<float>(yaw), halfSegmentLength, -1.0
	);
	const Vec2 segmentEnd = CapsuleEndpoint(
		center, static_cast<float>(yaw), halfSegmentLength, 1.0
	);
	const Vec2 segmentPoint = ClosestPointOnSegment(
		circleCenter, segmentStart, segmentEnd
	);
	const Vec2 offset = Subtract(segmentPoint, circleCenter);
	const double centerDistance = std::sqrt(LengthSquared(offset));
	DistanceInfo result{};
	result.distance = (std::max)(centerDistance - circleRadius, 0.0);
	result.segmentPoint = segmentPoint;
	if (centerDistance > kSegmentEpsilon) {
		result.polygonPoint = Add(
			circleCenter,
			Multiply(offset, circleRadius / centerDistance)
		);
	} else {
		result.polygonPoint = circleCenter;
	}
	return result;
}

DistanceInfo ObstacleDistance(
	Vec2 center,
	double yaw,
	double halfSegmentLength,
	const ObstacleGeometry& obstacle
) {
	if (obstacle.shape == ObstacleShape::Circle) {
		return CapsuleCircleDistance(
			center, yaw, halfSegmentLength,
			obstacle.center, obstacle.radius
		);
	}
	return CapsulePolygonDistance(
		center, yaw, halfSegmentLength, obstacle.hull
	);
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
	const std::vector<ObstacleGeometry>& obstacles,
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
	for (const ObstacleGeometry& obstacle : obstacles) {
		const DistanceInfo distance = ObstacleDistance(
			center, yaw, halfSegmentLength, obstacle
		);
		if (distance.distance < radius - epsilon) {
			return false;
		}
	}
	return true;
}

void AddContact(
	std::vector<Contact>& contacts,
	Vec2 normal,
	ContactSource source
) {
	Vec2 normalized{};
	if (Normalize(normal, normalized)) {
		contacts.push_back({ normalized, source });
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
			AddContact(hit.contacts, normal, ContactSource::Water);
		} else if (std::abs(clampedParameter - hit.time) <= kEpsilon) {
			AddContact(hit.contacts, normal, ContactSource::Water);
		}
	}
}

bool CastObstacle(
	Vec2 center,
	Vec2 delta,
	double yaw,
	double obstacleRadius,
	double halfSegmentLength,
	const ObstacleGeometry& obstacle,
	double skin,
	double epsilon,
	CastHit& hit
) {
	double parameter = 0.0;
	double lastSafeParameter = 0.0;
	Vec2 lastNormal{};
	bool hasLastNormal = false;
	for (int iteration = 0; iteration < kCastIterations; ++iteration) {
		const Vec2 current = Add(center, Multiply(delta, parameter));
		const DistanceInfo distance = ObstacleDistance(
			current, yaw, halfSegmentLength, obstacle
		);
		if (distance.distance < obstacleRadius - epsilon) {
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
			if (obstacle.shape != ObstacleShape::Circle) {
				return false;
			}
			if (hasLastNormal) {
				normal = lastNormal;
			} else if (!Normalize(Multiply(delta, -1.0), normal)) {
				return false;
			}
		}
		lastNormal = normal;
		hasLastNormal = true;
		const double gap = distance.distance - obstacleRadius - skin;
		const double closing = -Dot(delta, normal);
		if (closing <= kEpsilon) {
			return false;
		}
		if (gap <= epsilon) {
			hit = {};
			hit.hit = true;
			hit.time = parameter;
			AddContact(hit.contacts, normal, ContactSource::Obstacle);
			return true;
		}
		const double step = gap / closing;
		if (!std::isfinite(step) || step <= kEpsilon) {
			hit = {};
			hit.hit = true;
			hit.time = parameter;
			AddContact(hit.contacts, lastNormal, ContactSource::Obstacle);
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
	AddContact(hit.contacts, lastNormal, ContactSource::Obstacle);
	return !hit.contacts.empty();
}

CastHit Cast(
	Vec2 center,
	Vec2 delta,
	double yaw,
	double obstacleRadius,
	double halfSegmentLength,
	const std::vector<ObstacleGeometry>& obstacles,
	const std::vector<size_t>& obstacleOrder,
	const CenterBounds& bounds,
	double skin,
	double epsilon
) {
	CastHit best{};
	AddWaterCast(center, delta, bounds, best);
	for (const size_t index : obstacleOrder) {
		CastHit candidate{};
		if (!CastObstacle(
			center, delta, yaw, obstacleRadius, halfSegmentLength,
			obstacles[index], skin, epsilon, candidate
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

Vec2 ApplySlideAssist(
	Vec2 value,
	Vec2 projected,
	double strength
) {
	if (
		strength <= 0.0 ||
		LengthSquared(value) <= kSegmentEpsilon ||
		LengthSquared(projected) <= kSegmentEpsilon
	) {
		return projected;
	}
	const double inputLength = std::sqrt(LengthSquared(value));
	const double projectedLength = std::sqrt(LengthSquared(projected));
	if (
		!std::isfinite(inputLength) ||
		!std::isfinite(projectedLength) ||
		inputLength <= kSegmentEpsilon ||
		projectedLength <= kSegmentEpsilon
	) {
		return projected;
	}
	const double tangentRatio = std::clamp(
		projectedLength / inputLength,
		0.0,
		1.0
	);
	const double assistedRatio = tangentRatio +
		(std::sqrt(tangentRatio) - tangentRatio) * strength;
	return Multiply(
		projected,
		inputLength * assistedRatio / projectedLength
	);
}

double MaximumTranslationStep(double radius) {
	return (std::max)(
		radius * kTranslationStepRadiusScale,
		kMinimumTranslationStep
	);
}

struct RotationSolveResult {
	Vec2 center{};
	double yaw = 0.0;
	bool complete = false;
	bool iterationLimited = false;
};

bool TryFindRotationEscapeCenter(
	Vec2 currentCenter,
	double currentYaw,
	double nextYaw,
	Vec2 desiredCenter,
	double obstacleRadius,
	double halfSegmentLength,
	const std::vector<ObstacleGeometry>& obstacles,
	const std::vector<size_t>& obstacleOrder,
	const CenterBounds& bounds,
	double skin,
	double epsilon,
	double remainingEscapeBudget,
	Vec2& outputCenter,
	double& outputDistance
) {
	if (remainingEscapeBudget <= epsilon) {
		return false;
	}
	Vec2 preferredDirection = Subtract(desiredCenter, currentCenter);
	if (!Normalize(preferredDirection, preferredDirection)) {
		preferredDirection = Forward(static_cast<float>(currentYaw));
	}
	const Vec2 leftDirection = {
		-preferredDirection.y,
		preferredDirection.x
	};
	for (int ring = 1; ring <= kRotationEscapeRingCount; ++ring) {
		const double distance = remainingEscapeBudget *
			static_cast<double>(ring) /
			static_cast<double>(kRotationEscapeRingCount);
		for (int order = 0; order < kRotationEscapeDirectionCount; ++order) {
			const int directionOffset = order == 0
				? 0
				: (order % 2 == 0 ? -1 : 1) * ((order + 1) / 2);
			const double angle = 2.0 * kPi *
				static_cast<double>(directionOffset) /
				static_cast<double>(kRotationEscapeDirectionCount);
			const Vec2 direction = Add(
				Multiply(preferredDirection, std::cos(angle)),
				Multiply(leftDirection, std::sin(angle))
			);
			const Vec2 candidateCenter = Add(
				currentCenter,
				Multiply(direction, distance)
			);
			const CastHit path = Cast(
				currentCenter,
				Subtract(candidateCenter, currentCenter),
				currentYaw,
				obstacleRadius,
				halfSegmentLength,
			obstacles,
			obstacleOrder,
			bounds,
			skin,
			epsilon
			);
			if (path.hit ||
				!IsSafePose(
					candidateCenter, currentYaw, obstacleRadius, halfSegmentLength,
					obstacles, bounds, epsilon
				) ||
				!IsSafePose(
					candidateCenter, nextYaw, obstacleRadius, halfSegmentLength,
					obstacles, bounds, epsilon
				)) {
				continue;
			}
			outputCenter = candidateCenter;
			outputDistance = distance;
			return true;
		}
	}
	return false;
}

RotationSolveResult SolveRotationWithEscape(
	Vec2 startCenter,
	double startYaw,
	double desiredYaw,
	Vec2 desiredCenter,
	double formationRadius,
	double obstacleRadius,
	double halfSegmentLength,
	const std::vector<ObstacleGeometry>& obstacles,
	const std::vector<size_t>& obstacleOrder,
	const CenterBounds& bounds,
	double skin,
	double epsilon
) {
	RotationSolveResult result{};
	result.center = startCenter;
	result.yaw = startYaw;
	const double deltaYaw = WrapAngle(desiredYaw - startYaw);
	if (halfSegmentLength <= kSegmentEpsilon || std::abs(deltaYaw) <= kEpsilon) {
		result.yaw = desiredYaw;
		result.complete = true;
		return result;
	}
	double progress = 0.0;
	double consumedEscapeDistance = 0.0;
	const double maximumEscapeDistance = MaximumTranslationStep(formationRadius);
	for (int iteration = 0; iteration < kRotationIterations; ++iteration) {
		if (progress >= 1.0 - kEpsilon) {
			result.yaw = desiredYaw;
			result.complete = true;
			return result;
		}
		const double currentYaw = result.yaw;
		double minimumGap = std::numeric_limits<double>::max();
		for (const ObstacleGeometry& obstacle : obstacles) {
			const DistanceInfo distance = ObstacleDistance(
				result.center, currentYaw, halfSegmentLength, obstacle
			);
			minimumGap = (std::min)(minimumGap, distance.distance - obstacleRadius);
		}
		const double angularTravel = halfSegmentLength * std::abs(deltaYaw);
		double maximumProgress = minimumGap > epsilon &&
			angularTravel > kSegmentEpsilon
			? std::clamp((minimumGap - epsilon) * 0.8 / angularTravel, 0.0, 1.0)
			: 0.0;
		if (maximumProgress <= 1.0e-6) {
			const double minimumAngularStep =
				(std::max)(skin, formationRadius * 0.05) / halfSegmentLength;
			maximumProgress = (std::min)(
				1.0 - progress,
				minimumAngularStep / std::abs(deltaYaw)
			);
		}
		const double step = (std::min)(1.0 - progress, maximumProgress);
		if (step <= 1.0e-6) {
			result.iterationLimited = true;
			return result;
		}
		const double nextProgress = progress + step;
		const double nextYaw = startYaw + deltaYaw * nextProgress;
		if (IsSafePose(
			result.center, nextYaw, obstacleRadius, halfSegmentLength,
			obstacles, bounds, epsilon
		)) {
			result.yaw = nextYaw;
			progress = nextProgress;
			continue;
		}
		Vec2 escapeCenter{};
		double escapeDistance = 0.0;
		if (!TryFindRotationEscapeCenter(
			result.center,
			currentYaw,
			nextYaw,
			desiredCenter,
			obstacleRadius,
			halfSegmentLength,
			obstacles,
			obstacleOrder,
			bounds,
			skin,
			epsilon,
			maximumEscapeDistance - consumedEscapeDistance,
			escapeCenter,
			escapeDistance
		)) {
			return result;
		}
		result.center = escapeCenter;
		result.yaw = nextYaw;
		consumedEscapeDistance += escapeDistance;
		progress = nextProgress;
	}
	if (progress >= 1.0 - kEpsilon) {
		result.yaw = desiredYaw;
		result.complete = true;
		return result;
	}
	result.iterationLimited = true;
	return result;
}

bool ValidateRequest(
	const Request& request,
	const std::vector<Obstacle>& obstacles,
	std::vector<ObstacleGeometry>& geometries,
	std::vector<size_t>& obstacleOrder,
	std::vector<uint64_t>& obstacleIds
) {
	if (
		!IsFinite(request.startYaw) || !IsFinite(request.desiredYaw) ||
		!IsFinite(request.radius) || !IsFinite(request.halfSegmentLength) ||
		!IsFinite(request.startCenter.x) || !IsFinite(request.startCenter.y) ||
		!IsFinite(request.desiredCenter.x) || !IsFinite(request.desiredCenter.y) ||
		!IsFinite(request.desiredVelocity.x) || !IsFinite(request.desiredVelocity.y) ||
		!IsFinite(request.slideAssistStrength) ||
		!IsFinite(request.rockVisualClearance) ||
		request.radius <= 0.0f || request.halfSegmentLength < 0.0f ||
		request.slideAssistStrength < 0.0f ||
		request.slideAssistStrength > 1.0f ||
		request.rockVisualClearance < 0.0f ||
		request.rockVisualClearance > 100.0f
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
	geometries.clear();
	obstacleOrder.clear();
	obstacleIds.clear();
	geometries.reserve(obstacles.size());
	for (size_t index = 0; index < obstacles.size(); ++index) {
		ObstacleGeometry geometry{};
		geometry.shape = obstacles[index].shape;
		if (geometry.shape == ObstacleShape::Circle) {
			geometry.center = ToVec2(obstacles[index].center);
			geometry.radius = static_cast<double>(obstacles[index].radius);
			if (!IsFinite(geometry.center) ||
				!std::isfinite(geometry.radius) || geometry.radius <= 0.0) {
				return false;
			}
		} else {
			if (!ValidatePolygon(obstacles[index], geometry.hull)) {
				return false;
			}
		}
		geometries.push_back(std::move(geometry));
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
	std::vector<ObstacleGeometry> geometries;
	std::vector<size_t> obstacleOrder;
	std::vector<uint64_t> obstacleIds;
	if (!ValidateRequest(request, obstacles, geometries, obstacleOrder, obstacleIds)) {
		return false;
	}
	const Vec2 startCenter = ToVec2(request.startCenter);
	const Vec2 desiredCenter = ToVec2(request.desiredCenter);
	const double formationRadius = static_cast<double>(request.radius);
	const double obstacleCollisionRadius = formationRadius +
		static_cast<double>(request.rockVisualClearance);
	if (!std::isfinite(obstacleCollisionRadius) || obstacleCollisionRadius <= 0.0) {
		return false;
	}
	const double halfSegmentLength = static_cast<double>(request.halfSegmentLength);
	const double skin = std::clamp(formationRadius * 0.01, 0.001, 0.02);
	const double epsilon = skin * 0.1;
	if (!IsSafePose(
		startCenter, request.startYaw, obstacleCollisionRadius, halfSegmentLength,
		geometries, request.bounds, epsilon
	)) {
		return false;
	}

	const RotationSolveResult rotation = SolveRotationWithEscape(
		startCenter,
		static_cast<double>(request.startYaw),
		static_cast<double>(request.desiredYaw),
		desiredCenter,
		formationRadius,
		obstacleCollisionRadius,
		halfSegmentLength,
		geometries,
		obstacleOrder,
		request.bounds,
		skin,
		epsilon
	);
	const double solvedYaw = rotation.yaw;
	const bool rotationIterationLimited = rotation.iterationLimited;
	result.rotationBlocked = !rotation.complete;
	result.yaw = static_cast<float>(solvedYaw);

	Vec2 currentCenter = rotation.center;
	Vec2 remaining = Subtract(desiredCenter, currentCenter);
	Vec2 solvedVelocity = ToVec2(request.desiredVelocity);
	std::vector<Contact> activeContacts;
	Vec2 obstacleContactNormalSum{};
	bool slideIterationLimited = false;
	Vec2 lastVerifiedCenter = currentCenter;
	const double maximumTranslationStep = MaximumTranslationStep(formationRadius);
	for (int iteration = 0; iteration < kSlideIterations; ++iteration) {
		if (LengthSquared(remaining) <= epsilon * epsilon) {
			break;
		}
		Vec2 stepDelta = remaining;
		const double remainingLength = std::sqrt(LengthSquared(remaining));
		if (remainingLength > maximumTranslationStep) {
			stepDelta = Multiply(
				remaining,
				maximumTranslationStep / remainingLength
			);
		}
		const CastHit hit = Cast(
			currentCenter,
			stepDelta,
			solvedYaw,
			obstacleCollisionRadius,
			halfSegmentLength,
			geometries,
			obstacleOrder,
			request.bounds,
			skin,
			epsilon
		);
		if (!hit.hit) {
			currentCenter = Add(currentCenter, stepDelta);
			if (!IsSafePose(
				currentCenter, solvedYaw, obstacleCollisionRadius, halfSegmentLength,
				geometries, request.bounds, epsilon
			)) {
				currentCenter = lastVerifiedCenter;
				slideIterationLimited = true;
				break;
			}
			lastVerifiedCenter = currentCenter;
			remaining = Subtract(remaining, stepDelta);
			continue;
		}
		const double hitTime = std::clamp(hit.time, 0.0, 1.0);
		currentCenter = Add(currentCenter, Multiply(stepDelta, hitTime));
		if (!IsSafePose(
			currentCenter, solvedYaw, obstacleCollisionRadius, halfSegmentLength,
			geometries, request.bounds, epsilon
		)) {
			currentCenter = lastVerifiedCenter;
			slideIterationLimited = true;
			break;
		}
		lastVerifiedCenter = currentCenter;
		for (const Contact& contact : hit.contacts) {
			activeContacts.push_back(contact);
			if (contact.source == ContactSource::Obstacle) {
				obstacleContactNormalSum = Add(
					obstacleContactNormalSum, contact.normal
				);
			}
		}
		const Vec2 leftover = Add(
			Multiply(stepDelta, 1.0 - hitTime),
			Subtract(remaining, stepDelta)
		);
		remaining = ApplySlideAssist(
			leftover,
			ProjectToContactCone(leftover, activeContacts),
			static_cast<double>(request.slideAssistStrength)
		);
		solvedVelocity = ApplySlideAssist(
			solvedVelocity,
			ProjectToContactCone(solvedVelocity, activeContacts),
			static_cast<double>(request.slideAssistStrength)
		);
	}
	if (LengthSquared(remaining) > epsilon * epsilon) {
		slideIterationLimited = true;
	}
	if (slideIterationLimited) {
		currentCenter = lastVerifiedCenter;
		solvedVelocity = {};
	}
	if (!IsSafePose(
		currentCenter, solvedYaw, obstacleCollisionRadius, halfSegmentLength,
		geometries, request.bounds, epsilon
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
	Vec2 obstacleContactNormal{};
	if (Normalize(obstacleContactNormalSum, obstacleContactNormal) &&
		ToFloat(obstacleContactNormal, result.obstacleContactNormal)) {
		result.obstacleContact = true;
	}
	result.iterationLimited = rotationIterationLimited || slideIterationLimited;
	return true;
}

} // namespace FishingFormationMotion
