// 役割: Runtime専用のSceneDocument読込と遷移時の差し替えを実装する。
#include "RuntimeSession.h"

#include <utility>

bool RuntimeSession::Initialize(
	const std::string& sceneId,
	const std::string& sceneFilePath
) {
	if (!LoadRuntimeScene(sceneId, sceneFilePath)) {
		return false;
	}
	runtimeSessionState_.Clear();
	return true;
}

bool RuntimeSession::LoadRuntimeScene(
	const std::string& sceneId,
	const std::string& sceneFilePath
) {
	if (sceneId.empty() || sceneFilePath.empty()) {
		return false;
	}

	SceneDocument loadedDocument;
	if (!loadedDocument.Load(sceneFilePath)) {
		lastLoadError_ = loadedDocument.GetLastLoadError();
		return false;
	}
	loadedDocument.MarkClean();
	document_ = std::move(loadedDocument);
	sceneId_ = sceneId;
	sceneFilePath_ = sceneFilePath;
	lastLoadError_.clear();
	return true;
}
