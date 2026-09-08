// 役割: LightとShadowの設定をGPUへ渡し、Shadow Passを実行する。
#include "SceneLightingSystem.h"

#include "../../../engine/base/DirectXCommon.h"
#include "../../../engine/3d/LightManager.h"
#include "../../../engine/3d/ShadowManager.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <cmath>
#include <cstdint>

namespace {
	constexpr float kPi = 3.1415926535f;

	Vector3 ExtractPosition(const Matrix4x4& world) {
		return { world.m[3][0], world.m[3][1], world.m[3][2] };
	}

	Vector3 ExtractForward(const Matrix4x4& world) {
		Vector3 forward{ world.m[2][0], world.m[2][1], world.m[2][2] };
		if (Math::Length(forward) <= 0.000001f) {
			return { 0.0f, 0.0f, 1.0f };
		}
		return Math::Normalize(forward);
	}

	LightManager::ShadowSettings MakeShadowSettings(
		const SceneComponent& component,
		const Vector3& target
	) {
		LightManager::ShadowSettings settings{};
		settings.enable = component.lightCastsShadow;
		settings.bias = component.lightShadowBias;
		settings.normalBias = component.lightShadowNormalBias;
		settings.strength = component.lightShadowStrength;
		settings.target = target;
		settings.distance = component.lightShadowDistance;
		settings.orthographicSize = component.lightShadowOrthographicSize;
		settings.nearClip = component.lightShadowNearClip;
		settings.farClip = component.lightShadowFarClip;
		settings.texelSnap = component.lightShadowTexelSnap;
		return settings;
	}
}

SceneLightingSystem::SceneLightingSystem() = default;

SceneLightingSystem::~SceneLightingSystem() {
	Finalize();
}

void SceneLightingSystem::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager
) {
	Finalize();
	dxCommon_ = dxCommon;
	if (!dxCommon_ || !srvManager) {
		return;
	}

	lightManager_ = std::make_unique<LightManager>();
	lightManager_->Initialize(dxCommon_);

	shadowManager_ = std::make_unique<ShadowManager>();
	shadowManager_->Initialize(
		dxCommon_,
		srvManager,
		lightManager_->GetShadowMapSize()
	);
}

void SceneLightingSystem::Sync(const SceneDocument* document) {
	if (!lightManager_) {
		return;
	}

	auto& directional = lightManager_->GetDirectionalLight();
	auto& pointLights = lightManager_->GetPointLights();
	auto& spotLights = lightManager_->GetSpotLights();
	auto& directionalShadow =
		lightManager_->GetDirectionalShadowSettings();
	auto& pointShadows = lightManager_->GetPointShadowSettings();
	auto& spotShadows = lightManager_->GetSpotShadowSettings();
	directional = {};
	directional.direction = { 0.0f, -1.0f, 0.0f };
	directionalShadow = {};
	pointLights.clear();
	spotLights.clear();
	pointShadows.clear();
	spotShadows.clear();

	if (!document) {
		lightManager_->SyncToGPU();
		return;
	}

	lightManager_->SetShadowMapSize(
		document->GetLightingSettings().shadowMapSize
	);
	if (shadowManager_) {
		shadowManager_->SetShadowMapSize(lightManager_->GetShadowMapSize());
	}
	bool hasDirectional = false;
	for (const SceneEntity& entity : document->GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(*document, entity)) {
			continue;
		}
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(entity, "Light");
		if (!component) {
			continue;
		}

		const Matrix4x4 world =
			SceneTransformResolver::ResolveSceneWorldMatrix(*document, entity);
		const Vector3 position = ExtractPosition(world);
		const Vector3 direction = ExtractForward(world);
		if (component->lightType == "Directional") {
			if (hasDirectional) {
				continue;
			}
			directional.color = component->lightColor;
			directional.direction = direction;
			directional.intensity = component->lightIntensity;
			directional.enable = true;
			directionalShadow = MakeShadowSettings(*component, position);
			hasDirectional = true;
			continue;
		}

		if (component->lightType == "Point") {
			if (pointLights.size() >= LightManager::kMaxPointLights) {
				continue;
			}
			LightManager::PointLight light{};
			light.color = component->lightColor;
			light.position = position;
			light.intensity = component->lightIntensity;
			light.radius = component->lightRange;
			light.decay = component->lightDecay;
			light.enable = true;
			pointLights.push_back(light);
			pointShadows.emplace_back();
			continue;
		}

		if (
			component->lightType == "Spot" &&
			spotLights.size() < LightManager::kMaxSpotLights
		) {
			LightManager::SpotLight light{};
			light.color = component->lightColor;
			light.position = position;
			light.intensity = component->lightIntensity;
			light.direction = direction;
			light.distance = component->lightRange;
			light.decay = component->lightDecay;
			light.cosAngle = std::cos(
				component->lightSpotOuterAngle * kPi / 180.0f
			);
			light.cosFalloffStart = std::cos(
				component->lightSpotInnerAngle * kPi / 180.0f
			);
			light.enable = true;
			spotLights.push_back(light);
			spotShadows.push_back(MakeShadowSettings(*component, position));
		}
	}
	lightManager_->SyncToGPU();
}

void SceneLightingSystem::Bind() {
	if (!dxCommon_) {
		return;
	}

	auto* commandList = dxCommon_->GetCommandList();
	if (lightManager_) {
		lightManager_->Bind(commandList, 3);
	}
	if (shadowManager_) {
		shadowManager_->Bind(commandList, 5, 6);
	}
}

void SceneLightingSystem::RenderShadows(
	const std::vector<Object3d*>& shadowCasters
) {
	if (!lightManager_ || !shadowManager_) {
		return;
	}

	shadowManager_->SetShadowMapSize(lightManager_->GetShadowMapSize());
	// Casterが一時的に無くなっても、前フレームのDepthを残さない。
	// Renderは有効なMapをClearしてからobjectCount分だけ描画するため、
	// 空配列ではnullptrを渡してClearだけを実行する。
	shadowManager_->Render(
		*lightManager_,
		shadowCasters.empty() ? nullptr : shadowCasters.data(),
		static_cast<uint32_t>(shadowCasters.size())
	);
}

void SceneLightingSystem::Finalize() {
	// ShadowManagerはLightManagerを参照するため、依存側から先に破棄する。
	shadowManager_.reset();
	lightManager_.reset();
	dxCommon_ = nullptr;
}
