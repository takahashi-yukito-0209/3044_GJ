// 役割: 編集用と実行用のSceneDocumentを切り替える処理を実装する。
#include "EditorSession.h"

#include <algorithm>
#include <utility>

namespace {
	constexpr size_t kMaxUndoSnapshots = 100;
}

bool EditorSession::Initialize(
	const std::string& sceneId,
	const std::string& sceneName,
	const std::string& sceneFilePath
) {
	runtimeSessionState_.Clear();
	editSceneId_ = sceneId;
	editSceneFilePath_ = sceneFilePath;
	runtimeSceneId_.clear();
	runtimeSceneFilePath_.clear();
	state_ = EditorPlayState::Edit;
	reloadRequested_ = false;
	editFrameActive_ = false;
	frameStartRevision_ = 0;
	undoStack_.clear();
	redoStack_.clear();
	lastLoadError_.clear();

	if (editDocument_.Load(editSceneFilePath_)) {
		frameStartDocument_ = editDocument_;
		return true;
	}

	lastLoadError_ = editDocument_.GetLastLoadError();
	editDocument_.Clear(sceneName);
	frameStartDocument_ = editDocument_;
	return false;
}

bool EditorSession::OpenEditScene(
	const std::string& sceneId,
	const std::string& sceneName,
	const std::string& sceneFilePath,
	bool discardUnsavedChanges
) {
	if (!IsEditing() || sceneId.empty() || sceneFilePath.empty()) {
		return false;
	}
	if (editDocument_.IsDirty() && !discardUnsavedChanges) {
		return false;
	}

	SceneDocument loadedDocument;
	if (!loadedDocument.Load(sceneFilePath)) {
		lastLoadError_ = loadedDocument.GetLastLoadError();
		return false;
	}
	if (loadedDocument.GetSceneName().empty()) {
		loadedDocument.SetSceneName(sceneName);
		loadedDocument.MarkClean();
	}

	editDocument_ = std::move(loadedDocument);
	editSceneId_ = sceneId;
	editSceneFilePath_ = sceneFilePath;
	frameStartDocument_ = editDocument_;
	frameStartRevision_ = editDocument_.GetRevision();
	editFrameActive_ = false;
	undoStack_.clear();
	redoStack_.clear();
	reloadRequested_ = true;
	lastLoadError_.clear();
	return true;
}

bool EditorSession::LoadRuntimeScene(
	const std::string& sceneId,
	const std::string& sceneFilePath
) {
	if (IsEditing() || sceneId.empty() || sceneFilePath.empty()) {
		return false;
	}

	SceneDocument loadedDocument;
	if (!loadedDocument.Load(sceneFilePath)) {
		lastLoadError_ = loadedDocument.GetLastLoadError();
		return false;
	}
	loadedDocument.MarkClean();
	runtimeDocument_ = std::move(loadedDocument);
	runtimeSceneId_ = sceneId;
	runtimeSceneFilePath_ = sceneFilePath;
	lastLoadError_.clear();
	return true;
}

bool EditorSession::AdoptPreloadedRuntimeScene(
	const std::string& sceneId,
	const std::string& sceneFilePath,
	SceneDocument&& document
) {
	if (IsEditing() || sceneId.empty() || sceneFilePath.empty()) {
		return false;
	}

	runtimeDocument_ = std::move(document);
	runtimeSceneId_ = sceneId;
	runtimeSceneFilePath_ = sceneFilePath;
	lastLoadError_.clear();
	return true;
}

void EditorSession::Play() {
	if (state_ != EditorPlayState::Edit) {
		return;
	}
	runtimeSessionState_.Clear();
	runtimeDocument_ = editDocument_;
	runtimeDocument_.MarkClean();
	runtimeSceneId_ = editSceneId_;
	runtimeSceneFilePath_ = editSceneFilePath_;
	state_ = EditorPlayState::Playing;
	reloadRequested_ = true;
}

void EditorSession::Pause() {
	if (state_ == EditorPlayState::Playing) {
		state_ = EditorPlayState::Paused;
	}
}

void EditorSession::Resume() {
	if (state_ == EditorPlayState::Paused) {
		state_ = EditorPlayState::Playing;
	}
}

void EditorSession::Stop() {
	if (state_ == EditorPlayState::Edit) {
		return;
	}
	state_ = EditorPlayState::Edit;
	runtimeSessionState_.Clear();
	runtimeDocument_.Clear();
	runtimeSceneId_.clear();
	runtimeSceneFilePath_.clear();
	reloadRequested_ = true;
}

bool EditorSession::Save() {
	if (!IsEditing() || editSceneFilePath_.empty()) {
		return false;
	}
	const bool saved = editDocument_.Save(editSceneFilePath_);
	if (saved) {
		frameStartDocument_ = editDocument_;
		frameStartRevision_ = editDocument_.GetRevision();
		editFrameActive_ = false;
	}
	return saved;
}

bool EditorSession::CommitRuntimeEditAndSave(
	const SceneDocument& beforeSnapshot
) {
	if (
		(!IsPlaying() && !IsPaused()) ||
		editSceneFilePath_.empty()
	) {
		return false;
	}
	if (editDocument_.GetRevision() != beforeSnapshot.GetRevision()) {
		PushUndoSnapshot(beforeSnapshot);
		redoStack_.clear();
	}
	const bool saved = editDocument_.Save(editSceneFilePath_);
	if (saved) {
		frameStartDocument_ = editDocument_;
		frameStartRevision_ = editDocument_.GetRevision();
		editFrameActive_ = false;
	}
	return saved;
}

void EditorSession::BeginEditFrame() {
	if (!IsEditing() || editFrameActive_) {
		return;
	}
	frameStartDocument_ = editDocument_;
	frameStartRevision_ = editDocument_.GetRevision();
	editFrameActive_ = true;
}

void EditorSession::EndEditFrame(bool commit) {
	if (!editFrameActive_) {
		return;
	}
	if (!IsEditing()) {
		editFrameActive_ = false;
		return;
	}
	if (editDocument_.GetRevision() == frameStartRevision_) {
		editFrameActive_ = false;
		return;
	}
	if (!commit) {
		return;
	}
	editFrameActive_ = false;
	PushUndoSnapshot(frameStartDocument_);
	redoStack_.clear();
}

bool EditorSession::Undo() {
	if (!IsEditing() || undoStack_.empty()) {
		return false;
	}
	redoStack_.push_back(editDocument_);
	editDocument_ = undoStack_.back();
	undoStack_.pop_back();
	editDocument_.MarkDirty();
	frameStartDocument_ = editDocument_;
	frameStartRevision_ = editDocument_.GetRevision();
	editFrameActive_ = false;
	reloadRequested_ = true;
	return true;
}

bool EditorSession::Redo() {
	if (!IsEditing() || redoStack_.empty()) {
		return false;
	}
	undoStack_.push_back(editDocument_);
	editDocument_ = redoStack_.back();
	redoStack_.pop_back();
	editDocument_.MarkDirty();
	frameStartDocument_ = editDocument_;
	frameStartRevision_ = editDocument_.GetRevision();
	editFrameActive_ = false;
	reloadRequested_ = true;
	return true;
}

SceneDocument& EditorSession::GetActiveDocument() {
	return IsEditing() ? editDocument_ : runtimeDocument_;
}

const SceneDocument& EditorSession::GetActiveDocument() const {
	return IsEditing() ? editDocument_ : runtimeDocument_;
}

const std::string& EditorSession::GetActiveSceneId() const {
	return IsEditing() ? editSceneId_ : runtimeSceneId_;
}

const std::string& EditorSession::GetActiveSceneFilePath() const {
	return IsEditing() ? editSceneFilePath_ : runtimeSceneFilePath_;
}

bool EditorSession::ConsumeReloadRequest() {
	const bool requested = reloadRequested_;
	reloadRequested_ = false;
	return requested;
}

void EditorSession::PushUndoSnapshot(const SceneDocument& snapshot) {
	undoStack_.push_back(snapshot);
	if (undoStack_.size() > kMaxUndoSnapshots) {
		undoStack_.erase(undoStack_.begin());
	}
}
