// 役割: UTF-8 TextRenderer設定をWindows Fontで透過bitmapへ変換する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../math/Vector2.h"
#include "../math/Vector4.h"

class TextFontResource;

class TextRasterizer {
public:
	struct Settings {
		std::string text;
		std::string fontFamily;
		float fontSize = 32.0f;
		bool bold = false;
		bool italic = false;
		Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		float opacity = 1.0f;
		std::string horizontalAlignment = "Left";
		std::string verticalAlignment = "Top";
		bool wordWrap = false;
		std::string overflowMode = "Overflow";
		Vector2 layoutSize = { 0.0f, 0.0f };
		float characterSpacing = 0.0f;
		float lineSpacing = 1.0f;
		bool outlineEnabled = false;
		Vector4 outlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		float outlineWidth = 2.0f;
		bool shadowEnabled = false;
		Vector4 shadowColor = { 0.0f, 0.0f, 0.0f, 0.5f };
		Vector2 shadowOffset = { 2.0f, 2.0f };
		std::shared_ptr<const TextFontResource> resourceFont;
	};

	struct Bitmap {
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> bgraPixels;

		bool IsValid() const {
			return width > 0 && height > 0 && !bgraPixels.empty();
		}
	};

	bool Rasterize(const Settings& settings, Bitmap& bitmap) const;
};
