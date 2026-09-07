// 役割: SceneTransition Componentを評価し、遷移要求を生成する。
#pragma once

#include <string>

class SceneDocument;

struct SceneTransitionRequest {
	std::string targetSceneId;
	bool useEffect = true;
};

// 状態を保持せず、入力条件を満たした遷移要求だけを返す。
class SceneTransitionSystem {
public:
	SceneTransitionRequest Update(const SceneDocument& document) const;
};
