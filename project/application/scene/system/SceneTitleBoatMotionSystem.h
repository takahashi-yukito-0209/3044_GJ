// 役割: Title Scene上の船モデルへ波に合わせた見た目の揺れを加える。
#pragma once

#include <vector>

class SceneDocument;
struct SceneRuntimeObjectBinding;

// Title Scene専用の船揺れ表現を担当する。
class SceneTitleBoatMotionSystem {
public:
	/// <summary>
	/// タイトル用の船モデルに、上下揺れと傾きの見た目補正を反映します。
	/// </summary>
	void Update(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime
	);

	/// <summary>
	/// タイトルシーン外へ出たときに再生時間を初期化します。
	/// </summary>
	void Clear();

private:
	float elapsedSeconds_ = 0.0f; // 船揺れの再生時間。
	float wakeSheetEmissionTimer_ = 0.0f; // 水押し分け帯の発生間隔管理。
	bool particleGroupsPrepared_ = false; // タイトル船用ParticleGroupの準備状態。
};
