// 役割: 水面の反射、屈折、ライトシャフト描画を管理する。
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"

class Camera;
class DirectXCommon;

class WaterSurfaceRenderer {
public:
	struct FoamEmitter {
		Vector3 center{};
		float radiusX = 1.0f;
		float radiusZ = 1.0f;
		float yaw = 0.0f;
		uint32_t seed = 0;
	};

	struct Settings {
		Vector4 baseColor{ 0.04f, 0.55f, 0.78f, 1.0f };
		Vector4 highlightColor{ 0.42f, 0.95f, 1.20f, 1.0f };
		float alpha = 0.36f;
		float fresnelPower = 3.0f;
		float normalStrength = 0.75f;
		float waveScale = 1.0f;
		bool enabled = true;
	};

	void Initialize(DirectXCommon* dxCommon, uint32_t gridResolution = 72);
	void Finalize();
	void Update(float deltaTime);
	void Draw(
		const Camera* camera,
		const Vector3& center,
		const Vector3& halfSize,
		const Settings& settings,
		const std::vector<FoamEmitter>& foamEmitters = {}
	);
	// 水面を横切るMeshの輪郭を、途切れた白い泡のリングとして重ねる。
	void DrawFoam(const Camera* camera, const std::vector<FoamEmitter>& emitters);

private:
	struct SurfaceVertex {
		Vector3 position;
		Vector2 uv;
	};
	struct FoamVertex {
		Vector3 position;
		Vector4 color;
	};
	struct FoamCameraData {
		Matrix4x4 viewProjection;
	};

	struct SurfaceData {
		Matrix4x4 viewProjection;
		Vector4 centerTime;
		Vector4 halfSizeAlpha;
		Vector4 cameraPositionFresnel;
		Vector4 waveA;
		Vector4 waveB;
		Vector4 waveC;
		Vector4 baseColor;
		Vector4 highlightColorNormal;
		Vector4 foamEmitters[8]; // x, z, X半径, Z半径
		Vector4 foamOrientations[8]; // x: sin(yaw), y: cos(yaw)
		Vector4 foamInfo; // x: 有効な泡Emitter数
	};

	void CreateRootSignature();
	void CreateGraphicsPipeline();
	void CreateFoamPipeline();
	void CreateResources(uint32_t gridResolution);
	void CreateFoamResources();

	DirectXCommon* dxCommon_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> foamRootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> foamPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> surfaceResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> foamVertexResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> foamCameraResource_;
	SurfaceData* surfaceData_ = nullptr;
	FoamVertex* foamVertices_ = nullptr;
	uint8_t* foamCameraData_ = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
	D3D12_VERTEX_BUFFER_VIEW foamVertexBufferView_{};
	uint32_t vertexCount_ = 0;
	uint32_t foamDrawSlot_ = 0;
	float time_ = 0.0f;
	static constexpr uint32_t kFoamSegmentCount = 32;
	static constexpr uint32_t kMaxFoamEmitters = 32;
	static constexpr uint32_t kFoamVerticesPerSlot =
		kFoamSegmentCount * 6 * kMaxFoamEmitters;
	static constexpr uint32_t kFoamDrawSlotCount = 8;
	static constexpr uint32_t kFoamCameraSlotSize = 256;
};
