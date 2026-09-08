// 役割: Scene実行に必要なDocumentと実行状態をEditor/Runtime共通で公開する。
#pragma once

#include <string>

#include "SceneRuntimeSessionState.h"

class SceneDocument;

class SceneExecutionContext {
public:
	virtual ~SceneExecutionContext() = default;

	virtual SceneDocument& GetActiveDocument() = 0;
	virtual const SceneDocument& GetActiveDocument() const = 0;
	virtual const std::string& GetActiveSceneId() const = 0;
	virtual const std::string& GetActiveSceneFilePath() const = 0;
	virtual bool IsEditing() const = 0;
	virtual bool IsPlaying() const = 0;
	virtual bool IsPaused() const = 0;
	virtual SceneRuntimeSessionState& GetRuntimeSessionState() = 0;
	virtual const SceneRuntimeSessionState& GetRuntimeSessionState() const = 0;
	virtual bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) = 0;
	// 先行ロード済みDocumentをRuntime Sceneとして確定する。
	virtual bool AdoptPreloadedRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath,
		SceneDocument&& document
	) = 0;
};
