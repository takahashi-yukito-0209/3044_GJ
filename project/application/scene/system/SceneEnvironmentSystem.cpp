// 役割: EnvironmentとWaterVolume Componentを実行時の環境描画へ反映する。
#include "SceneEnvironmentSystem.h"

#include "../../../engine/2d/TextureManager.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/3d/Object3dCommon.h"
#include "../../../engine/3d/Skybox.h"
#include "../../../engine/effect/WaterSurfaceRenderer.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "../../../engine/utility/EditableResourcePath.h"
#include "../../../engine/utility/StringUtility.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
	constexpr char kDefaultEnvironmentMapPath[] =
		"resources/rostock_laage_airport_4k.dds";

	float ResolveReflectionIntensity(
		const SceneComponent* meshRenderer,
		float environmentDefault
	) {
		if (
			meshRenderer &&
			meshRenderer->meshEnvironmentReflectionOverride
		) {
			return std::clamp(
				meshRenderer->meshEnvironmentReflectionIntensity,
				0.0f,
				1.0f
			);
		}
		return std::clamp(environmentDefault, 0.0f, 1.0f);
	}
}

SceneEnvironmentSystem::SceneEnvironmentSystem() = default;

SceneEnvironmentSystem::~SceneEnvironmentSystem() {
	Finalize();
}

void SceneEnvironmentSystem::Initialize(DirectXCommon* dxCommon) {
	Finalize();
	waterSurfaceRenderer_ = std::make_unique<WaterSurfaceRenderer>();
	waterSurfaceRenderer_->Initialize(dxCommon);
}

void SceneEnvironmentSystem::Sync(
	const SceneDocument* document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	const SceneComponent* environment = nullptr;
	if (document) {
		for (const SceneEntity& entity : document->GetEntities()) {
			if (!SceneEntityQuery::IsEntityActiveInHierarchy(*document, entity)) {
				continue;
			}
			environment =
				SceneEntityQuery::FindEnabledComponent(entity, "Environment");
			if (environment) {
				break;
			}
		}
	}

	bool skyboxEnabled = false;
	std::string requestedPath = kDefaultEnvironmentMapPath;
	float skyboxIntensity = 1.0f;
	float requestedReflectionIntensity = 0.3f;
	if (environment) {
		skyboxEnabled = environment->environmentSkyboxEnabled;
		requestedPath = environment->environmentSkyboxPath.empty()
			? kDefaultEnvironmentMapPath
			: environment->environmentSkyboxPath;
		skyboxIntensity =
			(std::max)(0.0f, environment->environmentSkyboxIntensity);
		requestedReflectionIntensity = std::clamp(
			environment->environmentReflectionIntensity,
			0.0f,
			1.0f
		);
	}

	const std::filesystem::path requestedFilePath =
		StringUtility::ToPath(requestedPath);
	const std::string texturePath = requestedFilePath.is_absolute()
		? StringUtility::ToUtf8(
			EditableResourcePath::ToProjectRelative(requestedFilePath)
		)
		: requestedPath;
	const bool environmentMapChanged = texturePath != environmentMapPath_;
	if (environmentMapChanged &&
		TextureManager::GetInstance()->LoadTexture(texturePath)) {
		environmentMapPath_ = texturePath;
	}
	reflectionIntensity_ = requestedReflectionIntensity;

	if (!skyboxEnabled || environmentMapPath_.empty()) {
		skybox_.reset();
	} else if (!skybox_ || environmentMapChanged) {
		skybox_ = std::make_unique<Skybox>();
		skybox_->Initialize(Object3dCommon::GetInstance(), environmentMapPath_);
		skybox_->SetScale({ 100.0f, 100.0f, 100.0f });
	}

	if (skybox_) {
		skybox_->SetColor({
			skyboxIntensity,
			skyboxIntensity,
			skyboxIntensity,
			1.0f
		});
	}

	// ObjectはEditor操作で再生成されるため、反射設定は現在のbindingsへ毎回適用する。
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.object) {
			continue;
		}
		const SceneComponent* meshRenderer =
			SceneEntityQuery::FindEnabledComponent(
				*binding.entity,
				"MeshRenderer"
			);
		const bool monitorSurface =
			SceneEntityQuery::HasComponent(*binding.entity, "MonitorRenderer");
		binding.object->SetEnvironmentMap(
			environmentMapPath_,
			monitorSurface
				? 0.0f
				: ResolveReflectionIntensity(
					meshRenderer,
					reflectionIntensity_
				)
		);
		if (monitorSurface) {
			binding.object->SetEnableLighting(false);
		}
	}
}

void SceneEnvironmentSystem::Update(float deltaTime) {
	if (waterSurfaceRenderer_) {
		waterSurfaceRenderer_->Update(deltaTime);
	}
}

void SceneEnvironmentSystem::ApplyRenderCamera(Camera* camera) {
	if (skybox_) {
		skybox_->SetCamera(camera);
		skybox_->Update();
	}
}

void SceneEnvironmentSystem::DrawSkybox() {
	if (skybox_) {
		skybox_->Draw();
	}
}

void SceneEnvironmentSystem::DrawWaterSurfaces(
	const SceneDocument& document,
	Camera* camera,
	uint64_t skipEntityId
) {
	if (!waterSurfaceRenderer_ || !camera) {
		return;
	}

	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			entity.id == skipEntityId ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}
		const SceneComponent* waterVolume =
			SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume");
		if (!waterVolume) {
			continue;
		}

		const Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const Vector3 center{
			transform.translate.x + waterVolume->waterOffset.x,
			transform.translate.y + waterVolume->waterOffset.y,
			transform.translate.z + waterVolume->waterOffset.z
		};
		const Vector3 halfSize{
			(std::max)(waterVolume->waterHalfSize.x, 0.05f),
			(std::max)(waterVolume->waterHalfSize.y, 0.05f),
			(std::max)(waterVolume->waterHalfSize.z, 0.05f)
		};

		WaterSurfaceRenderer::Settings settings{};
		settings.enabled = waterVolume->waterSurfaceEnabled;
		settings.baseColor = waterVolume->waterSurfaceBaseColor;
		settings.highlightColor = waterVolume->waterSurfaceHighlightColor;
		settings.alpha = waterVolume->waterSurfaceAlpha;
		settings.waveScale = waterVolume->waterSurfaceWaveScale;
		settings.normalStrength = waterVolume->waterSurfaceNormalStrength;
		settings.fresnelPower = waterVolume->waterSurfaceFresnelPower;

		// 水面を実際に横切るMeshだけに、モデルのXZスケールに沿った泡を置く。
		std::vector<WaterSurfaceRenderer::FoamEmitter> foamEmitters;
		foamEmitters.reserve(16);
		for (const SceneEntity& candidate : document.GetEntities()) {
			if (
				candidate.id == entity.id ||
				candidate.id == skipEntityId ||
				!SceneEntityQuery::IsEntityActiveInHierarchy(document, candidate) ||
				!SceneEntityQuery::FindEnabledComponent(candidate, "MeshRenderer")
			) {
				continue;
			}
			const Transform candidateTransform =
				SceneTransformResolver::ResolveScene3DTransform(document, candidate);
			const float scaleX = std::abs(candidateTransform.scale.x);
			const float scaleY = std::abs(candidateTransform.scale.y);
			const float scaleZ = std::abs(candidateTransform.scale.z);
			const SceneComponent* collider =
				SceneEntityQuery::FindEnabledComponent(candidate, "OBBCollider");
			if ((std::max)(scaleX, scaleZ) > 32.0f) {
				continue;
			}
			// OBBがあれば、そのサイズを正確な水面上の輪郭として使う。
			const float footprintX = collider
				? std::abs(collider->colliderSizeMultiplier.x) * scaleX
				: scaleX;
			const float footprintY = collider
				? std::abs(collider->colliderSizeMultiplier.y) * scaleY
				: scaleY;
			const float footprintZ = collider
				? std::abs(collider->colliderSizeMultiplier.z) * scaleZ
				: scaleZ;
			// 上端／下端の間に水面がなければ、完全に水上または水中なので泡を出さない。
			const float verticalReach = (std::max)(footprintY, 0.35f);
			const float objectBottom = candidateTransform.translate.y - verticalReach;
			const float objectTop = candidateTransform.translate.y + verticalReach;
			// わずかなBounds接触ではなく、目に見える量が水上へ出ている場合だけ泡を出す。
			constexpr float kMinimumEmergence = 0.28f;
			constexpr float kWaterlineOverlap = 0.06f;
			if (
				objectBottom > center.y - kWaterlineOverlap ||
				objectTop < center.y + kMinimumEmergence
			) {
				continue;
			}
			const float yaw = candidateTransform.rotate.y;
			const float cosine = std::cos(yaw);
			const float sine = std::sin(yaw);
			const float offsetX = collider ? collider->colliderOffset.x * scaleX : 0.0f;
			const float offsetZ = collider ? collider->colliderOffset.z * scaleZ : 0.0f;
			foamEmitters.push_back({
				{ candidateTransform.translate.x + offsetX * cosine + offsetZ * sine,
					center.y + 0.035f,
					candidateTransform.translate.z - offsetX * sine + offsetZ * cosine },
				(std::max)(footprintX, 0.25f),
				(std::max)(footprintZ, 0.25f),
				yaw,
				static_cast<uint32_t>(candidate.id)
			});
		}
		waterSurfaceRenderer_->Draw(camera, center, halfSize, settings, foamEmitters);
	}
}

void SceneEnvironmentSystem::DrawEditor(SceneDocument& document) {
	const auto generatedSkybox = starFieldGenerator_.DrawImGui("Environment");
	if (
		!generatedSkybox ||
		!TextureManager::GetInstance()->ReloadTexture(*generatedSkybox)
	) {
		return;
	}

	SceneComponent* targetEnvironment = nullptr;
	for (SceneEntity& entity : document.GetEntities()) {
		SceneComponent* environment =
			SceneEntityQuery::FindComponent(entity, "Environment");
		if (environment) {
			targetEnvironment = environment;
			break;
		}
	}
	if (!targetEnvironment) {
		SceneEntity& entity = document.CreateEntity("Environment");
		document.AddComponent(entity.id, "Environment");
		targetEnvironment = &entity.components.back();
	}
	targetEnvironment->environmentSkyboxEnabled = true;
	targetEnvironment->environmentSkyboxPath =
		StringUtility::ToUtf8(EditableResourcePath::ToProjectRelative(
			StringUtility::ToPath(*generatedSkybox)
		));
	document.MarkDirty();
}

void SceneEnvironmentSystem::Finalize() {
	if (waterSurfaceRenderer_) {
		waterSurfaceRenderer_->Finalize();
		waterSurfaceRenderer_.reset();
	}
	skybox_.reset();
	environmentMapPath_.clear();
	reflectionIntensity_ = 0.3f;
}
