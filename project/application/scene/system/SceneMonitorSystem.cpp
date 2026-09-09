// 役割: MonitorRendererの出力先を同期し、指定CameraからSceneを描画する。
#include "SceneMonitorSystem.h"

#include "SceneCameraSystem.h"
#include "../../../engine/base/RenderFormats.h"
#include "../../../engine/base/SceneRenderTarget.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/3d/SrvManager.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"
#include "../../../externals/imgui/imgui.h"

#include <algorithm>
#include <unordered_set>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;

	bool ResolveMonitorTargetCamera(
		const SceneDocument& document,
		const SceneComponent& monitorRenderer,
		const SceneEntity*& cameraEntity,
		const SceneComponent*& cameraComponent
	) {
		cameraEntity = nullptr;
		cameraComponent = nullptr;

		auto tryResolveEntity = [&document](
			const SceneEntity* entity,
			const SceneEntity*& resolvedEntity,
			const SceneComponent*& resolvedCamera
		) {
			if (!entity || !IsEntityActiveInHierarchy(document, *entity)) {
				return false;
			}
			const SceneComponent* camera =
				FindEnabledComponent(*entity, "Camera");
			if (!camera) {
				return false;
			}
			resolvedEntity = entity;
			resolvedCamera = camera;
			return true;
		};

		if (monitorRenderer.monitorCameraEntityId != 0) {
			const SceneEntity* entity =
				document.FindEntity(monitorRenderer.monitorCameraEntityId);
			const bool idMatchesStoredName =
				monitorRenderer.monitorCameraName.empty() ||
				(entity && entity->name == monitorRenderer.monitorCameraName);
			if (
				idMatchesStoredName &&
				tryResolveEntity(entity, cameraEntity, cameraComponent)
			) {
				return true;
			}
		}

		if (!monitorRenderer.monitorCameraName.empty()) {
			const SceneEntity* entity =
				document.FindEntityByName(monitorRenderer.monitorCameraName);
			if (tryResolveEntity(entity, cameraEntity, cameraComponent)) {
				return true;
			}
		}

		if (monitorRenderer.monitorCameraEntityId != 0) {
			const SceneEntity* entity =
				document.FindEntity(monitorRenderer.monitorCameraEntityId);
			if (tryResolveEntity(entity, cameraEntity, cameraComponent)) {
				return true;
			}
		}

		return false;
	}

	bool HasMonitorCameraBinding(const SceneComponent& monitorRenderer) {
		return
			monitorRenderer.monitorCameraEntityId != 0 ||
			!monitorRenderer.monitorCameraName.empty();
	}
}

SceneMonitorSystem::SceneMonitorSystem() = default;

SceneMonitorSystem::~SceneMonitorSystem() {
	Finalize();
}

void SceneMonitorSystem::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager
) {
	Finalize();
	dxCommon_ = dxCommon;
	srvManager_ = srvManager;
}

Object3d* SceneMonitorSystem::FindObject(
	uint64_t entityId,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) const {
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			binding.entityId == entityId
		) {
			return binding.object;
		}
	}
	return nullptr;
}

void SceneMonitorSystem::ClearTextureOverride(
	uint64_t entityId,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) const {
	if (Object3d* object = FindObject(entityId, bindings)) {
		object->ClearTextureOverride();
	}
}

void SceneMonitorSystem::Sync(
	const SceneDocument* document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	// 消えたMonitorのTexture Overrideを解除してからRuntimeを破棄する。
	if (!document || !dxCommon_ || !srvManager_) {
		for (const auto& [entityId, runtime] : runtimes_) {
			(void)runtime;
			ClearTextureOverride(entityId, bindings);
		}
		runtimes_.clear();
		return;
	}

	std::unordered_set<uint64_t> requiredIds;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* monitorRenderer =
			FindEnabledComponent(entity, "MonitorRenderer");
		if (!monitorRenderer) {
			continue;
		}

		requiredIds.insert(entity.id);
		MonitorRuntime& runtime = runtimes_[entity.id];
		const uint32_t width = std::clamp<uint32_t>(
			monitorRenderer->monitorWidth,
			64,
			2048
		);
		const uint32_t height = std::clamp<uint32_t>(
			monitorRenderer->monitorHeight,
			64,
			2048
		);
		runtime.hideSelf = monitorRenderer->monitorHideSelf;
		if (!runtime.camera) {
			runtime.camera = std::make_unique<Camera>();
		}
		if (
			!runtime.renderTarget ||
			runtime.width != width ||
			runtime.height != height
		) {
			runtime.renderTarget = std::make_unique<SceneRenderTarget>();
			SceneRenderTarget::Desc desc{};
			desc.width = width;
			desc.height = height;
			desc.format = RenderFormats::kSceneHdrFormat;
			desc.clearColor[0] = 0.02f;
			desc.clearColor[1] = 0.02f;
			desc.clearColor[2] = 0.025f;
			desc.clearColor[3] = 1.0f;
			runtime.renderTarget->Initialize(
				dxCommon_,
				srvManager_,
				desc
			);
			runtime.width = width;
			runtime.height = height;
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			ClearTextureOverride(iterator->first, bindings);
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneMonitorSystem::DrawOffscreen(
	const SceneDocument* document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneCameraSystem& cameraSystem,
	const DrawSceneCallback& drawScene
) {
	// RenderTargetの内容が確定した後だけMonitor ObjectへSRVを設定する。
	++debugFrame_;
	Sync(document, bindings);
	if (!document || !drawScene) {
		return;
	}

	for (auto& [monitorEntityId, runtime] : runtimes_) {
		const SceneEntity* monitorEntity =
			document->FindEntity(monitorEntityId);
		Object3d* monitorObject = FindObject(monitorEntityId, bindings);
		runtime.debugPassFrame = debugFrame_;
		runtime.debugResolvedCamera = false;
		runtime.debugRendered = false;
		runtime.debugTextureApplied = false;
		runtime.debugTargetCameraId = 0;
		runtime.debugTargetCameraName.clear();
		runtime.debugTargetTranslate = {};
		runtime.debugTargetRotate = {};
		runtime.debugTargetIsMain = false;
		runtime.debugTargetHasPlayerBehavior = false;
		runtime.debugSrvPtr = runtime.renderTarget
			? runtime.renderTarget->GetSrvGpuHandle().ptr
			: 0;
		runtime.debugAppliedTextureOverridePtr = monitorObject
			? monitorObject->GetTextureOverridePtr()
			: 0;
		runtime.debugStatus = "Pending";
		if (
			!monitorEntity ||
			!IsEntityActiveInHierarchy(*document, *monitorEntity) ||
			!runtime.camera ||
			!runtime.renderTarget ||
			!monitorObject
		) {
			if (!monitorEntity) {
				runtime.debugStatus = "Monitor entity missing";
			} else if (!IsEntityActiveInHierarchy(*document, *monitorEntity)) {
				runtime.debugStatus = "Monitor entity inactive";
			} else if (!runtime.camera) {
				runtime.debugStatus = "Runtime camera missing";
			} else if (!runtime.renderTarget) {
				runtime.debugStatus = "Render target missing";
			} else {
				runtime.debugStatus = "Monitor mesh object missing";
			}
			if (monitorObject) {
				monitorObject->ClearTextureOverride();
			}
			runtime.debugAppliedTextureOverridePtr = 0;
			continue;
		}

		const SceneComponent* monitorRenderer =
			FindEnabledComponent(*monitorEntity, "MonitorRenderer");
		if (monitorRenderer) {
			runtime.debugTargetCameraId =
				monitorRenderer->monitorCameraEntityId;
			runtime.debugTargetCameraName =
				monitorRenderer->monitorCameraName;
		}
		const SceneEntity* cameraEntity = nullptr;
		const SceneComponent* cameraComponent = nullptr;
		if (
			!monitorRenderer ||
			!HasMonitorCameraBinding(*monitorRenderer) ||
			!ResolveMonitorTargetCamera(
				*document,
				*monitorRenderer,
				cameraEntity,
				cameraComponent
			)
		) {
			if (!monitorRenderer) {
				runtime.debugStatus = "MonitorRenderer component missing";
			} else if (!HasMonitorCameraBinding(*monitorRenderer)) {
				runtime.debugStatus = "Target camera not assigned";
			} else {
				runtime.debugStatus = "Target camera unresolved";
			}
			monitorObject->ClearTextureOverride();
			runtime.debugAppliedTextureOverridePtr = 0;
			continue;
		}

		runtime.debugResolvedCamera = true;
		runtime.debugTargetCameraId = cameraEntity->id;
		runtime.debugTargetCameraName = cameraEntity->name;
		const Transform targetCameraTransform =
			ResolveScene3DTransform(*document, *cameraEntity);
		runtime.debugTargetTranslate = targetCameraTransform.translate;
		runtime.debugTargetRotate = targetCameraTransform.rotate;
		runtime.debugTargetIsMain = cameraComponent->cameraIsMain;
		runtime.debugTargetHasPlayerBehavior =
			HasComponent(*cameraEntity, "PlayerBehavior");

		const float aspectRatio =
			static_cast<float>(runtime.width) /
			static_cast<float>((std::max)(runtime.height, 1u));
		if (!cameraSystem.ApplyComponentToCamera(
			*document,
			*cameraEntity,
			*cameraComponent,
			runtime.camera.get(),
			aspectRatio
		)) {
			runtime.debugStatus = "Failed to apply camera component";
			monitorObject->ClearTextureOverride();
			runtime.debugAppliedTextureOverridePtr = 0;
			continue;
		}
		if (debugForceProbeCamera_) {
			runtime.camera->SetTranslate({ 0.0f, 80.0f, -80.0f });
			runtime.camera->SetRotate({ 0.75f, 0.0f, 0.0f });
			runtime.camera->SetFovY(0.12f);
			runtime.camera->Update();
		}

		runtime.renderTarget->Begin();
		srvManager_->PreDraw();
		drawScene(
			runtime.camera.get(),
			runtime.hideSelf ? monitorEntityId : 0
		);
		runtime.renderTarget->End();

		runtime.debugSrvPtr = runtime.renderTarget->GetSrvGpuHandle().ptr;
		runtime.debugRendered = true;
		runtime.debugTextureApplied = runtime.debugSrvPtr != 0;
		runtime.debugStatus =
			"Rendered from #" +
			std::to_string(cameraEntity->id) +
			" " +
			cameraEntity->name;
		if (debugForceProbeCamera_) {
			runtime.debugStatus += " (probe camera forced)";
		}
		monitorObject->SetTextureOverride(
			runtime.renderTarget->GetSrvGpuHandle()
		);
		runtime.debugAppliedTextureOverridePtr =
			monitorObject->GetTextureOverridePtr();
	}
}

void SceneMonitorSystem::DrawEditor(
	const SceneDocument* document,
	Camera* sceneViewCamera
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!ImGui::Begin(
		"Monitor Debug",
		nullptr,
		ImGuiWindowFlags_NoFocusOnAppearing
	)) {
		ImGui::End();
		return;
	}

	auto drawVector3 = [](const char* label, const Vector3& value) {
		ImGui::Text(
			"%s: %.3f, %.3f, %.3f",
			label,
			value.x,
			value.y,
			value.z
		);
	};
	auto yesNo = [](bool value) {
		return value ? "yes" : "no";
	};

	ImGui::Text(
		"Last offscreen pass frame: %llu",
		static_cast<unsigned long long>(debugFrame_)
	);
	ImGui::Text("Monitor runtimes: %zu", runtimes_.size());
	ImGui::Checkbox(
		"Force probe camera for monitor RT",
		&debugForceProbeCamera_
	);

	if (sceneViewCamera) {
		ImGui::SeparatorText("Scene View Camera");
		drawVector3("Translate", sceneViewCamera->GetTranslate());
		drawVector3("Rotate", sceneViewCamera->GetRotate());
		ImGui::Text(
			"FOV: %.3f / Near: %.3f / Far: %.3f",
			sceneViewCamera->GetFovY(),
			sceneViewCamera->GetNearClip(),
			sceneViewCamera->GetFarClip()
		);
	}

	if (!document) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
			"No active scene document"
		);
		ImGui::End();
		return;
	}

	std::unordered_set<uint64_t> listedRuntimes;
	uint32_t monitorComponentCount = 0;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* monitorRenderer =
			FindEnabledComponent(entity, "MonitorRenderer");
		if (!monitorRenderer) {
			continue;
		}

		++monitorComponentCount;
		listedRuntimes.insert(entity.id);
		const auto runtimeIterator = runtimes_.find(entity.id);
		const MonitorRuntime* runtime = runtimeIterator != runtimes_.end()
			? &runtimeIterator->second
			: nullptr;

		std::string header = entity.name.empty()
			? std::string("Monitor")
			: entity.name;
		header += " ##MonitorDebug";
		header += std::to_string(entity.id);
		if (!ImGui::TreeNode(header.c_str())) {
			continue;
		}

		ImGui::Text(
			"Monitor Entity: #%llu / active: %s",
			static_cast<unsigned long long>(entity.id),
			yesNo(IsEntityActiveInHierarchy(*document, entity))
		);
		drawVector3("Monitor Translate", entity.transform.translate);
		drawVector3(
			"Monitor Rotate",
			MakeEulerFromQuaternion(entity.transform.rotate)
		);

		ImGui::SeparatorText("Component Target");
		ImGui::Text(
			"Target id: %llu",
			static_cast<unsigned long long>(
				monitorRenderer->monitorCameraEntityId
			)
		);
		ImGui::Text(
			"Target name: %s",
			monitorRenderer->monitorCameraName.empty()
				? "(empty)"
				: monitorRenderer->monitorCameraName.c_str()
		);
		ImGui::Text(
			"Size: %u x %u / Hide self: %s",
			monitorRenderer->monitorWidth,
			monitorRenderer->monitorHeight,
			yesNo(monitorRenderer->monitorHideSelf)
		);
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		const std::string monitorModelPath = meshRenderer
			? meshRenderer->modelPath
			: std::string{};
		ImGui::Text(
			"Surface mesh: %s",
			monitorModelPath.empty()
				? "(none)"
				: monitorModelPath.c_str()
		);
		if (monitorModelPath == "Cube.obj") {
			ImGui::TextColored(
				ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
				"Cube.obj uses atlas UVs; RT will appear cropped."
			);
		} else if (monitorModelPath == "plane.obj") {
			ImGui::TextColored(
				ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
				"plane.obj faces +Z; use monitor_screen.obj for monitors."
			);
		} else if (monitorModelPath == "monitor_screen.obj") {
			ImGui::TextColored(
				ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
				"monitor_screen.obj faces -Z with full-screen UVs."
			);
		}

		const SceneEntity* liveCameraEntity = nullptr;
		const SceneComponent* liveCameraComponent = nullptr;
		const bool liveResolved = ResolveMonitorTargetCamera(
			*document,
			*monitorRenderer,
			liveCameraEntity,
			liveCameraComponent
		);
		ImGui::SeparatorText("Live Resolve");
		if (liveResolved && liveCameraEntity && liveCameraComponent) {
			const Transform liveCameraTransform =
				ResolveScene3DTransform(*document, *liveCameraEntity);
			ImGui::TextColored(
				ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
				"Resolved: #%llu %s",
				static_cast<unsigned long long>(liveCameraEntity->id),
				liveCameraEntity->name.c_str()
			);
			ImGui::Text(
				"Main: %s / PlayerBehavior: %s",
				yesNo(liveCameraComponent->cameraIsMain),
				yesNo(HasComponent(*liveCameraEntity, "PlayerBehavior"))
			);
			drawVector3(
				"Target Translate",
				liveCameraTransform.translate
			);
			drawVector3(
				"Target Rotate",
				liveCameraTransform.rotate
			);
		} else {
			ImGui::TextColored(
				ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
				"Resolved: no"
			);
		}

		ImGui::SeparatorText("Last Offscreen Pass");
		if (!runtime) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
				"No runtime has been created yet"
			);
			ImGui::TreePop();
			continue;
		}

		ImGui::Text("Status: %s", runtime->debugStatus.c_str());
		ImGui::Text(
			"Pass frame: %llu",
			static_cast<unsigned long long>(runtime->debugPassFrame)
		);
		ImGui::Text(
			"Resolved: %s / Rendered: %s / Texture applied: %s",
			yesNo(runtime->debugResolvedCamera),
			yesNo(runtime->debugRendered),
			yesNo(runtime->debugTextureApplied)
		);
		ImGui::Text(
			"Last target: #%llu %s",
			static_cast<unsigned long long>(runtime->debugTargetCameraId),
			runtime->debugTargetCameraName.empty()
				? "(none)"
				: runtime->debugTargetCameraName.c_str()
		);
		ImGui::Text(
			"Last target main: %s / player: %s",
			yesNo(runtime->debugTargetIsMain),
			yesNo(runtime->debugTargetHasPlayerBehavior)
		);
		drawVector3(
			"Last target translate",
			runtime->debugTargetTranslate
		);
		drawVector3(
			"Last target rotate",
			runtime->debugTargetRotate
		);
		if (runtime->camera) {
			drawVector3(
				"Runtime camera translate",
				runtime->camera->GetTranslate()
			);
			drawVector3(
				"Runtime camera rotate",
				runtime->camera->GetRotate()
			);
		}
		ImGui::Text(
			"RT: %s / %u x %u / SRV: 0x%llX",
			runtime->renderTarget ? "yes" : "no",
			runtime->width,
			runtime->height,
			static_cast<unsigned long long>(runtime->debugSrvPtr)
		);
		ImGui::Text(
			"Applied override SRV: 0x%llX / match: %s",
			static_cast<unsigned long long>(
				runtime->debugAppliedTextureOverridePtr
			),
			yesNo(
				runtime->debugSrvPtr != 0 &&
				runtime->debugSrvPtr ==
					runtime->debugAppliedTextureOverridePtr
			)
		);

		if (runtime->renderTarget && runtime->debugSrvPtr != 0) {
			const float availableWidth = ImGui::GetContentRegionAvail().x;
			float previewWidth = std::clamp(
				availableWidth,
				160.0f,
				360.0f
			);
			const float aspect = runtime->height > 0
				? static_cast<float>(runtime->width) /
					static_cast<float>(runtime->height)
				: 1.0f;
			float previewHeight = previewWidth /
				(aspect > 0.0f ? aspect : 1.0f);
			if (previewHeight > 260.0f) {
				previewHeight = 260.0f;
				previewWidth = previewHeight *
					(aspect > 0.0f ? aspect : 1.0f);
			}
			ImGui::TextDisabled("RT preview from previous pass");
			ImGui::Image(
				ImTextureRef(
					static_cast<ImTextureID>(runtime->debugSrvPtr)
				),
				ImVec2(previewWidth, previewHeight)
			);
		}

		ImGui::TreePop();
	}

	if (monitorComponentCount == 0) {
		ImGui::TextDisabled("No enabled MonitorRenderer components");
	}

	for (const auto& [entityId, runtime] : runtimes_) {
		if (listedRuntimes.contains(entityId)) {
			continue;
		}
		ImGui::TextColored(
			ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
			"Orphan runtime: #%llu / %s",
			static_cast<unsigned long long>(entityId),
			runtime.debugStatus.c_str()
		);
	}

	ImGui::End();
#else
	(void)document;
	(void)sceneViewCamera;
#endif
}

void SceneMonitorSystem::Finalize(
	const std::vector<SceneRuntimeObjectBinding>* bindings
) {
	if (bindings) {
		for (const auto& [entityId, runtime] : runtimes_) {
			(void)runtime;
			ClearTextureOverride(entityId, *bindings);
		}
	}
	runtimes_.clear();
	dxCommon_ = nullptr;
	srvManager_ = nullptr;
	debugFrame_ = 0;
	debugForceProbeCamera_ = false;
}
