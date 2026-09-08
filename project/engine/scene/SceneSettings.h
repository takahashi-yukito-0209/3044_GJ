// 役割: EntityのHierarchyとは独立して保存するシーン全体設定を定義する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"

// Scene-wide settings that are serialized independently of the entity hierarchy.
struct SceneLightingSettings {
	uint32_t shadowMapSize = 4096;
};

struct ScenePostProcessSettings {
	bool bloomEnabled = true;
	float baseExposure = 1.0f;
	int toneMapMode = 0;
	float bloomThreshold = 1.0f;
	float bloomSoftKnee = 0.5f;
	float bloomIntensity = 0.7f;
	int bloomBlurIterations = 4;
	int bloomDownsampleScale = 2;
	float bloomBlurRadius = 1.0f;
	bool grayscaleEnabled = false;
	bool vignetteEnabled = false;
	bool boxBlurEnabled = false;
	bool gaussianBlurEnabled = false;
	bool depthOfFieldEnabled = false;
	bool motionBlurEnabled = false;
	float motionBlurStrength = 0.5f;
	int motionBlurSamples = 8;
	float motionBlurMaxRadius = 24.0f;
	bool radialBlurEnabled = false;
	bool noiseEnabled = false;
	bool dissolveEnabled = false;
	bool outlineEnabled = false;
	bool underwaterEnabled = false;
	bool waterRefractionEnabled = false;
	bool pixelationEnabled = false;
	int pixelationBlockSize = 4;
	bool chromaticAberrationEnabled = false;
	Vector2 chromaticAberrationCenter = { 0.5f, 0.5f };
	float chromaticAberrationIntensity = 0.003f;
	float chromaticAberrationFalloff = 1.5f;
	float vignetteScale = 16.0f;
	float vignettePower = 0.8f;
	float vignetteIntensity = 1.0f;
	int boxBlurKernelSize = 3;
	float boxBlurStrength = 1.0f;
	int gaussianBlurKernelSize = 3;
	float gaussianBlurSigma = 1.0f;
	float gaussianBlurStrength = 1.0f;
	float depthOfFieldFocusDistance = 10.0f;
	float depthOfFieldFocusRange = 2.0f;
	float depthOfFieldBlurStrength = 1.0f;
	float depthOfFieldNearStrength = 0.0f;
	float depthOfFieldFarStrength = 1.0f;
	float depthOfFieldMaxRadius = 4.0f;
	Vector2 radialBlurCenter = { 0.5f, 0.5f };
	float radialBlurWidth = 0.01f;
	int radialBlurSamples = 10;
	bool noiseAnimate = true;
	float noiseAmount = 0.25f;
	float noiseScale = 1.0f;
	float noiseSpeed = 1.0f;
	float noiseSeed = 0.0f;
	int dissolveMaskIndex = 0;
	float dissolveThreshold = 0.0f;
	float dissolveEdgeWidth = 0.03f;
	Vector4 dissolveEdgeColor = { 1.0f, 0.4f, 0.3f, 1.0f };
	bool outlineLuminanceEnabled = false;
	bool outlineDepthEnabled = true;
	float outlineLuminanceWeight = 1.0f;
	float outlineDepthWeight = 1.0f;
	float outlineThreshold = 0.1f;
	float outlineSoftness = 0.05f;
	float outlineThickness = 1.0f;
	Vector4 outlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	Vector4 underwaterTintColor = { 0.02f, 0.45f, 0.68f, 1.0f };
	float underwaterIntensity = 0.65f;
	float underwaterFogDensity = 0.035f;
	float underwaterDistortion = 0.012f;
	Vector4 waterRefractionTintColor = { 0.02f, 0.55f, 0.82f, 1.0f };
	float waterRefractionStrength = 0.018f;
	float waterRefractionEdgeSoftness = 0.08f;
	float waterRefractionTintStrength = 0.12f;
};

struct SceneDebugSettings {
	bool showCameraDirection = false;
	bool showColliders = false;
	bool showCameraPath = true;
	bool showCameraPathPointCameraDirection = true;
	bool showSkeleton = false;
	bool showJointNames = false;
	bool showJointAxes = true;
	float jointRadius = 0.018f;
	float jointAxisLength = 0.06f;
};

// 全FishingObstacleで共有する岩モデル別Collider設定。
struct SceneFishingObstacleColliderProfile {
	std::string modelPath;
	bool enabled = true;
	Vector3 colliderOffset{};
	Vector3 colliderRotation{};
	Vector3 colliderSizeMultiplier = { 1.0f, 1.0f, 1.0f };
	float colliderSphereRadius = 0.5f;
};

struct SceneFishingObstacleSettings {
	std::vector<SceneFishingObstacleColliderProfile> colliderProfiles;
};
