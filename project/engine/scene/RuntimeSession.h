// 役割: Runtime専用起動で単一の実行用SceneDocumentを所有する。
#pragma once

#include "SceneDocument.h"
#include "SceneExecutionContext.h"

class RuntimeSession final : public SceneExecutionContext {
public:
	bool Initialize(
		const std::string& sceneId,
		const std::string& sceneFilePath
	);
	bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) override;
	bool AdoptPreloadedRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath,
		SceneDocument&& document
	) override;

	SceneDocument& GetActiveDocument() override { return document_; }
	const SceneDocument& GetActiveDocument() const override { return document_; }
	const std::string& GetActiveSceneId() const override { return sceneId_; }
	const std::string& GetActiveSceneFilePath() const override {
		return sceneFilePath_;
	}
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	bool IsEditing() const override { return false; }
	bool IsPlaying() const override { return true; }
	bool IsPaused() const override { return false; }
	SceneRuntimeSessionState& GetRuntimeSessionState() override {
		return runtimeSessionState_;
	}
	const SceneRuntimeSessionState& GetRuntimeSessionState() const override {
		return runtimeSessionState_;
	}

private:
	SceneDocument document_;
	std::string sceneId_;
	std::string sceneFilePath_;
	std::string lastLoadError_;
	SceneRuntimeSessionState runtimeSessionState_;
};
