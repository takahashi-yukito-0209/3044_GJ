// 役割: 編集、再生、一時停止の状態とシーン複製を管理する。
#pragma once

#include <string>
#include <vector>

#include "SceneDocument.h"
#include "SceneExecutionContext.h"

enum class EditorPlayState {
	Edit,
	Playing,
	Paused
};

class EditorSession final : public SceneExecutionContext {
public:
	bool Initialize(
		const std::string& sceneId,
		const std::string& sceneName,
		const std::string& sceneFilePath
	);
	bool OpenEditScene(
		const std::string& sceneId,
		const std::string& sceneName,
		const std::string& sceneFilePath,
		bool discardUnsavedChanges = false
	);
	bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) override;

	void Play();
	void Pause();
	void Resume();
	void Stop();
	bool Save();
	bool CommitRuntimeEditAndSave(const SceneDocument& beforeSnapshot);
	void BeginEditFrame();
	void EndEditFrame(bool commit);
	bool Undo();
	bool Redo();
	bool CanUndo() const { return !undoStack_.empty(); }
	bool CanRedo() const { return !redoStack_.empty(); }

	EditorPlayState GetState() const { return state_; }
	bool IsPlaying() const override { return state_ == EditorPlayState::Playing; }
	bool IsPaused() const override { return state_ == EditorPlayState::Paused; }
	bool IsEditing() const override { return state_ == EditorPlayState::Edit; }
	SceneRuntimeSessionState& GetRuntimeSessionState() override {
		return runtimeSessionState_;
	}
	const SceneRuntimeSessionState& GetRuntimeSessionState() const override {
		return runtimeSessionState_;
	}

	SceneDocument& GetEditDocument() { return editDocument_; }
	const SceneDocument& GetEditDocument() const { return editDocument_; }
	SceneDocument& GetActiveDocument() override;
	const SceneDocument& GetActiveDocument() const override;
	const std::string& GetEditSceneId() const { return editSceneId_; }
	const std::string& GetRuntimeSceneId() const { return runtimeSceneId_; }
	const std::string& GetActiveSceneId() const override;
	const std::string& GetSceneFilePath() const { return editSceneFilePath_; }
	const std::string& GetActiveSceneFilePath() const override;
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	void RequestSceneReload() { reloadRequested_ = true; }

	bool ConsumeReloadRequest();

private:
	void PushUndoSnapshot(const SceneDocument& snapshot);

	SceneDocument editDocument_;
	SceneDocument runtimeDocument_;
	SceneDocument frameStartDocument_;
	std::string editSceneId_;
	std::string editSceneFilePath_;
	std::string runtimeSceneId_;
	std::string runtimeSceneFilePath_;
	std::string lastLoadError_;
	EditorPlayState state_ = EditorPlayState::Edit;
	bool reloadRequested_ = false;
	bool editFrameActive_ = false;
	uint64_t frameStartRevision_ = 0;
	SceneRuntimeSessionState runtimeSessionState_;
	std::vector<SceneDocument> undoStack_;
	std::vector<SceneDocument> redoStack_;
};
