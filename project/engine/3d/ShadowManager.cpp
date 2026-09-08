// 役割: ShadowMapリソース生成と影描画パスの開始、終了を実装する。
#include "ShadowManager.h"

#include "Object3d.h"
#include "Object3dCommon.h"
#include "SrvManager.h"
#include "../base/DirectXCommon.h"
#include "../math/Math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
	constexpr float kPi = 3.1415926535f;
	constexpr uint32_t kDefaultShadowMapSize = 2048;
	constexpr uint32_t kMinShadowMapSize = 512;
	constexpr uint32_t kMaxShadowMapSize = 4096;

	uint32_t NormalizeShadowMapSize(uint32_t shadowMapSize) {
		shadowMapSize = std::clamp(
			shadowMapSize,
			kMinShadowMapSize,
			kMaxShadowMapSize
		);
		uint32_t result = kMinShadowMapSize;
		while (result < shadowMapSize && result < kMaxShadowMapSize) {
			result *= 2;
		}
		return result;
	}

	ShadowManager::ShadowInfoForGPU MakeDisabledShadowInfo() {
		ShadowManager::ShadowInfoForGPU info{};
		info.viewProjection = MakeIdentity4x4();
		info.enable = false;
		info.mapIndex = 0;
		info.bias = 0.0f;
		info.normalBias = 0.0f;
		info.strength = 0.0f;
		return info;
	}

	Matrix4x4 MakeShadowOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip) {
		Matrix4x4 result{};
		result.m[0][0] = 2.0f / (right - left);
		result.m[1][1] = 2.0f / (top - bottom);
		result.m[2][2] = 1.0f / (farClip - nearClip);
		result.m[3][0] = (left + right) / (left - right);
		result.m[3][1] = (top + bottom) / (bottom - top);
		result.m[3][2] = nearClip / (nearClip - farClip);
		result.m[3][3] = 1.0f;
		return result;
	}
}

ShadowManager::~ShadowManager() {
	if (srvManager_ && srvIndex_ != UINT32_MAX) {
		srvManager_->Free(srvIndex_);
	}
}

void ShadowManager::Initialize(DirectXCommon* dxCommon, SrvManager* srvManager) {
	Initialize(dxCommon, srvManager, kDefaultShadowMapSize);
}

void ShadowManager::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager,
	uint32_t shadowMapSize
) {
	assert(dxCommon);
	assert(srvManager);

	dxCommon_ = dxCommon;
	srvManager_ = srvManager;
	shadowMapSize_ = NormalizeShadowMapSize(shadowMapSize);

	CreateResources();
	CreateDsv();
	CreateSrv();
	CreateShadowDataResource();

	initialized_ = true;
}

void ShadowManager::SetShadowMapSize(uint32_t shadowMapSize) {
	const uint32_t normalizedSize = NormalizeShadowMapSize(shadowMapSize);
	if (shadowMapSize_ == normalizedSize) {
		return;
	}

	shadowMapSize_ = normalizedSize;
	if (!initialized_) {
		return;
	}

	shadowMapResource_.Reset();
	dsvHeap_.Reset();
	CreateResources();
	CreateDsv();
	CreateSrv();
}

void ShadowManager::CreateResources() {
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = shadowMapSize_;
	resourceDesc.Height = shadowMapSize_;
	resourceDesc.DepthOrArraySize = static_cast<UINT16>(kShadowMapCount);
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;

	HRESULT hr = dxCommon_->GetDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&shadowMapResource_)
	);
	assert(SUCCEEDED(hr));
}

void ShadowManager::CreateDsv() {
	dsvHeap_ = dxCommon_->CreateDescriptorHeap(
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
		kShadowMapCount,
		false
	);
	descriptorSizeDSV_ = dxCommon_->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	for (uint32_t i = 0; i < kShadowMapCount; ++i) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.ArraySize = 1;
		dsvDesc.Texture2DArray.FirstArraySlice = i;

		dxCommon_->GetDevice()->CreateDepthStencilView(
			shadowMapResource_.Get(),
			&dsvDesc,
			GetDsvHandle(i)
		);
	}
}

void ShadowManager::CreateSrv() {
	if (srvIndex_ == UINT32_MAX) {
		assert(srvManager_->CanAllocate());
		srvIndex_ = srvManager_->Allocate();
	}
	srvManager_->CreateSRVforTexture2DArray(
		srvIndex_,
		shadowMapResource_.Get(),
		DXGI_FORMAT_R32_FLOAT,
		1,
		kShadowMapCount
	);
}

void ShadowManager::CreateShadowDataResource() {
	shadowDataResource_ = dxCommon_->CreateBufferResource(sizeof(ShadowForGPU));
	shadowDataResource_->Map(0, nullptr, reinterpret_cast<void**>(&shadowData_));
	*shadowData_ = {};
	shadowData_->directional = MakeDisabledShadowInfo();
	for (auto& info : shadowData_->spotLights) {
		info = MakeDisabledShadowInfo();
	}
}

void ShadowManager::UpdateShadowData(const LightManager& lightManager) {
	hasRenderableShadow_ = false;
	activeMaps_.fill(false);

	shadowData_->directional = MakeDisabledShadowInfo();
	for (auto& info : shadowData_->spotLights) {
		info = MakeDisabledShadowInfo();
	}

	const auto& directionalLight = lightManager.GetDirectionalLight();
	const auto& directionalShadow = lightManager.GetDirectionalShadowSettings();
	if (directionalLight.enable != 0 && directionalShadow.enable != 0) {
		const uint32_t mapIndex = 0;
		lightViewProjections_[mapIndex] = MakeDirectionalLightViewProjection(
			directionalLight,
			directionalShadow
		);
		activeMaps_[mapIndex] = true;
		hasRenderableShadow_ = true;

		shadowData_->directional.viewProjection = lightViewProjections_[mapIndex];
		shadowData_->directional.enable = true;
		shadowData_->directional.mapIndex = static_cast<int32_t>(mapIndex);
		shadowData_->directional.bias = directionalShadow.bias;
		shadowData_->directional.normalBias = directionalShadow.normalBias;
		shadowData_->directional.strength = directionalShadow.strength;
	}

	const auto& spotLights = lightManager.GetSpotLights();
	const auto& spotShadows = lightManager.GetSpotShadowSettings();
	uint32_t usedSpotShadowCount = 0;

	for (uint32_t i = 0; i < spotLights.size() && i < LightManager::kMaxSpotLights; ++i) {
		const LightManager::ShadowSettings shadow =
			i < spotShadows.size() ? spotShadows[i] : LightManager::ShadowSettings{};
		if (spotLights[i].enable == 0 || shadow.enable == 0 || usedSpotShadowCount >= kMaxSpotShadowMaps) {
			continue;
		}

		const uint32_t mapIndex = 1 + usedSpotShadowCount;
		lightViewProjections_[mapIndex] = MakeSpotLightViewProjection(spotLights[i]);
		activeMaps_[mapIndex] = true;
		hasRenderableShadow_ = true;

		shadowData_->spotLights[i].viewProjection = lightViewProjections_[mapIndex];
		shadowData_->spotLights[i].enable = true;
		shadowData_->spotLights[i].mapIndex = static_cast<int32_t>(mapIndex);
		shadowData_->spotLights[i].bias = shadow.bias;
		shadowData_->spotLights[i].normalBias = shadow.normalBias;
		shadowData_->spotLights[i].strength = shadow.strength;

		++usedSpotShadowCount;
	}
}

void ShadowManager::Render(const LightManager& lightManager, Object3d* const* objects, uint32_t objectCount) {
	if (!initialized_) {
		return;
	}

	UpdateShadowData(lightManager);
	if (!hasRenderableShadow_) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = shadowMapResource_.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);

	Object3dCommon::GetInstance()->SetShadowRenderState();

	for (uint32_t mapIndex = 0; mapIndex < kShadowMapCount; ++mapIndex) {
		if (!activeMaps_[mapIndex]) {
			continue;
		}

		BeginShadowPass(mapIndex);
		for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
			if (objects[objectIndex]) {
				objects[objectIndex]->DrawShadow(lightViewProjections_[mapIndex]);
			}
		}
	}

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
}

void ShadowManager::Bind(ID3D12GraphicsCommandList* commandList, UINT shadowTextureRootIndex, UINT shadowDataRootIndex) {
	if (!initialized_) {
		return;
	}

	commandList->SetGraphicsRootDescriptorTable(shadowTextureRootIndex, srvManager_->GetGPUDescriptorHandle(srvIndex_));
	commandList->SetGraphicsRootConstantBufferView(shadowDataRootIndex, shadowDataResource_->GetGPUVirtualAddress());
}

void ShadowManager::BeginShadowPass(uint32_t mapIndex) {
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = GetDsvHandle(mapIndex);
	dxCommon_->GetCommandList()->OMSetRenderTargets(0, nullptr, false, &dsvHandle);
	dxCommon_->GetCommandList()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	D3D12_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(shadowMapSize_);
	viewport.Height = static_cast<float>(shadowMapSize_);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	dxCommon_->GetCommandList()->RSSetViewports(1, &viewport);

	D3D12_RECT scissorRect{};
	scissorRect.right = static_cast<LONG>(shadowMapSize_);
	scissorRect.bottom = static_cast<LONG>(shadowMapSize_);
	dxCommon_->GetCommandList()->RSSetScissorRects(1, &scissorRect);
}

Matrix4x4 ShadowManager::MakeLookAtMatrix(const Vector3& eye, const Vector3& target, const Vector3& up) const {
	Vector3 forward = Math::Normalize(Math::Subtract(target, eye));
	if (Math::Length(forward) < 0.000001f) {
		forward = { 0.0f, 0.0f, 1.0f };
	}

	Vector3 right = Math::Normalize(Math::Cross(up, forward));
	if (Math::Length(right) < 0.000001f) {
		right = { 1.0f, 0.0f, 0.0f };
	}

	Vector3 trueUp = Math::Cross(forward, right);

	Matrix4x4 world = MakeIdentity4x4();
	world.m[0][0] = right.x;
	world.m[0][1] = right.y;
	world.m[0][2] = right.z;
	world.m[1][0] = trueUp.x;
	world.m[1][1] = trueUp.y;
	world.m[1][2] = trueUp.z;
	world.m[2][0] = forward.x;
	world.m[2][1] = forward.y;
	world.m[2][2] = forward.z;
	world.m[3][0] = eye.x;
	world.m[3][1] = eye.y;
	world.m[3][2] = eye.z;
	world.m[3][3] = 1.0f;

	return Inverse(world);
}

Matrix4x4 ShadowManager::MakeDirectionalLightViewProjection(
	const LightManager::DirectionalLight& light,
	const LightManager::ShadowSettings& shadowSettings
) const {
	Vector3 direction = Math::Normalize(light.direction);
	if (Math::Length(direction) < 0.000001f) {
		direction = { 0.0f, -1.0f, 0.0f };
	}

	Vector3 target = shadowSettings.target;
	const float lightDistance = (std::max)(shadowSettings.distance, 1.0f);
	Vector3 up = { 0.0f, 1.0f, 0.0f };
	if (std::abs(Math::Cross(up, direction).x) +
		std::abs(Math::Cross(up, direction).y) +
		std::abs(Math::Cross(up, direction).z) < 0.001f) {
		up = { 0.0f, 0.0f, 1.0f };
	}

	Vector3 right = Math::Normalize(Math::Cross(up, direction));
	if (Math::Length(right) < 0.000001f) {
		right = { 1.0f, 0.0f, 0.0f };
	}
	Vector3 trueUp = Math::Normalize(Math::Cross(direction, right));
	if (shadowSettings.texelSnap && shadowMapSize_ > 0) {
		const float orthographicSize =
			(std::max)(shadowSettings.orthographicSize, 1.0f);
		const float worldUnitsPerTexel =
			(orthographicSize * 2.0f) /
			static_cast<float>(shadowMapSize_);
		if (worldUnitsPerTexel > 0.0f) {
			const float targetRight = Math::Dot(target, right);
			const float targetUp = Math::Dot(target, trueUp);
			const float snappedRight =
				std::round(targetRight / worldUnitsPerTexel) *
				worldUnitsPerTexel;
			const float snappedUp =
				std::round(targetUp / worldUnitsPerTexel) *
				worldUnitsPerTexel;
			target = Math::Add(
				target,
				Math::Add(
					Math::Multiply(right, snappedRight - targetRight),
					Math::Multiply(trueUp, snappedUp - targetUp)
				)
			);
		}
	}

	const Vector3 eye = Math::Subtract(
		target,
		Math::Multiply(direction, lightDistance)
	);
	Matrix4x4 view = MakeLookAtMatrix(eye, target, up);
	const float orthographicSize =
		(std::max)(shadowSettings.orthographicSize, 1.0f);
	const float nearClip = (std::max)(shadowSettings.nearClip, 0.001f);
	const float farClip = (std::max)(
		shadowSettings.farClip,
		nearClip + 0.001f
	);
	Matrix4x4 projection = MakeShadowOrthographicMatrix(
		-orthographicSize,
		orthographicSize,
		orthographicSize,
		-orthographicSize,
		nearClip,
		farClip
	);
	return Multiply(view, projection);
}

Matrix4x4 ShadowManager::MakeSpotLightViewProjection(const LightManager::SpotLight& light) const {
	Vector3 direction = Math::Normalize(light.direction);
	if (Math::Length(direction) < 0.000001f) {
		direction = { 0.0f, -1.0f, 0.0f };
	}

	const Vector3 target = Math::Add(light.position, direction);
	Vector3 up = { 0.0f, 1.0f, 0.0f };
	if (std::abs(Math::Cross(up, direction).x) +
		std::abs(Math::Cross(up, direction).y) +
		std::abs(Math::Cross(up, direction).z) < 0.001f) {
		up = { 0.0f, 0.0f, 1.0f };
	}

	const float cosAngle = std::clamp(light.cosAngle, -0.99f, 0.99f);
	const float fovY = std::clamp(std::acos(cosAngle) * 2.0f, 0.1f, kPi * 0.95f);

	Matrix4x4 view = MakeLookAtMatrix(light.position, target, up);
	const float farClip = (std::max)(light.distance, 1.0f);
	Matrix4x4 projection = MakePerspectiveFovMatrix(fovY, 1.0f, 0.1f, farClip);
	return Multiply(view, projection);
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowManager::GetDsvHandle(uint32_t index) const {
	D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += descriptorSizeDSV_ * index;
	return handle;
}
