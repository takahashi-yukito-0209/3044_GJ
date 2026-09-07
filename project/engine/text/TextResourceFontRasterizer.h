// 役割: Resource fontのDirectWrite／software rasterizerを所有する。
#pragma once

#include "TextRasterizer.h"

class TextResourceFontRasterizer {
public:
	bool Rasterize(
		const TextRasterizer::Settings& settings,
		TextRasterizer::Bitmap& bitmap
	) const;
};
