// 役割: Skinning用のJointPaletteと頂点Weight情報を定義する。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"

class DirectXCommon;
class Model;
class SrvManager;
struct Skeleton;

class SkinCluster {
public:
	~SkinCluster();
	static constexpr uint32_t kMaxInfluence = 4;

	struct VertexInfluence {
		std::array<float, kMaxInfluence> weights{};
		std::array<uint32_t, kMaxInfluence> jointIndices{};
	};

	struct PaletteWell {
		Matrix4x4 skeletonSpaceMatrix;
		Matrix4x4 skeletonSpaceInverseTransposeMatrix;
	};

	struct SkinningInformation {
		uint32_t numVertices = 0;
		float padding[3]{};
	};

	void Initialize(
		DirectXCommon* dxCommon,
		SrvManager* srvManager,
		const Skeleton& skeleton,
		const Model& model
	);
	void Update(const Skeleton& skeleton);
	void TransitionOutputResource(
		ID3D12GraphicsCommandList* commandList,
		D3D12_RESOURCE_STATES stateAfter
	);

	const D3D12_VERTEX_BUFFER_VIEW& GetInfluenceBufferView() const {
		return influenceBufferView_;
	}
	const D3D12_VERTEX_BUFFER_VIEW& GetSkinnedVertexBufferView() const {
		return skinnedVertexBufferView_;
	}
	uint32_t GetPaletteSrvIndex() const { return paletteSrvIndex_; }
	uint32_t GetInputVertexSrvIndex() const { return inputVertexSrvIndex_; }
	uint32_t GetInfluenceSrvIndex() const { return influenceSrvIndex_; }
	uint32_t GetOutputVertexUavIndex() const { return outputVertexUavIndex_; }
	ID3D12Resource* GetSkinningInformationResource() const {
		return skinningInformationResource_.Get();
	}
	uint32_t GetVertexCount() const {
		return mappedSkinningInformation_
			? mappedSkinningInformation_->numVertices
			: 0;
	}
	bool IsValid() const {
		return influenceResource_ != nullptr &&
			paletteResource_ != nullptr &&
			outputVertexResource_ != nullptr &&
			skinningInformationResource_ != nullptr;
	}

private:
	void AddInfluence(
		VertexInfluence& influence,
		float weight,
		uint32_t jointIndex
	);

	SrvManager* srvManager_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> influenceResource_;
	D3D12_VERTEX_BUFFER_VIEW influenceBufferView_{};
	VertexInfluence* mappedInfluences_ = nullptr;
	uint32_t influenceSrvIndex_ = UINT32_MAX;

	Microsoft::WRL::ComPtr<ID3D12Resource> paletteResource_;
	PaletteWell* mappedPalette_ = nullptr;
	uint32_t paletteSrvIndex_ = UINT32_MAX;
	uint32_t inputVertexSrvIndex_ = UINT32_MAX;
	Microsoft::WRL::ComPtr<ID3D12Resource> outputVertexResource_;
	D3D12_VERTEX_BUFFER_VIEW skinnedVertexBufferView_{};
	uint32_t outputVertexUavIndex_ = UINT32_MAX;
	D3D12_RESOURCE_STATES outputVertexResourceState_ =
		D3D12_RESOURCE_STATE_COMMON;
	Microsoft::WRL::ComPtr<ID3D12Resource> skinningInformationResource_;
	SkinningInformation* mappedSkinningInformation_ = nullptr;
	uint32_t jointCount_ = 0;
	std::vector<Matrix4x4> inverseBindPoseMatrices_;
};
