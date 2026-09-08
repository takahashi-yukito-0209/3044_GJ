// 役割: SceneInstanceが所有するScene本体のライフサイクルを管理する。
#include "SceneInstance.h"

#include <utility>

#include "BaseScene.h"
#include "SceneDocument.h"

SceneInstance::SceneInstance(
	SceneInstanceId id,
	std::string sceneId,
	std::string instanceKey,
	std::unique_ptr<BaseScene> scene
)
	: id_(id),
	  sceneId_(std::move(sceneId)),
	  instanceKey_(std::move(instanceKey)),
	  scene_(std::move(scene)) {
}

SceneInstance::~SceneInstance() {
	Finalize();
}

void SceneInstance::Initialize(SceneManager* sceneManager) {
	if (!scene_ || initialized_) {
		return;
	}

	scene_->SetSceneManager(sceneManager);
	scene_->SetSceneInstance(this);
	scene_->Initialize();
	initialized_ = true;
}

void SceneInstance::Finalize() {
	if (!scene_ || !initialized_) {
		return;
	}

	scene_->Finalize();
	initialized_ = false;
}

void SceneInstance::BindDocument(SceneDocument* document) {
	ownedDocument_.reset();
	document_ = document;
}

void SceneInstance::OwnDocument(std::unique_ptr<SceneDocument> document) {
	ownedDocument_ = std::move(document);
	document_ = ownedDocument_.get();
}

std::unique_ptr<SceneDocument> SceneInstance::ReleaseOwnedDocument() {
	document_ = nullptr;
	return std::move(ownedDocument_);
}

void SceneInstance::SetPersistent(bool persistent) {
	persistent_ = persistent;
	if (persistent_) {
		DetachDocument();
	}
}

void SceneInstance::DetachDocument() {
	if (!document_ || document_ == ownedDocument_.get()) {
		return;
	}
	OwnDocument(std::make_unique<SceneDocument>(*document_));
}
