// 役割: CPUおよびGPUパーティクルの更新、Emitter、描画を統合管理する。
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <wrl.h>
#include <d3d12.h>

#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Vector2.h"
#include "../math/Transform.h"
#include "../math/Matrix4x4.h"

#include "ParticleCommon.h"
#include "GpuParticle.h"

class SrvManager;
class Camera;
class ParticleEmitter;
struct ParticleEffectDesc;

class ParticleManager {
private:
	static ParticleManager* instance_;

	ParticleManager() = default;
	~ParticleManager();
	ParticleManager(const ParticleManager&) = delete;
	ParticleManager& operator=(const ParticleManager&) = delete;

public:
	static ParticleManager* GetInstance();
	static void DeleteInstance();

public:
	struct TransformationMatrix {
		Matrix4x4 WVP;
		Matrix4x4 World;
		Vector4 color;
	};

	struct Material {
		Vector4 color;
		int32_t enableLighting;
		float alphaCutoff;
		int32_t flipU;
		int32_t flipV;
		Matrix4x4 uvTransform;
		float emissiveIntensity;
		float padding[3];
	};

	struct DirectionalLight {
		Vector4 color;
		Vector3 direction;
		float intensity;
	};

	enum class ColorChangeMode {
		kConstant,     // 最初の色のまま
		kOverLife,    // 寿命に応じて startColor から endColor へ変化
		kRandomLoop,  // 生存中ずっとランダム色へ変化し続ける
	};

	enum class MovementMode {
		kLinear,        // 通常の速度、加速度移動
		kVortexInward,  // 中心を回りながら吸い込まれる
	};

	enum class VortexAxis {
		kX, // X軸まわり。YZ平面で回る
		kY, // Y軸まわり。XZ平面で回る
		kZ, // Z軸まわり。XY平面で回る
	};

	enum class ParticleAlignmentAxis {
		kX,
		kY,
		kZ,
	};

	struct ParticleLifeDesc {
		bool isLooping = false;
		float loopDuration = 1.0f;
		bool loopPingPong = true;

		float lifeTimeMin = 1.0f;
		float lifeTimeMax = 1.0f;

		bool enableLifeFade = true;
		float fadeOutStartRatio = 0.7f;
	};

	struct ParticleScaleDesc {
		Vector3 startScaleMin = { 1.0f, 1.0f, 1.0f };
		Vector3 startScaleMax = { 1.0f, 1.0f, 1.0f };

		bool enableScaleOverLife = false;

		Vector3 endScaleMin = { 1.0f, 1.0f, 1.0f };
		Vector3 endScaleMax = { 1.0f, 1.0f, 1.0f };
	};

	struct ParticleRotationDesc {
		Vector3 initialRotationMin = { 0.0f, 0.0f, 0.0f };
		Vector3 initialRotationMax = { 0.0f, 0.0f, 0.0f };
		bool enableRotationOverTime = false;
		Vector3 rotationSpeed = { 0.0f, 0.0f, 0.0f };
		bool alignToVelocity = false;
		ParticleAlignmentAxis alignAxis = ParticleAlignmentAxis::kY;
	};

	struct ParticleLinearMotionDesc {
		Vector3 baseVelocity = { 0.0f, -1.8f, 0.0f };
		Vector3 velocityRandomRange = { 0.0f, 0.0f, 0.01f };

		bool enableAcceleration = true;
		Vector3 baseAcceleration = { 0.0f, -0.001f, 0.0f };
		Vector3 accelerationRandomRange = { 0.0f, 0.0f, 0.0f };
	};

	struct ParticleSwayDesc {
		float amplitude = 0.0f;
		float frequency = 0.0f;
	};

	struct ParticleVortexDesc {
		// trueならEmitter位置からのオフセット、falseならワールド座標として扱う
		bool useEmitterOffset = true;

		// 渦の中心。useEmitterOffsetがtrueならEmitterからのオフセット
		Vector3 center = { 0.0f, 0.0f, 0.0f };

		// 回転軸
		// kX: YZ平面で回る
		// kY: XZ平面で回る
		// kZ: XY平面で回る
		VortexAxis axis = VortexAxis::kY;

		// 角速度。マイナス値を使うと逆回転も可能
		float angularSpeedMin = 4.0f;
		float angularSpeedMax = 8.0f;

		// 中心へ近づく速さ
		float inwardSpeedMin = 0.8f;
		float inwardSpeedMax = 1.8f;

		// 回転軸方向への移動速度
		// axis が kY のときは Y方向、kX のときは X方向、kZ のときは Z方向
		float verticalSpeedMin = -0.1f;
		float verticalSpeedMax = 0.1f;
	};

	// 軸状のVortexとは別に、一点を基準として力を加えるフィールド。
	struct ParticlePointFieldDesc {
		bool enabled = false;
		bool useEmitterOffset = true;
		Vector3 center = { 0.0f, 0.0f, 0.0f };
		float radius = 0.0f; // 0なら距離制限なし
		float attractionStrength = 0.0f;
		float repulsionStrength = 0.0f;
		float orbitStrength = 0.0f;
		Vector3 orbitAxis = { 0.0f, 1.0f, 0.0f };
		float falloff = 1.0f;
		float damping = 0.0f;
	};

	struct ParticleWindFieldDesc {
		bool enabled = false;
		bool useEmitterOffset = true;
		Vector3 center = { 0.0f, 0.0f, 0.0f };
		Vector3 size = { 20.0f, 20.0f, 20.0f };
		Vector3 direction = { 1.0f, 0.0f, 0.0f };
		float strength = 0.0f;
		bool smoothVelocity = true;
		float acceleration = 8.0f;
		bool recoverOutsideField = true;
		float deceleration = 5.0f;
		bool enableBoundaryFalloff = true;
		float boundaryFalloff = 2.0f;
		float turbulenceStrength = 0.0f;
		float turbulenceFrequency = 1.0f;
		float turbulenceScale = 0.1f;
	};

	struct ParticleMotionDesc {
		MovementMode mode = MovementMode::kLinear;

		ParticleLinearMotionDesc linear;
		ParticleSwayDesc sway;
		ParticleVortexDesc vortex;
		ParticlePointFieldDesc pointField;
		ParticleWindFieldDesc wind;
	};

	struct ParticleColorDesc {
		ColorChangeMode mode = ColorChangeMode::kConstant;

		Vector4 startColorMin = { 1.0f, 1.0f, 1.0f, 1.0f };
		Vector4 startColorMax = { 1.0f, 1.0f, 1.0f, 1.0f };

		Vector4 endColorMin = { 1.0f, 1.0f, 1.0f, 1.0f };
		Vector4 endColorMax = { 1.0f, 1.0f, 1.0f, 1.0f };

		// RandomLoop用
		Vector4 randomColorMin = { 0.0f, 0.0f, 0.0f, 1.0f };
		Vector4 randomColorMax = { 1.0f, 1.0f, 1.0f, 1.0f };

		float randomColorChangeIntervalMin = 0.15f;
		float randomColorChangeIntervalMax = 0.35f;
		float randomColorLerpSpeed = 6.0f;
	};

	enum class BillboardMode {
		kNone,
		kBillboard,
	};

	enum class PrimitiveType {
		kPlane,
		kRing,
		kCylinder,
	};

	enum class RingUvMode {
		kHorizontal,
		kVertical,
	};

	struct RingPrimitiveDesc {
		uint32_t divisions = 32;
		float outerRadius = 1.0f;
		float innerRadius = 0.2f;
		float startAngle = 0.0f;
		float endAngle = 6.2831853f;
		Vector4 outerColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		Vector4 innerColor = { 1.0f, 1.0f, 1.0f, 0.0f };
		RingUvMode uvMode = RingUvMode::kHorizontal;
	};

	struct CylinderPrimitiveDesc {
		uint32_t divisions = 32;
		float topRadius = 1.0f;
		float bottomRadius = 1.0f;
		float height = 3.0f;
		float startAngle = 0.0f;
		float endAngle = 6.2831853f;
		Vector4 topColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		Vector4 bottomColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		RingUvMode uvMode = RingUvMode::kHorizontal;
	};

	struct ParticleRenderDesc {
		BillboardMode billboardMode = BillboardMode::kBillboard;
		PrimitiveType primitiveType = PrimitiveType::kPlane;
		RingPrimitiveDesc ring;
		CylinderPrimitiveDesc cylinder;
		Vector2 uvScrollSpeed = { 0.0f, 0.0f };
		bool flipU = false;
		bool flipV = false;
		float alphaCutoff = 0.0f;
		float emissiveIntensity = 1.0f;
		ParticleCommon::CullMode cullMode = ParticleCommon::CullMode::kNone;
		bool depthTest = true;
		bool depthWrite = false;
	};

	struct ParticleBehavior {
		ParticleLifeDesc life;
		ParticleScaleDesc scale;
		ParticleRotationDesc rotation;
		ParticleMotionDesc motion;
		ParticleColorDesc color;
		ParticleRenderDesc render;
	};

	struct LightningEmitterDesc {
		bool enabled = false;
		Vector3 startOffset = { 0.0f, 6.0f, 0.0f };
		Vector3 endOffset = { 0.0f, 0.0f, 0.0f };
		Vector3 randomRange = { 4.0f, 0.0f, 4.0f };
		Vector4 coreColor = { 8.0f, 10.0f, 16.0f, 1.0f };
		Vector4 branchColor = { 2.0f, 4.0f, 12.0f, 1.0f };
		float jitter = 0.45f;
		float branchLength = 0.8f;
		float branchProbability = 0.35f;
		float thickness = 0.04f;
		float duration = 0.12f;
		uint32_t segmentCount = 16;
		bool flashExposure = true;
		float flashExposureValue = 3.5f;
		float flashReturnSpeed = 6.0f;
	};

	struct LightningEvent {
		LightningEmitterDesc desc;
		Vector3 start = { 0.0f, 0.0f, 0.0f };
		Vector3 end = { 0.0f, 0.0f, 0.0f };
		uint32_t seed = 0;
	};

	struct ExposureFlashEvent {
		float exposure = 1.0f;
		float returnSpeed = 6.0f;
	};

	struct Particle {
		Transform transform;

		Vector3 velocity;
		Vector3 acceleration;

		Vector3 startScale = { 1.0f, 1.0f, 1.0f };
		Vector3 endScale = { 1.0f, 1.0f, 1.0f };
		bool enableScaleOverLife = false;

		float currentTime = 0.0f;
		float lifeTime = 1.0f;
		bool isLooping = false;
		float loopDuration = 1.0f;
		bool loopPingPong = true;

		Vector4 color;
		Vector4 startColor;
		Vector4 endColor;

		ColorChangeMode colorChangeMode = ColorChangeMode::kConstant;

		Vector4 randomCurrentColor;
		Vector4 randomTargetColor;
		Vector4 randomColorMin;
		Vector4 randomColorMax;

		float randomColorChangeTimer = 0.0f;
		float randomColorChangeInterval = 0.0f;
		float randomColorChangeIntervalMin = 0.0f;
		float randomColorChangeIntervalMax = 0.0f;
		float randomColorLerpSpeed = 0.0f;

		bool enableLifeFade = true;
		float fadeOutStartRatio = 0.7f;
		bool enableRotationOverTime = false;
		Vector3 rotationSpeed = { 0.0f, 0.0f, 0.0f };
		Vector3 rotationOffset = { 0.0f, 0.0f, 0.0f };
		bool alignToVelocity = false;
		ParticleAlignmentAxis alignAxis = ParticleAlignmentAxis::kY;
		Vector3 velocityAlignmentDirection = { 0.0f, -1.0f, 0.0f };
		bool hasVelocityAlignmentDirection = false;

		MovementMode movementMode = MovementMode::kLinear;
		bool linearAccelerationEnabled = true;

		float swayTime = 0.0f;
		float swayPhase = 0.0f;
		Vector3 swayAxis = { 0.0f, 0.0f, 0.0f };
		float swayAmplitude = 0.0f;
		float swayFrequency = 0.0f;

		Vector3 vortexCenter = { 0.0f, 0.0f, 0.0f };
		VortexAxis vortexAxis = VortexAxis::kY;

		float vortexAngle = 0.0f;
		float vortexRadius = 0.0f;
		float vortexAngularSpeed = 0.0f;
		float vortexInwardSpeed = 0.0f;
		float vortexVerticalSpeed = 0.0f;
		float vortexHeightOffset = 0.0f;

		bool pointFieldEnabled = false;
		Vector3 pointFieldCenter = { 0.0f, 0.0f, 0.0f };
		float pointFieldRadius = 0.0f;
		float pointFieldAttraction = 0.0f;
		float pointFieldRepulsion = 0.0f;
		float pointFieldOrbit = 0.0f;
		Vector3 pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
		float pointFieldFalloff = 1.0f;
		float pointFieldDamping = 0.0f;

		bool windEnabled = false;
		Vector3 windFieldCenter = { 0.0f, 0.0f, 0.0f };
		Vector3 windFieldHalfSize = { 10.0f, 10.0f, 10.0f };
		Vector3 windDirection = { 1.0f, 0.0f, 0.0f };
		float windStrength = 0.0f;
		Vector3 windVelocity = { 0.0f, 0.0f, 0.0f };
		bool windSmoothVelocity = true;
		float windAcceleration = 8.0f;
		bool windRecoverOutsideField = true;
		float windDeceleration = 5.0f;
		bool windBoundaryFalloffEnabled = true;
		float windBoundaryFalloff = 2.0f;
		float windTurbulenceStrength = 0.0f;
		float windTurbulenceFrequency = 1.0f;
		float windTurbulenceScale = 0.1f;
		float windPhase = 0.0f;
		Vector3 windTurbulenceAxis = { 0.0f, 0.0f, 1.0f };

		BillboardMode billboardMode = BillboardMode::kBillboard;
	};

private:
	struct ParticleGroup {
		std::string textureFilePath;
		uint32_t textureSrvIndex = 0;

		std::vector<Particle> particles;

		Microsoft::WRL::ComPtr<ID3D12Resource> materialResource;
		Material* materialData = nullptr;

		Microsoft::WRL::ComPtr<ID3D12Resource> instancingResource;
		TransformationMatrix* instancingData = nullptr;

		uint32_t instanceSrvIndex = 0;
		uint32_t instanceCount = 0;

		ParticleCommon::BlendMode blendMode = ParticleCommon::BlendMode::kBlendModeAdd;
		ParticleRenderDesc render;
		bool parentTransformEnabled = false;
		Vector3 parentTranslation{};
		float parentYaw = 0.0f;
		Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource;
		D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
		uint32_t vertexCount = 0;
		Vector2 uvOffset{};
	};

public:
	static const uint32_t kMaxInstanceCount = 20480;

public:
	enum class WaterDrawMode {
		kAll,
		kRefracted,
		kForeground,
	};

	struct WaterDrawFilter {
		WaterDrawMode mode = WaterDrawMode::kAll;
		Vector3 cameraPosition{};
		Vector3 waterCenter{};
		Vector3 waterHalfSize{};
	};

	void Initialize(ParticleCommon* particleCommon, SrvManager* srvManager);
	void Reset();
	void Update();
	void RefreshCpuParticleInstancesForCamera(Camera* camera);
	void RefreshCpuParticleInstancesForCamera(
		Camera* camera,
		const WaterDrawFilter& filter
	);
	void Draw(bool drawGpuParticles = true);

	void CreateParticleGroup(const std::string& name, const std::string& textureFilePath);

	bool HasParticleGroup(const std::string& name) const;
	void ClearParticleGroup(const std::string& name);
	void ClearActiveParticles();
	void CreateParticleGroupIfNeeded(
		const std::string& name,
		const std::string& textureFilePath
	);
	bool SetParticleGroupTexture(
		const std::string& name,
		const std::string& textureFilePath
	);
	bool SetParticleGroupParentTransform(
		const std::string& name,
		const Vector3& translation,
		float yaw
	);
	void ClearParticleGroupParentTransform(const std::string& name);

	void Emit(
		const std::string& name,
		const Vector3& position,
		const Vector3& spawnSize,
		uint32_t count,
		const ParticleBehavior& behavior
	);

	struct RuntimeStats {
		float cpuParticleUpdateMs = 0.0f;
		float gpuParticleCpuUpdateMs = 0.0f;
		float totalParticleUpdateMs = 0.0f;
		uint32_t cpuParticleActiveCount = 0;
		uint32_t cpuParticleInstanceCount = 0;
		uint32_t gpuParticleInstanceCount = 0;
		bool gpuParticleEnabled = false;
		GpuParticle::RuntimeInfo gpuParticle;
	};

	void SetCamera(Camera* camera) { camera_ = camera; }

	void SetGroupBlendMode(const std::string& name, ParticleCommon::BlendMode blendMode);
	void SetGroupRenderDesc(const std::string& name, const ParticleRenderDesc& render);
	void SetGpuParticleEnabled(bool enabled);
	bool IsGpuParticleEnabled() const { return gpuParticleEnabled_; }
	void ApplyGpuParticleEffect(const ParticleEffectDesc& effect);
	void ClearGpuParticles();
	void ClearGpuParticlePreview();
	void RequestGpuParticleReset();
	void EmitGpuParticleOnce();
	const RuntimeStats& GetRuntimeStats() const { return runtimeStats_; }

	bool LoadSceneParticleLayout(
		const std::string& filePath = "resources/particles/scene_particles.json",
		bool resumeGpuSync = false
	);
	bool SaveSceneParticleLayout(const std::string& filePath = "resources/particles/scene_particles.json") const;
	void UpdateSceneParticles(const std::string& sceneName);
	void ReleaseSceneParticles(const std::string& sceneName);
	void CycleSceneParticleAssets(const std::string& sceneName);
	void DrawSceneParticleImGui(
		const std::string& currentSceneName,
		const char* windowTitle = "Scene Particles"
	);
	void RefreshPlacementAssetsForEffect(const std::string& effectFilePath);

	void QueueLightning(
		const LightningEmitterDesc& desc,
		const Vector3& emitterPosition
	);
	bool ConsumeLightningEvent(LightningEvent& outEvent);
	bool ConsumeExposureFlashEvent(ExposureFlashEvent& outEvent);

private:
	struct SceneParticlePlacement {
		std::string label = "Particle";
		std::string effectFilePath = "resources/particles/core_burst.json";
		bool enabled = true;
		bool emitterActive = true;
		Vector3 translate = { 0.0f, 0.0f, 0.0f };
		Vector3 spawnSize = { 1.0f, 1.0f, 1.0f };
		uint32_t count = 1;
		float frequency = 0.1f;
		ParticleEffectDesc* effect = nullptr;
		ParticleEmitter* emitter = nullptr;
	};
	struct ParticlePlacementAsset {
		std::string name;
		std::vector<SceneParticlePlacement> placements;
	};
	struct SceneParticleAssetInstance {
		std::string label = "Particle Asset";
		std::string assetName;
		bool enabled = true;
		Vector3 translate = { 0.0f, 0.0f, 0.0f };
		std::vector<SceneParticlePlacement> runtimePlacements;
	};

	void CreateDirectionalLightResource();
	void CreateGroupVertexResource(ParticleGroup& group);
	void RebuildSceneParticleEmitter(
		SceneParticlePlacement& placement,
		bool clearParticles,
		bool useEffectEmitterSettings = false
	);
	void ApplySceneParticleEmitterSettings(SceneParticlePlacement& placement);
	void ReleasePlacements(std::vector<SceneParticlePlacement>& placements);
	void RebuildParticleAssetInstance(SceneParticleAssetInstance& instance);
	void RebuildInstancesUsingAsset(const std::string& assetName);
	bool LoadPlacementEmitterSettings(SceneParticlePlacement& placement);
	GpuParticle* GetOrCreateGpuParticle(const std::string& key);
	GpuParticle* FindGpuParticle(const std::string& key);
	void ApplyGpuParticleEffectToKey(
		const std::string& key,
		const ParticleEffectDesc& effect
	);
	void EraseGpuParticle(const std::string& key);
	std::string MakeSceneGpuParticleKey(
		const std::string& sceneName,
		size_t instanceIndex,
		size_t placementIndex,
		const SceneParticleAssetInstance& instance,
		const SceneParticlePlacement& placement
	) const;
	bool SceneParticlePlacementUsesGpu(const SceneParticlePlacement& placement) const;
	bool SceneParticleAssetContainsGpuPlacement(const std::string& assetName) const;
	bool SceneParticleAssetHasEnabledInstance(const std::string& assetName) const;
	bool SceneParticleInstanceContainsGpuPlacement(
		const SceneParticleAssetInstance& instance
	) const;
	bool SceneParticleSceneContainsEnabledGpuPlacement(
		const std::string& sceneName
	) const;
	void ApplySceneParticlePlacement(
		SceneParticlePlacement& placement,
		const std::string& syncKey = {}
	);
	void SyncSceneGpuParticle(const std::string& sceneName);
	void ApplySceneGpuPlacement(
		SceneParticlePlacement& placement,
		const std::string& syncKey
	);
	void MarkSceneGpuParticleSyncDirty(bool resumeGpuSync);

	float RandomRange(float min, float max);
	Vector3 RandomVector3Range(const Vector3& min, const Vector3& max);
	Vector4 RandomVector4Range(const Vector4& min, const Vector4& max);

	Vector4 LerpColor(const Vector4& start, const Vector4& end, float t);

	void InitializeParticleLife(Particle& particle, const ParticleBehavior& behavior);
	void InitializeParticleMotion(
		Particle& particle,
		const ParticleBehavior& behavior,
		const Vector3& emitterPosition
	);
	void InitializeParticleColor(Particle& particle, const ParticleBehavior& behavior);

	void UpdateParticleMotion(Particle& particle);
	void UpdateParticleColor(Particle& particle);
	void UpdateParticleRotation(Particle& particle);

	bool IsDeadParticle(const Particle& particle) const;
	float GetAnimationRatio(const Particle& particle) const;

	Vector3 LerpVector3(const Vector3& start, const Vector3& end, float t);
	void UpdateParticleScale(Particle& particle);
	Vector3 ResolveParticleWorldPosition(
		const ParticleGroup& group,
		const Vector3& localPosition
	) const;
	uint32_t RebuildCpuParticleInstances(
		Camera* camera,
		const WaterDrawFilter& filter
	);

private:
	ParticleCommon* particleCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	Camera* camera_ = nullptr;

	std::unordered_map<std::string, ParticleGroup> particleGroups_;
	std::unordered_map<std::string, std::unique_ptr<GpuParticle>> gpuParticles_;
	std::unordered_set<std::string> sceneGpuParticleKeys_;
	bool gpuParticleEnabled_ = false;
	std::unordered_map<std::string, ParticlePlacementAsset> particlePlacementAssets_;
	std::unordered_map<std::string, std::vector<SceneParticleAssetInstance>> sceneParticleAssetInstances_;
	std::unordered_map<std::string, size_t> sceneParticleAssetCycleSteps_;
	bool sceneParticleLayoutLoaded_ = false;
	mutable bool sceneParticleLayoutDirty_ = false;
	mutable std::string sceneParticlePersistenceMessage_;
	std::vector<LightningEvent> pendingLightningEvents_;
	std::vector<ExposureFlashEvent> pendingExposureFlashEvents_;
	uint32_t lightningSeed_ = 1;

	Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightResource_;
	DirectionalLight* directionalLightData_ = nullptr;

	RuntimeStats runtimeStats_;
	float deltaTime_ = 1.0f / 60.0f;
};
