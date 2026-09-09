// 役割: ScreenOverlayの基準Canvasと実Viewport間のuniform layoutを共有する内部utility。
#pragma once

#include <cstdint>
#include <string>

#include "../../../engine/math/Vector2.h"

class SceneDocument;
struct SceneComponent;

struct SceneScreenOverlayCanvasLayout {
	float scale = 1.0f;
	Vector2 origin = { 0.0f, 0.0f };
	Vector2 referenceSize = { 0.0f, 0.0f };
	uint32_t viewportWidth = 0;
	uint32_t viewportHeight = 0;
	bool responsive = false;

	Vector2 ResolvePosition(
		const Vector2& viewportAnchor,
		const Vector2& authoredPixelOffset
	) const;
	Vector2 ScalePixelOffset(const Vector2& authoredPixelOffset) const;
	Vector2 ScaleSize(const Vector2& authoredSize) const;
};

SceneScreenOverlayCanvasLayout ResolveSceneScreenOverlayCanvasLayout(
	const SceneDocument& document,
	const SceneComponent& renderer,
	uint32_t viewportWidth,
	uint32_t viewportHeight
);
