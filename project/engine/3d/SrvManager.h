// 役割: SRV、UAV、CBV用DescriptorHeapの割り当てとGPUハンドル取得を管理する。
#pragma once
#include "../base/DirectXCommon.h"
#include <array>
#include <cstdint>
#include <vector>
class SrvManager{

public:
	static SrvManager* GetInstance();

	//初期化
	void Initialize(DirectXCommon* dxCommon);

	uint32_t Allocate();
	void Free(uint32_t index);

	// SRV確保可能チェック
	bool CanAllocate() const;

	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(uint32_t index);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(uint32_t index);

	//SRV生成(テクスチャ用)
	void CreateSRVforTexture2D(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels);
	void CreateSRVforTexture2DArray(uint32_t srvIndex, ID3D12Resource* pResource, DXGI_FORMAT Format, UINT MipLevels, UINT arraySize);
	//SRV生成(Structured Buffer用)
	void CreateSRVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements,UINT structureByteStride);
	//UAV生成(Structured Buffer用)
	void CreateUAVforStructuredBuffer(uint32_t srvIndex, ID3D12Resource* pResource, UINT numElements, UINT structureByteStride);

	ID3D12DescriptorHeap* GetDescriptorHeap() const;

	void PreDraw();

	void SetGraphicsRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex);
	void SetComputeRootDescriptorTable(UINT RootParameterIndex, uint32_t srvIndex);

private:
	DirectXCommon* directXCommon = nullptr;
	static SrvManager* instance_;

	// CBV/SRV/UAVデスクリプタヒープで確保できる最大数
	static constexpr uint32_t kMaxSRVCount = 4096;
	// SRV用のデスクリプタサイズ
	uint32_t descriptorSize;
	// SRV用デスクリプタヒープ
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap;

	//次に使用するSRVインデックス
	uint32_t useIndex = 0;
	std::array<bool, kMaxSRVCount> allocatedIndices_{};
	std::vector<uint32_t> freeIndices_;
};

