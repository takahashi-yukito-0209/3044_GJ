// 役割: Scene保存入力名をKeyboard／Mouse／GamepadのRuntime入力へ評価する。
#pragma once

#include "../../engine/scene/SceneDocument.h"

#include <optional>
#include <string>

namespace SceneRuntimeInput {

bool IsTriggered(
	const std::string& inputName,
	const std::string& phase = "Pressed"
);

bool EvaluateExpression(
	const std::optional<SceneInputExpression>& expression,
	const std::string& legacyInput
);

}
