// 役割: ShadowMap、ライト視点Camera、影描画状態を管理する。
#pragma once

#include <array>
#include <cstdint>
#include <wrl.h>
#include <d3d12.h>

#include "../math/Matrix4x4.h"
#include "../math/Vector3.h"
#include "LightManager.h"

class DirectXCommon;
class SrvManager;
class Object3d;

class ShadowManager {
public:
	~ShadowManager();
	static const uint32_t kMaxSpotShadowMaps = 4;
	static const uint32_t kShadowMapCount = 1 + kMaxSpotShadowMaps;

	struct ShadowInfoForGPU {
		Matrix4x4 viewProjection;
		int32_t enable;
		int32_t mapIndex;
		float bias;
		float normalBias;
		float strength;
		float padding[3];
	};

	struct ShadowForGPU {
		ShadowInfoForGPU directional;
		ShadowInfoForGPU spotLights[LightManager::kMaxSpotLights];
	};

public:
	void Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);
	void Initialize(
		DirectXCommon* dxCommon,
		SrvManager* srvManager,
		uint32_t shadowMapSize
	);
	void Render(const LightManager& lightManager, Object3d* const* objects, uint32_t objectCount);
	void Bind(ID3D12GraphicsCommandList* commandList, UINT shadowTextureRootIndex, UINT shadowDataRootIndex);

	void SetShadowMapSize(uint32_t shadowMapSize);
	uint32_t GetShadowMapSize() const { return shadowMapSize_; }

private:
	void CreateResources();
	void CreateDsv();
	void CreateSrv();
	void CreateShadowDataResource();
	void UpdateShadowData(const LightManager& lightManager);
	void BeginShadowPass(uint32_t mapIndex);
	Matrix4x4 MakeLookAtMatrix(const Vector3& eye, const Vector3& target, const Vector3& up) const;
	Matrix4x4 MakeDirectionalLightViewProjection(
		const LightManager::DirectionalLight& light,
		const LightManager::ShadowSettings& shadowSettings
	) const;
	Matrix4x4 MakeSpotLightViewProjection(const LightManager::SpotLight& light) const;
	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle(uint32_t index) const;

private:
	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;

	uint32_t shadowMapSize_ = 2048;
	uint32_t srvIndex_ = UINT32_MAX;
	bool initialized_ = false;
	bool hasRenderableShadow_ = false;

	Microsoft::WRL::ComPtr<ID3D12Resource> shadowMapResource_;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
	Microsoft::WRL::ComPtr<ID3D12Resource> shadowDataResource_;
	ShadowForGPU* shadowData_ = nullptr;

	std::array<Matrix4x4, kShadowMapCount> lightViewProjections_{};
	std::array<bool, kShadowMapCount> activeMaps_{};
	uint32_t descriptorSizeDSV_ = 0;
};
