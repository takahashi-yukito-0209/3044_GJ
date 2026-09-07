// 役割: パーティクルの生成、更新、GPU Compute、描画を実装する。
#include "ParticleManager.h"
#include "ParticleCommon.h"

#include "../3d/SrvManager.h"
#include "../3d/Camera.h"
#include "../base/DirectXCommon.h"
#include "../2d/TextureManager.h"

#include <cassert>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#include "../externals/imgui/imgui.h"
#include "../externals/nlohmann/json.hpp"
#include "../utility/EditableResourcePath.h"
#include "../utility/Logger.h"
#include "../utility/StringUtility.h"
#include "ParticleEffectResource.h"

using json = nlohmann::json;

ParticleManager* ParticleManager::instance_ = nullptr;

namespace {
	std::string NormalizeSceneParticleKey(const std::string& sceneId) {
		std::string normalized = sceneId;
		std::transform(
			normalized.begin(),
			normalized.end(),
			normalized.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			}
		);
		return normalized;
	}

	bool IntersectsSegmentAabb(
		const Vector3& start,
		const Vector3& end,
		const Vector3& center,
		const Vector3& halfSize
	) {
		const Vector3 direction{
			end.x - start.x,
			end.y - start.y,
			end.z - start.z
		};
		float tMin = 0.0f;
		float tMax = 1.0f;

		auto testAxis = [&](
			float origin,
			float delta,
			float minimum,
			float maximum
		) {
			if (std::abs(delta) < 0.000001f) {
				return origin >= minimum && origin <= maximum;
			}

			const float invDelta = 1.0f / delta;
			float nearT = (minimum - origin) * invDelta;
			float farT = (maximum - origin) * invDelta;
			if (nearT > farT) {
				std::swap(nearT, farT);
			}
			tMin = (std::max)(tMin, nearT);
			tMax = (std::min)(tMax, farT);
			return tMin <= tMax;
		};

		const Vector3 safeHalfSize{
			(std::max)(halfSize.x, 0.001f),
			(std::max)(halfSize.y, 0.001f),
			(std::max)(halfSize.z, 0.001f)
		};
		const Vector3 boxMin{
			center.x - safeHalfSize.x,
			center.y - safeHalfSize.y,
			center.z - safeHalfSize.z
		};
		const Vector3 boxMax{
			center.x + safeHalfSize.x,
			center.y + safeHalfSize.y,
			center.z + safeHalfSize.z
		};

		return
			testAxis(start.x, direction.x, boxMin.x, boxMax.x) &&
			testAxis(start.y, direction.y, boxMin.y, boxMax.y) &&
			testAxis(start.z, direction.z, boxMin.z, boxMax.z) &&
			tMax >= 0.0f &&
			tMin <= 1.0f;
	}

	bool ShouldIncludeParticleInWaterPass(
		const Vector3& particlePosition,
		const ParticleManager::WaterDrawFilter& filter
	) {
		if (filter.mode == ParticleManager::WaterDrawMode::kAll) {
			return true;
		}

		const bool refracted =
			IntersectsSegmentAabb(
				filter.cameraPosition,
				particlePosition,
				filter.waterCenter,
				filter.waterHalfSize
			);
		return filter.mode == ParticleManager::WaterDrawMode::kRefracted
			? refracted
			: !refracted;
	}
}

ParticleManager* ParticleManager::GetInstance() {
	if (instance_ == nullptr) {
		instance_ = new ParticleManager();
	}
	return instance_;
}

void ParticleManager::DeleteInstance() {
	delete instance_;
	instance_ = nullptr;
}

ParticleManager::~ParticleManager() {
	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			ReleasePlacements(instance.runtimePlacements);
		}
	}
}

void ParticleManager::Initialize(ParticleCommon* particleCommon, SrvManager* srvManager) {
	assert(particleCommon);
	assert(srvManager);

	particleCommon_ = particleCommon;
	srvManager_ = srvManager;
	camera_ = particleCommon_->GetDefaultCamera();

	CreateDirectionalLightResource();
}

void ParticleManager::Reset() {
	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			ReleasePlacements(instance.runtimePlacements);
		}
	}
	particlePlacementAssets_.clear();
	sceneParticleAssetInstances_.clear();
	sceneParticleAssetCycleSteps_.clear();
	sceneGpuParticleKeys_.clear();
	sceneParticleLayoutLoaded_ = false;
	sceneParticleLayoutDirty_ = false;
	sceneParticlePersistenceMessage_.clear();

	particleGroups_.clear();
	gpuParticles_.clear();
	gpuParticleEnabled_ = false;
	pendingLightningEvents_.clear();
	pendingExposureFlashEvents_.clear();
	lightningSeed_ = 1;

	directionalLightResource_.Reset();
	directionalLightData_ = nullptr;

	particleCommon_ = nullptr;
	srvManager_ = nullptr;
	camera_ = nullptr;
}

void ParticleManager::QueueLightning(
	const LightningEmitterDesc& desc,
	const Vector3& emitterPosition
) {
	if (!desc.enabled) {
		return;
	}

	const Vector3 halfRange = {
		desc.randomRange.x * 0.5f,
		desc.randomRange.y * 0.5f,
		desc.randomRange.z * 0.5f
	};
	const Vector3 randomOffset = RandomVector3Range(
		{ -halfRange.x, -halfRange.y, -halfRange.z },
		{ halfRange.x, halfRange.y, halfRange.z }
	);

	LightningEvent event{};
	event.desc = desc;
	event.start = {
		emitterPosition.x + desc.startOffset.x + randomOffset.x,
		emitterPosition.y + desc.startOffset.y + randomOffset.y,
		emitterPosition.z + desc.startOffset.z + randomOffset.z
	};
	event.end = {
		emitterPosition.x + desc.endOffset.x + randomOffset.x,
		emitterPosition.y + desc.endOffset.y + randomOffset.y,
		emitterPosition.z + desc.endOffset.z + randomOffset.z
	};
	event.seed = lightningSeed_++;

	pendingLightningEvents_.push_back(event);

	if (desc.flashExposure) {
		pendingExposureFlashEvents_.push_back({
			desc.flashExposureValue,
			desc.flashReturnSpeed
		});
	}
}

bool ParticleManager::ConsumeLightningEvent(LightningEvent& outEvent) {
	if (pendingLightningEvents_.empty()) {
		return false;
	}

	outEvent = pendingLightningEvents_.front();
	pendingLightningEvents_.erase(pendingLightningEvents_.begin());
	return true;
}

bool ParticleManager::ConsumeExposureFlashEvent(
	ExposureFlashEvent& outEvent
) {
	if (pendingExposureFlashEvents_.empty()) {
		return false;
	}

	outEvent = pendingExposureFlashEvents_.front();
	pendingExposureFlashEvents_.erase(pendingExposureFlashEvents_.begin());
	return true;
}

void ParticleManager::SetGroupBlendMode(
	const std::string& name,
	ParticleCommon::BlendMode blendMode
) {
	auto it = particleGroups_.find(name);
	assert(it != particleGroups_.end());

	it->second.blendMode = blendMode;
}

void ParticleManager::SetGroupRenderDesc(
	const std::string& name,
	const ParticleRenderDesc& render
) {
	auto it = particleGroups_.find(name);
	assert(it != particleGroups_.end());

	ParticleGroup& group = it->second;
	const ParticleRenderDesc& current = group.render;
	const auto sameColor = [](const Vector4& a, const Vector4& b) {
		return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
	};
	const bool geometryUnchanged =
		current.primitiveType == render.primitiveType &&
		current.ring.divisions == render.ring.divisions &&
		current.ring.outerRadius == render.ring.outerRadius &&
		current.ring.innerRadius == render.ring.innerRadius &&
		current.ring.startAngle == render.ring.startAngle &&
		current.ring.endAngle == render.ring.endAngle &&
		sameColor(current.ring.outerColor, render.ring.outerColor) &&
		sameColor(current.ring.innerColor, render.ring.innerColor) &&
		current.ring.uvMode == render.ring.uvMode &&
		current.cylinder.divisions == render.cylinder.divisions &&
		current.cylinder.topRadius == render.cylinder.topRadius &&
		current.cylinder.bottomRadius == render.cylinder.bottomRadius &&
		current.cylinder.height == render.cylinder.height &&
		current.cylinder.startAngle == render.cylinder.startAngle &&
		current.cylinder.endAngle == render.cylinder.endAngle &&
		sameColor(current.cylinder.topColor, render.cylinder.topColor) &&
		sameColor(current.cylinder.bottomColor, render.cylinder.bottomColor) &&
		current.cylinder.uvMode == render.cylinder.uvMode;

	group.render = render;
	group.materialData->alphaCutoff = std::clamp(render.alphaCutoff, 0.0f, 1.0f);
	group.materialData->flipU = render.flipU ? 1 : 0;
	group.materialData->flipV = render.flipV ? 1 : 0;
	group.materialData->emissiveIntensity = (std::max)(0.0f, render.emissiveIntensity);

	if (!geometryUnchanged) {
		CreateGroupVertexResource(group);
	}
}

void ParticleManager::ApplyGpuParticleEffect(const ParticleEffectDesc& effect) {
	gpuParticleEnabled_ = true;
	ApplyGpuParticleEffectToKey("editor:preview", effect);
	Logger::Log("GPU Particle enabled by effect: " + effect.name + "\n");
}

void ParticleManager::ClearGpuParticles() {
	gpuParticles_.clear();
	sceneGpuParticleKeys_.clear();
}

void ParticleManager::ClearGpuParticlePreview() {
	EraseGpuParticle("editor:preview");
}

void ParticleManager::RequestGpuParticleReset() {
	if (GpuParticle* particle = FindGpuParticle("editor:preview")) {
		particle->RequestResetBuffer();
	}
}

void ParticleManager::EmitGpuParticleOnce() {
	gpuParticleEnabled_ = true;
	if (GpuParticle* particle = FindGpuParticle("editor:preview")) {
		particle->EmitOnce();
	}
}

void ParticleManager::SetGpuParticleEnabled(bool enabled) {
	if (gpuParticleEnabled_ != enabled) {
		Logger::Log(std::string("GPU Particle enabled: ") + (enabled ? "true\n" : "false\n"));
	}
	gpuParticleEnabled_ = enabled;
}

namespace {

constexpr float kMinEmitterFrequency = 1.0f / 60.0f;

float NormalizeEmitterFrequency(float frequency) {
	return frequency > 0.0f ? frequency : kMinEmitterFrequency;
}

json ToJson(const Vector3& v) {
	return json::array({ v.x, v.y, v.z });
}

Vector3 ReadVector3(const json& j, const Vector3& defaultValue) {
	if (!j.is_array() || j.size() < 3) {
		return defaultValue;
	}

	return {
		j.at(0).get<float>(),
		j.at(1).get<float>(),
		j.at(2).get<float>()
	};
}

void CopyText(char* destination, size_t destinationSize, const std::string& source) {
	if (!destination || destinationSize == 0) {
		return;
	}
	strncpy_s(destination, destinationSize, source.c_str(), _TRUNCATE);
}

std::string MakeDefaultEffectPath(const char* name) {
	std::string safeName = name && name[0] != '\0' ? name : "newParticle";
	for (char& c : safeName) {
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|') {
			c = '_';
		}
	}
	return "resources/particles/" + safeName + ".json";
}

Vector3 NormalizeOrZero(const Vector3& value) {
	const float lengthSquared =
		value.x * value.x + value.y * value.y + value.z * value.z;

	if (lengthSquared <= 0.000001f) {
		return { 0.0f, 0.0f, 0.0f };
	}

	const float invLength = 1.0f / std::sqrt(lengthSquared);
	return {
		value.x * invLength,
		value.y * invLength,
		value.z * invLength
	};
}

float WrapRadian(float value) {
	constexpr float kTwoPi = 6.2831853f;
	value = std::fmod(value, kTwoPi);
	if (value < -kTwoPi) {
		value += kTwoPi;
	}
	return value;
}

Vector3 AddVector3(const Vector3& a, const Vector3& b) {
	return {
		a.x + b.x,
		a.y + b.y,
		a.z + b.z
	};
}

Vector3 CalculateVelocityAlignedRotation(
	const Vector3& direction,
	ParticleManager::ParticleAlignmentAxis axis
) {
	const Vector3 dir = NormalizeOrZero(direction);
	constexpr float kEpsilon = 0.000001f;

	if (
		std::fabs(dir.x) < kEpsilon &&
		std::fabs(dir.y) < kEpsilon &&
		std::fabs(dir.z) < kEpsilon
		) {
		return { 0.0f, 0.0f, 0.0f };
	}

	switch (axis) {
	case ParticleManager::ParticleAlignmentAxis::kX: {
		const float yzLength = std::sqrt(dir.y * dir.y + dir.z * dir.z);
		return {
			0.0f,
			std::atan2(-dir.z, dir.x),
			std::atan2(dir.y, yzLength)
		};
	}
	case ParticleManager::ParticleAlignmentAxis::kY: {
		const float yzLength = std::sqrt(dir.y * dir.y + dir.z * dir.z);
		return {
			std::atan2(dir.z, dir.y),
			0.0f,
			std::atan2(-dir.x, yzLength)
		};
	}
	case ParticleManager::ParticleAlignmentAxis::kZ:
	default: {
		const float xzLength = std::sqrt(dir.x * dir.x + dir.z * dir.z);
		return {
			std::atan2(-dir.y, xzLength),
			std::atan2(dir.x, dir.z),
			0.0f
		};
	}
	}
}

void CollectEffectFiles(std::vector<std::string>& paths, std::vector<std::string>& names) {
	paths.clear();
	names.clear();

	std::error_code error;
	const std::filesystem::path particleDirectory =
		EditableResourcePath::Resolve("resources/particles");
	for (const std::filesystem::directory_entry& entry :
		std::filesystem::directory_iterator(particleDirectory, error)) {
		if (error) {
			break;
		}
		if (!entry.is_regular_file() || entry.path().extension() != ".json") {
			continue;
		}
		if (entry.path().filename() == "scene_particles.json") {
			continue;
		}
		paths.push_back(
			StringUtility::ToUtf8(
				EditableResourcePath::ToProjectRelative(entry.path())
			)
		);
	}

	std::sort(paths.begin(), paths.end());
	for (const std::string& path : paths) {
		names.push_back(
			StringUtility::ToUtf8(StringUtility::ToPath(path).filename())
		);
	}
}

} // namespace

void ParticleManager::ApplySceneParticleEmitterSettings(SceneParticlePlacement& placement) {
	ApplySceneParticlePlacement(placement);
}

void ParticleManager::RebuildSceneParticleEmitter(
	SceneParticlePlacement& placement,
	bool clearParticles,
	bool useEffectEmitterSettings
) {
	delete placement.emitter;
	placement.emitter = nullptr;

	if (!placement.effect) {
		placement.effect = new ParticleEffectDesc();
	}

	if (!ParticleEffectResource::Load(placement.effectFilePath, *placement.effect)) {
		*placement.effect = ParticleEffectDesc{};
		placement.effect->name = placement.label.empty() ? "newParticle" : placement.label;
	}

	if (useEffectEmitterSettings) {
		placement.translate = placement.effect->emitter.translate;
		placement.spawnSize = placement.effect->emitter.spawnSize;
		placement.count = placement.effect->emitter.count;
		placement.frequency = NormalizeEmitterFrequency(placement.effect->emitter.frequency);
		placement.emitterActive = placement.effect->emitter.isActive;
	}

	placement.effect->emitter.translate = placement.translate;
	placement.effect->emitter.spawnSize = placement.spawnSize;
	placement.effect->emitter.count = placement.count;
	placement.effect->emitter.frequency = NormalizeEmitterFrequency(placement.frequency);
	placement.effect->emitter.isActive = placement.emitterActive;

	if (placement.effect->simulationType == ParticleSimulationType::kGPU) {
		return;
	}

	ParticleEffectResource::PrepareParticleGroup(*placement.effect, clearParticles);
	placement.emitter = new ParticleEmitter();
	placement.emitter->Initialize(this, placement.effect->name);
	ParticleEffectResource::ApplyToEmitter(*placement.emitter, *placement.effect);
}

void ParticleManager::ReleasePlacements(std::vector<SceneParticlePlacement>& placements) {
	for (SceneParticlePlacement& placement : placements) {
		delete placement.effect;
		placement.effect = nullptr;
		delete placement.emitter;
		placement.emitter = nullptr;
	}
	placements.clear();
}

void ParticleManager::RebuildParticleAssetInstance(SceneParticleAssetInstance& instance) {
	ReleasePlacements(instance.runtimePlacements);
	const auto asset = particlePlacementAssets_.find(instance.assetName);
	if (asset == particlePlacementAssets_.end()) return;

	for (const SceneParticlePlacement& source : asset->second.placements) {
		SceneParticlePlacement placement{};
		placement.label = source.label;
		placement.effectFilePath = source.effectFilePath;
		placement.enabled = source.enabled;
		placement.emitterActive = source.emitterActive;
		placement.translate = AddVector3(source.translate, instance.translate);
		placement.spawnSize = source.spawnSize;
		placement.count = source.count;
		placement.frequency = source.frequency;
		RebuildSceneParticleEmitter(placement, false, false);
		instance.runtimePlacements.push_back(placement);
		instance.runtimePlacements.back().effect = placement.effect;
		instance.runtimePlacements.back().emitter = placement.emitter;
		placement.effect = nullptr;
		placement.emitter = nullptr;
	}
}

void ParticleManager::RebuildInstancesUsingAsset(const std::string& assetName) {
	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			if (instance.assetName == assetName) {
				RebuildParticleAssetInstance(instance);
			}
		}
	}
	MarkSceneGpuParticleSyncDirty(
		SceneParticleAssetContainsGpuPlacement(assetName) &&
		SceneParticleAssetHasEnabledInstance(assetName)
	);
}

bool ParticleManager::LoadPlacementEmitterSettings(SceneParticlePlacement& placement) {
	ParticleEffectDesc effect{};
	if (!ParticleEffectResource::Load(placement.effectFilePath, effect)) return false;
	placement.translate = effect.emitter.translate;
	placement.spawnSize = effect.emitter.spawnSize;
	placement.count = effect.emitter.count;
	placement.frequency = NormalizeEmitterFrequency(effect.emitter.frequency);
	placement.emitterActive = effect.emitter.isActive;
	return true;
}

GpuParticle* ParticleManager::GetOrCreateGpuParticle(const std::string& key) {
	auto it = gpuParticles_.find(key);
	if (it != gpuParticles_.end()) {
		return it->second.get();
	}
	if (!particleCommon_ || !srvManager_) {
		return nullptr;
	}

	auto particle = std::make_unique<GpuParticle>();
	particle->Initialize(particleCommon_, srvManager_, "resources/circle.png");
	GpuParticle* result = particle.get();
	gpuParticles_.emplace(key, std::move(particle));
	return result;
}

GpuParticle* ParticleManager::FindGpuParticle(const std::string& key) {
	auto it = gpuParticles_.find(key);
	return it != gpuParticles_.end() ? it->second.get() : nullptr;
}

void ParticleManager::ApplyGpuParticleEffectToKey(
	const std::string& key,
	const ParticleEffectDesc& effect
) {
	GpuParticle* particle = GetOrCreateGpuParticle(key);
	if (!particle) {
		return;
	}

	particle->ApplyEffectDesc(effect);
}

void ParticleManager::EraseGpuParticle(const std::string& key) {
	gpuParticles_.erase(key);
	sceneGpuParticleKeys_.erase(key);
}

std::string ParticleManager::MakeSceneGpuParticleKey(
	const std::string& sceneName,
	size_t instanceIndex,
	size_t placementIndex,
	const SceneParticleAssetInstance& instance,
	const SceneParticlePlacement& placement
) const {
	std::ostringstream key;
	key << "scene|" << sceneName << "|"
		<< instanceIndex << "|"
		<< placementIndex << "|"
		<< instance.assetName << "|"
		<< placement.effectFilePath << "|"
		<< placement.translate.x << ","
		<< placement.translate.y << ","
		<< placement.translate.z << "|"
		<< placement.spawnSize.x << ","
		<< placement.spawnSize.y << ","
		<< placement.spawnSize.z << "|"
		<< placement.count << "|"
		<< NormalizeEmitterFrequency(placement.frequency);
	return key.str();
}

bool ParticleManager::SceneParticlePlacementUsesGpu(
	const SceneParticlePlacement& placement
) const {
	if (placement.effect) {
		return placement.effect->simulationType == ParticleSimulationType::kGPU;
	}

	ParticleEffectDesc effect{};
	return ParticleEffectResource::Load(placement.effectFilePath, effect) &&
		effect.simulationType == ParticleSimulationType::kGPU;
}

bool ParticleManager::SceneParticleAssetContainsGpuPlacement(
	const std::string& assetName
) const {
	const auto asset = particlePlacementAssets_.find(assetName);
	if (asset == particlePlacementAssets_.end()) {
		return false;
	}

	for (const SceneParticlePlacement& placement : asset->second.placements) {
		if (SceneParticlePlacementUsesGpu(placement)) {
			return true;
		}
	}
	return false;
}

bool ParticleManager::SceneParticleAssetHasEnabledInstance(
	const std::string& assetName
) const {
	for (const auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (const SceneParticleAssetInstance& instance : instances) {
			if (instance.enabled && instance.assetName == assetName) {
				return true;
			}
		}
	}
	return false;
}

bool ParticleManager::SceneParticleInstanceContainsGpuPlacement(
	const SceneParticleAssetInstance& instance
) const {
	for (const SceneParticlePlacement& placement : instance.runtimePlacements) {
		if (placement.enabled && SceneParticlePlacementUsesGpu(placement)) {
			return true;
		}
	}
	return false;
}

bool ParticleManager::SceneParticleSceneContainsEnabledGpuPlacement(
	const std::string& sceneName
) const {
	const auto scene = sceneParticleAssetInstances_.find(sceneName);
	if (scene == sceneParticleAssetInstances_.end()) {
		return false;
	}

	for (const SceneParticleAssetInstance& instance : scene->second) {
		if (!instance.enabled) {
			continue;
		}
		if (SceneParticleInstanceContainsGpuPlacement(instance)) {
			return true;
		}
	}
	return false;
}

void ParticleManager::ApplySceneParticlePlacement(
	SceneParticlePlacement& placement,
	const std::string& syncKey
) {
	if (!placement.effect) {
		RebuildSceneParticleEmitter(placement, false, false);
	}
	if (!placement.effect || !placement.enabled || !placement.emitterActive) {
		return;
	}

	placement.effect->emitter.translate = placement.translate;
	placement.effect->emitter.spawnSize = placement.spawnSize;
	placement.effect->emitter.count = placement.count;
	placement.effect->emitter.frequency =
		NormalizeEmitterFrequency(placement.frequency);
	placement.effect->emitter.isActive = placement.emitterActive;

	if (placement.effect->simulationType == ParticleSimulationType::kGPU) {
		ApplySceneGpuPlacement(placement, syncKey);
		return;
	}

	if (!placement.emitter) {
		RebuildSceneParticleEmitter(placement, false, false);
		if (!placement.emitter || !placement.effect) {
			return;
		}
	}

	ParticleEffectResource::PrepareParticleGroup(*placement.effect, false);
	ParticleEffectResource::ApplyToEmitter(*placement.emitter, *placement.effect);
}

void ParticleManager::ApplySceneGpuPlacement(
	SceneParticlePlacement& placement,
	const std::string& syncKey
) {
	if (!placement.effect ||
		placement.effect->simulationType != ParticleSimulationType::kGPU ||
		!placement.enabled ||
		!placement.emitterActive ||
		placement.count == 0) {
		return;
	}

	placement.effect->emitter.translate = placement.translate;
	placement.effect->emitter.spawnSize = placement.spawnSize;
	placement.effect->emitter.count = placement.count;
	placement.effect->emitter.frequency =
		NormalizeEmitterFrequency(placement.frequency);
	placement.effect->emitter.isActive = placement.emitterActive;

	delete placement.emitter;
	placement.emitter = nullptr;

	const std::string key = syncKey.empty()
		? std::string("scene:manual:") + placement.effectFilePath
		: syncKey;
	ApplyGpuParticleEffectToKey(key, *placement.effect);
	sceneGpuParticleKeys_.insert(key);
}

void ParticleManager::SyncSceneGpuParticle(const std::string& sceneName) {
	auto scene = sceneParticleAssetInstances_.find(sceneName);
	if (scene == sceneParticleAssetInstances_.end()) {
		return;
	}

	std::unordered_set<std::string> desiredSceneKeys;
	for (size_t instanceIndex = 0; instanceIndex < scene->second.size(); ++instanceIndex) {
		SceneParticleAssetInstance& instance = scene->second[instanceIndex];
		if (!instance.enabled) {
			continue;
		}

		for (size_t placementIndex = 0;
			placementIndex < instance.runtimePlacements.size();
			++placementIndex) {
			SceneParticlePlacement& placement =
				instance.runtimePlacements[placementIndex];
			if (!placement.effect) {
				RebuildSceneParticleEmitter(placement, false, false);
			}
			if (!placement.effect ||
				placement.effect->simulationType != ParticleSimulationType::kGPU ||
				!placement.enabled ||
				!placement.emitterActive ||
				placement.count == 0) {
				continue;
			}

			const std::string syncKey = MakeSceneGpuParticleKey(
				sceneName,
				instanceIndex,
				placementIndex,
				instance,
				placement
			);
			desiredSceneKeys.insert(syncKey);
			if (!sceneGpuParticleKeys_.contains(syncKey) ||
				!FindGpuParticle(syncKey)) {
				ApplySceneParticlePlacement(placement, syncKey);
			}
		}
	}

	const std::string sceneKeyPrefix = "scene|" + sceneName + "|";
	for (auto it = sceneGpuParticleKeys_.begin();
		it != sceneGpuParticleKeys_.end();) {
		const std::string key = *it;
		if (
			key.rfind(sceneKeyPrefix, 0) == 0 &&
			!desiredSceneKeys.contains(key)
		) {
			gpuParticles_.erase(key);
			it = sceneGpuParticleKeys_.erase(it);
		}
		else {
			++it;
		}
	}
}

void ParticleManager::MarkSceneGpuParticleSyncDirty(bool resumeGpuSync) {
	if (!resumeGpuSync) {
		return;
	}
	for (const std::string& key : sceneGpuParticleKeys_) {
		gpuParticles_.erase(key);
	}
	sceneGpuParticleKeys_.clear();
}

void ParticleManager::RefreshPlacementAssetsForEffect(const std::string& effectFilePath) {
	bool refreshed = false;
	for (auto& [assetName, asset] : particlePlacementAssets_) {
		bool assetChanged = false;
		for (SceneParticlePlacement& placement : asset.placements) {
			if (StringUtility::ToPath(placement.effectFilePath).lexically_normal() !=
				StringUtility::ToPath(effectFilePath).lexically_normal()) {
				continue;
			}
			assetChanged |= LoadPlacementEmitterSettings(placement);
		}
		if (assetChanged) {
			RebuildInstancesUsingAsset(assetName);
			refreshed = true;
		}
	}
	if (refreshed) sceneParticleLayoutDirty_ = true;
}

bool ParticleManager::LoadSceneParticleLayout(
	const std::string& filePath,
	bool resumeGpuSync
) {
	json root;
	const std::filesystem::path resolvedPath = EditableResourcePath::Resolve(filePath);
	const std::filesystem::path candidates[] = {
		resolvedPath,
		EditableResourcePath::BackupPath(resolvedPath)
	};
	bool parsed = false;
	for (const std::filesystem::path& candidate : candidates) {
		std::string text;
		if (!EditableResourcePath::ReadTextFile(candidate, text)) {
			continue;
		}
		try {
			root = json::parse(text);
			if (root.contains("scenes") && root.at("scenes").is_object()) {
				parsed = true;
				break;
			}
		} catch (...) {
		}
	}

	if (!parsed) {
		sceneParticleLayoutLoaded_ = true;
		sceneParticlePersistenceMessage_ =
			"Load failed. Existing scene placements were kept.";
		return false;
	}

	std::unordered_map<std::string, ParticlePlacementAsset> loadedAssets;
	std::unordered_map<std::string, std::vector<SceneParticleAssetInstance>> loadedScenes;
	const auto readPlacement = [](const json& placementJson) {
		SceneParticlePlacement placement{};
		placement.label = placementJson.value("label", placement.label);
		placement.effectFilePath =
			placementJson.value("effectFilePath", placement.effectFilePath);
		placement.enabled = placementJson.value("enabled", placement.enabled);
		placement.emitterActive =
			placementJson.value("emitterActive", placement.emitterActive);
		if (placementJson.contains("translate")) {
			placement.translate = ReadVector3(placementJson.at("translate"), placement.translate);
		}
		if (placementJson.contains("spawnSize")) {
			placement.spawnSize = ReadVector3(placementJson.at("spawnSize"), placement.spawnSize);
		}
		placement.count = placementJson.value("count", placement.count);
		placement.frequency = NormalizeEmitterFrequency(
			placementJson.value("frequency", placement.frequency));
		return placement;
	};

	try {
		if (root.contains("assets") && root.at("assets").is_object()) {
			for (auto it = root.at("assets").begin(); it != root.at("assets").end(); ++it) {
				if (!it.value().is_array()) throw std::runtime_error("Asset is not an array");
				ParticlePlacementAsset asset{};
				asset.name = it.key();
				for (const json& placementJson : it.value()) {
					asset.placements.push_back(readPlacement(placementJson));
				}
				loadedAssets.emplace(asset.name, std::move(asset));
			}
			for (auto it = root.at("scenes").begin(); it != root.at("scenes").end(); ++it) {
				if (!it.value().is_array()) throw std::runtime_error("Scene is not an array");
				for (const json& instanceJson : it.value()) {
					SceneParticleAssetInstance instance{};
					instance.assetName = instanceJson.value("assetName", std::string{});
					instance.label = instanceJson.value("label", instance.assetName);
					instance.enabled = instanceJson.value("enabled", instance.enabled);
					if (instanceJson.contains("translate")) {
						instance.translate = ReadVector3(instanceJson.at("translate"), instance.translate);
					}
					loadedScenes[NormalizeSceneParticleKey(it.key())].push_back(
						std::move(instance)
					);
				}
			}
		} else {
			// Version 1 migration: each scene's direct placements become one reusable asset.
			for (auto it = root.at("scenes").begin(); it != root.at("scenes").end(); ++it) {
				if (!it.value().is_array()) throw std::runtime_error("Scene is not an array");
				const std::string assetName = "Legacy_" + it.key();
				ParticlePlacementAsset asset{};
				asset.name = assetName;
				for (const json& placementJson : it.value()) {
					asset.placements.push_back(readPlacement(placementJson));
				}
				loadedAssets.emplace(assetName, std::move(asset));
				SceneParticleAssetInstance instance{};
				instance.assetName = assetName;
				instance.label = assetName;
				loadedScenes[NormalizeSceneParticleKey(it.key())].push_back(
					std::move(instance)
				);
			}
		}
	} catch (...) {
		sceneParticlePersistenceMessage_ =
			"Load failed. Existing scene placements were kept.";
		return false;
	}

	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			ReleasePlacements(instance.runtimePlacements);
		}
	}
	particlePlacementAssets_ = std::move(loadedAssets);
	sceneParticleAssetInstances_ = std::move(loadedScenes);
	sceneParticleAssetCycleSteps_.clear();
	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			RebuildParticleAssetInstance(instance);
		}
	}
	MarkSceneGpuParticleSyncDirty(resumeGpuSync);
	sceneParticleLayoutLoaded_ = true;
	sceneParticleLayoutDirty_ = false;
	sceneParticlePersistenceMessage_ = "Layout loaded from project resources.";
	return true;
}

bool ParticleManager::SaveSceneParticleLayout(const std::string& filePath) const {
	json root;
	root["version"] = 2;
	root["assets"] = json::object();
	root["scenes"] = json::object();

	for (const auto& [assetName, asset] : particlePlacementAssets_) {
		json placementArray = json::array();
		for (const SceneParticlePlacement& placement : asset.placements) {
			placementArray.push_back({
				{ "label", placement.label },
				{ "effectFilePath", placement.effectFilePath },
				{ "enabled", placement.enabled },
				{ "emitterActive", placement.emitterActive },
				{ "translate", ToJson(placement.translate) },
				{ "spawnSize", ToJson(placement.spawnSize) },
				{ "count", placement.count },
				{ "frequency", placement.frequency }
			});
		}
		root["assets"][assetName] = placementArray;
	}
	for (const auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		json instanceArray = json::array();
		for (const SceneParticleAssetInstance& instance : instances) {
			instanceArray.push_back({
				{ "label", instance.label },
				{ "assetName", instance.assetName },
				{ "enabled", instance.enabled },
				{ "translate", ToJson(instance.translate) }
			});
		}
		root["scenes"][sceneName] = instanceArray;
	}

	std::ostringstream output;
	output << std::setw(2) << root << '\n';
	const bool saved = EditableResourcePath::WriteTextAtomically(filePath, output.str());
	if (saved) {
		sceneParticleLayoutDirty_ = false;
	}
	sceneParticlePersistenceMessage_ = saved
		? "Layout saved to project resources. Backup updated."
		: "Save failed. The previous layout was not overwritten.";
	return saved;
}

void ParticleManager::UpdateSceneParticles(const std::string& sceneName) {
	if (!sceneParticleLayoutLoaded_) {
		LoadSceneParticleLayout();
	}

	auto it = sceneParticleAssetInstances_.find(sceneName);
	if (it == sceneParticleAssetInstances_.end()) {
		return;
	}

	SyncSceneGpuParticle(sceneName);

	for (SceneParticleAssetInstance& instance : it->second) {
		if (!instance.enabled) continue;
		for (SceneParticlePlacement& placement : instance.runtimePlacements) {
			if (placement.enabled && placement.emitter) placement.emitter->Update();
		}
	}
}

void ParticleManager::ReleaseSceneParticles(const std::string& sceneName) {
	const std::string sceneKeyPrefix = "scene|" + sceneName + "|";
	for (auto it = sceneGpuParticleKeys_.begin();
		it != sceneGpuParticleKeys_.end();) {
		const std::string& key = *it;
		if (key.rfind(sceneKeyPrefix, 0) == 0) {
			gpuParticles_.erase(key);
			it = sceneGpuParticleKeys_.erase(it);
		} else {
			++it;
		}
	}
}

void ParticleManager::CycleSceneParticleAssets(const std::string& sceneName) {
	if (!sceneParticleLayoutLoaded_) LoadSceneParticleLayout();
	auto scene = sceneParticleAssetInstances_.find(sceneName);
	if (scene == sceneParticleAssetInstances_.end() || scene->second.empty()) return;

	auto& instances = scene->second;
	size_t& step = sceneParticleAssetCycleSteps_[sceneName];
	if (step > instances.size()) step = 0;

	const bool enableAll = step == instances.size();
	for (size_t index = 0; index < instances.size(); ++index) {
		SceneParticleAssetInstance& instance = instances[index];
		const bool wasEnabled = instance.enabled;
		instance.enabled = enableAll || index == step;
		if (instance.enabled && !wasEnabled) {
			for (SceneParticlePlacement& placement : instance.runtimePlacements) {
				if (placement.emitter) placement.emitter->ResetEmissionState();
			}
		}
	}
	MarkSceneGpuParticleSyncDirty(
		SceneParticleSceneContainsEnabledGpuPlacement(sceneName)
	);
	step = (step + 1u) % (instances.size() + 1u);
}

void ParticleManager::DrawSceneParticleImGui(
	const std::string& currentSceneName,
	const char* windowTitle
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!sceneParticleLayoutLoaded_) {
		LoadSceneParticleLayout();
	}

	static char layoutPath[260] = "resources/particles/scene_particles.json";
	static char newAssetName[128] = "ParticleSet";
	static std::string selectedAssetName;
	static int assetToAddIndex = 0;
	std::vector<std::string> effectPaths;
	std::vector<std::string> effectNames;
	CollectEffectFiles(effectPaths, effectNames);

	ImGui::Begin(
		windowTitle,
		nullptr,
		ImGuiWindowFlags_NoFocusOnAppearing
	);
	ImGui::InputText("Layout", layoutPath, sizeof(layoutPath));
	if (ImGui::Button("Load Layout")) LoadSceneParticleLayout(layoutPath, true);
	ImGui::SameLine();
	if (ImGui::Button("Save Layout")) SaveSceneParticleLayout(layoutPath);
	if (!sceneParticlePersistenceMessage_.empty()) {
		ImGui::TextWrapped("%s", sceneParticlePersistenceMessage_.c_str());
	}
	if (sceneParticleLayoutDirty_) {
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Unsaved layout changes");
	}

	std::vector<std::string> assetNames;
	assetNames.reserve(particlePlacementAssets_.size());
	for (const auto& [name, asset] : particlePlacementAssets_) {
		(void)asset;
		assetNames.push_back(name);
	}
	std::sort(assetNames.begin(), assetNames.end());
	if (selectedAssetName.empty() && !assetNames.empty()) selectedAssetName = assetNames.front();
	if (assetToAddIndex >= static_cast<int>(assetNames.size())) assetToAddIndex = 0;

	if (ImGui::BeginTabBar("ParticleAssetTabs")) {
		if (ImGui::BeginTabItem("Assets")) {
			ImGui::InputText("New Asset", newAssetName, sizeof(newAssetName));
			ImGui::SameLine();
			if (ImGui::Button("Create Asset") && newAssetName[0] != '\0' &&
				!particlePlacementAssets_.contains(newAssetName)) {
				ParticlePlacementAsset asset{};
				asset.name = newAssetName;
				particlePlacementAssets_.emplace(asset.name, std::move(asset));
				selectedAssetName = newAssetName;
				sceneParticleLayoutDirty_ = true;
				assetNames.push_back(selectedAssetName);
				std::sort(assetNames.begin(), assetNames.end());
			}

			const char* selectedAssetLabel = selectedAssetName.empty()
				? "(select asset)" : selectedAssetName.c_str();
			if (ImGui::BeginCombo("Placement Asset", selectedAssetLabel)) {
				for (const std::string& assetName : assetNames) {
					const bool selected = assetName == selectedAssetName;
					if (ImGui::Selectable(assetName.c_str(), selected)) selectedAssetName = assetName;
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			auto assetIt = particlePlacementAssets_.find(selectedAssetName);
			if (assetIt != particlePlacementAssets_.end()) {
				auto& placements = assetIt->second.placements;
				if (ImGui::Button("Add Placement")) {
					SceneParticlePlacement placement{};
					placement.label = "Particle " + std::to_string(placements.size());
					if (!effectPaths.empty()) placement.effectFilePath = effectPaths.front();
					if (!effectPaths.empty()) LoadPlacementEmitterSettings(placement);
					placements.push_back(std::move(placement));
					RebuildInstancesUsingAsset(selectedAssetName);
					sceneParticleLayoutDirty_ = true;
				}

				int removePlacement = -1;
				for (int index = 0; index < static_cast<int>(placements.size()); ++index) {
					SceneParticlePlacement& placement = placements[index];
					ImGui::PushID(index);
					if (ImGui::CollapsingHeader(placement.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
						char label[128]{};
						CopyText(label, sizeof(label), placement.label);
						bool changed = false;
						if (ImGui::Checkbox("Enabled", &placement.enabled)) changed = true;
						ImGui::SameLine();
						if (ImGui::Button("Remove")) removePlacement = index;
						if (ImGui::InputText("Label", label, sizeof(label))) {
							placement.label = label;
							changed = true;
						}
						const std::string effectLabel = StringUtility::ToUtf8(
							StringUtility::ToPath(placement.effectFilePath).filename()
						);
						if (ImGui::BeginCombo("Effect", effectLabel.c_str())) {
							for (int effectIndex = 0; effectIndex < static_cast<int>(effectPaths.size()); ++effectIndex) {
								const bool selected = placement.effectFilePath == effectPaths[effectIndex];
								if (ImGui::Selectable(effectNames[effectIndex].c_str(), selected)) {
									placement.effectFilePath = effectPaths[effectIndex];
									LoadPlacementEmitterSettings(placement);
									changed = true;
								}
								if (selected) ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
						if (ImGui::Button("Reload Emitter from Effect")) {
							changed |= LoadPlacementEmitterSettings(placement);
						}
						changed |= ImGui::DragFloat3("Translate", &placement.translate.x, 0.05f);
						changed |= ImGui::DragFloat3("SpawnSize", &placement.spawnSize.x, 0.05f);
						int count = static_cast<int>(placement.count);
						if (ImGui::DragInt("Count", &count, 1, 0, 1000)) {
							placement.count = static_cast<uint32_t>((std::max)(count, 0));
							changed = true;
						}
						changed |= ImGui::DragFloat("Frequency", &placement.frequency, 0.001f, 1.0f / 60.0f, 10.0f);
						placement.frequency = NormalizeEmitterFrequency(placement.frequency);
						changed |= ImGui::Checkbox("Emitter Active", &placement.emitterActive);
						if (changed) {
							RebuildInstancesUsingAsset(selectedAssetName);
							sceneParticleLayoutDirty_ = true;
						}
					}
					ImGui::PopID();
				}
				if (removePlacement >= 0) {
					placements.erase(placements.begin() + removePlacement);
					RebuildInstancesUsingAsset(selectedAssetName);
					sceneParticleLayoutDirty_ = true;
				}
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Scenes")) {
			std::vector<std::string> sceneNames;
			sceneNames.reserve(sceneParticleAssetInstances_.size() + 1);
			for (const auto& [sceneName, instances] :
				sceneParticleAssetInstances_) {
				(void)instances;
				sceneNames.push_back(sceneName);
			}
			const std::string currentSceneKey =
				NormalizeSceneParticleKey(currentSceneName);
			if (std::find(
				sceneNames.begin(),
				sceneNames.end(),
				currentSceneKey
			) == sceneNames.end()) {
				sceneNames.push_back(currentSceneKey);
			}
			std::sort(sceneNames.begin(), sceneNames.end());
			if (ImGui::BeginTabBar("ParticleSceneTabs")) {
				for (const std::string& sceneName : sceneNames) {
					const ImGuiTabItemFlags flags = currentSceneKey == sceneName
						? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
					if (!ImGui::BeginTabItem(sceneName.c_str(), nullptr, flags)) continue;
					auto& instances = sceneParticleAssetInstances_[sceneName];
					const char* assetLabel = assetNames.empty() ? "(no assets)" : assetNames[assetToAddIndex].c_str();
					if (ImGui::BeginCombo("Asset to Add", assetLabel)) {
						for (int index = 0; index < static_cast<int>(assetNames.size()); ++index) {
							if (ImGui::Selectable(assetNames[index].c_str(), index == assetToAddIndex)) assetToAddIndex = index;
						}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					if (ImGui::Button("Add Asset Instance") && !assetNames.empty()) {
						SceneParticleAssetInstance instance{};
						instance.assetName = assetNames[assetToAddIndex];
						instance.label = instance.assetName + " " + std::to_string(instances.size());
						RebuildParticleAssetInstance(instance);
						const bool containsGpu =
							SceneParticleInstanceContainsGpuPlacement(instance);
						instances.push_back(std::move(instance));
						MarkSceneGpuParticleSyncDirty(containsGpu);
						sceneParticleLayoutDirty_ = true;
					}

					int removeInstance = -1;
					for (int index = 0; index < static_cast<int>(instances.size()); ++index) {
						SceneParticleAssetInstance& instance = instances[index];
						ImGui::PushID(index);
						if (ImGui::CollapsingHeader(instance.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
							char label[128]{};
							CopyText(label, sizeof(label), instance.label);
							if (ImGui::Checkbox("Enabled", &instance.enabled)) {
								MarkSceneGpuParticleSyncDirty(
									SceneParticleInstanceContainsGpuPlacement(instance)
								);
								sceneParticleLayoutDirty_ = true;
							}
							ImGui::SameLine();
							if (ImGui::Button("Emit Now")) {
								MarkSceneGpuParticleSyncDirty(
									SceneParticleInstanceContainsGpuPlacement(instance)
								);
								for (SceneParticlePlacement& placement : instance.runtimePlacements) {
									if (!placement.enabled) {
										continue;
									}
									if (placement.effect &&
										placement.effect->simulationType == ParticleSimulationType::kGPU) {
										ApplySceneParticlePlacement(placement);
									}
									else if (placement.emitter) {
										placement.emitter->Emit();
									}
								}
							}
							ImGui::SameLine();
							if (ImGui::Button("Remove")) removeInstance = index;
							if (ImGui::InputText("Label", label, sizeof(label))) {
								instance.label = label;
								sceneParticleLayoutDirty_ = true;
							}
							if (ImGui::BeginCombo("Asset", instance.assetName.c_str())) {
								for (const std::string& assetName : assetNames) {
									const bool selected = assetName == instance.assetName;
									if (ImGui::Selectable(assetName.c_str(), selected)) {
										instance.assetName = assetName;
										RebuildParticleAssetInstance(instance);
										MarkSceneGpuParticleSyncDirty(
											SceneParticleInstanceContainsGpuPlacement(instance)
										);
										sceneParticleLayoutDirty_ = true;
									}
								}
								ImGui::EndCombo();
							}
							if (ImGui::DragFloat3("Offset", &instance.translate.x, 0.05f)) {
								RebuildParticleAssetInstance(instance);
								MarkSceneGpuParticleSyncDirty(
									SceneParticleInstanceContainsGpuPlacement(instance)
								);
								sceneParticleLayoutDirty_ = true;
							}
						}
						ImGui::PopID();
					}
					if (removeInstance >= 0) {
						const bool removedGpu =
							SceneParticleInstanceContainsGpuPlacement(
								instances[removeInstance]
							);
						ReleasePlacements(instances[removeInstance].runtimePlacements);
						instances.erase(instances.begin() + removeInstance);
						MarkSceneGpuParticleSyncDirty(removedGpu);
						sceneParticleLayoutDirty_ = true;
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
	return;
#else
	(void)currentSceneName;
	(void)windowTitle;
#endif
}

void ParticleManager::CreateGroupVertexResource(ParticleGroup& group) {
	if (group.render.primitiveType == PrimitiveType::kPlane) {
		group.vertexResource.Reset();
		group.vertexBufferView = particleCommon_->GetVertexBufferView();
		group.vertexCount = 6;
		return;
	}

	std::vector<ParticleCommon::VertexData> vertices;
	if (group.render.primitiveType == PrimitiveType::kRing) {
		const RingPrimitiveDesc& ring = group.render.ring;
		const uint32_t divisions = std::clamp(ring.divisions, 3u, 256u);
		const float outerRadius = (std::max)(ring.outerRadius, 0.001f);
		const float innerRadius = std::clamp(ring.innerRadius, 0.0f, outerRadius);
		const float angleRange = ring.endAngle - ring.startAngle;
		vertices.reserve(static_cast<size_t>(divisions) * 6);

		for (uint32_t index = 0; index < divisions; ++index) {
			const float t0 = static_cast<float>(index) / static_cast<float>(divisions);
			const float t1 = static_cast<float>(index + 1) / static_cast<float>(divisions);
			const float angle0 = ring.startAngle + angleRange * t0;
			const float angle1 = ring.startAngle + angleRange * t1;
			const float sin0 = std::sin(angle0);
			const float cos0 = std::cos(angle0);
			const float sin1 = std::sin(angle1);
			const float cos1 = std::cos(angle1);

			const Vector2 outerUv0 = ring.uvMode == RingUvMode::kHorizontal
				? Vector2{ t0, 0.0f } : Vector2{ 0.0f, t0 };
			const Vector2 outerUv1 = ring.uvMode == RingUvMode::kHorizontal
				? Vector2{ t1, 0.0f } : Vector2{ 0.0f, t1 };
			const Vector2 innerUv0 = ring.uvMode == RingUvMode::kHorizontal
				? Vector2{ t0, 1.0f } : Vector2{ 1.0f, t0 };
			const Vector2 innerUv1 = ring.uvMode == RingUvMode::kHorizontal
				? Vector2{ t1, 1.0f } : Vector2{ 1.0f, t1 };

			const ParticleCommon::VertexData outer0{
				{ -sin0 * outerRadius, cos0 * outerRadius, 0.0f, 1.0f },
				outerUv0, { 0.0f, 0.0f, -1.0f }, ring.outerColor
			};
			const ParticleCommon::VertexData outer1{
				{ -sin1 * outerRadius, cos1 * outerRadius, 0.0f, 1.0f },
				outerUv1, { 0.0f, 0.0f, -1.0f }, ring.outerColor
			};
			const ParticleCommon::VertexData inner0{
				{ -sin0 * innerRadius, cos0 * innerRadius, 0.0f, 1.0f },
				innerUv0, { 0.0f, 0.0f, -1.0f }, ring.innerColor
			};
			const ParticleCommon::VertexData inner1{
				{ -sin1 * innerRadius, cos1 * innerRadius, 0.0f, 1.0f },
				innerUv1, { 0.0f, 0.0f, -1.0f }, ring.innerColor
			};

			vertices.insert(vertices.end(), { outer0, outer1, inner0, inner0, outer1, inner1 });
		}
	}
	else {
		const CylinderPrimitiveDesc& cylinder = group.render.cylinder;
		const uint32_t divisions = std::clamp(cylinder.divisions, 3u, 256u);
		const float topRadius = (std::max)(cylinder.topRadius, 0.0f);
		const float bottomRadius = (std::max)(cylinder.bottomRadius, 0.0f);
		const float height = (std::max)(cylinder.height, 0.001f);
		const float angleRange = cylinder.endAngle - cylinder.startAngle;
		vertices.reserve(static_cast<size_t>(divisions) * 6);

		for (uint32_t index = 0; index < divisions; ++index) {
			const float t0 = static_cast<float>(index) / static_cast<float>(divisions);
			const float t1 = static_cast<float>(index + 1) / static_cast<float>(divisions);
			const float angle0 = cylinder.startAngle + angleRange * t0;
			const float angle1 = cylinder.startAngle + angleRange * t1;
			const float sin0 = std::sin(angle0);
			const float cos0 = std::cos(angle0);
			const float sin1 = std::sin(angle1);
			const float cos1 = std::cos(angle1);

			const Vector2 topUv0 = cylinder.uvMode == RingUvMode::kHorizontal
				? Vector2{ t0, 0.0f } : Vector2{ 0.0f, t0 };
			const Vector2 topUv1 = cylinder.uvMode == RingUvMode::kHorizontal
				? Vector2{ t1, 0.0f } : Vector2{ 0.0f, t1 };
			const Vector2 bottomUv0 = cylinder.uvMode == RingUvMode::kHorizontal
				? Vector2{ t0, 1.0f } : Vector2{ 1.0f, t0 };
			const Vector2 bottomUv1 = cylinder.uvMode == RingUvMode::kHorizontal
				? Vector2{ t1, 1.0f } : Vector2{ 1.0f, t1 };

			const Vector3 normal0 = { -sin0, 0.0f, cos0 };
			const Vector3 normal1 = { -sin1, 0.0f, cos1 };
			const ParticleCommon::VertexData top0{
				{ -sin0 * topRadius, height, cos0 * topRadius, 1.0f },
				topUv0, normal0, cylinder.topColor
			};
			const ParticleCommon::VertexData top1{
				{ -sin1 * topRadius, height, cos1 * topRadius, 1.0f },
				topUv1, normal1, cylinder.topColor
			};
			const ParticleCommon::VertexData bottom0{
				{ -sin0 * bottomRadius, 0.0f, cos0 * bottomRadius, 1.0f },
				bottomUv0, normal0, cylinder.bottomColor
			};
			const ParticleCommon::VertexData bottom1{
				{ -sin1 * bottomRadius, 0.0f, cos1 * bottomRadius, 1.0f },
				bottomUv1, normal1, cylinder.bottomColor
			};

			vertices.insert(vertices.end(), { top0, top1, bottom0, bottom0, top1, bottom1 });
		}
	}

	group.vertexResource = particleCommon_->GetDxCommon()->CreateBufferResource(
		sizeof(ParticleCommon::VertexData) * vertices.size()
	);
	ParticleCommon::VertexData* mappedVertices = nullptr;
	group.vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&mappedVertices));
	std::memcpy(
		mappedVertices,
		vertices.data(),
		sizeof(ParticleCommon::VertexData) * vertices.size()
	);

	group.vertexBufferView.BufferLocation = group.vertexResource->GetGPUVirtualAddress();
	group.vertexBufferView.SizeInBytes =
		static_cast<UINT>(sizeof(ParticleCommon::VertexData) * vertices.size());
	group.vertexBufferView.StrideInBytes = sizeof(ParticleCommon::VertexData);
	group.vertexCount = static_cast<uint32_t>(vertices.size());
}

void ParticleManager::CreateDirectionalLightResource() {
	directionalLightResource_ = particleCommon_->GetDxCommon()->CreateBufferResource(sizeof(DirectionalLight));
	directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData_));

	directionalLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
	directionalLightData_->direction = { 0.0f, -1.0f, 0.0f };
	directionalLightData_->intensity = 1.0f;
}

float ParticleManager::RandomRange(float min, float max) {
	if (min > max) {
		std::swap(min, max);
	}

	static std::random_device seedGenerator;
	static std::mt19937 randomEngine(seedGenerator());

	std::uniform_real_distribution<float> distribution(min, max);
	return distribution(randomEngine);
}

Vector3 ParticleManager::RandomVector3Range(const Vector3& min, const Vector3& max) {
	return {
		RandomRange(min.x, max.x),
		RandomRange(min.y, max.y),
		RandomRange(min.z, max.z)
	};
}

Vector4 ParticleManager::RandomVector4Range(const Vector4& min, const Vector4& max) {
	return {
		RandomRange(min.x, max.x),
		RandomRange(min.y, max.y),
		RandomRange(min.z, max.z),
		RandomRange(min.w, max.w)
	};
}

Vector4 ParticleManager::LerpColor(const Vector4& start, const Vector4& end, float t) {
	t = std::clamp(t, 0.0f, 1.0f);

	return {
		start.x + (end.x - start.x) * t,
		start.y + (end.y - start.y) * t,
		start.z + (end.z - start.z) * t,
		start.w + (end.w - start.w) * t
	};
}

void ParticleManager::CreateParticleGroup(const std::string& name, const std::string& textureFilePath) {
	assert(particleCommon_);
	assert(srvManager_);

	assert(!particleGroups_.contains(name));
	assert(srvManager_->CanAllocate());

	TextureManager::GetInstance()->LoadTexture(textureFilePath);

	ParticleGroup group{};
	group.textureFilePath = textureFilePath;
	group.textureSrvIndex = TextureManager::GetInstance()->GetSrvIndex(textureFilePath);

	group.materialResource = particleCommon_->GetDxCommon()->CreateBufferResource(sizeof(Material));
	group.materialResource->Map(0, nullptr, reinterpret_cast<void**>(&group.materialData));
	group.materialData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
	group.materialData->enableLighting = false;
	group.materialData->alphaCutoff = 0.0f;
	group.materialData->flipU = false;
	group.materialData->flipV = false;
	group.materialData->uvTransform = MakeIdentity4x4();
	group.materialData->emissiveIntensity = 1.0f;

	group.instancingResource =
		particleCommon_->GetDxCommon()->CreateBufferResource(sizeof(TransformationMatrix) * kMaxInstanceCount);

	group.instancingResource->Map(0, nullptr, reinterpret_cast<void**>(&group.instancingData));

	group.instanceSrvIndex = srvManager_->Allocate();

	srvManager_->CreateSRVforStructuredBuffer(
		group.instanceSrvIndex,
		group.instancingResource.Get(),
		kMaxInstanceCount,
		sizeof(TransformationMatrix)
	);
	group.render = {};
	CreateGroupVertexResource(group);

	particleGroups_.emplace(name, std::move(group));
}

bool ParticleManager::HasParticleGroup(const std::string& name) const {
	return particleGroups_.contains(name);
}

void ParticleManager::ClearParticleGroup(const std::string& name) {
	auto it = particleGroups_.find(name);
	if (it == particleGroups_.end()) {
		return;
	}

	it->second.particles.clear();
	it->second.instanceCount = 0;
}

void ParticleManager::ClearActiveParticles() {
	for (auto& [name, group] : particleGroups_) {
		(void)name;
		group.particles.clear();
		group.instanceCount = 0;
	}

	for (auto& [key, particle] : gpuParticles_) {
		(void)key;
		particle->ClearParticles();
	}
	pendingLightningEvents_.clear();
	pendingExposureFlashEvents_.clear();

	for (auto& [sceneName, instances] : sceneParticleAssetInstances_) {
		(void)sceneName;
		for (SceneParticleAssetInstance& instance : instances) {
			for (SceneParticlePlacement& placement : instance.runtimePlacements) {
				if (placement.emitter) placement.emitter->ResetEmissionState();
			}
		}
	}
}

void ParticleManager::CreateParticleGroupIfNeeded(
	const std::string& name,
	const std::string& textureFilePath
) {
	if (HasParticleGroup(name)) {
		return;
	}

	CreateParticleGroup(name, textureFilePath);
}

bool ParticleManager::SetParticleGroupTexture(
	const std::string& name,
	const std::string& textureFilePath
) {
	auto group = particleGroups_.find(name);
	if (group == particleGroups_.end() || textureFilePath.empty()) {
		return false;
	}
	if (group->second.textureFilePath == textureFilePath) {
		return true;
	}
	if (!TextureManager::GetInstance()->LoadTexture(textureFilePath)) {
		return false;
	}
	group->second.textureFilePath = textureFilePath;
	group->second.textureSrvIndex =
		TextureManager::GetInstance()->GetSrvIndex(textureFilePath);
	return true;
}

bool ParticleManager::SetParticleGroupParentTransform(
	const std::string& name,
	const Vector3& translation,
	float yaw
) {
	if (
		!std::isfinite(translation.x) ||
		!std::isfinite(translation.y) ||
		!std::isfinite(translation.z) ||
		!std::isfinite(yaw)
	) {
		return false;
	}
	auto it = particleGroups_.find(name);
	if (it == particleGroups_.end()) {
		return false;
	}
	it->second.parentTransformEnabled = true;
	it->second.parentTranslation = translation;
	it->second.parentYaw = yaw;
	return true;
}

void ParticleManager::ClearParticleGroupParentTransform(const std::string& name) {
	auto it = particleGroups_.find(name);
	if (it == particleGroups_.end()) {
		return;
	}
	it->second.parentTransformEnabled = false;
	it->second.parentTranslation = {};
	it->second.parentYaw = 0.0f;
}

void ParticleManager::InitializeParticleLife(Particle& particle, const ParticleBehavior& behavior) {
	particle.currentTime = 0.0f;
	particle.lifeTime = RandomRange(behavior.life.lifeTimeMin, behavior.life.lifeTimeMax);
	particle.isLooping = behavior.life.isLooping;
	particle.loopDuration = (std::max)(behavior.life.loopDuration, 0.001f);
	particle.loopPingPong = behavior.life.loopPingPong;

	particle.enableLifeFade =
		!particle.isLooping && behavior.life.enableLifeFade;
	particle.fadeOutStartRatio = std::clamp(behavior.life.fadeOutStartRatio, 0.0f, 0.99f);
}

void ParticleManager::InitializeParticleMotion(
	Particle& particle,
	const ParticleBehavior& behavior,
	const Vector3& emitterPosition
) {
	particle.movementMode = behavior.motion.mode;

	const ParticleLinearMotionDesc& linear = behavior.motion.linear;

	Vector3 velocityRandom = RandomVector3Range(
		{
			-linear.velocityRandomRange.x,
			-linear.velocityRandomRange.y,
			-linear.velocityRandomRange.z
		},
		{
			linear.velocityRandomRange.x,
			linear.velocityRandomRange.y,
			linear.velocityRandomRange.z
		}
	);

	Vector3 accelerationRandom = RandomVector3Range(
		{
			-linear.accelerationRandomRange.x,
			-linear.accelerationRandomRange.y,
			-linear.accelerationRandomRange.z
		},
		{
			linear.accelerationRandomRange.x,
			linear.accelerationRandomRange.y,
			linear.accelerationRandomRange.z
		}
	);

	particle.velocity = {
		linear.baseVelocity.x + velocityRandom.x,
		linear.baseVelocity.y + velocityRandom.y,
		linear.baseVelocity.z + velocityRandom.z
	};

	particle.acceleration = {
		linear.baseAcceleration.x + accelerationRandom.x,
		linear.baseAcceleration.y + accelerationRandom.y,
		linear.baseAcceleration.z + accelerationRandom.z
	};

	const ParticleSwayDesc& sway = behavior.motion.sway;

	particle.swayTime = 0.0f;
	particle.swayPhase = RandomRange(0.0f, 6.2831853f);
	particle.swayAxis = RandomVector3Range(
		{ -1.0f, -1.0f, -1.0f },
		{ 1.0f, 1.0f, 1.0f }
	);
	particle.swayAmplitude = sway.amplitude;
	particle.swayFrequency = sway.frequency;

	const ParticleWindFieldDesc& wind = behavior.motion.wind;
	particle.windEnabled = wind.enabled;
	particle.windFieldCenter = wind.useEmitterOffset
		? Vector3{
			emitterPosition.x + wind.center.x,
			emitterPosition.y + wind.center.y,
			emitterPosition.z + wind.center.z
		}
		: wind.center;
	particle.windFieldHalfSize = {
		std::abs(wind.size.x) * 0.5f,
		std::abs(wind.size.y) * 0.5f,
		std::abs(wind.size.z) * 0.5f
	};
	particle.linearAccelerationEnabled = linear.enableAcceleration;
	particle.windDirection = NormalizeOrZero(wind.direction);
	particle.windStrength = wind.strength;
	particle.windVelocity = { 0.0f, 0.0f, 0.0f };
	particle.windSmoothVelocity = wind.smoothVelocity;
	particle.windAcceleration = (std::max)(wind.acceleration, 0.0f);
	particle.windRecoverOutsideField = wind.recoverOutsideField;
	particle.windDeceleration = (std::max)(wind.deceleration, 0.0f);
	particle.windBoundaryFalloffEnabled = wind.enableBoundaryFalloff;
	particle.windBoundaryFalloff = (std::max)(wind.boundaryFalloff, 0.0f);
	particle.windTurbulenceStrength = wind.turbulenceStrength;
	particle.windTurbulenceFrequency = wind.turbulenceFrequency;
	particle.windTurbulenceScale = wind.turbulenceScale;
	particle.windPhase = RandomRange(0.0f, 6.2831853f);
	particle.windTurbulenceAxis = NormalizeOrZero(
		RandomVector3Range(
			{ -1.0f, -0.25f, -1.0f },
			{ 1.0f, 0.25f, 1.0f }
		)
	);

	const ParticlePointFieldDesc& pointField = behavior.motion.pointField;
	particle.pointFieldEnabled = pointField.enabled;
	particle.pointFieldCenter = pointField.useEmitterOffset
		? Vector3{
			emitterPosition.x + pointField.center.x,
			emitterPosition.y + pointField.center.y,
			emitterPosition.z + pointField.center.z
		}
		: pointField.center;
	particle.pointFieldRadius = (std::max)(pointField.radius, 0.0f);
	particle.pointFieldAttraction = pointField.attractionStrength;
	particle.pointFieldRepulsion = pointField.repulsionStrength;
	particle.pointFieldOrbit = pointField.orbitStrength;
	particle.pointFieldOrbitAxis = NormalizeOrZero(pointField.orbitAxis);
	if (
		particle.pointFieldOrbitAxis.x == 0.0f &&
		particle.pointFieldOrbitAxis.y == 0.0f &&
		particle.pointFieldOrbitAxis.z == 0.0f
	) {
		particle.pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
	}
	particle.pointFieldFalloff = (std::max)(pointField.falloff, 0.0f);
	particle.pointFieldDamping = (std::max)(pointField.damping, 0.0f);

	if (particle.movementMode == MovementMode::kVortexInward) {
		const ParticleVortexDesc& vortex = behavior.motion.vortex;

		if (vortex.useEmitterOffset) {
			particle.vortexCenter = {
				emitterPosition.x + vortex.center.x,
				emitterPosition.y + vortex.center.y,
				emitterPosition.z + vortex.center.z
			};
		} else {
			particle.vortexCenter = vortex.center;
		}
		particle.vortexAxis = vortex.axis;

		Vector3 offset = {
			particle.transform.translate.x - particle.vortexCenter.x,
			particle.transform.translate.y - particle.vortexCenter.y,
			particle.transform.translate.z - particle.vortexCenter.z
		};

		switch (particle.vortexAxis) {
		case VortexAxis::kX:
			// X軸まわり。YZ平面で回転し、Xが軸方向
			particle.vortexRadius = std::sqrt(offset.y * offset.y + offset.z * offset.z);
			particle.vortexAngle = std::atan2(offset.z, offset.y);
			particle.vortexHeightOffset = offset.x;
			break;

		case VortexAxis::kY:
			// Y軸まわり。XZ平面で回転し、Yが軸方向
			particle.vortexRadius = std::sqrt(offset.x * offset.x + offset.z * offset.z);
			particle.vortexAngle = std::atan2(offset.z, offset.x);
			particle.vortexHeightOffset = offset.y;
			break;

		case VortexAxis::kZ:
			// Z軸まわり。XY平面で回転し、Zが軸方向
			particle.vortexRadius = std::sqrt(offset.x * offset.x + offset.y * offset.y);
			particle.vortexAngle = std::atan2(offset.y, offset.x);
			particle.vortexHeightOffset = offset.z;
			break;
		}

		particle.vortexAngularSpeed = RandomRange(
			vortex.angularSpeedMin,
			vortex.angularSpeedMax
		);

		particle.vortexInwardSpeed = RandomRange(
			vortex.inwardSpeedMin,
			vortex.inwardSpeedMax
		);

		particle.vortexVerticalSpeed = RandomRange(
			vortex.verticalSpeedMin,
			vortex.verticalSpeedMax
		);
	}
}

void ParticleManager::InitializeParticleColor(Particle& particle, const ParticleBehavior& behavior) {
	const ParticleColorDesc& color = behavior.color;

	particle.colorChangeMode = color.mode;

	particle.startColor = RandomVector4Range(color.startColorMin, color.startColorMax);
	particle.endColor = RandomVector4Range(color.endColorMin, color.endColorMax);

	particle.randomColorMin = color.randomColorMin;
	particle.randomColorMax = color.randomColorMax;

	particle.randomColorChangeIntervalMin = color.randomColorChangeIntervalMin;
	particle.randomColorChangeIntervalMax = color.randomColorChangeIntervalMax;
	particle.randomColorLerpSpeed = color.randomColorLerpSpeed;

	particle.randomCurrentColor = particle.startColor;
	particle.randomTargetColor = RandomVector4Range(color.randomColorMin, color.randomColorMax);
	particle.randomColorChangeTimer = 0.0f;
	particle.randomColorChangeInterval = RandomRange(
		color.randomColorChangeIntervalMin,
		color.randomColorChangeIntervalMax
	);

	particle.color = particle.startColor;
}

void ParticleManager::Emit(
	const std::string& name,
	const Vector3& position,
	const Vector3& spawnSize,
	uint32_t count,
	const ParticleBehavior& behavior
) {
	auto it = particleGroups_.find(name);
	assert(it != particleGroups_.end());

	ParticleGroup& group = it->second;

	for (uint32_t i = 0; i < count; ++i) {
		if (group.particles.size() >= kMaxInstanceCount) {
			break;
		}

		Particle particle{};

		Vector3 halfSpawnSize = {
			spawnSize.x * 0.5f,
			spawnSize.y * 0.5f,
			spawnSize.z * 0.5f
		};

		Vector3 spawnOffset = RandomVector3Range(
			{ -halfSpawnSize.x, -halfSpawnSize.y, -halfSpawnSize.z },
			{ halfSpawnSize.x, halfSpawnSize.y, halfSpawnSize.z }
		);

		particle.transform.translate = {
			position.x + spawnOffset.x,
			position.y + spawnOffset.y,
			position.z + spawnOffset.z
		};

		particle.transform.rotate = RandomVector3Range(
			behavior.rotation.initialRotationMin,
			behavior.rotation.initialRotationMax
		);
		particle.rotationOffset = particle.transform.rotate;
		particle.enableRotationOverTime =
			behavior.rotation.enableRotationOverTime;
		particle.rotationSpeed = behavior.rotation.rotationSpeed;
		particle.alignToVelocity = behavior.rotation.alignToVelocity;
		particle.alignAxis = behavior.rotation.alignAxis;
		particle.startScale = RandomVector3Range(
			behavior.scale.startScaleMin,
			behavior.scale.startScaleMax
		);

		particle.endScale = RandomVector3Range(
			behavior.scale.endScaleMin,
			behavior.scale.endScaleMax
		);

		particle.enableScaleOverLife = behavior.scale.enableScaleOverLife;
		particle.transform.scale = particle.startScale;

		InitializeParticleLife(particle, behavior);
		InitializeParticleMotion(particle, behavior, position);
		InitializeParticleColor(particle, behavior);

		particle.billboardMode = behavior.render.billboardMode;

		group.particles.push_back(particle);
	}
}

void ParticleManager::UpdateParticleMotion(Particle& particle) {
	const Vector3 previousPosition = particle.transform.translate;

	if (particle.pointFieldEnabled && particle.movementMode == MovementMode::kLinear) {
		const Vector3 toCenter = {
			particle.pointFieldCenter.x - particle.transform.translate.x,
			particle.pointFieldCenter.y - particle.transform.translate.y,
			particle.pointFieldCenter.z - particle.transform.translate.z
		};
		const float distance = std::sqrt(
			toCenter.x * toCenter.x + toCenter.y * toCenter.y + toCenter.z * toCenter.z
		);
		if (distance > 1.0e-5f && (particle.pointFieldRadius <= 0.0f || distance <= particle.pointFieldRadius)) {
			const Vector3 radial = {
				toCenter.x / distance,
				toCenter.y / distance,
				toCenter.z / distance
			};
			float weight = 1.0f;
			if (particle.pointFieldRadius > 0.0f) {
				weight = std::clamp(1.0f - distance / particle.pointFieldRadius, 0.0f, 1.0f);
				weight = std::pow(weight, particle.pointFieldFalloff);
			} else if (particle.pointFieldFalloff > 0.0f) {
				weight = 1.0f / (1.0f + particle.pointFieldFalloff * distance * distance);
			}

			const Vector3 tangent = {
				particle.pointFieldOrbitAxis.y * radial.z - particle.pointFieldOrbitAxis.z * radial.y,
				particle.pointFieldOrbitAxis.z * radial.x - particle.pointFieldOrbitAxis.x * radial.z,
				particle.pointFieldOrbitAxis.x * radial.y - particle.pointFieldOrbitAxis.y * radial.x
			};
			const float radialStrength = particle.pointFieldAttraction - particle.pointFieldRepulsion;
			particle.velocity.x += (radial.x * radialStrength + tangent.x * particle.pointFieldOrbit) * weight * deltaTime_;
			particle.velocity.y += (radial.y * radialStrength + tangent.y * particle.pointFieldOrbit) * weight * deltaTime_;
			particle.velocity.z += (radial.z * radialStrength + tangent.z * particle.pointFieldOrbit) * weight * deltaTime_;
		}

		const float damping = std::exp(-particle.pointFieldDamping * deltaTime_);
		particle.velocity.x *= damping;
		particle.velocity.y *= damping;
		particle.velocity.z *= damping;
	}

	if (particle.movementMode == MovementMode::kLinear) {
		if (particle.linearAccelerationEnabled) {
			particle.velocity.x += particle.acceleration.x * deltaTime_;
			particle.velocity.y += particle.acceleration.y * deltaTime_;
			particle.velocity.z += particle.acceleration.z * deltaTime_;
		}

		particle.transform.translate.x += particle.velocity.x * deltaTime_;
		particle.transform.translate.y += particle.velocity.y * deltaTime_;
		particle.transform.translate.z += particle.velocity.z * deltaTime_;
	}
	else if (particle.movementMode == MovementMode::kVortexInward) {
		particle.vortexAngle += particle.vortexAngularSpeed * deltaTime_;
		particle.vortexRadius -= particle.vortexInwardSpeed * deltaTime_;
		particle.vortexHeightOffset += particle.vortexVerticalSpeed * deltaTime_;

		if (particle.vortexRadius < 0.0f) {
			particle.vortexRadius = 0.0f;
		}

		const float cosAngle = std::cos(particle.vortexAngle);
		const float sinAngle = std::sin(particle.vortexAngle);

		switch (particle.vortexAxis) {
		case VortexAxis::kX:
			// X軸まわり。YZ平面で回る
			particle.transform.translate.x =
				particle.vortexCenter.x + particle.vortexHeightOffset;

			particle.transform.translate.y =
				particle.vortexCenter.y + cosAngle * particle.vortexRadius;

			particle.transform.translate.z =
				particle.vortexCenter.z + sinAngle * particle.vortexRadius;
			break;

		case VortexAxis::kY:
			// Y軸まわり。XZ平面で回る
			particle.transform.translate.x =
				particle.vortexCenter.x + cosAngle * particle.vortexRadius;

			particle.transform.translate.y =
				particle.vortexCenter.y + particle.vortexHeightOffset;

			particle.transform.translate.z =
				particle.vortexCenter.z + sinAngle * particle.vortexRadius;
			break;

		case VortexAxis::kZ:
			// Z軸まわり。XY平面で回る
			particle.transform.translate.x =
				particle.vortexCenter.x + cosAngle * particle.vortexRadius;

			particle.transform.translate.y =
				particle.vortexCenter.y + sinAngle * particle.vortexRadius;

			particle.transform.translate.z =
				particle.vortexCenter.z + particle.vortexHeightOffset;
			break;
		}
	}

	const Vector3 windFieldOffset = {
		particle.transform.translate.x - particle.windFieldCenter.x,
		particle.transform.translate.y - particle.windFieldCenter.y,
		particle.transform.translate.z - particle.windFieldCenter.z
	};
	const bool isInsideWindField =
		std::abs(windFieldOffset.x) <= particle.windFieldHalfSize.x &&
		std::abs(windFieldOffset.y) <= particle.windFieldHalfSize.y &&
		std::abs(windFieldOffset.z) <= particle.windFieldHalfSize.z;

	if (particle.windEnabled) {
		float fieldWeight = isInsideWindField ? 1.0f : 0.0f;
		if (
			isInsideWindField &&
			particle.windBoundaryFalloffEnabled &&
			particle.windBoundaryFalloff > 0.0f
		) {
			const float distanceToBoundary = (std::min)({
				particle.windFieldHalfSize.x - std::abs(windFieldOffset.x),
				particle.windFieldHalfSize.y - std::abs(windFieldOffset.y),
				particle.windFieldHalfSize.z - std::abs(windFieldOffset.z)
			});
			const float t = std::clamp(
				distanceToBoundary / particle.windBoundaryFalloff,
				0.0f,
				1.0f
			);
			fieldWeight = t * t * (3.0f - 2.0f * t);
		}

		Vector3 targetWindVelocity = {
			particle.windDirection.x * particle.windStrength,
			particle.windDirection.y * particle.windStrength,
			particle.windDirection.z * particle.windStrength
		};

		if (isInsideWindField && particle.windTurbulenceStrength != 0.0f) {
			const float spatial =
				(
					particle.transform.translate.x +
					particle.transform.translate.y +
					particle.transform.translate.z
				) * particle.windTurbulenceScale;
			const float wave = std::sin(
				spatial +
				particle.currentTime * particle.windTurbulenceFrequency +
				particle.windPhase
			);

			targetWindVelocity.x +=
				particle.windTurbulenceAxis.x * wave * particle.windTurbulenceStrength;
			targetWindVelocity.y +=
				particle.windTurbulenceAxis.y * wave * particle.windTurbulenceStrength;
			targetWindVelocity.z +=
				particle.windTurbulenceAxis.z * wave * particle.windTurbulenceStrength;
		}

		targetWindVelocity.x *= fieldWeight;
		targetWindVelocity.y *= fieldWeight;
		targetWindVelocity.z *= fieldWeight;

		if (particle.windSmoothVelocity) {
			if (isInsideWindField || particle.windRecoverOutsideField) {
				const float response = isInsideWindField
					? particle.windAcceleration
					: particle.windDeceleration;
				const Vector3 difference = {
					targetWindVelocity.x - particle.windVelocity.x,
					targetWindVelocity.y - particle.windVelocity.y,
					targetWindVelocity.z - particle.windVelocity.z
				};
				const float distance = std::sqrt(
					difference.x * difference.x +
					difference.y * difference.y +
					difference.z * difference.z
				);
				const float maxChange = response * deltaTime_;
				if (distance <= maxChange || distance <= 0.000001f) {
					particle.windVelocity = targetWindVelocity;
				} else {
					const float scale = maxChange / distance;
					particle.windVelocity.x += difference.x * scale;
					particle.windVelocity.y += difference.y * scale;
					particle.windVelocity.z += difference.z * scale;
				}
			}
		} else {
			particle.windVelocity = targetWindVelocity;
		}

		particle.transform.translate.x += particle.windVelocity.x * deltaTime_;
		particle.transform.translate.y += particle.windVelocity.y * deltaTime_;
		particle.transform.translate.z += particle.windVelocity.z * deltaTime_;
	}

	if (particle.swayAmplitude != 0.0f) {
		particle.swayTime += deltaTime_;

		float swayValue =
			std::sin(particle.swayTime * particle.swayFrequency + particle.swayPhase) *
			particle.swayAmplitude;

		particle.transform.translate.x += particle.swayAxis.x * swayValue * deltaTime_;
		particle.transform.translate.y += particle.swayAxis.y * swayValue * deltaTime_;
		particle.transform.translate.z += particle.swayAxis.z * swayValue * deltaTime_;
	}

	if (particle.alignToVelocity) {
		const Vector3 moveDelta = {
			particle.transform.translate.x - previousPosition.x,
			particle.transform.translate.y - previousPosition.y,
			particle.transform.translate.z - previousPosition.z
		};
		const Vector3 direction = NormalizeOrZero(moveDelta);

		if (
			direction.x != 0.0f ||
			direction.y != 0.0f ||
			direction.z != 0.0f
			) {
			particle.velocityAlignmentDirection = direction;
			particle.hasVelocityAlignmentDirection = true;
		}
	}
}

void ParticleManager::UpdateParticleColor(Particle& particle) {
	const float lifeRatio = GetAnimationRatio(particle);

	Vector4 baseColor = particle.startColor;

	switch (particle.colorChangeMode) {
	case ColorChangeMode::kConstant:
		baseColor = particle.startColor;
		break;

	case ColorChangeMode::kOverLife:
		baseColor = LerpColor(particle.startColor, particle.endColor, lifeRatio);
		break;

	case ColorChangeMode::kRandomLoop:
		particle.randomColorChangeTimer += deltaTime_;

		if (particle.randomColorChangeTimer >= particle.randomColorChangeInterval) {
			particle.randomColorChangeTimer = 0.0f;

			particle.randomTargetColor = RandomVector4Range(
				particle.randomColorMin,
				particle.randomColorMax
			);

			particle.randomColorChangeInterval = RandomRange(
				particle.randomColorChangeIntervalMin,
				particle.randomColorChangeIntervalMax
			);
		}

		particle.randomCurrentColor = LerpColor(
			particle.randomCurrentColor,
			particle.randomTargetColor,
			std::clamp(particle.randomColorLerpSpeed * deltaTime_, 0.0f, 1.0f)
		);

		baseColor = particle.randomCurrentColor;
		break;
	}

	if (particle.enableLifeFade) {
		float fadeRatio =
			(lifeRatio - particle.fadeOutStartRatio) /
			(1.0f - particle.fadeOutStartRatio);

		fadeRatio = std::clamp(fadeRatio, 0.0f, 1.0f);

		baseColor.w *= (1.0f - fadeRatio);
	}

	particle.color = baseColor;
}

bool ParticleManager::IsDeadParticle(const Particle& particle) const {
	if (particle.isLooping) {
		return false;
	}
	return particle.currentTime >= particle.lifeTime;
}

float ParticleManager::GetAnimationRatio(const Particle& particle) const {
	if (!particle.isLooping) {
		return std::clamp(particle.currentTime / particle.lifeTime, 0.0f, 1.0f);
	}

	float ratio = std::fmod(particle.currentTime, particle.loopDuration) /
		particle.loopDuration;
	if (particle.loopPingPong) {
		ratio = ratio < 0.5f ? ratio * 2.0f : (1.0f - ratio) * 2.0f;
	}
	return ratio;
}

Vector3 ParticleManager::LerpVector3(const Vector3& start, const Vector3& end, float t) {
	t = std::clamp(t, 0.0f, 1.0f);

	return {
		start.x + (end.x - start.x) * t,
		start.y + (end.y - start.y) * t,
		start.z + (end.z - start.z) * t
	};
}

void ParticleManager::UpdateParticleScale(Particle& particle) {
	if (!particle.enableScaleOverLife) {
		return;
	}

	const float lifeRatio = GetAnimationRatio(particle);

	particle.transform.scale = LerpVector3(
		particle.startScale,
		particle.endScale,
		lifeRatio
	);
}

void ParticleManager::UpdateParticleRotation(Particle& particle) {
	if (!particle.enableRotationOverTime && !particle.alignToVelocity) {
		return;
	}

	if (particle.enableRotationOverTime) {
		Vector3& rotateTarget =
			particle.alignToVelocity ? particle.rotationOffset : particle.transform.rotate;

		rotateTarget.x = WrapRadian(rotateTarget.x + particle.rotationSpeed.x * deltaTime_);
		rotateTarget.y = WrapRadian(rotateTarget.y + particle.rotationSpeed.y * deltaTime_);
		rotateTarget.z = WrapRadian(rotateTarget.z + particle.rotationSpeed.z * deltaTime_);
	}

	if (particle.alignToVelocity && particle.hasVelocityAlignmentDirection) {
		particle.transform.rotate = AddVector3(
			CalculateVelocityAlignedRotation(
				particle.velocityAlignmentDirection,
				particle.alignAxis
			),
			particle.rotationOffset
		);
	}
}

Vector3 ParticleManager::ResolveParticleWorldPosition(
	const ParticleGroup& group,
	const Vector3& localPosition
) const {
	if (!group.parentTransformEnabled) {
		return localPosition;
	}
	const float cosine = std::cos(group.parentYaw);
	const float sine = std::sin(group.parentYaw);
	return {
		group.parentTranslation.x + localPosition.x * cosine + localPosition.z * sine,
		group.parentTranslation.y + localPosition.y,
		group.parentTranslation.z - localPosition.x * sine + localPosition.z * cosine
	};
}

void ParticleManager::Update() {
	using Clock = std::chrono::steady_clock;
	const auto updateStart = Clock::now();
	auto elapsedMs = [](Clock::time_point start, Clock::time_point end) {
		return std::chrono::duration<float, std::milli>(end - start).count();
	};

	runtimeStats_.cpuParticleUpdateMs = 0.0f;
	runtimeStats_.gpuParticleCpuUpdateMs = 0.0f;
	runtimeStats_.totalParticleUpdateMs = 0.0f;
	runtimeStats_.cpuParticleActiveCount = 0;
	runtimeStats_.cpuParticleInstanceCount = 0;
	runtimeStats_.gpuParticleInstanceCount =
		static_cast<uint32_t>(gpuParticles_.size());
	runtimeStats_.gpuParticleEnabled = gpuParticleEnabled_;
	runtimeStats_.gpuParticle = {};
	runtimeStats_.gpuParticle.maxParticles = 0;
	for (const auto& [key, particle] : gpuParticles_) {
		(void)key;
		const GpuParticle::RuntimeInfo info = particle->GetRuntimeInfo();
		runtimeStats_.gpuParticle.initialized |= info.initialized;
		runtimeStats_.gpuParticle.autoEmit |= info.autoEmit;
		runtimeStats_.gpuParticle.maxParticles += info.maxParticles;
		runtimeStats_.gpuParticle.emitCount += info.emitCount;
		runtimeStats_.gpuParticle.emitFlags |= info.emitFlags;
		if (runtimeStats_.gpuParticle.frequency == 0.0f) {
			runtimeStats_.gpuParticle.frequency = info.frequency;
			runtimeStats_.gpuParticle.frequencyTime = info.frequencyTime;
		}
	}

	if (!camera_) {
		return;
	}

	if (gpuParticleEnabled_) {
		const auto gpuUpdateStart = Clock::now();
		for (auto& [key, particle] : gpuParticles_) {
			(void)key;
			particle->Update();
		}
		runtimeStats_.gpuParticleCpuUpdateMs =
			elapsedMs(gpuUpdateStart, Clock::now());
	}

	const auto cpuUpdateStart = Clock::now();
	uint32_t cpuParticleActiveCount = 0;
	for (auto& [name, group] : particleGroups_) {
		group.uvOffset.x += group.render.uvScrollSpeed.x * deltaTime_;
		group.uvOffset.y += group.render.uvScrollSpeed.y * deltaTime_;
		group.materialData->uvTransform = MakeIdentity4x4();
		group.materialData->uvTransform.m[3][0] = group.uvOffset.x;
		group.materialData->uvTransform.m[3][1] = group.uvOffset.y;

		for (auto& particle : group.particles) {
			particle.currentTime += deltaTime_;

			UpdateParticleMotion(particle);
			UpdateParticleRotation(particle);
			UpdateParticleScale(particle);
			UpdateParticleColor(particle);
		}

		group.particles.erase(
			std::remove_if(
				group.particles.begin(),
				group.particles.end(),
				[this](const Particle& particle) {
					return IsDeadParticle(particle);
				}
			),
			group.particles.end()
		);

		group.instanceCount = 0;
		cpuParticleActiveCount += static_cast<uint32_t>(
			(std::min)(group.particles.size(), static_cast<size_t>(kMaxInstanceCount))
		);
	}
	const uint32_t cpuParticleInstanceCount =
		RebuildCpuParticleInstances(camera_, WaterDrawFilter{});
	runtimeStats_.cpuParticleUpdateMs =
		elapsedMs(cpuUpdateStart, Clock::now());
	runtimeStats_.cpuParticleActiveCount = cpuParticleActiveCount;
	runtimeStats_.cpuParticleInstanceCount = cpuParticleInstanceCount;
	runtimeStats_.gpuParticle = {};
	runtimeStats_.gpuParticle.maxParticles = 0;
	for (const auto& [key, particle] : gpuParticles_) {
		(void)key;
		const GpuParticle::RuntimeInfo info = particle->GetRuntimeInfo();
		runtimeStats_.gpuParticle.initialized |= info.initialized;
		runtimeStats_.gpuParticle.autoEmit |= info.autoEmit;
		runtimeStats_.gpuParticle.maxParticles += info.maxParticles;
		runtimeStats_.gpuParticle.emitCount += info.emitCount;
		runtimeStats_.gpuParticle.emitFlags |= info.emitFlags;
		if (runtimeStats_.gpuParticle.frequency == 0.0f) {
			runtimeStats_.gpuParticle.frequency = info.frequency;
			runtimeStats_.gpuParticle.frequencyTime = info.frequencyTime;
		}
	}
	runtimeStats_.totalParticleUpdateMs = elapsedMs(updateStart, Clock::now());
}

void ParticleManager::RefreshCpuParticleInstancesForCamera(Camera* camera) {
	RefreshCpuParticleInstancesForCamera(camera, WaterDrawFilter{});
}

void ParticleManager::RefreshCpuParticleInstancesForCamera(
	Camera* camera,
	const WaterDrawFilter& filter
) {
	if (!camera) {
		return;
	}
	camera_ = camera;
	runtimeStats_.cpuParticleInstanceCount =
		RebuildCpuParticleInstances(camera, filter);
}

uint32_t ParticleManager::RebuildCpuParticleInstances(
	Camera* camera,
	const WaterDrawFilter& filter
) {
	if (!camera) {
		return 0;
	}

	uint32_t cpuParticleInstanceCount = 0;
	for (auto& [name, group] : particleGroups_) {
		(void)name;
		group.instanceCount = 0;
		for (const auto& particle : group.particles) {
			if (group.instanceCount >= kMaxInstanceCount) {
				break;
			}
			const Vector3 worldPosition = ResolveParticleWorldPosition(
				group,
				particle.transform.translate
			);
			if (!ShouldIncludeParticleInWaterPass(worldPosition, filter)) {
				continue;
			}
			Vector3 worldRotation = particle.transform.rotate;
			if (group.parentTransformEnabled &&
				particle.billboardMode != BillboardMode::kBillboard) {
				worldRotation.y = WrapRadian(worldRotation.y + group.parentYaw);
			}

			Matrix4x4 worldMatrix{};
			if (particle.billboardMode == BillboardMode::kBillboard) {
				worldMatrix = MakeBillboardMatrix(
					camera->GetWorldMatrix(),
					particle.transform.scale,
					worldPosition,
					particle.transform.rotate.z
				);
			} else {
				worldMatrix = MakeAffineMatrix(
					particle.transform.scale,
					worldRotation,
					worldPosition
				);
			}
			Matrix4x4 wvp = Multiply(
				worldMatrix,
				camera->GetViewProjectionMatrix()
			);

			group.instancingData[group.instanceCount].World = worldMatrix;
			group.instancingData[group.instanceCount].WVP = wvp;
			group.instancingData[group.instanceCount].color = particle.color;

			++group.instanceCount;
		}
		cpuParticleInstanceCount += group.instanceCount;
	}
	return cpuParticleInstanceCount;
}

void ParticleManager::Draw(bool drawGpuParticles) {
	auto* commandList = particleCommon_->GetDxCommon()->GetCommandList();

	for (auto& [name, group] : particleGroups_) {
		if (group.instanceCount == 0) {
			continue;
		}

		// ParticleGroupごとにBlendModeを切り替える
		particleCommon_->SetRenderState(
			group.blendMode,
			group.render.cullMode,
			group.render.depthTest,
			group.render.depthWrite
		);
		particleCommon_->SetCommonRenderState();
		commandList->IASetVertexBuffers(0, 1, &group.vertexBufferView);

		// b0 Material
		commandList->SetGraphicsRootConstantBufferView(
			0,
			group.materialResource->GetGPUVirtualAddress()
		);

		// VS t0 StructuredBuffer<TransformationMatrix>
		srvManager_->SetGraphicsRootDescriptorTable(1, group.instanceSrvIndex);

		// PS t0 Texture2D
		srvManager_->SetGraphicsRootDescriptorTable(2, group.textureSrvIndex);

		// b1 DirectionalLight
		commandList->SetGraphicsRootConstantBufferView(
			3,
			directionalLightResource_->GetGPUVirtualAddress()
		);

		commandList->DrawInstanced(group.vertexCount, group.instanceCount, 0, 0);
	}

	if (gpuParticleEnabled_ && drawGpuParticles) {
		for (auto& [key, particle] : gpuParticles_) {
			(void)key;
			particle->Draw(camera_);
		}
	}
}
