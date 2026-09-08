// 役割: テクスチャの読み込み、SRV割り当て、キャッシュを管理する。
#pragma once
#include <string>
#include <DirectXTex.h>
#include <wrl.h>  
#include "d3dx12.h"
#include <unordered_map>
#include <unordered_set>

class DirectXCommon;
class SrvManager;

class TextureManager{
private:
	//テクスチャ一枚分のデータ
	struct  TextureData{
		DirectX::TexMetadata metadata;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		uint32_t srvIndex;
		D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU;
		D3D12_GPU_DESCRIPTOR_HANDLE srvHandleGPU;
	};
private:
	static TextureManager* instance;

	TextureManager() = default;
	~TextureManager() = default;
	TextureManager(TextureManager&) = delete;
	TextureManager& operator=(TextureManager&) = delete;

public:
	enum class TextureColorSpace {
		Automatic,
		Srgb,
		Linear
	};

	//初期化
	void Initialize(DirectXCommon* directXCommon,SrvManager* srvManager);
	//シングルトンインスタンスの取得
	static TextureManager* GetInstance();
	//終了
	void Finalize();

	// メタデータの取得
	const DirectX::TexMetadata& GetMetaData(const std::string& filePath);
	// SRVインデックスの取得
	uint32_t GetSrvIndex(const std::string& filePath);
	// GPUハンドルの取得
	D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandleGPU(const std::string& filePath);


	/// <summary>
/// テクスチャファイルの読み込み
/// </summary>
	bool LoadTexture(
		const std::string& filePath,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	bool ReloadTexture(
		const std::string& filePath,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	bool HasTexture(const std::string& textureKey) const;
	// 管理対象のTextureと対応するSRVスロットを解除する。
	bool ReleaseTexture(const std::string& textureKey);
	bool LoadTextureFromMemory(
		const std::string& textureKey,
		const uint8_t* data,
		size_t dataSize,
		const std::string& formatHint,
		TextureColorSpace colorSpace = TextureColorSpace::Automatic
	);
	// Runtime生成Textureを同じkeyへ再登録する場合は、既存SRVを再利用する。
	bool UpdateTextureFromPixels(
		const std::string& textureKey,
		const uint8_t* pixels,
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
	);
	void ClearFailedTextureCache();
private:
	bool RegisterTexture(
		const std::string& textureKey,
		const DirectX::ScratchImage& loadedImage,
		const DirectX::TexMetadata& metadata
	);

	//DirectXCommonを参照
	DirectXCommon* dxCommon;
	//SrvManagerも参照
	SrvManager* srvManager;
	//SRVインデックス開始番号
	static uint32_t kSRVIndexTop;
	//テクスチャデータ
	std::unordered_map<std::string, TextureData> textureDatas;
	// 読み込み失敗を毎フレーム繰り返さないための負のキャッシュ
	std::unordered_set<std::string> failedTextureKeys;
};

