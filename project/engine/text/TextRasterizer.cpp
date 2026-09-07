// 役割: Windows GDIのSystem Font fallbackを使いText bitmapを生成する。
#include "TextRasterizer.h"

#include "TextResourceFontRasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <windows.h>
#pragma comment(lib, "gdi32.lib")

namespace {
	std::wstring ToWide(const std::string& value) {
		if (value.empty()) {
			return {};
		}
		const int length = MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0
		);
		if (length <= 0) {
			return {};
		}
		std::wstring result(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			result.data(),
			length
		);
		return result;
	}

	BYTE ToByte(float value) {
		return static_cast<BYTE>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	}

	void CompositeMask(
		std::vector<uint8_t>& destination,
		const uint8_t* mask,
		size_t pixelCount,
		const Vector4& color,
		float opacity
	) {
		const float sourceRed = std::clamp(color.x, 0.0f, 1.0f);
		const float sourceGreen = std::clamp(color.y, 0.0f, 1.0f);
		const float sourceBlue = std::clamp(color.z, 0.0f, 1.0f);
		const float sourceOpacity =
			std::clamp(color.w, 0.0f, 1.0f) * std::clamp(opacity, 0.0f, 1.0f);
		for (size_t index = 0; index < pixelCount; ++index) {
			const float maskAlpha = static_cast<float>(mask[index]) / 255.0f;
			const float sourceAlpha = maskAlpha * sourceOpacity;
			if (sourceAlpha <= 0.0f) {
				continue;
			}
			uint8_t* destinationPixel = destination.data() + index * 4;
			const float destinationAlpha = static_cast<float>(destinationPixel[3]) / 255.0f;
			const float outputAlpha = sourceAlpha + destinationAlpha * (1.0f - sourceAlpha);
			const float destinationBlue = static_cast<float>(destinationPixel[0]) / 255.0f;
			const float destinationGreen = static_cast<float>(destinationPixel[1]) / 255.0f;
			const float destinationRed = static_cast<float>(destinationPixel[2]) / 255.0f;
			if (outputAlpha > 0.0f) {
				destinationPixel[0] = ToByte((sourceBlue * sourceAlpha +
					destinationBlue * destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
				destinationPixel[1] = ToByte((sourceGreen * sourceAlpha +
					destinationGreen * destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
				destinationPixel[2] = ToByte((sourceRed * sourceAlpha +
					destinationRed * destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
			}
			destinationPixel[3] = ToByte(outputAlpha);
		}
	}
}

bool TextRasterizer::Rasterize(const Settings& settings, Bitmap& bitmap) const {
	bitmap = {};
	if (settings.resourceFont) {
		TextResourceFontRasterizer resourceRasterizer;
		if (resourceRasterizer.Rasterize(settings, bitmap)) {
			return true;
		}
		Settings fallback = settings;
		fallback.resourceFont.reset();
		fallback.fontFamily = "Yu Gothic UI";
		return Rasterize(fallback, bitmap);
	}
	const std::wstring text = ToWide(settings.text);
	if (text.empty()) {
		return false;
	}

	HDC deviceContext = CreateCompatibleDC(nullptr);
	if (!deviceContext) {
		return false;
	}
	const std::wstring family = ToWide(
		settings.fontFamily.empty() ? "Yu Gothic UI" : settings.fontFamily
	);
	HFONT font = CreateFontW(
		-static_cast<LONG>(std::round(std::clamp(settings.fontSize, 1.0f, 512.0f))),
		0,
		0,
		0,
		settings.bold ? FW_BOLD : FW_NORMAL,
		settings.italic ? TRUE : FALSE,
		FALSE,
		FALSE,
		DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE,
		family.c_str()
	);
	if (!font) {
		DeleteDC(deviceContext);
		return false;
	}
	const HGDIOBJ previousFont = SelectObject(deviceContext, font);
	SetBkMode(deviceContext, TRANSPARENT);
	SetTextCharacterExtra(
		deviceContext,
		static_cast<int>(std::round(settings.characterSpacing))
	);

	UINT flags = DT_NOPREFIX | DT_EXPANDTABS;
	if (settings.wordWrap) {
		flags |= DT_WORDBREAK;
	}
	if (settings.horizontalAlignment == "Center") {
		flags |= DT_CENTER;
	} else if (settings.horizontalAlignment == "Right") {
		flags |= DT_RIGHT;
	} else {
		flags |= DT_LEFT;
	}
	if (settings.overflowMode == "Ellipsis") {
		flags |= DT_END_ELLIPSIS;
	}

	const int requestedWidth = settings.layoutSize.x > 0.0f
		? static_cast<int>(std::ceil(settings.layoutSize.x)) : 4096;
	RECT measuredRect{ 0, 0, requestedWidth, 4096 };
	DrawTextW(
		deviceContext,
		text.c_str(),
		static_cast<int>(text.size()),
		&measuredRect,
		flags | DT_CALCRECT
	);
	const int contentWidth = (std::max)(static_cast<int>(measuredRect.right - measuredRect.left), 1);
	const int contentHeight = (std::max)(static_cast<int>(measuredRect.bottom - measuredRect.top), 1);
	const int layoutWidth = settings.layoutSize.x > 0.0f
		? static_cast<int>(std::ceil(settings.layoutSize.x)) : contentWidth;
	const int layoutHeight = settings.layoutSize.y > 0.0f
		? static_cast<int>(std::ceil(settings.layoutSize.y))
		: (std::max)(1, static_cast<int>(std::ceil(
			contentHeight * std::clamp(settings.lineSpacing, 0.1f, 8.0f)
		)));
	const int outlinePadding = settings.outlineEnabled
		? static_cast<int>(std::ceil(std::clamp(settings.outlineWidth, 0.0f, 32.0f)))
		: 0;
	const int shadowPadding = settings.shadowEnabled
		? static_cast<int>(std::ceil((std::max)(
			std::abs(settings.shadowOffset.x), std::abs(settings.shadowOffset.y)
		)))
		: 0;
	const int padding = outlinePadding + shadowPadding + 2;
	const int width = (std::min)(layoutWidth + padding * 2, 4096);
	const int height = (std::min)(layoutHeight + padding * 2, 4096);

	BITMAPINFO bitmapInfo{};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = width;
	bitmapInfo.bmiHeader.biHeight = -height;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;
	void* rawPixels = nullptr;
	HBITMAP dib = CreateDIBSection(
		deviceContext,
		&bitmapInfo,
		DIB_RGB_COLORS,
		&rawPixels,
		nullptr,
		0
	);
	if (!dib || !rawPixels) {
		SelectObject(deviceContext, previousFont);
		DeleteObject(font);
		DeleteDC(deviceContext);
		return false;
	}
	const HGDIOBJ previousBitmap = SelectObject(deviceContext, dib);
	const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
	std::vector<uint8_t> mask(pixelCount);
	std::vector<uint8_t> output(pixelCount * 4, 0);

	auto drawMask = [&](int offsetX, int offsetY, const Vector4& color) {
		std::memset(rawPixels, 0, pixelCount * 4);
		SetTextColor(deviceContext, RGB(255, 255, 255));
		RECT drawRect{ padding + offsetX, padding + offsetY,
			padding + offsetX + layoutWidth, padding + offsetY + layoutHeight };
		if (settings.verticalAlignment == "Center") {
			const int shift = (layoutHeight - contentHeight) / 2;
			drawRect.top += shift;
			drawRect.bottom += shift;
		} else if (settings.verticalAlignment == "Bottom") {
			const int shift = layoutHeight - contentHeight;
			drawRect.top += shift;
			drawRect.bottom += shift;
		}
		DrawTextW(
			deviceContext,
			text.c_str(),
			static_cast<int>(text.size()),
			&drawRect,
			flags
		);
		const uint8_t* source = static_cast<const uint8_t*>(rawPixels);
		for (size_t index = 0; index < pixelCount; ++index) {
			const uint8_t* pixel = source + index * 4;
			mask[index] = (std::max)(pixel[0], (std::max)(pixel[1], pixel[2]));
		}
		CompositeMask(output, mask.data(), pixelCount, color, settings.opacity);
	};

	if (settings.shadowEnabled) {
		drawMask(
			static_cast<int>(std::round(settings.shadowOffset.x)),
			static_cast<int>(std::round(settings.shadowOffset.y)),
			settings.shadowColor
		);
	}
	if (settings.outlineEnabled && outlinePadding > 0) {
		for (int y = -outlinePadding; y <= outlinePadding; ++y) {
			for (int x = -outlinePadding; x <= outlinePadding; ++x) {
				if (x == 0 && y == 0 || x * x + y * y > outlinePadding * outlinePadding) {
					continue;
				}
				drawMask(x, y, settings.outlineColor);
			}
		}
	}
	drawMask(0, 0, settings.color);

	SelectObject(deviceContext, previousBitmap);
	DeleteObject(dib);
	SelectObject(deviceContext, previousFont);
	DeleteObject(font);
	DeleteDC(deviceContext);

	bitmap.width = static_cast<uint32_t>(width);
	bitmap.height = static_cast<uint32_t>(height);
	bitmap.bgraPixels = std::move(output);
	return true;
}
