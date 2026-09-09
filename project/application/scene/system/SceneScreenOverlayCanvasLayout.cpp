// 役割: ScreenOverlay Canvasのactive解決とFit/Cover/Legacy計算を提供する。
#include "SceneScreenOverlayCanvasLayout.h"

#include <algorithm>
#include <cmath>

#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;

	const SceneComponent* FindActiveCanvas(const SceneDocument& document) {
		for (const SceneEntity& entity : document.GetEntities()) {
			if (!IsEntityActiveInHierarchy(document, entity)) {
				continue;
			}
			if (const SceneComponent* canvas =
				FindEnabledComponent(entity, "ScreenOverlayCanvas")) {
				return canvas;
			}
		}
		return nullptr;
	}
}

Vector2 SceneScreenOverlayCanvasLayout::ResolvePosition(
	const Vector2& viewportAnchor,
	const Vector2& authoredPixelOffset
) const {
	if (!responsive) {
		return {
			viewportAnchor.x * static_cast<float>(viewportWidth) + authoredPixelOffset.x,
			viewportAnchor.y * static_cast<float>(viewportHeight) + authoredPixelOffset.y
		};
	}
	return {
		origin.x + viewportAnchor.x * referenceSize.x * scale +
			authoredPixelOffset.x * scale,
		origin.y + viewportAnchor.y * referenceSize.y * scale +
			authoredPixelOffset.y * scale
	};
}

Vector2 SceneScreenOverlayCanvasLayout::ScalePixelOffset(
	const Vector2& authoredPixelOffset
) const {
	return responsive
		? Vector2{ authoredPixelOffset.x * scale, authoredPixelOffset.y * scale }
		: authoredPixelOffset;
}

Vector2 SceneScreenOverlayCanvasLayout::ScaleSize(
	const Vector2& authoredSize
) const {
	return responsive
		? Vector2{ authoredSize.x * scale, authoredSize.y * scale }
		: authoredSize;
}

SceneScreenOverlayCanvasLayout ResolveSceneScreenOverlayCanvasLayout(
	const SceneDocument& document,
	const SceneComponent& renderer,
	uint32_t viewportWidth,
	uint32_t viewportHeight
) {
	SceneScreenOverlayCanvasLayout result{};
	result.viewportWidth = viewportWidth;
	result.viewportHeight = viewportHeight;
	const SceneComponent* canvas = FindActiveCanvas(document);
	if (!canvas || viewportWidth == 0 || viewportHeight == 0) {
		return result;
	}
	const Vector2 referenceSize = canvas->screenOverlayCanvasReferenceSize;
	if (!std::isfinite(referenceSize.x) || !std::isfinite(referenceSize.y) ||
		referenceSize.x <= 0.0f || referenceSize.y <= 0.0f) {
		return result;
	}
	std::string mode = renderer.screenOverlayScaleMode;
	if (mode == "Inherit") {
		mode = "Fit";
	}
	if (mode == "Legacy") {
		return result;
	}
	if (mode != "Fit" && mode != "Cover") {
		return result;
	}
	const float viewportWidthValue = static_cast<float>(viewportWidth);
	const float viewportHeightValue = static_cast<float>(viewportHeight);
	const float widthScale = viewportWidthValue / referenceSize.x;
	const float heightScale = viewportHeightValue / referenceSize.y;
	const float scale = mode == "Cover"
		? (std::max)(widthScale, heightScale)
		: (std::min)(widthScale, heightScale);
	if (!std::isfinite(scale) || scale <= 0.0f) {
		return result;
	}
	result.scale = scale;
	result.referenceSize = referenceSize;
	result.origin = {
		(viewportWidthValue - referenceSize.x * scale) * 0.5f,
		(viewportHeightValue - referenceSize.y * scale) * 0.5f
	};
	result.responsive = true;
	return result;
}
