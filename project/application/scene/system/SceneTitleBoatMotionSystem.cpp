// 役割: Title Scene上の船モデルへ波に合わせた見た目の揺れを加える。
#include "SceneTitleBoatMotionSystem.h"

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/math/Quaternion.h"
#include "../../../engine/particle/ParticleManager.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
	constexpr const char* kTitleBoatEntityName = "TitleFishingBoat";
	constexpr const char* kWakeSheetParticleGroupName = "TitleBoatWakeSheet";
	constexpr const char* kFoamTexturePath = "resources/circle.png";
	constexpr float kPi = 3.1415926535f;
	constexpr float kBoatLengthSampleDistance = 1.85f;
	constexpr float kBoatWidthSampleDistance = 0.55f;
	constexpr float kPitchResponsiveness = 0.72f;
	constexpr float kRollResponsiveness = 0.52f;
	constexpr float kForwardDriftAmplitude = 0.16f;
	constexpr float kForwardDriftSpeed = 0.42f;
	constexpr float kExitMoveDistance = 4.2f;
	constexpr float kWakeSheetEmissionInterval = 0.11f;
	constexpr float kBowWaveXOffset = 1.46f;
	constexpr float kBowWaveZOffset = 0.24f;
	constexpr float kSternWakeXOffset = -1.5f;
	constexpr float kFallbackVerticalAmplitude = 0.06f;
	constexpr float kFallbackRollAmplitude = 0.035f;
	constexpr float kFallbackWaveSpeed = 1.15f;

	struct WaveParameter {
		float directionX = 0.0f; // 波のX方向成分。
		float directionZ = 0.0f; // 波のZ方向成分。
		float amplitude = 0.0f; // 波の高さ。
		float wavelength = 1.0f; // 波長。
	};

	struct WaterContext {
		bool valid = false; // 水面が見つかったか。
		Vector3 center{}; // 水面中心位置。
		Vector3 halfSize{}; // 水面半径。
		float waveScale = 1.0f; // WaterVolumeの波倍率。
	};

	/// <summary>
	/// 0から1の範囲で滑らかに補間する値を返します。
	/// </summary>
	float SmoothStep(float value) {
		const float t = std::clamp(value, 0.0f, 1.0f); // 補間に使う正規化値。
		return t * t * (3.0f - 2.0f * t);
	}

	/// <summary>
	/// タイトル退出時の船移動量を返します。
	/// </summary>
	float EvaluateExitMove(float exitProgress) {
		const float easedProgress = SmoothStep(exitProgress); // 退出演出の補間値。
		return easedProgress * kExitMoveDistance;
	}

	/// <summary>
	/// タイトル構図内で船が右方向へ進む見た目のずれ量を返します。
	/// </summary>
	float EvaluateForwardDrift(float time) {
		return (1.0f - std::cos(time * kForwardDriftSpeed)) *
			kForwardDriftAmplitude;
	}

	/// <summary>
	/// 指定方向を正規化した波パラメータを作成します。
	/// </summary>
	WaveParameter MakeWave(
		float directionX,
		float directionZ,
		float amplitude,
		float wavelength
	) {
		const float length = std::sqrt(
			directionX * directionX + directionZ * directionZ
		); // 方向ベクトルの長さ。
		const float inverseLength = length > 0.000001f
			? 1.0f / length
			: 1.0f; // 正規化用の逆数。
		return {
			directionX * inverseLength,
			directionZ * inverseLength,
			amplitude,
			wavelength
		};
	}

	/// <summary>
	/// WaterSurfaceRendererと同じ比率の波パラメータを作成します。
	/// </summary>
	std::array<WaveParameter, 3> BuildWaves(const WaterContext& water) {
		const float span = (std::max)(water.halfSize.x, water.halfSize.z); // 水面の広い軸。
		const float waveFitScale = std::clamp(span / 8.0f, 0.45f, 1.6f); // 水面サイズへの波調整。
		const float amplitudeScale = water.waveScale * waveFitScale; // 高さ倍率。
		const float wavelengthScale = (std::max)(waveFitScale, 0.65f); // 波長倍率。
		return { {
			MakeWave(0.86f, 0.32f, 0.12f * amplitudeScale, 5.8f * wavelengthScale),
			MakeWave(-0.28f, 0.96f, 0.075f * amplitudeScale, 3.1f * wavelengthScale),
			MakeWave(0.58f, -0.74f, 0.045f * amplitudeScale, 1.65f * wavelengthScale)
		} };
	}

	/// <summary>
	/// タイトルシーン上の水面情報を取得します。
	/// </summary>
	WaterContext FindWaterContext(const SceneDocument& document) {
		for (const SceneEntity& entity : document.GetEntities()) { // Scene上のEntity。
			if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
				continue;
			}
			const SceneComponent* waterVolume =
				SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume"); // 水面Component。
			if (!waterVolume || !waterVolume->waterSurfaceEnabled) {
				continue;
			}

			const Transform transform =
				SceneTransformResolver::ResolveScene3DTransform(document, entity); // 水面基準Transform。
			const Vector3 halfSize{
				(std::max)(waterVolume->waterHalfSize.x, 0.05f),
				(std::max)(waterVolume->waterHalfSize.y, 0.05f),
				(std::max)(waterVolume->waterHalfSize.z, 0.05f)
			}; // 水面の半径。
			return {
				true,
				{
					transform.translate.x + waterVolume->waterOffset.x,
					transform.translate.y + waterVolume->waterOffset.y,
					transform.translate.z + waterVolume->waterOffset.z
				},
				halfSize,
				waterVolume->waterSurfaceWaveScale
			};
		}
		return {};
	}

	/// <summary>
	/// 水面端で波を弱める係数を計算します。
	/// </summary>
	float EvaluateEdgeDamping(const WaterContext& water, const Vector3& position) {
		const float localX = (position.x - water.center.x) / water.halfSize.x; // 水面内X位置。
		const float localZ = (position.z - water.center.z) / water.halfSize.z; // 水面内Z位置。
		const float edgeDistance = (std::min)(
			1.0f - std::abs(localX),
			1.0f - std::abs(localZ)
		); // 水面端からの距離。
		return SmoothStep(std::clamp(edgeDistance, 0.0f, 1.0f) / 0.18f);
	}

	/// <summary>
	/// Gerstner波の高さ成分だけを水面上の指定位置で評価します。
	/// </summary>
	float EvaluateWaveHeight(
		const WaterContext& water,
		const std::array<WaveParameter, 3>& waves,
		const Vector3& position,
		float time
	) {
		const float surfaceY = water.center.y + water.halfSize.y; // 静止時の水面高さ。
		const float damping = EvaluateEdgeDamping(water, position); // 水面端の減衰。
		float height = surfaceY; // 合成後の水面高さ。
		for (const WaveParameter& wave : waves) { // 合成する波。
			const float frequency = 2.0f * kPi / (std::max)(wave.wavelength, 0.05f);
			const float speed = std::sqrt(9.8f / frequency) * 0.85f;
			const float phase =
				frequency * (wave.directionX * position.x + wave.directionZ * position.z) +
				speed * time; // 波の位相。
			height += damping * wave.amplitude * std::sin(phase);
		}
		return height;
	}

	/// <summary>
	/// 水押し分け表現用のParticleGroupを準備します。
	/// </summary>
	void PrepareTitleBoatParticleGroups() {
		ParticleManager* particleManager =
			ParticleManager::GetInstance(); // 共有Particle管理。
		particleManager->CreateParticleGroupIfNeeded(
			kWakeSheetParticleGroupName,
			kFoamTexturePath
		);

		ParticleManager::ParticleRenderDesc wakeRender{}; // 水押し分け帯の描画設定。
		wakeRender.billboardMode = ParticleManager::BillboardMode::kBillboard;
		wakeRender.primitiveType = ParticleManager::PrimitiveType::kPlane;
		wakeRender.alphaCutoff = 0.02f;
		wakeRender.emissiveIntensity = 0.86f;
		wakeRender.depthTest = true;
		wakeRender.depthWrite = false;
		particleManager->SetGroupBlendMode(
			kWakeSheetParticleGroupName,
			ParticleCommon::BlendMode::kBlendModeScreen
		);
		particleManager->SetGroupRenderDesc(
			kWakeSheetParticleGroupName,
			wakeRender
		);
	}

	/// <summary>
	/// 船体が水をかき分ける大きい泡帯の挙動を作成します。
	/// </summary>
	ParticleManager::ParticleBehavior BuildWakeSheetBehavior(
		float sideSign,
		bool bowWave
	) {
		ParticleManager::ParticleBehavior behavior{}; // 水押し分け帯の挙動。
		behavior.life.lifeTimeMin = bowWave ? 0.55f : 1.08f;
		behavior.life.lifeTimeMax = bowWave ? 0.78f : 1.48f;
		behavior.life.enableLifeFade = true;
		behavior.life.fadeOutStartRatio = bowWave ? 0.08f : 0.16f;
		behavior.scale.startScaleMin = bowWave
			? Vector3{ 0.28f, 0.07f, 1.0f }
			: Vector3{ 0.72f, 0.1f, 1.0f };
		behavior.scale.startScaleMax = bowWave
			? Vector3{ 0.42f, 0.11f, 1.0f }
			: Vector3{ 1.02f, 0.16f, 1.0f };
		behavior.scale.enableScaleOverLife = true;
		behavior.scale.endScaleMin = bowWave
			? Vector3{ 0.58f, 0.14f, 1.0f }
			: Vector3{ 2.0f, 0.24f, 1.0f };
		behavior.scale.endScaleMax = bowWave
			? Vector3{ 0.82f, 0.22f, 1.0f }
			: Vector3{ 2.72f, 0.36f, 1.0f };
		behavior.rotation.initialRotationMin = {
			0.0f,
			0.0f,
			sideSign * (bowWave ? 0.16f : 0.03f)
		};
		behavior.rotation.initialRotationMax = {
			0.0f,
			0.0f,
			sideSign * (bowWave ? 0.28f : 0.08f)
		};
		behavior.motion.linear.baseVelocity = bowWave
			? Vector3{ -0.16f, 0.0f, sideSign * 0.025f }
			: Vector3{ -0.56f, 0.0f, sideSign * 0.025f };
		behavior.motion.linear.velocityRandomRange = bowWave
			? Vector3{ 0.05f, 0.01f, 0.018f }
			: Vector3{ 0.16f, 0.01f, 0.04f };
		behavior.motion.linear.enableAcceleration = true;
		behavior.motion.linear.baseAcceleration = bowWave
			? Vector3{ -0.012f, -0.008f, sideSign * 0.006f }
			: Vector3{ -0.055f, -0.005f, sideSign * 0.008f };
		behavior.motion.linear.accelerationRandomRange = { 0.012f, 0.005f, 0.01f };
		behavior.color.mode = ParticleManager::ColorChangeMode::kOverLife;
		behavior.color.startColorMin = bowWave
			? Vector4{ 0.82f, 0.98f, 1.0f, 0.48f }
			: Vector4{ 0.78f, 0.96f, 1.0f, 0.58f };
		behavior.color.startColorMax = bowWave
			? Vector4{ 1.0f, 1.0f, 1.0f, 0.66f }
			: Vector4{ 1.0f, 1.0f, 1.0f, 0.76f };
		behavior.color.endColorMin = { 0.5f, 0.82f, 1.0f, 0.0f };
		behavior.color.endColorMax = { 0.76f, 0.94f, 1.0f, 0.0f };
		behavior.render.billboardMode = ParticleManager::BillboardMode::kBillboard;
		return behavior;
	}

	/// <summary>
	/// 船首左右から水をかき分ける大きい航跡帯を発生させます。
	/// </summary>
	void EmitBoatWakeSheets(
		const WaterContext& water,
		const std::array<WaveParameter, 3>& waves,
		const Vector3& boatPosition,
		float time,
		float deltaTime,
		float& wakeSheetEmissionTimer
	) {
		if (!water.valid) {
			return;
		}

		ParticleManager* particleManager =
			ParticleManager::GetInstance(); // 共有Particle管理。
		wakeSheetEmissionTimer += (std::max)(deltaTime, 0.0f);
		while (wakeSheetEmissionTimer >= kWakeSheetEmissionInterval) {
			for (const float sideSign : { -1.0f, 1.0f }) { // 左右の水押し分け方向。
				const Vector3 bowSideSample{
					boatPosition.x + kBowWaveXOffset,
					boatPosition.y,
					boatPosition.z + sideSign * kBowWaveZOffset
				}; // 船首横の水面サンプル位置。
				const float bowSideSurfaceHeight =
					EvaluateWaveHeight(
						water,
						waves,
						bowSideSample,
						time
					); // 船首横の水面高さ。
				const Vector3 wakePosition{
					bowSideSample.x - 0.04f,
					bowSideSurfaceHeight + 0.105f,
					bowSideSample.z + sideSign * 0.025f
				}; // 船首の水押し分け発生位置。
				const ParticleManager::ParticleBehavior bowWakeBehavior =
					BuildWakeSheetBehavior(sideSign, true); // 船首側の流れ。
				particleManager->Emit(
					kWakeSheetParticleGroupName,
					wakePosition,
					{ 0.045f, 0.0f, 0.025f },
					1,
					bowWakeBehavior
				);
			}
			const Vector3 sternSample{
				boatPosition.x + kSternWakeXOffset,
				boatPosition.y,
				boatPosition.z
			}; // 船尾中央の水面サンプル位置。
			const float sternSurfaceHeight =
				EvaluateWaveHeight(water, waves, sternSample, time); // 船尾中央の水面高さ。
			const Vector3 sternWakePosition{
				sternSample.x - 0.1f,
				sternSurfaceHeight + 0.09f,
				sternSample.z
			}; // 船尾の引き波発生位置。
			const ParticleManager::ParticleBehavior sternWakeBehavior =
				BuildWakeSheetBehavior(0.0f, false); // 船尾側の流れ。
			particleManager->Emit(
				kWakeSheetParticleGroupName,
				sternWakePosition,
				{ 0.18f, 0.0f, 0.16f },
				1,
				sternWakeBehavior
			);
			wakeSheetEmissionTimer -= kWakeSheetEmissionInterval;
		}
	}
}

void SceneTitleBoatMotionSystem::Update(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	float exitProgress
) {
	elapsedSeconds_ += (std::max)(deltaTime, 0.0f);
	if (!particleGroupsPrepared_) {
		PrepareTitleBoatParticleGroups();
		particleGroupsPrepared_ = true;
	}
	const WaterContext water = FindWaterContext(document); // タイトル上の水面情報。
	const std::array<WaveParameter, 3> waves = BuildWaves(water); // 船揺れに使う波。

	for (const SceneRuntimeObjectBinding& binding : bindings) { // Runtime Objectとの対応。
		if (
			!binding.entity ||
			!binding.object ||
			binding.entity->name != kTitleBoatEntityName
		) {
			continue;
		}

		Vector3 translate = binding.entity->transform.translate; // Scene上の基準位置。
		Vector3 rotate =
			MakeEulerFromQuaternion(binding.entity->transform.rotate); // Scene上の基準回転。
		const float time = elapsedSeconds_; // 波揺れ計算に使う経過時間。
		translate.x += EvaluateForwardDrift(time); // 右方向への進行感。
		translate.x += EvaluateExitMove(exitProgress); // START後の退出移動。
		if (water.valid) {
			const Vector3 centerSample = translate; // 船中央の水面サンプル位置。
			const Vector3 bowSample{
				translate.x + kBoatLengthSampleDistance,
				translate.y,
				translate.z
			}; // 船首側の水面サンプル位置。
			const Vector3 sternSample{
				translate.x - kBoatLengthSampleDistance,
				translate.y,
				translate.z
			}; // 船尾側の水面サンプル位置。
			const Vector3 rightSample{
				translate.x,
				translate.y,
				translate.z + kBoatWidthSampleDistance
			}; // 右舷側の水面サンプル位置。
			const Vector3 leftSample{
				translate.x,
				translate.y,
				translate.z - kBoatWidthSampleDistance
			}; // 左舷側の水面サンプル位置。

			const float centerHeight =
				EvaluateWaveHeight(water, waves, centerSample, time); // 船中央の水面高さ。
			const float bowHeight =
				EvaluateWaveHeight(water, waves, bowSample, time); // 船首側の水面高さ。
			const float sternHeight =
				EvaluateWaveHeight(water, waves, sternSample, time); // 船尾側の水面高さ。
			const float rightHeight =
				EvaluateWaveHeight(water, waves, rightSample, time); // 右舷側の水面高さ。
			const float leftHeight =
				EvaluateWaveHeight(water, waves, leftSample, time); // 左舷側の水面高さ。
			const float calmSurfaceHeight =
				water.center.y + water.halfSize.y; // 波がない状態の水面高さ。
			const float authoredHeightOffset =
				binding.entity->transform.translate.y -
				calmSurfaceHeight; // Scene上で調整した船の浮き位置。

			translate.y = centerHeight + authoredHeightOffset;
			rotate.z += std::atan2(
				bowHeight - sternHeight,
				kBoatLengthSampleDistance * 2.0f
			) * kPitchResponsiveness;
			rotate.x += std::atan2(
				leftHeight - rightHeight,
				kBoatWidthSampleDistance * 2.0f
			) * kRollResponsiveness;
		} else {
			translate.y +=
				std::sin(time * kFallbackWaveSpeed) * kFallbackVerticalAmplitude;
			rotate.z +=
				std::sin(time * kFallbackWaveSpeed + 0.4f) *
				kFallbackRollAmplitude;
		}

		binding.object->SetTranslate(translate);
		binding.object->SetRotate(rotate);
		EmitBoatWakeSheets(
			water,
			waves,
			translate,
			time,
			deltaTime,
			wakeSheetEmissionTimer_
		);
	}
}

void SceneTitleBoatMotionSystem::Clear() {
	elapsedSeconds_ = 0.0f;
	wakeSheetEmissionTimer_ = 0.0f;
	if (particleGroupsPrepared_) {
		ParticleManager* particleManager =
			ParticleManager::GetInstance(); // 共有Particle管理。
		particleManager->ClearParticleGroup(kWakeSheetParticleGroupName);
		particleGroupsPrepared_ = false;
	}
}
