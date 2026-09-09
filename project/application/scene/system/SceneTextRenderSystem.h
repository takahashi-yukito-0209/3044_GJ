// 役割: TextRenderer ComponentのRuntime texture cacheと2D描画空間の振り分けを所有する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "../../../engine/2d/TextSprite.h"
#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector4.h"

class DirectXCommon;
class SceneDocument;
class TextFontResource;

class SceneTextRenderSystem {
public:
	~SceneTextRenderSystem();
	void Initialize(DirectXCommon* dxCommon, std::string runtimeKey);
	void SetTextOverride(uint64_t entityId, std::string text);
	void ClearTextOverrides();
	void SetTextColorOverride(uint64_t entityId, const Vector4& color);
	void ClearTextColorOverrides();
	// 画面座標をビューポート比率で上書きする。World座標に追従するHUDに使う。
	void SetViewportPositionOverride(uint64_t entityId, const Vector2& position);
	void ClearViewportPositionOverrides();
	void SetPresentationOverride(
		uint64_t entityId,
		const Vector2& positionOffset,
		float rotationOffset,
		const Vector2& scaleMultiplier,
		float opacityMultiplier
	);
	void ClearPresentationOverrides();
	void Sync(SceneDocument* document);
	void DrawScene2D(const SceneDocument& document, uint32_t width, uint32_t height) const;
	void DrawScreenOverlay(const SceneDocument& document, uint32_t width, uint32_t height);
	bool HasScreenOverlay(const SceneDocument& document) const;
	void Finalize();

private:
	struct RuntimeText {
		std::unique_ptr<TextSprite> sprite;
		std::shared_ptr<const TextFontResource> fontLease;
		std::string textureKey;
		std::string contentSignature;
		std::string fontResolutionKey;
		std::string fontDiagnostic;
		Vector2 bitmapSize{};
		float rasterScale = 1.0f;
		float screenOverlayScale = 1.0f;
		bool spriteInitialized = false;
	};
	struct PresentationOverride {
		Vector2 positionOffset{};
		float rotationOffset = 0.0f;
		Vector2 scaleMultiplier = { 1.0f, 1.0f };
		float opacityMultiplier = 1.0f;
	};

	const PresentationOverride* FindPresentationOverride(
		uint64_t entityId
	) const;

	DirectXCommon* dxCommon_ = nullptr;
	std::string runtimeKey_;
	std::unordered_map<uint64_t, RuntimeText> texts_;
	std::unordered_map<uint64_t, std::string> textOverrides_;
	std::unordered_map<uint64_t, Vector4> textColorOverrides_;
	std::unordered_map<uint64_t, Vector2> viewportPositionOverrides_;
	std::unordered_map<uint64_t, PresentationOverride> presentationOverrides_;
	uint32_t lastScreenOverlayViewportWidth_ = 0;
	uint32_t lastScreenOverlayViewportHeight_ = 0;
};
