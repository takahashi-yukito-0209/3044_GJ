// 役割: Object3d全体で共有する描画状態と既定Cameraを管理する。
#pragma once
#include <d3dx12.h>
#include "../math/Vector3.h"
#include "../math/Vector4.h"
class DirectXCommon;
class Camera;
class SkinCluster;

class Object3dCommon{
public: //メンバ関数
	enum class CullMode {
		kNone,
		kBack,
		kFront,
		kCount
	};

	// シングルトンインスタンスの取得
	static Object3dCommon* GetInstance();

	// コピー禁止
	Object3dCommon(const Object3dCommon&) = delete;
	Object3dCommon& operator=(const Object3dCommon&) = delete;

	//初期化
	void Initialize(DirectXCommon* dxCommon);
	//共通描画設定
	void SetCommonRenderState();
	void SetCommonRenderState(CullMode cullMode);
	void SetSkinningRenderState();
	void SetSkinningRenderState(CullMode cullMode);
	void SetShadowRenderState(CullMode cullMode = CullMode::kBack);
	void SetSkinningShadowRenderState(CullMode cullMode = CullMode::kBack);
	void DispatchSkinning(SkinCluster& skinCluster);

public:
	
	//setter
	void SetDefaultCamera(Camera* camera){ this->defaultCamera = camera; }

	//getter
	DirectXCommon* GetDxCommon() const{return dxCommon_;}
	Camera* GetDefaultCamera() const{ return defaultCamera; }

	// Skybox用描画設定
	void SetSkyboxRenderState();

private://非公開メンバ関数
	Object3dCommon() = default;
	~Object3dCommon() = default;

	//ルートシグネチャの作成
	void MakeRootSignature();
	//グラフィックパイプラインの生成
	void GenerateGraphicsPipeline();
	void GenerateSkinningGraphicsPipeline();
	void GenerateSkinningComputePipeline();

	// Skybox用グラフィックパイプラインの生成
	void GenerateSkyboxGraphicsPipeline();
	void GenerateShadowGraphicsPipeline();
	void GenerateSkinningShadowGraphicsPipeline();
	void MakeShadowRootSignature();

	// どこか共通ヘルパに
	inline D3D12_STATIC_SAMPLER_DESC MakeStaticSamplerS0();

	void CreateLightingResource();

private://メンバ変数

	//処理用にDirectXCommonを保持する
	DirectXCommon* dxCommon_ = nullptr;
	//ルートシグネチャ
	Microsoft::WRL::ComPtr < ID3D12RootSignature> rootSignature_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipelineStates_
		[static_cast<uint32_t>(CullMode::kCount)];
	Microsoft::WRL::ComPtr<ID3D12PipelineState> skinningPipelineStates_
		[static_cast<uint32_t>(CullMode::kCount)];
	Microsoft::WRL::ComPtr<ID3D12RootSignature> skinningComputeRootSignature_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> skinningComputePipelineState_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> shadowRootSignature_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPipelineStates_
		[static_cast<uint32_t>(CullMode::kCount)];
	Microsoft::WRL::ComPtr<ID3D12PipelineState> skinningShadowPipelineStates_
		[static_cast<uint32_t>(CullMode::kCount)];
	//デフォルトカメラ
	Camera* defaultCamera = nullptr;

	Microsoft::WRL::ComPtr<ID3D12PipelineState> skyboxPipelineState_ = nullptr;

private://メンバクラス
	enum class BlendMode{
		//!< ブレンドなし
		kBlendModeNone,
		//!< 通常アルファブレンド
		kBlendModeNormal,
		//!< 加算
		kBlendModeAdd,
		//!< 減算
		kBlendModeSubtract,
		//!< 乗算
		kBlendModeMultiply,
		//!< スクリーン
		kBlendModeScreen,
		//利用してはいけない
		kCountOfBlendMode,
	};
};

