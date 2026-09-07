// 役割: Resource fontをDirectWriteで組版し、software WIC/D2Dで透過bitmapへ変換する。
#include "TextResourceFontRasterizer.h"

#include "TextFontRegistry.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")

namespace {
	using Microsoft::WRL::ComPtr;

	std::wstring ToWide(const std::string& value) {
		if (value.empty() || value.size() > static_cast<size_t>(INT_MAX)) {
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
		if (MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			value.data(),
			static_cast<int>(value.size()),
			result.data(),
			length
		) != length) {
			return {};
		}
		return result;
	}

	uint8_t ToByte(float value) {
		return static_cast<uint8_t>(std::round(
			std::clamp(value, 0.0f, 1.0f) * 255.0f
		));
	}

	void CompositeMask(
		std::vector<uint8_t>& destination,
		const std::vector<uint8_t>& mask,
		uint32_t width,
		uint32_t height,
		int offsetX,
		int offsetY,
		const Vector4& color,
		float opacity
	) {
		const float sourceRed = std::clamp(color.x, 0.0f, 1.0f);
		const float sourceGreen = std::clamp(color.y, 0.0f, 1.0f);
		const float sourceBlue = std::clamp(color.z, 0.0f, 1.0f);
		const float sourceOpacity = std::clamp(color.w, 0.0f, 1.0f) *
			std::clamp(opacity, 0.0f, 1.0f);
		for (uint32_t sourceY = 0; sourceY < height; ++sourceY) {
			const int destinationY = static_cast<int>(sourceY) + offsetY;
			if (destinationY < 0 || destinationY >= static_cast<int>(height)) {
				continue;
			}
			for (uint32_t sourceX = 0; sourceX < width; ++sourceX) {
				const int destinationX = static_cast<int>(sourceX) + offsetX;
				if (destinationX < 0 || destinationX >= static_cast<int>(width)) {
					continue;
				}
				const size_t sourceIndex = static_cast<size_t>(sourceY) * width + sourceX;
				const float sourceAlpha =
					(static_cast<float>(mask[sourceIndex]) / 255.0f) * sourceOpacity;
				if (sourceAlpha <= 0.0f) {
					continue;
				}
				const size_t destinationIndex =
					(static_cast<size_t>(destinationY) * width +
						static_cast<size_t>(destinationX)) * 4;
				uint8_t* pixel = destination.data() + destinationIndex;
				const float destinationAlpha = static_cast<float>(pixel[3]) / 255.0f;
				const float outputAlpha = sourceAlpha +
					destinationAlpha * (1.0f - sourceAlpha);
				if (outputAlpha <= 0.0f) {
					continue;
				}
				const float destinationBlue = static_cast<float>(pixel[0]) / 255.0f;
				const float destinationGreen = static_cast<float>(pixel[1]) / 255.0f;
				const float destinationRed = static_cast<float>(pixel[2]) / 255.0f;
				pixel[0] = ToByte((sourceBlue * sourceAlpha + destinationBlue *
					destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
				pixel[1] = ToByte((sourceGreen * sourceAlpha + destinationGreen *
					destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
				pixel[2] = ToByte((sourceRed * sourceAlpha + destinationRed *
					destinationAlpha * (1.0f - sourceAlpha)) / outputAlpha);
				pixel[3] = ToByte(outputAlpha);
			}
		}
	}

	DWRITE_TEXT_ALIGNMENT ResolveHorizontalAlignment(const std::string& value) {
		if (value == "Center") return DWRITE_TEXT_ALIGNMENT_CENTER;
		if (value == "Right" || value == "Trailing") {
			return DWRITE_TEXT_ALIGNMENT_TRAILING;
		}
		return DWRITE_TEXT_ALIGNMENT_LEADING;
	}

	DWRITE_PARAGRAPH_ALIGNMENT ResolveParagraphAlignment(const std::string& value) {
		if (value == "Center") return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
		if (value == "Bottom") return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
		return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
	}
}

bool TextResourceFontRasterizer::Rasterize(
	const TextRasterizer::Settings& settings,
	TextRasterizer::Bitmap& bitmap
) const {
	bitmap = {};
	if (!settings.resourceFont || settings.text.empty() ||
		!settings.resourceFont->GetFactory() ||
		!settings.resourceFont->GetCollection() ||
		!std::isfinite(settings.fontSize) || settings.fontSize < 1.0f ||
		settings.fontSize > 512.0f) {
		return false;
	}
	const std::wstring text = ToWide(settings.text);
	const std::wstring familyName = ToWide(settings.fontFamily);
	if (text.empty() || familyName.empty() ||
		text.size() > static_cast<size_t>((std::numeric_limits<UINT32>::max)())) {
		return false;
	}
	UINT32 familyIndex = 0;
	BOOL familyExists = FALSE;
	if (FAILED(settings.resourceFont->GetCollection()->FindFamilyName(
		familyName.c_str(), &familyIndex, &familyExists
	)) || !familyExists) {
		return false;
	}

	ComPtr<IDWriteTextFormat> format;
	const DWRITE_FONT_WEIGHT weight = settings.bold
		? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
	const DWRITE_FONT_STYLE style = settings.italic
		? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
	if (FAILED(settings.resourceFont->GetFactory()->CreateTextFormat(
		familyName.c_str(),
		settings.resourceFont->GetCollection(),
		weight,
		style,
		DWRITE_FONT_STRETCH_NORMAL,
		settings.fontSize,
		L"ja-jp",
		&format
	)) || !format) {
		return false;
	}
	if (FAILED(format->SetTextAlignment(ResolveHorizontalAlignment(
		settings.horizontalAlignment
	))) || FAILED(format->SetParagraphAlignment(ResolveParagraphAlignment(
		settings.verticalAlignment
	))) || FAILED(format->SetWordWrapping(settings.wordWrap
		? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP))) {
		return false;
	}

	float layoutWidth = settings.layoutSize.x > 0.0f
		? settings.layoutSize.x : 4096.0f;
	float layoutHeight = settings.layoutSize.y > 0.0f
		? settings.layoutSize.y : 4096.0f;
	layoutWidth = std::clamp(layoutWidth, 1.0f, 4096.0f);
	layoutHeight = std::clamp(layoutHeight, 1.0f, 4096.0f);
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(settings.resourceFont->GetFactory()->CreateTextLayout(
		text.c_str(),
		static_cast<UINT32>(text.size()),
		format.Get(),
		layoutWidth,
		layoutHeight,
		&layout
	)) || !layout) {
		return false;
	}

	DWRITE_FONT_METRICS fontMetrics{};
	ComPtr<IDWriteFontFamily> family;
	ComPtr<IDWriteFont> font;
	if (SUCCEEDED(settings.resourceFont->GetCollection()->GetFontFamily(
		familyIndex, &family
	)) && family && SUCCEEDED(family->GetFirstMatchingFont(
		weight, DWRITE_FONT_STRETCH_NORMAL, style, &font
	)) && font) {
		font->GetMetrics(&fontMetrics);
	}
	float lineHeight = settings.fontSize * 1.2f *
		std::clamp(settings.lineSpacing, 0.1f, 8.0f);
	float baseline = (std::min)(settings.fontSize, lineHeight);
	if (fontMetrics.designUnitsPerEm != 0) {
		const float scale = settings.fontSize /
			static_cast<float>(fontMetrics.designUnitsPerEm);
		lineHeight = (fontMetrics.ascent + fontMetrics.descent + fontMetrics.lineGap) *
			scale * std::clamp(settings.lineSpacing, 0.1f, 8.0f);
		baseline = fontMetrics.ascent * scale;
		lineHeight = (std::max)(lineHeight, 1.0f);
		baseline = std::clamp(baseline, 0.0f, lineHeight);
	}
	ComPtr<IDWriteTextLayout3> layout3;
	if (SUCCEEDED(layout.As(&layout3)) && layout3) {
		const DWRITE_LINE_SPACING lineSpacing{
			DWRITE_LINE_SPACING_METHOD_UNIFORM,
			lineHeight,
			baseline,
			0.0f,
			DWRITE_FONT_LINE_GAP_USAGE_DEFAULT
		};
		if (FAILED(layout3->SetLineSpacing(&lineSpacing))) {
			return false;
		}
	}
	ComPtr<IDWriteTextLayout1> layout1;
	if (SUCCEEDED(layout.As(&layout1)) && layout1 && FAILED(layout1->SetCharacterSpacing(
		0.0f, settings.characterSpacing, 0.0f,
		DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(text.size()) }
	))) {
		return false;
	}
	if (settings.overflowMode == "Ellipsis") {
		ComPtr<IDWriteInlineObject> ellipsis;
		DWRITE_TRIMMING trimming{
			DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0
		};
		if (FAILED(settings.resourceFont->GetFactory()->CreateEllipsisTrimmingSign(
			format.Get(), &ellipsis
		)) || FAILED(layout->SetTrimming(&trimming, ellipsis.Get()))) {
			return false;
		}
	}

	DWRITE_TEXT_METRICS metrics{};
	if (FAILED(layout->GetMetrics(&metrics))) {
		return false;
	}
	const uint32_t measuredWidth = static_cast<uint32_t>(std::clamp(
		std::ceil(metrics.widthIncludingTrailingWhitespace), 1.0f, 4096.0f
	));
	const uint32_t measuredHeight = static_cast<uint32_t>(std::clamp(
		std::ceil(metrics.height), 1.0f, 4096.0f
	));
	const uint32_t contentWidth = settings.layoutSize.x > 0.0f
		? static_cast<uint32_t>(std::clamp(std::ceil(settings.layoutSize.x), 1.0f, 4096.0f))
		: measuredWidth;
	const uint32_t contentHeight = settings.layoutSize.y > 0.0f
		? static_cast<uint32_t>(std::clamp(std::ceil(settings.layoutSize.y), 1.0f, 4096.0f))
		: measuredHeight;
	layout->SetMaxWidth(static_cast<float>(contentWidth));
	layout->SetMaxHeight(static_cast<float>(contentHeight));
	const int outlinePadding = settings.outlineEnabled
		? static_cast<int>(std::ceil(std::clamp(settings.outlineWidth, 0.0f, 32.0f))) : 0;
	const int shadowPadding = settings.shadowEnabled
		? static_cast<int>(std::ceil((std::max)(
			std::abs(settings.shadowOffset.x), std::abs(settings.shadowOffset.y)
		))) : 0;
	const int padding = outlinePadding + shadowPadding + 2;
	const uint32_t width = static_cast<uint32_t>((std::min)(
		static_cast<int>(contentWidth) + padding * 2, 4096
	));
	const uint32_t height = static_cast<uint32_t>((std::min)(
		static_cast<int>(contentHeight) + padding * 2, 4096
	));
	const size_t pixelCount = static_cast<size_t>(width) * height;
	if (pixelCount == 0 || pixelCount > static_cast<size_t>(4096) * 4096 ||
		pixelCount > (std::numeric_limits<size_t>::max)() / 4) {
		return false;
	}

	ComPtr<ID2D1Factory> d2dFactory;
	if (FAILED(D2D1CreateFactory(
		D2D1_FACTORY_TYPE_SINGLE_THREADED,
		IID_PPV_ARGS(&d2dFactory)
	)) || !d2dFactory) {
		return false;
	}
	ComPtr<IWICImagingFactory> wicFactory;
	if (FAILED(CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&wicFactory)
	)) || !wicFactory) {
		return false;
	}
	ComPtr<IWICBitmap> wicBitmap;
	if (FAILED(wicFactory->CreateBitmap(
		width,
		height,
		GUID_WICPixelFormat32bppPBGRA,
		WICBitmapCacheOnLoad,
		&wicBitmap
	)) || !wicBitmap) {
		return false;
	}
	const D2D1_RENDER_TARGET_PROPERTIES renderProperties =
		D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_SOFTWARE,
			D2D1::PixelFormat(
				DXGI_FORMAT_B8G8R8A8_UNORM,
				D2D1_ALPHA_MODE_PREMULTIPLIED
			),
			96.0f,
			96.0f,
			D2D1_RENDER_TARGET_USAGE_NONE,
			D2D1_FEATURE_LEVEL_DEFAULT
		);
	ComPtr<ID2D1RenderTarget> renderTarget;
	if (FAILED(d2dFactory->CreateWicBitmapRenderTarget(
		wicBitmap.Get(), renderProperties, &renderTarget
	)) || !renderTarget) {
		return false;
	}
	ComPtr<ID2D1SolidColorBrush> brush;
	if (FAILED(renderTarget->CreateSolidColorBrush(
		D2D1::ColorF(D2D1::ColorF::White), &brush
	)) || !brush) {
		return false;
	}
	renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	renderTarget->BeginDraw();
	renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));
	renderTarget->DrawTextLayout(
		D2D1::Point2F(static_cast<float>(padding), static_cast<float>(padding)),
		layout.Get(),
		brush.Get(),
		D2D1_DRAW_TEXT_OPTIONS_NONE
	);
	if (FAILED(renderTarget->EndDraw())) {
		return false;
	}
	std::vector<uint8_t> pixels(pixelCount * 4, 0);
	if (FAILED(wicBitmap->CopyPixels(
		nullptr,
		width * 4,
		static_cast<UINT>(pixels.size()),
		pixels.data()
	))) {
		return false;
	}
	std::vector<uint8_t> mask(pixelCount, 0);
	for (size_t index = 0; index < pixelCount; ++index) {
		const uint8_t* pixel = pixels.data() + index * 4;
		mask[index] = (std::max)(pixel[0], (std::max)(pixel[1], pixel[2]));
	}
	std::vector<uint8_t> output(pixelCount * 4, 0);
	if (settings.shadowEnabled) {
		CompositeMask(output, mask, width, height,
			static_cast<int>(std::round(settings.shadowOffset.x)),
			static_cast<int>(std::round(settings.shadowOffset.y)),
			settings.shadowColor, settings.opacity);
	}
	if (settings.outlineEnabled && outlinePadding > 0) {
		for (int y = -outlinePadding; y <= outlinePadding; ++y) {
			for (int x = -outlinePadding; x <= outlinePadding; ++x) {
				if ((x == 0 && y == 0) ||
					x * x + y * y > outlinePadding * outlinePadding) {
					continue;
				}
				CompositeMask(output, mask, width, height, x, y,
					settings.outlineColor, settings.opacity);
			}
		}
	}
	CompositeMask(output, mask, width, height, 0, 0,
		settings.color, settings.opacity);
	bitmap.width = width;
	bitmap.height = height;
	bitmap.bgraPixels = std::move(output);
	return true;
}
