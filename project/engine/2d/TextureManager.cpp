// 役割: DirectXTexを使ったテクスチャ読み込みとGPUリソース生成を実装する。
#include "TextureManager.h"
#include "TextureFormat.h"
#include "../base/DirectXCommon.h"
#include "../3d/SrvManager.h"
#include "../utility/StringUtility.h"
#include "../utility/Logger.h"
#include "../utility/EditableResourcePath.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace {

	DirectX::TGA_FLAGS GetTgaFlags(
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		if (colorSpace == TextureManager::TextureColorSpace::Srgb) {
			return DirectX::TGA_FLAGS_FORCE_SRGB;
		}
		if (colorSpace == TextureManager::TextureColorSpace::Linear) {
			return DirectX::TGA_FLAGS_FORCE_LINEAR;
		}
		return defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb
			? DirectX::TGA_FLAGS_DEFAULT_SRGB
			: DirectX::TGA_FLAGS_NONE;
	}

	DirectX::WIC_FLAGS GetWicFlags(
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		if (colorSpace == TextureManager::TextureColorSpace::Srgb) {
			return DirectX::WIC_FLAGS_FORCE_SRGB;
		}
		if (colorSpace == TextureManager::TextureColorSpace::Linear) {
			return DirectX::WIC_FLAGS_FORCE_LINEAR;
		}
		return defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb
			? DirectX::WIC_FLAGS_DEFAULT_SRGB
			: DirectX::WIC_FLAGS_NONE;
	}

	void ApplyColorSpace(
		DirectX::ScratchImage& image,
		DirectX::TexMetadata& metadata,
		TextureManager::TextureColorSpace colorSpace,
		TextureFormat::DefaultColorSpace defaultColorSpace
	) {
		const bool useSrgb = colorSpace == TextureManager::TextureColorSpace::Srgb ||
			(colorSpace == TextureManager::TextureColorSpace::Automatic &&
			 defaultColorSpace == TextureFormat::DefaultColorSpace::Srgb);
		const bool useLinear = colorSpace == TextureManager::TextureColorSpace::Linear ||
			(colorSpace == TextureManager::TextureColorSpace::Automatic &&
			 defaultColorSpace == TextureFormat::DefaultColorSpace::Linear);
		if (!useSrgb && !useLinear) {
			return;
		}

		const DXGI_FORMAT targetFormat = useSrgb
			? DirectX::MakeSRGB(metadata.format)
			: DirectX::MakeLinear(metadata.format);
		if (targetFormat != metadata.format && image.OverrideFormat(targetFormat)) {
			metadata = image.GetMetadata();
		}
	}

	std::string FormatHResult(HRESULT result) {
		std::ostringstream stream;
		stream << "0x" << std::uppercase << std::hex
			<< static_cast<unsigned long>(result);
		return stream.str();
	}

}

TextureManager* TextureManager::instance = nullptr;

uint32_t TextureManager::kSRVIndexTop = 1;

bool TextureManager::LoadTexture(
	const std::string& filePath,
	TextureColorSpace colorSpace
) {

	// 読み込み済みテクスチャを検索
	if (textureDatas.contains(filePath)) {
		return true;
	}
	if (failedTextureKeys.contains(filePath)) {
		return false;
	}

	DirectX::ScratchImage loadedImage{};
	DirectX::TexMetadata metadata{};
	const TextureFormat::Descriptor* format = TextureFormat::FindByPath(
		StringUtility::ToPath(filePath)
	);
	if (format == nullptr) {
		Logger::Log("Unsupported texture format: " + filePath + "\n");
		failedTextureKeys.insert(filePath);
		return false;
	}

	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(filePath));
	const std::wstring filePathW = resolvedPath.wstring();

	HRESULT hr = S_OK;
	switch (format->decoder) {
	case TextureFormat::Decoder::Dds:
		hr = DirectX::LoadFromDDSFile(
			filePathW.c_str(),
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Tga:
		hr = DirectX::LoadFromTGAFile(
			filePathW.c_str(),
			GetTgaFlags(colorSpace, format->colorSpace),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Hdr:
		hr = DirectX::LoadFromHDRFile(
			filePathW.c_str(),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Wic:
		hr = DirectX::LoadFromWICFile(
			filePathW.c_str(),
			GetWicFlags(colorSpace, format->colorSpace),
			&metadata,
			loadedImage
		);
		break;
	}

	if (FAILED(hr)) {
		Logger::Log(
			"Failed to load texture: " + filePath +
			" (HRESULT " + FormatHResult(hr) + ")\n"
		);
		failedTextureKeys.insert(filePath);
		return false;
	}

	ApplyColorSpace(loadedImage, metadata, colorSpace, format->colorSpace);
	const bool registered = RegisterTexture(filePath, loadedImage, metadata);
	if (registered) {
		failedTextureKeys.erase(filePath);
	}
	else {
		failedTextureKeys.insert(filePath);
	}
	return registered;
}

bool TextureManager::ReloadTexture(
	const std::string& filePath,
	TextureColorSpace colorSpace
) {
	ReleaseTexture(filePath);
	failedTextureKeys.erase(filePath);
	return LoadTexture(filePath, colorSpace);
}

bool TextureManager::LoadTextureFromMemory(
	const std::string& textureKey,
	const uint8_t* data,
	size_t dataSize,
	const std::string& formatHint,
	TextureColorSpace colorSpace
) {
	if (textureDatas.contains(textureKey)) {
		return true;
	}
	if (failedTextureKeys.contains(textureKey)) {
		return false;
	}
	if (data == nullptr || dataSize == 0) {
		Logger::Log("Embedded texture data is empty: " + textureKey + "\n");
		failedTextureKeys.insert(textureKey);
		return false;
	}

	const TextureFormat::Descriptor* format =
		TextureFormat::FindByExtension(formatHint);
	const TextureFormat::Decoder decoder = format != nullptr
		? format->decoder
		: TextureFormat::Decoder::Wic;
	const TextureFormat::DefaultColorSpace defaultColorSpace = format != nullptr
		? format->colorSpace
		: TextureFormat::DefaultColorSpace::Srgb;

	DirectX::ScratchImage loadedImage{};
	DirectX::TexMetadata metadata{};
	HRESULT hr = S_OK;
	switch (decoder) {
	case TextureFormat::Decoder::Dds:
		hr = DirectX::LoadFromDDSMemory(
			data,
			dataSize,
			DirectX::DDS_FLAGS_NONE,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Tga:
		hr = DirectX::LoadFromTGAMemory(
			data,
			dataSize,
			GetTgaFlags(colorSpace, defaultColorSpace),
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Hdr:
		hr = DirectX::LoadFromHDRMemory(
			data,
			dataSize,
			&metadata,
			loadedImage
		);
		break;
	case TextureFormat::Decoder::Wic:
		hr = DirectX::LoadFromWICMemory(
			data,
			dataSize,
			GetWicFlags(colorSpace, defaultColorSpace),
			&metadata,
			loadedImage
		);
		break;
	}

	if (FAILED(hr)) {
		Logger::Log(
			"Failed to load embedded texture: " + textureKey +
			" (HRESULT " + FormatHResult(hr) + ")\n"
		);
		failedTextureKeys.insert(textureKey);
		return false;
	}

	ApplyColorSpace(loadedImage, metadata, colorSpace, defaultColorSpace);
	const bool registered = RegisterTexture(textureKey, loadedImage, metadata);
	if (registered) {
		failedTextureKeys.erase(textureKey);
	}
	else {
		failedTextureKeys.insert(textureKey);
	}
	return registered;
}

bool TextureManager::RegisterTexture(
	const std::string& textureKey,
	const DirectX::ScratchImage& loadedImage,
	const DirectX::TexMetadata& metadata
) {
	if (loadedImage.GetImageCount() == 0 || loadedImage.GetImages() == nullptr) {
		Logger::Log("Texture contains no images: " + textureKey + "\n");
		return false;
	}

	const bool reusesExistingDescriptor = textureDatas.contains(textureKey);
	if (!reusesExistingDescriptor) {
		assert(srvManager->CanAllocate());
	}

	DirectX::ScratchImage mipImages{};
	const DirectX::ScratchImage* uploadImage = &loadedImage;
	HRESULT hr = S_OK;

	// 圧縮フォーマットは DirectXTex の GenerateMipMaps が直接扱えないことがある
	if (!DirectX::IsCompressed(metadata.format) && metadata.mipLevels <= 1) {
		const DirectX::TEX_FILTER_FLAGS filter = DirectX::IsSRGB(metadata.format)
			? DirectX::TEX_FILTER_SRGB
			: DirectX::TEX_FILTER_DEFAULT;
		hr = DirectX::GenerateMipMaps(
			loadedImage.GetImages(),
			loadedImage.GetImageCount(),
			loadedImage.GetMetadata(),
			filter,
			0,
			mipImages
		);
		if (SUCCEEDED(hr)) {
			uploadImage = &mipImages;
		}
	}

	TextureData& textureData = textureDatas[textureKey];

	textureData.metadata = uploadImage->GetMetadata();
	textureData.resource = dxCommon->CreateTextureResource(textureData.metadata);

	// 既存keyの更新はDescriptorを再利用し、Runtime編集でSRVを増やさない。
	if (!reusesExistingDescriptor) {
		textureData.srvIndex = srvManager->Allocate();
		textureData.srvHandleCPU = srvManager->GetCPUDescriptorHandle(textureData.srvIndex);
		textureData.srvHandleGPU = srvManager->GetGPUDescriptorHandle(textureData.srvIndex);
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = textureData.metadata.format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	if (textureData.metadata.IsCubemap()) {
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.MipLevels = UINT(textureData.metadata.mipLevels);
		srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	}
	else {
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = UINT(textureData.metadata.mipLevels);
	}

	dxCommon->GetDevice()->CreateShaderResourceView(
		textureData.resource.Get(),
		&srvDesc,
		textureData.srvHandleCPU
	);

	Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource;
	intermediateResource.Attach(
		dxCommon->UploadTextureData(textureData.resource, *uploadImage)
	);
	if (!intermediateResource) {
		const uint32_t srvIndex = textureData.srvIndex;
		textureDatas.erase(textureKey);
		srvManager->Free(srvIndex);
		Logger::Log("Failed to upload texture: " + textureKey + "\n");
		return false;
	}

	dxCommon->ExecuteCommandListAndWait();
	return true;
}

bool TextureManager::UpdateTextureFromPixels(
	const std::string& textureKey,
	const uint8_t* pixels,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format
) {
	if (textureKey.empty() || pixels == nullptr || width == 0 || height == 0) {
		return false;
	}

	DirectX::ScratchImage image{};
	const HRESULT initializeResult = image.Initialize2D(
		format,
		width,
		height,
		1,
		1
	);
	if (FAILED(initializeResult) || image.GetPixels() == nullptr) {
		Logger::Log("Failed to create runtime texture: " + textureKey + "\n");
		return false;
	}

	const size_t rowBytes = static_cast<size_t>(width) * 4;
	const DirectX::Image* targetImage = image.GetImage(0, 0, 0);
	if (!targetImage || targetImage->rowPitch < rowBytes) {
		return false;
	}
	for (uint32_t row = 0; row < height; ++row) {
		std::memcpy(
			image.GetPixels() + targetImage->rowPitch * row,
			pixels + rowBytes * row,
			rowBytes
		);
	}

	const bool registered = RegisterTexture(
		textureKey,
		image,
		image.GetMetadata()
	);
	if (registered) {
		failedTextureKeys.erase(textureKey);
	}
	return registered;
}

bool TextureManager::HasTexture(const std::string& textureKey) const {
	return textureDatas.contains(textureKey);
}

bool TextureManager::ReleaseTexture(const std::string& textureKey) {
	auto iterator = textureDatas.find(textureKey);
	if (iterator == textureDatas.end()) {
		return false;
	}

	const uint32_t srvIndex = iterator->second.srvIndex;
	textureDatas.erase(iterator);
	failedTextureKeys.erase(textureKey);
	srvManager->Free(srvIndex);
	return true;
}

void TextureManager::ClearFailedTextureCache() {
	failedTextureKeys.clear();
}

const DirectX::TexMetadata& TextureManager::GetMetaData(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.metadata;
}

uint32_t TextureManager::GetSrvIndex(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.srvIndex;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSrvHandleGPU(const std::string& filePath){
	auto it = textureDatas.find(filePath);
	assert(it != textureDatas.end());
	return it->second.srvHandleGPU;
}

void TextureManager::Initialize(DirectXCommon* directXCommon, SrvManager* srvManager){
	this->srvManager = srvManager;
	this->dxCommon = directXCommon;

}

TextureManager* TextureManager::GetInstance(){
	if(instance == nullptr){
		instance = new TextureManager;
	}
	return instance;
}

void TextureManager::Finalize(){
	delete instance;
	instance = nullptr;
}
