// 役割: ゲーム全体の更新、描画、ポストプロセスとエディタ再生制御を実装する。
#include "Game.h"
#include "PrefabPreviewRenderer.h"
#include "scene/EditorBootstrap.h"
#include "scene/RuntimeBootstrap.h"
#include "scene/SceneFactory.h"
#include "scene/SceneStartupErrorScreen.h"
#include "../engine/scene/EditorSession.h"
#include "../engine/scene/SceneAssetService.h"
#include "../engine/scene/SceneCatalog.h"
#include "../engine/scene/SceneEntityQuery.h"
#include "../engine/scene/SceneExecutionContext.h"
#include "../engine/scene/SceneTemplateRegistry.h"
#include "../engine/scene/SceneTransformResolver.h"
#include "../engine/scene/SceneValidator.h"

#include "../engine/base/DirectXCommon.h"
#include "../engine/base/BloomRenderer.h"
#include "../engine/base/FullscreenCopy.h"
#include "../engine/base/ImGuiManager.h"
#include "../engine/base/PostProcessSettingsEditor.h"
#include "../engine/base/RenderFormats.h"
#include "../engine/base/SceneRenderTarget.h"
#include "../engine/io/Input.h"
#include "../engine/2d/Sprite.h"
#include "../engine/2d/SpriteCommon.h"
#include "../engine/2d/TextureManager.h"
#include "../engine/3d/Camera.h"
#include "../engine/3d/Object3dCommon.h"
#include "../engine/3d/Object3d.h"
#include "../engine/3d/Model.h"
#include "../engine/3d/ModelManager.h"
#include "../engine/3d/SrvManager.h"
#include "../engine/particle/ParticleCommon.h"
#include "../engine/particle/ParticleManager.h"
#include "../engine/particle/ParticleEmitter.h"
#include "../engine/debug/DebugRenderer.h"
#include "../engine/debug/EditorGridRenderer.h"
#include "../engine/utility/EditableResourcePath.h"
#include "../engine/utility/Logger.h"
#include "../engine/utility/StringUtility.h"
#include "../externals/imgui/imgui.h"
#include "../engine/project/ProjectCompatibilityProbe.h"
#include "../engine/project/ProjectCreationService.h"
#include "../engine/project/ProjectDescriptor.h"
#include "../engine/project/ProjectRegistry.h"
#include "../engine/project/ProjectSolutionGenerationService.h"
#include "../engine/project/VisualStudioLocator.h"
#include "../engine/project/WindowsProcess.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;
	constexpr const char* kSceneTransitionMaskPath =
		"resources/transition/transition.png";

	constexpr SceneBuildConfiguration GetCurrentBuildConfiguration() {
#if defined(_DEBUG)
		return SceneBuildConfiguration::Debug;
#elif defined(DEVELOPMENT)
		return SceneBuildConfiguration::Development;
#else
		return SceneBuildConfiguration::Release;
#endif
	}

	std::string ResolveProjectResourcePath(const std::filesystem::path& relativePath) {
		return StringUtility::ToUtf8(EditableResourcePath::Resolve(relativePath));
	}

	std::string MakeProjectCreationSnapshotIdentity(const ProjectCreationServiceSnapshot& snapshot) {
		std::ostringstream identity;
		identity << static_cast<int>(snapshot.state) << '|';
		if (snapshot.operation.has_value()) {
			identity << snapshot.operation->operationId << '|'
				<< static_cast<int>(snapshot.operation->state) << '|'
				<< snapshot.operation->exitCode;
		}
		identity << '|' << snapshot.statusMessage;
		return identity.str();
	}

	std::string MakeVisualStudioSnapshotIdentity(const std::vector<VisualStudioInstance>& instances) {
		std::ostringstream identity;
		for (const VisualStudioInstance& instance : instances) {
			identity << instance.instanceId << '|'
				<< instance.installationVersion << '|'
				<< instance.openable << '|'
				<< instance.buildReady << ';';
		}
		return identity.str();
	}

	bool NearlyEqual(float a, float b) {
		return std::abs(a - b) <= 0.000001f;
	}

	bool EqualVector(const Vector2& a, const Vector2& b) {
		return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y);
	}

	bool EqualVector(const Vector4& a, const Vector4& b) {
		return
			NearlyEqual(a.x, b.x) &&
			NearlyEqual(a.y, b.y) &&
			NearlyEqual(a.z, b.z) &&
			NearlyEqual(a.w, b.w);
	}

	bool IsPointInsideAabb(
		const Vector3& point,
		const Vector3& center,
		const Vector3& halfSize
	) {
		return
			std::abs(point.x - center.x) <= halfSize.x &&
			std::abs(point.y - center.y) <= halfSize.y &&
			std::abs(point.z - center.z) <= halfSize.z;
	}

	bool EqualPostProcessSettings(
		const ScenePostProcessSettings& a,
		const ScenePostProcessSettings& b
	) {
		return
			a.bloomEnabled == b.bloomEnabled &&
			NearlyEqual(a.baseExposure, b.baseExposure) &&
			a.toneMapMode == b.toneMapMode &&
			NearlyEqual(a.bloomThreshold, b.bloomThreshold) &&
			NearlyEqual(a.bloomSoftKnee, b.bloomSoftKnee) &&
			NearlyEqual(a.bloomIntensity, b.bloomIntensity) &&
			a.bloomBlurIterations == b.bloomBlurIterations &&
			a.bloomDownsampleScale == b.bloomDownsampleScale &&
			NearlyEqual(a.bloomBlurRadius, b.bloomBlurRadius) &&
			a.grayscaleEnabled == b.grayscaleEnabled &&
			a.vignetteEnabled == b.vignetteEnabled &&
			a.boxBlurEnabled == b.boxBlurEnabled &&
			a.gaussianBlurEnabled == b.gaussianBlurEnabled &&
			a.depthOfFieldEnabled == b.depthOfFieldEnabled &&
			a.motionBlurEnabled == b.motionBlurEnabled &&
			NearlyEqual(a.motionBlurStrength, b.motionBlurStrength) &&
			a.motionBlurSamples == b.motionBlurSamples &&
			NearlyEqual(a.motionBlurMaxRadius, b.motionBlurMaxRadius) &&
			a.radialBlurEnabled == b.radialBlurEnabled &&
			a.noiseEnabled == b.noiseEnabled &&
			a.dissolveEnabled == b.dissolveEnabled &&
			a.outlineEnabled == b.outlineEnabled &&
			a.underwaterEnabled == b.underwaterEnabled &&
			a.waterRefractionEnabled == b.waterRefractionEnabled &&
			a.pixelationEnabled == b.pixelationEnabled &&
			a.pixelationBlockSize == b.pixelationBlockSize &&
			a.chromaticAberrationEnabled == b.chromaticAberrationEnabled &&
			NearlyEqual(a.chromaticAberrationCenter.x, b.chromaticAberrationCenter.x) &&
			NearlyEqual(a.chromaticAberrationCenter.y, b.chromaticAberrationCenter.y) &&
			NearlyEqual(a.chromaticAberrationIntensity, b.chromaticAberrationIntensity) &&
			NearlyEqual(a.chromaticAberrationFalloff, b.chromaticAberrationFalloff) &&
			NearlyEqual(a.vignetteScale, b.vignetteScale) &&
			NearlyEqual(a.vignettePower, b.vignettePower) &&
			NearlyEqual(a.vignetteIntensity, b.vignetteIntensity) &&
			a.boxBlurKernelSize == b.boxBlurKernelSize &&
			NearlyEqual(a.boxBlurStrength, b.boxBlurStrength) &&
			a.gaussianBlurKernelSize == b.gaussianBlurKernelSize &&
			NearlyEqual(a.gaussianBlurSigma, b.gaussianBlurSigma) &&
			NearlyEqual(a.gaussianBlurStrength, b.gaussianBlurStrength) &&
			NearlyEqual(a.depthOfFieldFocusDistance, b.depthOfFieldFocusDistance) &&
			NearlyEqual(a.depthOfFieldFocusRange, b.depthOfFieldFocusRange) &&
			NearlyEqual(a.depthOfFieldBlurStrength, b.depthOfFieldBlurStrength) &&
			NearlyEqual(a.depthOfFieldNearStrength, b.depthOfFieldNearStrength) &&
			NearlyEqual(a.depthOfFieldFarStrength, b.depthOfFieldFarStrength) &&
			NearlyEqual(a.depthOfFieldMaxRadius, b.depthOfFieldMaxRadius) &&
			EqualVector(a.radialBlurCenter, b.radialBlurCenter) &&
			NearlyEqual(a.radialBlurWidth, b.radialBlurWidth) &&
			a.radialBlurSamples == b.radialBlurSamples &&
			a.noiseAnimate == b.noiseAnimate &&
			NearlyEqual(a.noiseAmount, b.noiseAmount) &&
			NearlyEqual(a.noiseScale, b.noiseScale) &&
			NearlyEqual(a.noiseSpeed, b.noiseSpeed) &&
			NearlyEqual(a.noiseSeed, b.noiseSeed) &&
			a.dissolveMaskIndex == b.dissolveMaskIndex &&
			NearlyEqual(a.dissolveThreshold, b.dissolveThreshold) &&
			NearlyEqual(a.dissolveEdgeWidth, b.dissolveEdgeWidth) &&
			EqualVector(a.dissolveEdgeColor, b.dissolveEdgeColor) &&
			a.outlineLuminanceEnabled == b.outlineLuminanceEnabled &&
			a.outlineDepthEnabled == b.outlineDepthEnabled &&
			NearlyEqual(a.outlineLuminanceWeight, b.outlineLuminanceWeight) &&
			NearlyEqual(a.outlineDepthWeight, b.outlineDepthWeight) &&
			NearlyEqual(a.outlineThreshold, b.outlineThreshold) &&
			NearlyEqual(a.outlineSoftness, b.outlineSoftness) &&
			NearlyEqual(a.outlineThickness, b.outlineThickness) &&
			EqualVector(a.outlineColor, b.outlineColor) &&
			EqualVector(a.underwaterTintColor, b.underwaterTintColor) &&
			NearlyEqual(a.underwaterIntensity, b.underwaterIntensity) &&
			NearlyEqual(a.underwaterFogDensity, b.underwaterFogDensity) &&
			NearlyEqual(a.underwaterDistortion, b.underwaterDistortion) &&
			EqualVector(a.waterRefractionTintColor, b.waterRefractionTintColor) &&
			NearlyEqual(a.waterRefractionStrength, b.waterRefractionStrength) &&
			NearlyEqual(a.waterRefractionEdgeSoftness, b.waterRefractionEdgeSoftness) &&
			NearlyEqual(a.waterRefractionTintStrength, b.waterRefractionTintStrength);
	}

	const char* ToProjectCreationOperationStateText(ProjectCreationOperationState state) {
		switch (state) {
		case ProjectCreationOperationState::InProgress: return "In Progress";
		case ProjectCreationOperationState::Failed: return "Failed";
		case ProjectCreationOperationState::RegistrationIncomplete: return "Registration Incomplete";
		case ProjectCreationOperationState::Complete: return "Complete";
		default: return "Unknown";
		}
	}

	const char* ToSolutionGenerationOperationStateText(SolutionGenerationOperationState state) {
		switch (state) {
		case SolutionGenerationOperationState::Staged: return "Pending Migration";
		case SolutionGenerationOperationState::CommitInProgress: return "Commit In Progress";
		case SolutionGenerationOperationState::RollbackInProgress: return "Rollback In Progress";
		case SolutionGenerationOperationState::RolledBack: return "Rolled Back";
		case SolutionGenerationOperationState::Complete: return "Complete";
		case SolutionGenerationOperationState::RecoveryRequired: return "Recovery Required";
		default: return "Unknown";
		}
	}
}

void Game::Initialize() {
	// 基底クラスの初期化処理
	Framework::Initialize();

	const std::string sceneCatalogFilePath =
		ResolveProjectResourcePath("resources/scenes/scenes.json");
	auto showSceneStartupError = [this, &sceneCatalogFilePath](
		SceneStartupErrorKind errorKind,
		const std::string& detail
	) {
		Logger::Log(detail + "\n");
		startupErrorScreen_ = new SceneStartupErrorScreen();
		startupErrorScreen_->Show(
			winApp_->GetHwnd(),
			errorKind,
			detail,
			sceneCatalogFilePath
		);
	};

	sceneCatalog_ = new SceneCatalog();
	std::string sceneCatalogError;
	const bool sceneCatalogLoaded = sceneCatalog_->Load(
		sceneCatalogFilePath,
		sceneCatalogError
	);
	if (!sceneCatalogLoaded) {
		showSceneStartupError(
			SceneStartupErrorKind::CatalogLoad,
			sceneCatalogError
		);
		return;
	}
	std::vector<SceneValidationIssue> sceneValidationIssues;
	if (!SceneValidator::ValidateCatalog(
		*sceneCatalog_,
		sceneValidationIssues
	)) {
		showSceneStartupError(
			SceneStartupErrorKind::CatalogValidation,
			SceneValidator::FormatIssues(sceneValidationIssues)
		);
		return;
	}
	if (!sceneValidationIssues.empty()) {
		Logger::Log(SceneValidator::FormatIssues(sceneValidationIssues) + "\n");
	}
	const SceneDescriptor* startScene = sceneCatalog_->GetStartScene();
	if (!startScene) {
		showSceneStartupError(
			SceneStartupErrorKind::StartScene,
			"startScene does not reference a registered Scene"
		);
		return;
	}
	sceneFactory_ = new SceneFactory();
	std::string bootstrapError;
	const SceneStartupMode startupMode =
		sceneCatalog_->GetStartupMode(GetCurrentBuildConfiguration());
	if (startupMode == SceneStartupMode::Editor) {
		editorBootstrap_ = new EditorBootstrap();
		if (!editorBootstrap_->Initialize(
			sceneCatalog_,
			sceneFactory_,
			*startScene,
			bootstrapError
		)) {
			showSceneStartupError(
				SceneStartupErrorKind::EditorStartup,
				bootstrapError
			);
			return;
		}
		sceneManager_ = editorBootstrap_->GetSceneManager();
		editorSession_ = editorBootstrap_->GetEditorSession();
		executionContext_ = editorSession_;
		sceneAssetService_ = editorBootstrap_->GetSceneAssetService();
		sceneTemplateRegistry_ =
			editorBootstrap_->GetSceneTemplateRegistry();
	} else {
		runtimeBootstrap_ = new RuntimeBootstrap();
		if (!runtimeBootstrap_->Initialize(
			sceneCatalog_,
			sceneFactory_,
			*startScene,
			bootstrapError
		)) {
			showSceneStartupError(
				SceneStartupErrorKind::RuntimeStartup,
				bootstrapError
			);
			return;
		}
		sceneManager_ = runtimeBootstrap_->GetSceneManager();
		executionContext_ = sceneManager_->GetExecutionContext();
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		imguiManager_->SetEditorSession(editorSession_);
		imguiManager_->SetSceneCatalog(sceneCatalog_);
		imguiManager_->SetSceneManager(sceneManager_);
		imguiManager_->SetSceneTemplateRegistry(sceneTemplateRegistry_);
		InitializeProjectLauncher();
	}
#endif

	sceneRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc sceneTargetDesc{};
	sceneTargetDesc.width = dxCommon_->GetClientWidth();
	sceneTargetDesc.height = dxCommon_->GetClientHeight();
	sceneTargetDesc.format = RenderFormats::kSceneHdrFormat;
	sceneTargetDesc.createDepth = true;
	sceneTargetDesc.clearColor[0] = 0.1f;
	sceneTargetDesc.clearColor[1] = 0.2f;
	sceneTargetDesc.clearColor[2] = 0.8f;
	sceneTargetDesc.clearColor[3] = 1.0f;
	sceneRenderTarget_->Initialize(dxCommon_, srvManager_, sceneTargetDesc);
	for (SceneRenderTarget*& renderTarget : postProcessRenderTargets_) {
		renderTarget = new SceneRenderTarget();
		SceneRenderTarget::Desc postTargetDesc{};
		postTargetDesc.width = dxCommon_->GetClientWidth();
		postTargetDesc.height = dxCommon_->GetClientHeight();
		postTargetDesc.format = RenderFormats::kDisplayFormat;
		postTargetDesc.createDepth = false;
		postTargetDesc.clearColor[0] = 0.0f;
		postTargetDesc.clearColor[1] = 0.0f;
		postTargetDesc.clearColor[2] = 0.0f;
		postTargetDesc.clearColor[3] = 1.0f;
		renderTarget->Initialize(dxCommon_, srvManager_, postTargetDesc);
	}

	textOverlayRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc textOverlayDesc{};
	textOverlayDesc.width = dxCommon_->GetClientWidth();
	textOverlayDesc.height = dxCommon_->GetClientHeight();
	textOverlayDesc.format = RenderFormats::kDisplayFormat;
	textOverlayDesc.createDepth = false;
	textOverlayDesc.clearColor[3] = 1.0f;
	textOverlayRenderTarget_->Initialize(dxCommon_, srvManager_, textOverlayDesc);
	motionBlurHistoryRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc motionBlurHistoryDesc{};
	motionBlurHistoryDesc.width = dxCommon_->GetClientWidth();
	motionBlurHistoryDesc.height = dxCommon_->GetClientHeight();
	motionBlurHistoryDesc.format = RenderFormats::kDisplayFormat;
	motionBlurHistoryDesc.createDepth = false;
	motionBlurHistoryDesc.clearColor[3] = 1.0f;
	motionBlurHistoryRenderTarget_->Initialize(
		dxCommon_, srvManager_, motionBlurHistoryDesc
	);
	foregroundComposeRenderTarget_ = new SceneRenderTarget();
	SceneRenderTarget::Desc foregroundComposeDesc{};
	foregroundComposeDesc.width = dxCommon_->GetClientWidth();
	foregroundComposeDesc.height = dxCommon_->GetClientHeight();
	foregroundComposeDesc.format = RenderFormats::kSceneHdrFormat;
	foregroundComposeDesc.createDepth = false;
	foregroundComposeDesc.clearColor[0] = 0.0f;
	foregroundComposeDesc.clearColor[1] = 0.0f;
	foregroundComposeDesc.clearColor[2] = 0.0f;
	foregroundComposeDesc.clearColor[3] = 1.0f;
	foregroundComposeRenderTarget_->Initialize(
		dxCommon_,
		srvManager_,
		foregroundComposeDesc
	);
	fullscreenCopy_ = new FullscreenCopy();
	fullscreenCopy_->Initialize(dxCommon_);
	bloomRenderer_ = new BloomRenderer();
	bloomRenderer_->Initialize(dxCommon_, srvManager_);

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		modelPreviewRenderTarget_ = new SceneRenderTarget();
		SceneRenderTarget::Desc modelPreviewDesc{};
		modelPreviewDesc.width = 512;
		modelPreviewDesc.height = 512;
		modelPreviewDesc.format = RenderFormats::kSceneHdrFormat;
		modelPreviewDesc.createDepth = true;
		modelPreviewDesc.clearColor[0] = 0.035f;
		modelPreviewDesc.clearColor[1] = 0.04f;
		modelPreviewDesc.clearColor[2] = 0.05f;
		modelPreviewDesc.clearColor[3] = 1.0f;
		modelPreviewRenderTarget_->Initialize(
			dxCommon_,
			srvManager_,
			modelPreviewDesc
		);
		modelPreviewCamera_ = new Camera();
		modelPreviewCamera_->SetOrbitMode(true);
		modelPreviewCamera_->SetFovY(0.7f);
		modelPreviewCamera_->SetAspectRatio(1.0f);
		modelPreviewCamera_->SetNearClip(0.01f);
		modelPreviewCamera_->SetFarClip(10000.0f);
		modelPreviewObject_ = new Object3d();
		modelPreviewObject_->Initialize(Object3dCommon::GetInstance());
		modelPreviewObject_->SetCamera(modelPreviewCamera_);
		// Asset previews must not depend on the active scene's light bindings.
		modelPreviewObject_->SetEnableLighting(false);
		prefabPreviewRenderer_ = new PrefabPreviewRenderer();
		prefabPreviewRenderer_->Initialize(dxCommon_, srvManager_);
	}
#endif
	baseExposure_ = bloomParameters_.exposure;
	currentExposure_ = baseExposure_;

	TextureManager::GetInstance()->LoadTexture("resources/noise0.png");
	TextureManager::GetInstance()->LoadTexture("resources/noise1.png");
	TextureManager::GetInstance()->LoadTexture(kSceneTransitionMaskPath);

	if (executionContext_) {
		const SceneDocument* document = sceneManager_
			? sceneManager_->GetActiveSceneDocument()
			: &executionContext_->GetActiveDocument();
		ApplyPostProcessSettings(document->GetPostProcessSettings());
		appliedPostProcessRevision_ = document->GetRevision();
		appliedPostProcessSourceKey_ = GetActivePostProcessSourceKey();
	}
}

void Game::Update() {
	// 基底クラスの更新処理
	Framework::Update();

	if (startupErrorScreen_) {
		if (startupErrorScreen_->IsCloseRequested()) {
			endRequest_ = true;
		}
		return;
	}
	if (IsEndRequest()) {
		return;
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	UpdateProjectLauncher();
#endif
	const float deltaTime = GetDeltaTime();

	if (noiseAnimate_) {
		noiseTime_ += deltaTime * noiseSpeed_;
	}

	if (executionContext_) {
		const SceneDocument* document = sceneManager_
			? sceneManager_->GetActiveSceneDocument()
			: &executionContext_->GetActiveDocument();
		const std::string sourceKey = GetActivePostProcessSourceKey();
		if (
			sourceKey != appliedPostProcessSourceKey_ ||
			document->GetRevision() != appliedPostProcessRevision_
		) {
			const ScenePostProcessSettings& settings =
				document->GetPostProcessSettings();
			if (!EqualPostProcessSettings(CapturePostProcessSettings(), settings)) {
				ApplyPostProcessSettings(settings);
			}
			appliedPostProcessRevision_ = document->GetRevision();
			appliedPostProcessSourceKey_ = sourceKey;
		}
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {
		Input* input = Input::GetInstance();
		if (input->TriggerKey(DIK_F1)) {
			if (editorSession_->IsEditing()) {
				editorSession_->Play();
			} else {
				editorSession_->Stop();
			}
		}
		if (input->TriggerKey(DIK_F2)) {
			if (editorSession_->IsPlaying()) {
				editorSession_->Pause();
			} else if (editorSession_->IsPaused()) {
				editorSession_->Resume();
			}
		}
	}
#endif

	const bool isEditingAtFrameStart =
		executionContext_ && executionContext_->IsEditing();
	const bool continuedEditing = isEditingAtFrameStart && editorWasEditingLastFrame_;
	if (continuedEditing) {
		editorCameraSnapshot_ = CaptureCameraSnapshot();
	}

	DebugRenderer::GetInstance()->Clear();

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_) {

	ImGui::Begin("Post Process Stack");
	ScenePostProcessSettings postProcessSettings = CapturePostProcessSettings();
	bool postProcessSettingsChanged = false;
	ImGui::Text("Active: %d", GetEnabledPostEffectCount());
	ImGui::SameLine();
	if (ImGui::SmallButton("Disable All")) {
		postProcessSettings.bloomEnabled = false;
		postProcessSettings.grayscaleEnabled = false;
		postProcessSettings.vignetteEnabled = false;
		postProcessSettings.boxBlurEnabled = false;
		postProcessSettings.gaussianBlurEnabled = false;
		postProcessSettings.radialBlurEnabled = false;
		postProcessSettings.depthOfFieldEnabled = false;
		postProcessSettings.noiseEnabled = false;
		postProcessSettings.dissolveEnabled = false;
		postProcessSettings.outlineEnabled = false;
		postProcessSettings.underwaterEnabled = false;
		postProcessSettings.waterRefractionEnabled = false;
		postProcessSettings.pixelationEnabled = false;
		postProcessSettings.chromaticAberrationEnabled = false;
		postProcessSettingsChanged = true;
	}
	ImGui::TextDisabled("Applied from top to bottom");
	ImGui::Text("Current Exposure: %.2f", currentExposure_);
	ImGui::Separator();
	postProcessSettingsChanged |= DrawPostProcessSettingsEditor(postProcessSettings);
	if (ImGui::SmallButton("Reset Noise Time")) {
		noiseTime_ = 0.0f;
	}
	if (postProcessSettingsChanged) {
		ApplyPostProcessSettings(postProcessSettings);
	}

	ImGui::End();

	StorePostProcessSettingsToDocument();

	Camera* editorCamera =
		Object3dCommon::GetInstance()->GetDefaultCamera();
	if (
		editorCamera &&
		imguiManager_->GetSceneViewHeight() > 0
	) {
		editorCamera->SetAspectRatio(
			static_cast<float>(imguiManager_->GetSceneViewWidth()) /
			static_cast<float>(imguiManager_->GetSceneViewHeight())
		);
	}
	}
#endif

#if defined(_DEBUG) || defined(DEVELOPMENT)
	ProcessSceneAssetRequests();
	ProcessSceneInstanceRequests();
	ProcessProjectSettingsRequests();

	std::string requestedSceneId;
	bool discardUnsavedChanges = false;
	if (
		editorSession_ &&
		imguiManager_ &&
		imguiManager_->ConsumeOpenSceneRequest(
			requestedSceneId,
			discardUnsavedChanges
		)
	) {
		const SceneDescriptor* requestedScene = sceneCatalog_
			? sceneCatalog_->Find(requestedSceneId)
			: nullptr;
		if (!requestedScene) {
			Logger::Log(
				"Requested Edit Scene is not registered: " +
				requestedSceneId + "\n"
			);
		} else if (!editorSession_->OpenEditScene(
			requestedScene->id,
			requestedScene->displayName,
			requestedScene->filePath,
			discardUnsavedChanges
		)) {
			Logger::Log(
				"Edit Scene could not be opened: " +
				requestedScene->filePath + "\n"
			);
		} else {
			imguiManager_->NotifyEditSceneOpened();
		}
	}
#endif

	const bool reloadRequested =
		editorSession_ && editorSession_->ConsumeReloadRequest();
	const bool switchingEditScene =
		editorSession_ && editorSession_->IsEditing() &&
		sceneManager_->GetCurrentSceneId() != editorSession_->GetEditSceneId();
	const bool preserveEditorCamera =
		editorSession_ && editorSession_->IsEditing() &&
		reloadRequested && !switchingEditScene;
	const CameraSnapshot editorCameraSnapshot = preserveEditorCamera
		? (
			continuedEditing
				? CaptureCameraSnapshot()
				: editorCameraSnapshot_
		)
		: CameraSnapshot{};
	if (reloadRequested) {
		sceneManager_->ReloadCurrentScene();
	}
	if (Input* input = Input::GetInstance()) {
		const bool altHeld =
			input->PushKey(DIK_LMENU) || input->PushKey(DIK_RMENU);
		const bool playing =
			executionContext_ && executionContext_->IsPlaying();
		const bool gameplayMouseActive = playing && (
			imguiManager_
				? imguiManager_->IsGameplayCameraMouseActive(altHeld)
				: !altHeld
		);
		if (
			editorSession_ &&
			imguiManager_ &&
			playing &&
			gameplayMouseActive
		) {
			input->SetCursorCaptureRect(
				imguiManager_->GetSceneViewMinX(),
				imguiManager_->GetSceneViewMinY(),
				imguiManager_->GetSceneViewMaxX(),
				imguiManager_->GetSceneViewMaxY()
			);
		}
		input->SetCursorCapture(
			gameplayMouseActive,
			imguiManager_
				? imguiManager_->ShouldHideCursorForGameplayCameraMouse()
				: true
		);
	}
	const bool paused =
		executionContext_ && executionContext_->IsPaused();
	const bool pauseStarted =
		paused && !wasPausedLastFrame_;
	const bool pauseEnded =
		!paused && wasPausedLastFrame_;
	if (pauseStarted) {
		BeginPauseDebugCamera();
	} else if (pauseEnded) {
		EndPauseDebugCamera();
	}
	if (!paused) {
		sceneManager_->Update(deltaTime);
	} else {
		sceneManager_->UpdatePaused();
	}
	ApplyRuntimePostProcessSettings();
	if (preserveEditorCamera) {
		RestoreCameraSnapshot(editorCameraSnapshot);
	}
	editorWasEditingLastFrame_ =
		executionContext_ && executionContext_->IsEditing();
	wasPausedLastFrame_ = paused;

	ParticleManager::ExposureFlashEvent exposureFlash{};
	while (ParticleManager::GetInstance()->ConsumeExposureFlashEvent(exposureFlash)) {
		currentExposure_ = (std::max)(
			currentExposure_,
			exposureFlash.exposure
		);
		exposureReturnSpeed_ = (std::max)(
			exposureFlash.returnSpeed,
			0.01f
		);
	}

	const float exposureLerp =
		std::clamp(exposureReturnSpeed_ * deltaTime, 0.0f, 1.0f);
	currentExposure_ += (baseExposure_ - currentExposure_) * exposureLerp;
	bloomParameters_.exposure = currentExposure_;

}

void Game::ProcessSceneAssetRequests() {
	if (!imguiManager_ || !sceneAssetService_ || !editorSession_ ||
		!sceneCatalog_) {
		return;
	}
	SceneAssetRequest request{};
	if (!imguiManager_->ConsumeSceneAssetRequest(request)) {
		return;
	}

	std::string errorMessage;
	bool succeeded = false;
	if (!editorSession_->IsEditing()) {
		errorMessage = "Scene assets can only be managed in Edit Mode";
	} else if (editorSession_->GetEditDocument().IsDirty()) {
		errorMessage = "Save the active Scene before managing Scene assets";
	} else {
		switch (request.operation) {
		case SceneAssetOperation::Create:
			succeeded = sceneAssetService_->CreateScene(
				request.sceneId,
				request.displayName,
				request.assetPath,
				request.templateId,
				errorMessage
			);
			break;
		case SceneAssetOperation::Duplicate:
			succeeded = sceneAssetService_->DuplicateScene(
				request.sourceSceneId,
				request.sceneId,
				request.displayName,
				request.assetPath,
				errorMessage
			);
			break;
		case SceneAssetOperation::Rename:
			if (request.sceneId != editorSession_->GetEditSceneId()) {
				errorMessage = "Only the active Scene can be renamed";
				break;
			}
			succeeded = sceneAssetService_->RenameScene(
				request.sceneId,
				request.displayName,
				request.assetPath,
				errorMessage
			);
			break;
		case SceneAssetOperation::Delete:
			if (request.sceneId == editorSession_->GetEditSceneId()) {
				errorMessage = "The active Scene cannot be deleted";
				break;
			}
			succeeded = sceneAssetService_->DeleteScene(
				request.sceneId,
				errorMessage
			);
			break;
		default:
			errorMessage = "Unknown Scene asset operation";
			break;
		}
	}

	const bool assetChanged = succeeded;
	if (succeeded && request.operation != SceneAssetOperation::Delete) {
		if (!OpenRegisteredEditScene(request.sceneId, true, errorMessage)) {
			succeeded = false;
			if (errorMessage.empty()) {
				errorMessage =
					"Scene asset was updated but could not be opened";
			}
		}
	}
	if (!succeeded && !errorMessage.empty()) {
		Logger::Log(errorMessage + "\n");
	}
	if (assetChanged) {
		imguiManager_->NotifySceneAssetOperationResult(true, {});
	}
	if (!succeeded) {
		imguiManager_->NotifySceneAssetOperationResult(false, errorMessage);
	}
}

void Game::ProcessSceneInstanceRequests() {
	if (!imguiManager_ || !sceneManager_ || !editorSession_) {
		return;
	}
	SceneInstanceRequest request{};
	if (!imguiManager_->ConsumeSceneInstanceRequest(request)) {
		return;
	}

	bool succeeded = false;
	std::string message;
	if (editorSession_->IsEditing()) {
		message = "Scene Instance operations are available in Play Mode only";
	} else {
		switch (request.operation) {
		case SceneInstanceOperation::LoadAdditive:
			succeeded = sceneManager_->LoadSceneAdditive(
				request.sceneId,
				request.instanceKey
			) != kInvalidSceneInstanceId;
			message = succeeded
				? "Additive Scene load queued."
				: "Additive Scene could not be queued.";
			break;
		case SceneInstanceOperation::Unload:
			succeeded = sceneManager_->UnloadScene(request.instanceId);
			message = succeeded
				? "Scene unload queued."
				: "Scene Instance could not be unloaded.";
			break;
		case SceneInstanceOperation::SetActive:
			succeeded = sceneManager_->SetActiveScene(request.instanceId);
			message = succeeded
				? "Active Scene changed."
				: "Active Scene could not be changed.";
			break;
		case SceneInstanceOperation::SetPersistent:
			succeeded = sceneManager_->SetScenePersistent(
				request.instanceId,
				request.persistent
			);
			message = succeeded
				? "Persistent setting changed."
				: "Persistent setting could not be changed.";
			break;
		default:
			message = "Unknown Scene Instance operation.";
			break;
		}
	}
	if (!succeeded) {
		Logger::Log(message + "\n");
	}
	imguiManager_->NotifySceneInstanceOperationResult(succeeded, message);
}

void Game::ProcessProjectSettingsRequests() {
	if (!imguiManager_ || !sceneCatalog_) {
		return;
	}
	std::string requestedStartSceneId;
	if (imguiManager_->ConsumeStartSceneRequest(requestedStartSceneId)) {
		const SceneCatalog catalogSnapshot = *sceneCatalog_;
		std::string errorMessage;
		const bool succeeded = sceneCatalog_->SetStartScene(
			requestedStartSceneId,
			errorMessage
		) && sceneCatalog_->Save(errorMessage);
		if (!succeeded) {
			*sceneCatalog_ = catalogSnapshot;
			if (!errorMessage.empty()) {
				Logger::Log(errorMessage + "\n");
			}
		}
		imguiManager_->NotifyProjectSettingsResult(
			succeeded,
			errorMessage
		);
	}

	SceneBuildConfiguration configuration{};
	SceneStartupMode mode{};
	if (imguiManager_->ConsumeStartupModeRequest(configuration, mode)) {
		const SceneCatalog catalogSnapshot = *sceneCatalog_;
		std::string errorMessage;
		const bool succeeded = sceneCatalog_->SetStartupMode(
			configuration,
			mode,
			errorMessage
		) && sceneCatalog_->Save(errorMessage);
		if (!succeeded) {
			*sceneCatalog_ = catalogSnapshot;
			if (!errorMessage.empty()) {
				Logger::Log(errorMessage + "\n");
			}
		}
		imguiManager_->NotifyProjectSettingsResult(
			succeeded,
			errorMessage
		);
	}
}

void Game::InitializeProjectLauncher() {
	if (projectLauncherInitialized_ || !imguiManager_) {
		return;
	}
	projectLauncherInitialized_ = true;
	projectRegistry_ = new ProjectRegistry();
	std::string errorMessage;
	if (!projectRegistry_->Load(errorMessage)) {
		projectLauncherStatusMessage_ = "Project Registry: " + errorMessage;
		RefreshProjectLauncherView();
		return;
	}

	const std::filesystem::path currentProjectRoot =
		EditableResourcePath::FindProjectRoot();
	const std::filesystem::path templateSourceRoot =
		currentProjectRoot.empty() ? std::filesystem::path{} : currentProjectRoot.parent_path();
	std::error_code filesystemError;
	if (
		projectRegistry_->GetTemplateSourceRoot().empty() &&
		std::filesystem::is_regular_file(
			templateSourceRoot / L"project" / L"tools" / L"New-GameProject.ps1",
			filesystemError
		)
	) {
		projectRegistry_->SetTemplateSourceRoot(templateSourceRoot);
	}

	projectCreationService_ = new ProjectCreationService(*projectRegistry_);
	projectSolutionGenerationService_ = new ProjectSolutionGenerationService(*projectRegistry_);
	visualStudioLocator_ = new VisualStudioLocator();
	const std::filesystem::path discoveryLog =
		projectRegistry_->GetRegistryPath().parent_path() / L"visual-studio-discovery.log";
	if (!visualStudioLocator_->StartDiscovery(discoveryLog, errorMessage)) {
		projectLauncherStatusMessage_ = "Visual Studio discovery: " + errorMessage;
	} else if (visualStudioLocator_->UsedKnownRootFallback()) {
		projectLauncherStatusMessage_ = "Visual Studio discovery is using known install roots.";
	}
	if (!projectCreationService_->Reconcile(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Project recovery: " + errorMessage;
	}
	if (!projectSolutionGenerationService_->DiscoverPreviews(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Solution preview discovery: " + errorMessage;
	} else if (!projectSolutionGenerationService_->ReconcileRecovery(errorMessage) && projectLauncherStatusMessage_.empty()) {
		projectLauncherStatusMessage_ = "Solution generation recovery: " + errorMessage;
	}
	projectLauncherViewDirty_ = true;
	RefreshProjectLauncherView();
	projectLauncherViewDirty_ = false;
}

void Game::UpdateProjectLauncher() {
	if (!projectLauncherInitialized_ || !projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_ || !visualStudioLocator_) {
		return;
	}
	const std::string statusBefore = projectLauncherStatusMessage_;
	const std::string creationBefore = MakeProjectCreationSnapshotIdentity(projectCreationService_->GetSnapshot());
	const std::string visualStudioBefore = MakeVisualStudioSnapshotIdentity(visualStudioLocator_->GetInstances());
	std::string errorMessage;
	if (!visualStudioLocator_->PollDiscovery(errorMessage)) {
		projectLauncherStatusMessage_ = "Visual Studio discovery: " + errorMessage;
	}
	if (!projectCreationService_->Update(errorMessage)) {
		projectLauncherStatusMessage_ = "Project creation: " + errorMessage;
	}
	if (pendingCreateOpenSolution_ &&
		projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::Complete &&
		projectCreationService_->GetSnapshot().operation.has_value()) {
		const std::filesystem::path descriptorPath =
			projectCreationService_->GetSnapshot().operation->finalProjectRoot / L"game.project.json";
		if (!LaunchProjectSolution(descriptorPath, pendingCreateVisualStudioInstanceId_, errorMessage)) {
			projectLauncherStatusMessage_ = "Project created, but Solution could not be opened: " + errorMessage;
		}
		pendingCreateOpenSolution_ = false;
		pendingCreateVisualStudioInstanceId_.clear();
	}
	if (creationBefore != MakeProjectCreationSnapshotIdentity(projectCreationService_->GetSnapshot()) ||
		visualStudioBefore != MakeVisualStudioSnapshotIdentity(visualStudioLocator_->GetInstances())) {
		projectLauncherViewDirty_ = true;
	}
	const bool sceneDirty = editorSession_ && editorSession_->GetEditDocument().IsDirty();
	const bool prefabDirty = imguiManager_->HasUnsavedPrefabChanges();
	const bool playBlocked = editorSession_ && !editorSession_->IsEditing();
	if (!projectLauncherDynamicStateInitialized_ ||
		projectLauncherSceneDirty_ != sceneDirty ||
		projectLauncherPrefabDirty_ != prefabDirty ||
		projectLauncherPlayBlocked_ != playBlocked) {
		projectLauncherViewDirty_ = true;
		projectLauncherDynamicStateInitialized_ = true;
		projectLauncherSceneDirty_ = sceneDirty;
		projectLauncherPrefabDirty_ = prefabDirty;
		projectLauncherPlayBlocked_ = playBlocked;
	}
	if (statusBefore != projectLauncherStatusMessage_) {
		projectLauncherViewDirty_ = true;
	}
	if (ProcessProjectLauncherRequest()) {
		projectLauncherViewDirty_ = true;
	}
	if (projectLauncherViewDirty_) {
		RefreshProjectLauncherView();
		projectLauncherViewDirty_ = false;
	}
}

void Game::RefreshProjectLauncherView() {
	if (!imguiManager_) {
		return;
	}
	ProjectLauncherView view{};
	view.statusMessage = projectLauncherStatusMessage_;
	if (!projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_ || !visualStudioLocator_) {
		imguiManager_->SetProjectLauncherView(view);
		return;
	}
	view.templateSourceRoot = StringUtility::ToUtf8(projectRegistry_->GetTemplateSourceRoot());
	view.creationInProgress =
		projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::GeneratorRunning;
	view.switchBlockedBySceneDirty = editorSession_ && editorSession_->GetEditDocument().IsDirty();
	view.switchBlockedByPrefabDirty = imguiManager_->HasUnsavedPrefabChanges();
	view.switchBlockedByPlayMode = editorSession_ && !editorSession_->IsEditing();

	const std::optional<VisualStudioInstance> preferredInstance =
		visualStudioLocator_->SelectPreferred({ projectRegistry_->GetPreferredVisualStudioInstanceId(), false, {}, {} });
	bool hasOpenableVisualStudio = false;
	for (const VisualStudioInstance& instance : visualStudioLocator_->GetInstances()) {
		ProjectLauncherVisualStudioView instanceView{};
		instanceView.instanceId = instance.instanceId;
		instanceView.displayName = instance.displayName;
		instanceView.detail = instance.installationVersion + (instance.buildReady ? " (Build Ready)" : " (Openable only)");
		instanceView.openable = instance.openable;
		instanceView.selected = preferredInstance.has_value() && preferredInstance->instanceId == instance.instanceId;
		hasOpenableVisualStudio = hasOpenableVisualStudio || instance.openable;
		view.visualStudioInstances.push_back(std::move(instanceView));
	}

	ProjectCompatibilityProbe compatibilityProbe;
	for (const ProjectRegistryEntry& entry : projectRegistry_->GetProjects()) {
		ProjectLauncherProjectView projectView{};
		projectView.descriptorPath = StringUtility::ToUtf8(entry.descriptorPath);
		projectView.pinned = entry.pinned;
		ProjectDescriptor descriptor{};
		std::string errorMessage;
		if (!descriptor.Load(entry.descriptorPath, errorMessage)) {
			projectView.displayName = entry.descriptorPath.filename().string();
			projectView.status = std::filesystem::exists(entry.descriptorPath) ? "Invalid Descriptor" : "Missing";
			projectView.detail = errorMessage;
			view.projects.push_back(std::move(projectView));
			continue;
		}
		projectView.displayName = descriptor.GetDisplayName();
		projectView.projectId = descriptor.GetProjectId();
		projectView.projectRoot = StringUtility::ToUtf8(descriptor.GetProjectRoot());
		ProjectSolutionPreviewSnapshot generationSnapshot{};
		if (!projectSolutionGenerationService_->InspectProject(entry.descriptorPath, generationSnapshot, errorMessage)) {
			projectView.generationStatus = "Generation Input Error";
			projectView.generationDetail = errorMessage;
		} else {
			projectView.generationStatus = generationSnapshot.state == ProjectSolutionPreviewState::Ready ? "Generation Ready" : "Legacy Manual Solution";
			projectView.generationDetail = generationSnapshot.detail;
			projectView.canGenerateSolutionPreview = generationSnapshot.state == ProjectSolutionPreviewState::Ready;
			projectView.legacyArtifactDirectory = StringUtility::ToUtf8(generationSnapshot.legacyArtifactDirectory);
			projectView.groupedArtifactDirectory = StringUtility::ToUtf8(generationSnapshot.groupedArtifactDirectory);
			projectView.layoutMigrationRequired = generationSnapshot.layoutMigrationRequired;
			projectView.modifiedOwnedArtifactCount = generationSnapshot.modifiedOwnedArtifactCount;
		}
		bool hasPreviewCandidate = false;
		bool selectedPreviewIsActionable = false;
		for (const ProjectSolutionPreviewSnapshot& preview : projectSolutionGenerationService_->GetPreviews()) {
			if (preview.projectId != projectView.projectId || preview.state != ProjectSolutionPreviewState::PreviewReady) {
				continue;
			}
			std::string adoptionError;
			const bool canAdoptSolutionPreview = projectSolutionGenerationService_->CanAdoptPreview(entry.descriptorPath, preview.operationId, adoptionError);
			std::string migrationError;
			const bool canAdoptGroupedSolutionLayout = projectSolutionGenerationService_->CanMigrateOutputLayout(entry.descriptorPath, preview.operationId, migrationError);
			const bool previewIsActionable = canAdoptSolutionPreview || canAdoptGroupedSolutionLayout;
			if (hasPreviewCandidate && (selectedPreviewIsActionable || !previewIsActionable)) {
				continue;
			}
			hasPreviewCandidate = true;
			selectedPreviewIsActionable = previewIsActionable;
			projectView.previewOperationId = preview.operationId;
			projectView.previewSolutionPath = StringUtility::ToUtf8(preview.solutionPath);
			projectView.previewArtifactCount = static_cast<uint32_t>(preview.artifacts.size());
			projectView.canOpenSolutionPreview = true;
			projectView.canAdoptSolutionPreview = canAdoptSolutionPreview;
			projectView.canAdoptGroupedSolutionLayout = canAdoptGroupedSolutionLayout;
			projectView.retiredArtifactCount = generationSnapshot.retiredArtifactCount;
			projectView.generationStatus = "Preview Ready - Not Built";
			if (previewIsActionable) {
				projectView.generationDetail = preview.detail;
				if (projectView.modifiedOwnedArtifactCount != 0) {
					projectView.generationDetail += " Current generated drift: " + std::to_string(projectView.modifiedOwnedArtifactCount) + " owned artifacts will be replaced, not merged.";
				}
			} else {
				projectView.generationDetail = preview.detail + (adoptionError.empty() ? "" : " " + adoptionError) + (migrationError.empty() ? "" : " " + migrationError);
			}
		}
		const ProjectCompatibilityReport report = compatibilityProbe.Probe(descriptor);
		projectView.canOpenSolution = report.solution == ProjectCompatibilityFileState::Available && hasOpenableVisualStudio;
		projectView.canOpenEditor = report.developmentExecutable == ProjectCompatibilityFileState::Available;
		projectView.canSwitchEditor = projectView.canOpenEditor &&
			!view.creationInProgress && !view.switchBlockedBySceneDirty &&
			!view.switchBlockedByPrefabDirty && !view.switchBlockedByPlayMode;
		if (!report.IsDescriptorCompatible()) {
			projectView.status = "Invalid";
			projectView.detail = report.errors.front();
		} else if (!projectView.canOpenEditor) {
			projectView.status = "Not Built";
			projectView.detail = report.warnings.empty() ? "Development executable is unavailable." : report.warnings.front();
		} else if (!hasOpenableVisualStudio) {
			projectView.status = "IDE Missing";
			projectView.detail = "No openable Visual Studio instance is available.";
		} else {
			projectView.status = "Build Ready";
		}
		view.projects.push_back(std::move(projectView));
	}
	for (const ProjectCreationOperation& operation : projectRegistry_->GetCreationOperations()) {
		if (operation.state == ProjectCreationOperationState::Complete) {
			continue;
		}
		ProjectLauncherOperationView operationView{};
		operationView.operationId = operation.operationId;
		operationView.projectId = operation.projectId;
		operationView.state = ToProjectCreationOperationStateText(operation.state);
		operationView.detail = StringUtility::ToUtf8(operation.logPath);
		operationView.canRetryFinalize = operation.state == ProjectCreationOperationState::RegistrationIncomplete || operation.state == ProjectCreationOperationState::Failed;
		view.operations.push_back(std::move(operationView));
	}
	for (const ProjectSolutionRecoverySnapshot& recovery : projectSolutionGenerationService_->GetRecoverySnapshots()) {
		ProjectLauncherGenerationRecoveryView recoveryView{};
		recoveryView.operationId = recovery.operationId;
		recoveryView.projectId = recovery.projectId;
		recoveryView.state = ToSolutionGenerationOperationStateText(recovery.state);
		recoveryView.stagingRoot = StringUtility::ToUtf8(recovery.stagingRoot);
		recoveryView.rollbackRoot = StringUtility::ToUtf8(recovery.rollbackRoot);
		recoveryView.detail = recovery.detail;
		recoveryView.fileCount = recovery.fileCount;
		recoveryView.canRecheck = recovery.canRecheck;
		recoveryView.canCommitStaged = recovery.canCommitStaged;
		recoveryView.canResumeCommit = recovery.canResumeCommit;
		recoveryView.canRestorePrevious = recovery.canRestorePrevious;
		view.generationRecoveries.push_back(std::move(recoveryView));
	}
	imguiManager_->SetProjectLauncherView(view);
}

bool Game::ProcessProjectLauncherRequest() {
	if (!imguiManager_ || !projectRegistry_ || !projectCreationService_ || !projectSolutionGenerationService_) {
		return false;
	}
	ProjectLauncherRequest request{};
	if (!imguiManager_->ConsumeProjectLauncherRequest(request)) {
		return false;
	}
	std::string errorMessage;
	bool succeeded = false;
	switch (request.operation) {
	case ProjectLauncherRequestOperation::Create: {
		ProjectCreationRequest createRequest{};
		createRequest.projectId = request.projectId;
		createRequest.displayName = request.displayName;
		createRequest.destinationRoot = StringUtility::ToPath(request.destinationRoot);
		createRequest.startSceneId = request.startSceneId;
		createRequest.templateSourceRoot = projectRegistry_->GetTemplateSourceRoot();
		succeeded = projectCreationService_->Start(createRequest, errorMessage);
		if (succeeded) {
			pendingCreateOpenSolution_ = request.openSolutionAfterCreate;
			pendingCreateVisualStudioInstanceId_ = request.visualStudioInstanceId;
		}
		break;
	}
	case ProjectLauncherRequestOperation::Import: {
		ProjectDescriptor descriptor{};
		const std::filesystem::path descriptorPath = StringUtility::ToPath(request.descriptorPath);
		succeeded = descriptor.Load(descriptorPath, errorMessage);
		if (succeeded) {
			ProjectRegistryEntry entry{};
			entry.descriptorPath = descriptor.GetDescriptorPath();
			succeeded = projectRegistry_->RegisterProject(entry, errorMessage) && projectRegistry_->Save(errorMessage);
		}
		break;
	}
	case ProjectLauncherRequestOperation::Remove:
		succeeded = projectRegistry_->RemoveProject(StringUtility::ToPath(request.descriptorPath));
		if (succeeded) {
			succeeded = projectRegistry_->Save(errorMessage);
		} else {
			errorMessage = "Project is not registered.";
		}
		break;
	case ProjectLauncherRequestOperation::RetryFinalize:
		succeeded = projectCreationService_->RetryFinalize(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::SetPinned:
		for (const ProjectRegistryEntry& entry : projectRegistry_->GetProjects()) {
			if (StringUtility::ToUtf8(entry.descriptorPath) != request.descriptorPath) {
				continue;
			}
			ProjectRegistryEntry updated = entry;
			updated.pinned = request.pinned;
			succeeded = projectRegistry_->RegisterProject(updated, errorMessage) && projectRegistry_->Save(errorMessage);
			break;
		}
		if (!succeeded && errorMessage.empty()) {
			errorMessage = "Project is not registered.";
		}
		break;
	case ProjectLauncherRequestOperation::OpenFolder:
		succeeded = LaunchProjectFolder(StringUtility::ToPath(request.descriptorPath), errorMessage);
		break;
	case ProjectLauncherRequestOperation::OpenSolution:
		succeeded = LaunchProjectSolution(StringUtility::ToPath(request.descriptorPath), request.visualStudioInstanceId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::GenerateSolutionPreview:
	case ProjectLauncherRequestOperation::RetrySolutionPreview: {
		ProjectSolutionPreviewSnapshot preview{};
		succeeded = projectSolutionGenerationService_->CreatePreview(StringUtility::ToPath(request.descriptorPath), preview, errorMessage);
		if (succeeded && preview.state != ProjectSolutionPreviewState::PreviewReady) {
			errorMessage = preview.detail;
			succeeded = false;
		}
		break;
	}
	case ProjectLauncherRequestOperation::OpenSolutionPreview:
		for (const ProjectSolutionPreviewSnapshot& preview : projectSolutionGenerationService_->GetPreviews()) {
			if (preview.operationId == request.operationId && preview.state == ProjectSolutionPreviewState::PreviewReady) {
				succeeded = LaunchPreviewSolution(preview.solutionPath, request.visualStudioInstanceId, errorMessage);
				break;
			}
		}
		if (!succeeded && errorMessage.empty()) errorMessage = "Preview Solution is unavailable.";
		break;
	case ProjectLauncherRequestOperation::Refresh:
		succeeded = true;
		break;
	case ProjectLauncherRequestOperation::AdoptSolutionPreview:
		succeeded = projectSolutionGenerationService_->AdoptPreview(StringUtility::ToPath(request.descriptorPath), request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::AdoptGroupedSolutionLayout:
		succeeded = projectSolutionGenerationService_->MigrateOutputLayout(StringUtility::ToPath(request.descriptorPath), request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::CommitStagedSolutionGeneration:
		succeeded = projectSolutionGenerationService_->CommitStaged(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::ResumeSolutionGenerationCommit:
		succeeded = projectSolutionGenerationService_->ResumeCommit(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::RecheckSolutionGenerationRecovery:
		succeeded = projectSolutionGenerationService_->RecheckRecovery(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::RestorePreviousSolutionGeneration:
		succeeded = projectSolutionGenerationService_->RestorePrevious(request.operationId, errorMessage);
		break;
	case ProjectLauncherRequestOperation::OpenEditor:
		succeeded = LaunchProjectEditor(StringUtility::ToPath(request.descriptorPath), false, errorMessage);
		break;
	case ProjectLauncherRequestOperation::SwitchEditor:
		if ((editorSession_ && (!editorSession_->IsEditing() || editorSession_->GetEditDocument().IsDirty())) || imguiManager_->HasUnsavedPrefabChanges() || projectCreationService_->GetSnapshot().state == ProjectCreationServiceState::GeneratorRunning) {
			errorMessage = "Switch Editor is blocked by Play mode, unsaved Scene or Prefab changes, or Project creation.";
			break;
		}
		succeeded = LaunchProjectEditor(StringUtility::ToPath(request.descriptorPath), true, errorMessage);
		break;
	default:
		errorMessage = "Unknown Project Launcher request.";
		break;
	}
	projectLauncherStatusMessage_ = succeeded ? "Project Launcher request completed." : errorMessage;
	return true;
}

bool Game::LaunchProjectSolution(
	const std::filesystem::path& descriptorPath,
	const std::string& requestedInstanceId,
	std::string& errorMessage
) {
	if (!visualStudioLocator_) {
		errorMessage = "Visual Studio service is unavailable.";
		return false;
	}
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	ProjectCompatibilityProbe probe;
	const ProjectCompatibilityReport report = probe.Probe(descriptor);
	if (report.solution != ProjectCompatibilityFileState::Available) {
		errorMessage = report.errors.empty() ? "Solution is unavailable." : report.errors.front();
		return false;
	}
	std::optional<VisualStudioInstance> instance;
	for (const VisualStudioInstance& candidate : visualStudioLocator_->GetInstances()) {
		if (candidate.instanceId == requestedInstanceId && candidate.openable) {
			instance = candidate;
			break;
		}
	}
	if (!instance.has_value()) {
		instance = visualStudioLocator_->SelectPreferred(
			VisualStudioLocator::MakeSelectionRequest({}, false, report)
		);
	}
	if (!instance.has_value()) {
		errorMessage = "No openable Visual Studio instance was found.";
		return false;
	}
	WindowsProcess process;
	return visualStudioLocator_->LaunchSolution(
		*instance,
		descriptor.ResolveProjectPath(descriptor.GetPaths().solution),
		process,
		errorMessage
	);
}

bool Game::LaunchPreviewSolution(
	const std::filesystem::path& solutionPath,
	const std::string& requestedInstanceId,
	std::string& errorMessage
) {
	if (!visualStudioLocator_) {
		errorMessage = "Visual Studio service is unavailable.";
		return false;
	}
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(solutionPath, filesystemError) || filesystemError) {
		errorMessage = "Preview Solution is unavailable.";
		return false;
	}
	std::optional<VisualStudioInstance> instance;
	for (const VisualStudioInstance& candidate : visualStudioLocator_->GetInstances()) {
		if (candidate.instanceId == requestedInstanceId && candidate.openable) {
			instance = candidate;
			break;
		}
	}
	if (!instance.has_value()) {
		instance = visualStudioLocator_->SelectPreferred({ {}, false, "v143", "10.0.26100.0" });
	}
	if (!instance.has_value()) {
		errorMessage = "No openable Visual Studio instance was found.";
		return false;
	}
	WindowsProcess process;
	return visualStudioLocator_->LaunchSolution(*instance, solutionPath, process, errorMessage);
}

bool Game::LaunchProjectFolder(
	const std::filesystem::path& descriptorPath,
	std::string& errorMessage
) {
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	wchar_t windowsDirectory[MAX_PATH]{};
	const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		errorMessage = "Windows directory could not be resolved.";
		return false;
	}
	WindowsProcess process;
	WindowsProcessRequest request{};
	request.applicationPath = std::filesystem::path(windowsDirectory) / L"explorer.exe";
	request.arguments = { descriptor.GetProjectRoot().native() };
	request.workingDirectory = descriptor.GetProjectRoot();
	return process.Start(request, errorMessage);
}

bool Game::LaunchProjectEditor(
	const std::filesystem::path& descriptorPath,
	bool switchCurrentEditor,
	std::string& errorMessage
) {
	ProjectDescriptor descriptor{};
	if (!descriptor.Load(descriptorPath, errorMessage)) {
		return false;
	}
	const std::filesystem::path executable = descriptor.ResolveProjectPath(
		descriptor.GetPaths().developmentExecutable
	);
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(executable, filesystemError) || filesystemError) {
		errorMessage = "Target Development executable is unavailable.";
		return false;
	}
	WindowsProcess process;
	WindowsProcessRequest request{};
	request.applicationPath = executable;
	request.workingDirectory = descriptor.GetProjectRoot() / L"project";
	if (!process.Start(request, errorMessage)) {
		return false;
	}
	if (switchCurrentEditor) {
		endRequest_ = true;
	}
	return true;
}

bool Game::OpenRegisteredEditScene(
	const std::string& sceneId,
	bool discardUnsavedChanges,
	std::string& errorMessage
) {
	const SceneDescriptor* scene = sceneCatalog_
		? sceneCatalog_->Find(sceneId)
		: nullptr;
	if (!scene) {
		errorMessage = "Scene is not registered: " + sceneId;
		return false;
	}
	if (!editorSession_ || !editorSession_->OpenEditScene(
		scene->id,
		scene->displayName,
		scene->filePath,
		discardUnsavedChanges
	)) {
		errorMessage = "Edit Scene could not be opened: " + scene->filePath;
		return false;
	}
	if (imguiManager_) {
		imguiManager_->NotifyEditSceneOpened();
	}
	errorMessage.clear();
	return true;
}

void Game::Draw() {
	if (startupErrorScreen_) {
		dxCommon_->PreDraw();
#if defined(_DEBUG) || defined(DEVELOPMENT)
		srvManager_->PreDraw();
		imguiManager_->EndFrame();
#endif
		dxCommon_->PostDraw();
		return;
	}

	// ShadowMapを含む描画用SRVヒープを、影パスより先に設定する。
	srvManager_->PreDraw();

	uint32_t renderWidth = dxCommon_->GetClientWidth();
	uint32_t renderHeight = dxCommon_->GetClientHeight();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (editorSession_ && imguiManager_) {
		renderWidth = imguiManager_->GetSceneViewWidth();
		renderHeight = imguiManager_->GetSceneViewHeight();
	}
#endif
	if (renderHeight > 0) {
		sceneManager_->SetRenderAspectRatio(
			static_cast<float>(renderWidth) /
			static_cast<float>(renderHeight)
		);
	}

	sceneManager_->DrawShadow();
	sceneManager_->DrawOffscreenViews();

	sceneRenderTarget_->Resize(renderWidth, renderHeight);
	if (
		motionBlurHistoryRenderTarget_->GetWidth() != renderWidth ||
		motionBlurHistoryRenderTarget_->GetHeight() != renderHeight
	) {
		motionBlurHistoryRenderTarget_->Resize(renderWidth, renderHeight);
		InvalidateMotionBlurHistory();
	}
	for (SceneRenderTarget* renderTarget : postProcessRenderTargets_) {
		renderTarget->Resize(renderWidth, renderHeight);
	}
	textOverlayRenderTarget_->Resize(renderWidth, renderHeight);
	foregroundComposeRenderTarget_->Resize(
		renderWidth,
		renderHeight
	);

	const bool deferForegroundEffects = waterRefractionEnabled_;
	sceneManager_->SetDeferForegroundEffects(deferForegroundEffects);
	sceneRenderTarget_->Begin();
	srvManager_->PreDraw();
	Object3dCommon::GetInstance()->SetCommonRenderState();
	sceneManager_->Draw();
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (
		editorSession_ &&
		editorSession_->IsEditing() &&
		imguiManager_ &&
		imguiManager_->IsSceneGridVisible()
	) {
		EditorGridRenderer::AddGrid(*DebugRenderer::GetInstance());
	}
#endif
	DebugRenderer::GetInstance()->Draw(
		Object3dCommon::GetInstance()->GetDefaultCamera()
	);
	sceneRenderTarget_->End();

#if defined(_DEBUG) || defined(DEVELOPMENT)
	DrawModelPreview();
	DrawPrefabPreview();
#endif

	fullscreenCopy_->BeginFrame();
	const D3D12_GPU_DESCRIPTOR_HANDLE depthHandle =
		sceneRenderTarget_->GetDepthSrvGpuHandle();
	const char* dissolveMaskPath =
		dissolveMaskIndex_ == 1
			? "resources/noise1.png"
			: "resources/noise0.png";
	const D3D12_GPU_DESCRIPTOR_HANDLE maskHandle =
		TextureManager::GetInstance()->GetSrvHandleGPU(dissolveMaskPath);
	Camera* renderCamera =
		Object3dCommon::GetInstance()->GetDefaultCamera();
	const WaterPostEffectState waterState =
		ResolveWaterPostEffectState(renderCamera);
	auto fillWaterParameters = [&](
		FullscreenCopy::Parameters& parameters
	) {
		if (renderCamera) {
			parameters.cameraNear = renderCamera->GetNearClip();
			parameters.cameraFar = renderCamera->GetFarClip();
			const Matrix4x4& cameraWorld =
				renderCamera->GetWorldMatrix();
			parameters.cameraPositionFovY[0] = cameraWorld.m[3][0];
			parameters.cameraPositionFovY[1] = cameraWorld.m[3][1];
			parameters.cameraPositionFovY[2] = cameraWorld.m[3][2];
			parameters.cameraPositionFovY[3] = renderCamera->GetFovY();
			parameters.cameraRightAspect[0] = cameraWorld.m[0][0];
			parameters.cameraRightAspect[1] = cameraWorld.m[0][1];
			parameters.cameraRightAspect[2] = cameraWorld.m[0][2];
			parameters.cameraRightAspect[3] =
				renderCamera->GetAspectRatio();
			parameters.cameraUpTime[0] = cameraWorld.m[1][0];
			parameters.cameraUpTime[1] = cameraWorld.m[1][1];
			parameters.cameraUpTime[2] = cameraWorld.m[1][2];
			parameters.cameraForwardActive[0] = cameraWorld.m[2][0];
			parameters.cameraForwardActive[1] = cameraWorld.m[2][1];
			parameters.cameraForwardActive[2] = cameraWorld.m[2][2];
		}
		parameters.cameraUpTime[3] = noiseTime_;
		parameters.cameraForwardActive[3] =
			waterState.hasVolume ? 1.0f : 0.0f;
		parameters.underwaterParams[0] =
			waterState.cameraInside ? underwaterIntensity_ : 0.0f;
		parameters.underwaterParams[1] = underwaterFogDensity_;
		parameters.underwaterParams[2] = underwaterDistortion_;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.underwaterTintColor[index] =
				underwaterTintColor_[index];
		}
		parameters.waterVolumeCenterActive[0] = waterState.center.x;
		parameters.waterVolumeCenterActive[1] = waterState.center.y;
		parameters.waterVolumeCenterActive[2] = waterState.center.z;
		parameters.waterVolumeCenterActive[3] =
			waterState.hasVolume ? 1.0f : 0.0f;
		parameters.waterVolumeHalfSizeEdge[0] = waterState.halfSize.x;
		parameters.waterVolumeHalfSizeEdge[1] = waterState.halfSize.y;
		parameters.waterVolumeHalfSizeEdge[2] = waterState.halfSize.z;
		parameters.waterVolumeHalfSizeEdge[3] =
			waterRefractionEdgeSoftness_;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.waterRefractionTintColor[index] =
				waterRefractionTintColor_[index];
		}
		parameters.waterRefractionParams[0] =
			waterRefractionStrength_;
		parameters.waterRefractionParams[1] =
			waterRefractionTintStrength_;
		parameters.waterLightColorIntensity[0] =
			waterState.lightColor.x;
		parameters.waterLightColorIntensity[1] =
			waterState.lightColor.y;
		parameters.waterLightColorIntensity[2] =
			waterState.lightColor.z;
		parameters.waterLightColorIntensity[3] =
			waterState.lightShaftEnabled ? waterState.lightIntensity : 0.0f;
		parameters.waterLightDirectionDensity[0] =
			waterState.lightDirection.x;
		parameters.waterLightDirectionDensity[1] =
			waterState.lightDirection.y;
		parameters.waterLightDirectionDensity[2] =
			waterState.lightDirection.z;
		parameters.waterLightDirectionDensity[3] =
			waterState.lightDensity;
		parameters.waterLightParams[0] =
			waterState.causticsIntensity;
		parameters.waterLightParams[1] =
			waterState.causticsScale;
		parameters.waterLightParams[2] =
			waterState.causticsSpeed;
		parameters.waterLightParams[3] =
			static_cast<float>(waterState.lightSampleCount);
		parameters.waterLightNoiseParams[0] =
			waterState.lightBreakupStrength;
		parameters.waterLightNoiseParams[1] =
			waterState.lightWarpStrength;
		parameters.waterLightNoiseParams[2] =
			waterState.lightNoiseScale;
	};

	bool waterRefractionAppliedBeforeForeground = false;
	if (deferForegroundEffects) {
		if (
			waterRefractionEnabled_ &&
			waterState.hasVolume &&
			!waterState.cameraInside &&
			renderCamera
		) {
			FullscreenCopy::Parameters parameters{};
			fillWaterParameters(parameters);
			SceneRenderTarget* destination = foregroundComposeRenderTarget_;
			destination->Begin();
			srvManager_->PreDraw();
			fullscreenCopy_->SetParameters(parameters);
			fullscreenCopy_->Draw(
				sceneRenderTarget_->GetSrvGpuHandle(),
				depthHandle,
				maskHandle,
				FullscreenCopy::Effect::kWaterRefraction,
				FullscreenCopy::OutputFormat::kSceneHdr
			);
			destination->End();

			sceneRenderTarget_->Begin(false, false);
			srvManager_->PreDraw();
			fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
			fullscreenCopy_->Draw(
				destination->GetSrvGpuHandle(),
				maskHandle,
				maskHandle,
				FullscreenCopy::Effect::kCopy,
				FullscreenCopy::OutputFormat::kSceneHdr
			);
			sceneManager_->DrawForegroundEffects();
			sceneRenderTarget_->End();
			waterRefractionAppliedBeforeForeground = true;
		} else {
			sceneRenderTarget_->Begin(false, false);
			srvManager_->PreDraw();
			sceneManager_->DrawForegroundEffects();
			sceneRenderTarget_->End();
		}
	}
	sceneManager_->SetDeferForegroundEffects(false);

	auto applySceneHdrEffect = [&](
		FullscreenCopy::Effect effect,
		const FullscreenCopy::Parameters& parameters
	) {
		SceneRenderTarget* destination = foregroundComposeRenderTarget_;
		destination->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(parameters);
		fullscreenCopy_->Draw(
			sceneRenderTarget_->GetSrvGpuHandle(),
			depthHandle,
			maskHandle,
			effect,
			FullscreenCopy::OutputFormat::kSceneHdr
		);
		destination->End();

		sceneRenderTarget_->Begin(false, false);
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
		fullscreenCopy_->Draw(
			destination->GetSrvGpuHandle(),
			maskHandle,
			maskHandle,
			FullscreenCopy::Effect::kCopy,
			FullscreenCopy::OutputFormat::kSceneHdr
		);
		sceneRenderTarget_->End();
	};

	if (
		waterState.hasVolume &&
		waterState.lightShaftEnabled &&
		waterState.lightIntensity > 0.0f &&
		renderCamera
	) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applySceneHdrEffect(
			FullscreenCopy::Effect::kWaterLightShafts,
			parameters
		);
	}

	bloomRenderer_->BeginFrame();
	bloomRenderer_->SetParameters(bloomParameters_);
	bloomRenderer_->Apply(
		sceneRenderTarget_->GetSrvGpuHandle(),
		postProcessRenderTargets_[0]
	);
	D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle =
		postProcessRenderTargets_[0]->GetSrvGpuHandle();
	int passIndex = 1;
	auto applyEffect = [&](
		FullscreenCopy::Effect effect,
		const FullscreenCopy::Parameters& parameters
	) {
		SceneRenderTarget* destination =
			postProcessRenderTargets_[passIndex % 2];
		destination->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(parameters);
		fullscreenCopy_->Draw(
			sourceHandle,
			depthHandle,
			maskHandle,
			effect
		);
		destination->End();
		sourceHandle = destination->GetSrvGpuHandle();
		++passIndex;
	};

	if (grayscaleEnabled_) {
		applyEffect(
			FullscreenCopy::Effect::kGrayscale,
			FullscreenCopy::Parameters{}
		);
	}
	if (vignetteEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.vignetteScale = vignetteScale_;
		parameters.vignettePower = vignettePower_;
		parameters.vignetteIntensity = vignetteIntensity_;
		applyEffect(FullscreenCopy::Effect::kVignette, parameters);
	}
	if (boxBlurEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = boxBlurStrength_;
		parameters.blurRadius = boxBlurKernelSize_ == 5 ? 2u : 1u;
		applyEffect(FullscreenCopy::Effect::kBoxBlur, parameters);
	}
	if (gaussianBlurEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = gaussianBlurStrength_;
		parameters.blurRadius =
			gaussianBlurKernelSize_ == 5 ? 2u : 1u;
		parameters.gaussianSigma = gaussianBlurSigma_;
		applyEffect(FullscreenCopy::Effect::kGaussianBlur, parameters);
	}
	if (depthOfFieldEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.blurStrength = dofBlurStrength_;
		parameters.dofFocusDistance = dofFocusDistance_;
		parameters.dofFocusRange = dofFocusRange_;
		parameters.dofNearStrength = dofNearStrength_;
		parameters.dofFarStrength = dofFarStrength_;
		parameters.dofMaxRadius = dofMaxRadius_;
		Camera* camera =
			Object3dCommon::GetInstance()->GetDefaultCamera();
		if (camera) {
			parameters.cameraNear = camera->GetNearClip();
			parameters.cameraFar = camera->GetFarClip();
		}
		applyEffect(FullscreenCopy::Effect::kDepthOfField, parameters);
	}
	const bool motionBlurPlaying =
		executionContext_ && executionContext_->IsPlaying();
	const SceneInstanceId motionBlurSceneInstanceId = sceneManager_
		? sceneManager_->GetActiveSceneInstanceId()
		: kInvalidSceneInstanceId;
	if (
		motionBlurPlaying != motionBlurLastPlaying_ ||
		motionBlurSceneInstanceId != motionBlurLastSceneInstanceId_
	) {
		InvalidateMotionBlurHistory();
	}
	motionBlurLastPlaying_ = motionBlurPlaying;
	motionBlurLastSceneInstanceId_ = motionBlurSceneInstanceId;

	if (!motionBlurEnabled_) {
		motionBlurWasEnabled_ = false;
		InvalidateMotionBlurHistory();
	} else {
		Camera* motionBlurCamera =
			Object3dCommon::GetInstance()->GetDefaultCamera();
		if (!motionBlurCamera) {
			InvalidateMotionBlurHistory();
		} else {
			const D3D12_GPU_DESCRIPTOR_HANDLE preBlurSourceHandle = sourceHandle;
			const Matrix4x4 currentViewProjection =
				motionBlurCamera->GetViewProjectionMatrix();
			auto storeMotionBlurHistory = [&] {
				motionBlurHistoryRenderTarget_->Begin();
				srvManager_->PreDraw();
				fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
				fullscreenCopy_->Draw(
					preBlurSourceHandle,
					depthHandle,
					maskHandle,
					FullscreenCopy::Effect::kCopy
				);
				motionBlurHistoryRenderTarget_->End();
			};

			if (!motionBlurWasEnabled_ || !motionBlurHistoryValid_) {
				storeMotionBlurHistory();
				previousMotionBlurViewProjection_ = currentViewProjection;
				motionBlurHistoryValid_ = true;
			} else {
				FullscreenCopy::Parameters parameters{};
				parameters.motionBlurInverseCurrentViewProjection =
					Inverse(currentViewProjection);
				parameters.motionBlurPreviousViewProjection =
					previousMotionBlurViewProjection_;
				parameters.motionBlurParams[0] = motionBlurStrength_;
				parameters.motionBlurParams[1] =
					static_cast<float>(motionBlurSamples_);
				parameters.motionBlurParams[2] = motionBlurMaxRadius_;
				parameters.motionBlurTextureSizeHistoryValid[0] =
					static_cast<float>(postProcessRenderTargets_[0]->GetWidth());
				parameters.motionBlurTextureSizeHistoryValid[1] =
					static_cast<float>(postProcessRenderTargets_[0]->GetHeight());
				parameters.motionBlurTextureSizeHistoryValid[2] = 1.0f;

				SceneRenderTarget* destination =
					postProcessRenderTargets_[passIndex % 2];
				destination->Begin();
				srvManager_->PreDraw();
				fullscreenCopy_->SetParameters(parameters);
				fullscreenCopy_->Draw(
					preBlurSourceHandle,
					depthHandle,
					maskHandle,
					motionBlurHistoryRenderTarget_->GetSrvGpuHandle(),
					FullscreenCopy::Effect::kMotionBlur
				);
				destination->End();
				sourceHandle = destination->GetSrvGpuHandle();
				++passIndex;

				storeMotionBlurHistory();
				previousMotionBlurViewProjection_ = currentViewProjection;
			}
			motionBlurWasEnabled_ = true;
		}
	}
	if (radialBlurEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.radialBlurCenter[0] = radialBlurCenter_[0];
		parameters.radialBlurCenter[1] = radialBlurCenter_[1];
		parameters.radialBlurWidth = radialBlurWidth_;
		parameters.radialBlurSamples =
			static_cast<uint32_t>(radialBlurSamples_);
		applyEffect(FullscreenCopy::Effect::kRadialBlur, parameters);
	}
	if (noiseEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.noiseTime = noiseTime_;
		parameters.noiseAmount = noiseAmount_;
		parameters.noiseScale = noiseScale_;
		parameters.noiseSeed = noiseSeed_;
		applyEffect(FullscreenCopy::Effect::kNoise, parameters);
	}
	if (dissolveEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.dissolveThreshold = dissolveThreshold_;
		parameters.dissolveEdgeWidth = dissolveEdgeWidth_;
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.dissolveEdgeColor[index] =
				dissolveEdgeColor_[index];
		}
		applyEffect(FullscreenCopy::Effect::kDissolve, parameters);
	}
	if (outlineEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.outlineLuminanceWeight = outlineLuminanceWeight_;
		parameters.outlineDepthWeight = outlineDepthWeight_;
		parameters.outlineThreshold = outlineThreshold_;
		parameters.outlineSoftness = outlineSoftness_;
		parameters.outlineThickness = outlineThickness_;
		Camera* camera =
			Object3dCommon::GetInstance()->GetDefaultCamera();
		if (camera) {
			parameters.cameraNear = camera->GetNearClip();
			parameters.cameraFar = camera->GetFarClip();
		}
		parameters.outlineFlags =
			(outlineLuminanceEnabled_ ? 1u : 0u) |
			(outlineDepthEnabled_ ? 2u : 0u);
		for (uint32_t index = 0; index < 4; ++index) {
			parameters.outlineColor[index] = outlineColor_[index];
		}
		applyEffect(FullscreenCopy::Effect::kOutline, parameters);
	}
	if (
		waterRefractionEnabled_ &&
		!waterRefractionAppliedBeforeForeground &&
		waterState.hasVolume &&
		!waterState.cameraInside &&
		renderCamera
	) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applyEffect(FullscreenCopy::Effect::kWaterRefraction, parameters);
	}
	if (underwaterEnabled_ && waterState.cameraInside && renderCamera) {
		FullscreenCopy::Parameters parameters{};
		fillWaterParameters(parameters);
		applyEffect(FullscreenCopy::Effect::kUnderwater, parameters);
	}
	if (pixelationEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.pixelationParams[0] = static_cast<float>(pixelationBlockSize_);
		parameters.pixelationParams[1] = static_cast<float>(postProcessRenderTargets_[0]->GetWidth());
		parameters.pixelationParams[2] = static_cast<float>(postProcessRenderTargets_[0]->GetHeight());
		applyEffect(FullscreenCopy::Effect::kPixelation, parameters);
	}
	if (chromaticAberrationEnabled_) {
		FullscreenCopy::Parameters parameters{};
		parameters.chromaticCenterIntensity[0] = chromaticAberrationCenter_[0];
		parameters.chromaticCenterIntensity[1] = chromaticAberrationCenter_[1];
		parameters.chromaticCenterIntensity[2] = chromaticAberrationIntensity_;
		parameters.chromaticParams[0] = chromaticAberrationFalloff_;
		applyEffect(FullscreenCopy::Effect::kChromaticAberration, parameters);
	}
	if (sceneManager_ && sceneManager_->HasScreenOverlay()) {
		textOverlayRenderTarget_->Begin(false, false);
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(FullscreenCopy::Parameters{});
		fullscreenCopy_->Draw(
			sourceHandle,
			depthHandle,
			maskHandle,
			FullscreenCopy::Effect::kCopy
		);
		sceneManager_->DrawScreenOverlay(renderWidth, renderHeight);
		textOverlayRenderTarget_->End();
		sourceHandle = textOverlayRenderTarget_->GetSrvGpuHandle();
	}
	if (
		sceneManager_ &&
		sceneManager_->IsSceneTransitioning()
	) {
		FullscreenCopy::Parameters parameters{};
		parameters.radialBlurCenter[0] = 0.5f;
		parameters.radialBlurCenter[1] = 0.5f;
		parameters.radialBlurWidth = (
			1.0f - sceneManager_->GetSceneTransitionFadeAmount()
		);
		SceneRenderTarget* destination =
			postProcessRenderTargets_[passIndex % 2];
		destination->Begin();
		srvManager_->PreDraw();
		fullscreenCopy_->SetParameters(parameters);
		fullscreenCopy_->Draw(
			sourceHandle,
			depthHandle,
			TextureManager::GetInstance()->GetSrvHandleGPU(
				kSceneTransitionMaskPath
			),
			FullscreenCopy::Effect::kIrisTransition
		);
		destination->End();
		sourceHandle = destination->GetSrvGpuHandle();
		++passIndex;
	}

#if defined(_DEBUG) || defined(DEVELOPMENT)
	SceneRenderTarget* const postProcessOutputTarget =
		postProcessRenderTargets_[(passIndex - 1) % 2];
	if (editorSession_) {
		imguiManager_->DrawEditorWorkspace(
			sourceHandle,
			postProcessOutputTarget->GetWidth(),
			postProcessOutputTarget->GetHeight(),
			sceneManager_->GetCurrentSceneName().c_str()
		);
	}
#endif

	dxCommon_->PreDraw();
	srvManager_->PreDraw();
	fullscreenCopy_->Draw(sourceHandle, depthHandle, maskHandle);
#if defined(_DEBUG) || defined(DEVELOPMENT)
	imguiManager_->EndFrame();
#endif

	dxCommon_->PostDraw();
}

Game::CameraSnapshot Game::CaptureCameraSnapshot() const {
	CameraSnapshot snapshot{};
	const Object3dCommon* object3dCommon = Object3dCommon::GetInstance();
	const Camera* camera = object3dCommon
		? object3dCommon->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return snapshot;
	}

	snapshot.valid = true;
	snapshot.orbitMode = camera->IsOrbitMode();
	snapshot.translate = camera->GetTranslate();
	snapshot.rotate = camera->GetRotate();
	snapshot.orbitTarget = camera->GetOrbitTarget();
	snapshot.orbitDistance = camera->GetOrbitDistance();
	snapshot.orbitYaw = camera->GetOrbitYaw();
	snapshot.orbitPitch = camera->GetOrbitPitch();
	snapshot.fovY = camera->GetFovY();
	snapshot.aspectRatio = camera->GetAspectRatio();
	snapshot.nearClip = camera->GetNearClip();
	snapshot.farClip = camera->GetFarClip();
	return snapshot;
}

void Game::RestoreCameraSnapshot(const CameraSnapshot& snapshot) const {
	if (!snapshot.valid) {
		return;
	}
	Object3dCommon* object3dCommon = Object3dCommon::GetInstance();
	Camera* camera = object3dCommon
		? object3dCommon->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return;
	}

	camera->SetOrbitMode(snapshot.orbitMode);
	camera->SetTranslate(snapshot.translate);
	camera->SetRotate(snapshot.rotate);
	camera->SetOrbitTarget(snapshot.orbitTarget);
	camera->SetOrbitDistance(snapshot.orbitDistance);
	camera->SetOrbitAngle(snapshot.orbitYaw, snapshot.orbitPitch);
	camera->SetFovY(snapshot.fovY);
	camera->SetAspectRatio(snapshot.aspectRatio);
	camera->SetNearClip(snapshot.nearClip);
	camera->SetFarClip(snapshot.farClip);
	camera->UpdatePreviewMatrices();
}

void Game::BeginPauseDebugCamera() {
	pauseMainCameraSnapshot_ = CaptureCameraSnapshot();
}

void Game::EndPauseDebugCamera() {
	pauseMainCameraSnapshot_ = {};
}

Game::WaterPostEffectState Game::ResolveWaterPostEffectState(
	const Camera* camera
) const {
	WaterPostEffectState state{};
	if (!camera || !sceneManager_) {
		return state;
	}

	std::vector<const SceneDocument*> documents;
	const SceneDocument* activeDocument =
		sceneManager_->GetActiveSceneDocument();
	if (activeDocument) {
		documents.push_back(activeDocument);
	}
	for (const SceneInstance* instance :
		sceneManager_->GetLoadedSceneInstances()) {
		const SceneDocument* document = instance
			? instance->GetDocument()
			: nullptr;
		if (
			document &&
			std::find(documents.begin(), documents.end(), document) ==
				documents.end()
		) {
			documents.push_back(document);
		}
	}
	if (documents.empty()) {
		return state;
	}

	const Vector3 cameraPosition = camera->GetTranslate();
	WaterPostEffectState firstVolume{};
	for (const SceneDocument* document : documents) {
	for (const SceneEntity& entity : document->GetEntities()) {
		if (!IsEntityActiveInHierarchy(*document, entity)) {
			continue;
		}
		const SceneComponent* waterVolume =
			FindEnabledComponent(entity, "WaterVolume");
		if (!waterVolume) {
			continue;
		}

		const Transform transform =
			ResolveScene3DTransform(*document, entity);
		WaterPostEffectState candidate{};
		candidate.hasVolume = true;
		candidate.center = {
			transform.translate.x + waterVolume->waterOffset.x,
			transform.translate.y + waterVolume->waterOffset.y,
			transform.translate.z + waterVolume->waterOffset.z
		};
		candidate.halfSize = {
			(std::max)(waterVolume->waterHalfSize.x, 0.001f),
			(std::max)(waterVolume->waterHalfSize.y, 0.001f),
			(std::max)(waterVolume->waterHalfSize.z, 0.001f)
		};
		candidate.lightShaftEnabled =
			waterVolume->waterLightShaftEnabled;
		candidate.lightColor = waterVolume->waterLightColor;
		candidate.lightDirection = waterVolume->waterLightDirection;
		candidate.lightIntensity =
			(std::max)(waterVolume->waterLightIntensity, 0.0f);
		candidate.lightDensity =
			(std::max)(waterVolume->waterLightDensity, 0.0f);
		candidate.causticsIntensity =
			(std::max)(waterVolume->waterLightCausticsIntensity, 0.0f);
		candidate.causticsScale =
			(std::max)(waterVolume->waterLightCausticsScale, 0.001f);
		candidate.causticsSpeed = waterVolume->waterLightCausticsSpeed;
		candidate.lightBreakupStrength = std::clamp(
			waterVolume->waterLightBreakupStrength,
			0.0f,
			3.0f
		);
		candidate.lightWarpStrength = std::clamp(
			waterVolume->waterLightWarpStrength,
			0.0f,
			3.0f
		);
		candidate.lightNoiseScale =
			(std::max)(waterVolume->waterLightNoiseScale, 0.001f);
		candidate.lightSampleCount = std::clamp(
			waterVolume->waterLightSampleCount,
			4,
			32
		);
		candidate.cameraInside = IsPointInsideAabb(
			cameraPosition,
			candidate.center,
			candidate.halfSize
		);

		if (!firstVolume.hasVolume) {
			firstVolume = candidate;
		}
		if (candidate.cameraInside) {
			return candidate;
		}
	}
	}

	return firstVolume;
}

ScenePostProcessSettings Game::CapturePostProcessSettings() const {
	ScenePostProcessSettings settings{};
	settings.bloomEnabled = bloomParameters_.enabled != 0;
	settings.baseExposure = baseExposure_;
	settings.toneMapMode = bloomParameters_.toneMapMode;
	settings.bloomThreshold = bloomParameters_.threshold;
	settings.bloomSoftKnee = bloomParameters_.softKnee;
	settings.bloomIntensity = bloomParameters_.intensity;
	settings.bloomBlurIterations = bloomParameters_.blurIterations;
	settings.bloomDownsampleScale = bloomParameters_.downsampleScale;
	settings.bloomBlurRadius = bloomParameters_.blurRadius;
	settings.grayscaleEnabled = grayscaleEnabled_;
	settings.vignetteEnabled = vignetteEnabled_;
	settings.boxBlurEnabled = boxBlurEnabled_;
	settings.gaussianBlurEnabled = gaussianBlurEnabled_;
	settings.depthOfFieldEnabled = depthOfFieldEnabled_;
	settings.motionBlurEnabled = motionBlurEnabled_;
	settings.motionBlurStrength = motionBlurStrength_;
	settings.motionBlurSamples = motionBlurSamples_;
	settings.motionBlurMaxRadius = motionBlurMaxRadius_;
	settings.radialBlurEnabled = radialBlurEnabled_;
	settings.noiseEnabled = noiseEnabled_;
	settings.dissolveEnabled = dissolveEnabled_;
	settings.outlineEnabled = outlineEnabled_;
	settings.underwaterEnabled = underwaterEnabled_;
	settings.waterRefractionEnabled = waterRefractionEnabled_;
	settings.pixelationEnabled = pixelationEnabled_;
	settings.pixelationBlockSize = pixelationBlockSize_;
	settings.chromaticAberrationEnabled = chromaticAberrationEnabled_;
	settings.chromaticAberrationCenter = { chromaticAberrationCenter_[0], chromaticAberrationCenter_[1] };
	settings.chromaticAberrationIntensity = chromaticAberrationIntensity_;
	settings.chromaticAberrationFalloff = chromaticAberrationFalloff_;
	settings.vignetteScale = vignetteScale_;
	settings.vignettePower = vignettePower_;
	settings.vignetteIntensity = vignetteIntensity_;
	settings.boxBlurKernelSize = boxBlurKernelSize_;
	settings.boxBlurStrength = boxBlurStrength_;
	settings.gaussianBlurKernelSize = gaussianBlurKernelSize_;
	settings.gaussianBlurSigma = gaussianBlurSigma_;
	settings.gaussianBlurStrength = gaussianBlurStrength_;
	settings.depthOfFieldFocusDistance = dofFocusDistance_;
	settings.depthOfFieldFocusRange = dofFocusRange_;
	settings.depthOfFieldBlurStrength = dofBlurStrength_;
	settings.depthOfFieldNearStrength = dofNearStrength_;
	settings.depthOfFieldFarStrength = dofFarStrength_;
	settings.depthOfFieldMaxRadius = dofMaxRadius_;
	settings.radialBlurCenter = {
		radialBlurCenter_[0],
		radialBlurCenter_[1]
	};
	settings.radialBlurWidth = radialBlurWidth_;
	settings.radialBlurSamples = radialBlurSamples_;
	settings.noiseAnimate = noiseAnimate_;
	settings.noiseAmount = noiseAmount_;
	settings.noiseScale = noiseScale_;
	settings.noiseSpeed = noiseSpeed_;
	settings.noiseSeed = noiseSeed_;
	settings.dissolveMaskIndex = dissolveMaskIndex_;
	settings.dissolveThreshold = dissolveThreshold_;
	settings.dissolveEdgeWidth = dissolveEdgeWidth_;
	settings.dissolveEdgeColor = {
		dissolveEdgeColor_[0],
		dissolveEdgeColor_[1],
		dissolveEdgeColor_[2],
		dissolveEdgeColor_[3]
	};
	settings.outlineLuminanceEnabled = outlineLuminanceEnabled_;
	settings.outlineDepthEnabled = outlineDepthEnabled_;
	settings.outlineLuminanceWeight = outlineLuminanceWeight_;
	settings.outlineDepthWeight = outlineDepthWeight_;
	settings.outlineThreshold = outlineThreshold_;
	settings.outlineSoftness = outlineSoftness_;
	settings.outlineThickness = outlineThickness_;
	settings.outlineColor = {
		outlineColor_[0],
		outlineColor_[1],
		outlineColor_[2],
		outlineColor_[3]
	};
	settings.underwaterTintColor = {
		underwaterTintColor_[0],
		underwaterTintColor_[1],
		underwaterTintColor_[2],
		underwaterTintColor_[3]
	};
	settings.underwaterIntensity = underwaterIntensity_;
	settings.underwaterFogDensity = underwaterFogDensity_;
	settings.underwaterDistortion = underwaterDistortion_;
	settings.waterRefractionTintColor = {
		waterRefractionTintColor_[0],
		waterRefractionTintColor_[1],
		waterRefractionTintColor_[2],
		waterRefractionTintColor_[3]
	};
	settings.waterRefractionStrength = waterRefractionStrength_;
	settings.waterRefractionEdgeSoftness = waterRefractionEdgeSoftness_;
	settings.waterRefractionTintStrength = waterRefractionTintStrength_;
	return settings;
}

void Game::ApplyPostProcessSettings(
	const ScenePostProcessSettings& settings
) {
	bloomParameters_.enabled = settings.bloomEnabled ? 1 : 0;
	baseExposure_ = settings.baseExposure;
	currentExposure_ = baseExposure_;
	bloomParameters_.exposure = baseExposure_;
	bloomParameters_.toneMapMode = settings.toneMapMode;
	bloomParameters_.threshold = settings.bloomThreshold;
	bloomParameters_.softKnee = settings.bloomSoftKnee;
	bloomParameters_.intensity = settings.bloomIntensity;
	bloomParameters_.blurIterations = settings.bloomBlurIterations;
	bloomParameters_.downsampleScale = settings.bloomDownsampleScale;
	bloomParameters_.blurRadius = settings.bloomBlurRadius;
	grayscaleEnabled_ = settings.grayscaleEnabled;
	vignetteEnabled_ = settings.vignetteEnabled;
	boxBlurEnabled_ = settings.boxBlurEnabled;
	gaussianBlurEnabled_ = settings.gaussianBlurEnabled;
	depthOfFieldEnabled_ = settings.depthOfFieldEnabled;
	motionBlurEnabled_ = settings.motionBlurEnabled;
	motionBlurStrength_ = settings.motionBlurStrength;
	motionBlurSamples_ = settings.motionBlurSamples;
	motionBlurMaxRadius_ = settings.motionBlurMaxRadius;
	radialBlurEnabled_ = settings.radialBlurEnabled;
	noiseEnabled_ = settings.noiseEnabled;
	dissolveEnabled_ = settings.dissolveEnabled;
	outlineEnabled_ = settings.outlineEnabled;
	underwaterEnabled_ = settings.underwaterEnabled;
	waterRefractionEnabled_ = settings.waterRefractionEnabled;
	pixelationEnabled_ = settings.pixelationEnabled;
	pixelationBlockSize_ = settings.pixelationBlockSize;
	chromaticAberrationEnabled_ = settings.chromaticAberrationEnabled;
	chromaticAberrationCenter_[0] = settings.chromaticAberrationCenter.x;
	chromaticAberrationCenter_[1] = settings.chromaticAberrationCenter.y;
	chromaticAberrationIntensity_ = settings.chromaticAberrationIntensity;
	chromaticAberrationFalloff_ = settings.chromaticAberrationFalloff;
	vignetteScale_ = settings.vignetteScale;
	vignettePower_ = settings.vignettePower;
	vignetteIntensity_ = settings.vignetteIntensity;
	boxBlurKernelSize_ = settings.boxBlurKernelSize;
	boxBlurStrength_ = settings.boxBlurStrength;
	gaussianBlurKernelSize_ = settings.gaussianBlurKernelSize;
	gaussianBlurSigma_ = settings.gaussianBlurSigma;
	gaussianBlurStrength_ = settings.gaussianBlurStrength;
	dofFocusDistance_ = settings.depthOfFieldFocusDistance;
	dofFocusRange_ = settings.depthOfFieldFocusRange;
	dofBlurStrength_ = settings.depthOfFieldBlurStrength;
	dofNearStrength_ = settings.depthOfFieldNearStrength;
	dofFarStrength_ = settings.depthOfFieldFarStrength;
	dofMaxRadius_ = settings.depthOfFieldMaxRadius;
	radialBlurCenter_[0] = settings.radialBlurCenter.x;
	radialBlurCenter_[1] = settings.radialBlurCenter.y;
	radialBlurWidth_ = settings.radialBlurWidth;
	radialBlurSamples_ = settings.radialBlurSamples;
	noiseAnimate_ = settings.noiseAnimate;
	noiseAmount_ = settings.noiseAmount;
	noiseScale_ = settings.noiseScale;
	noiseSpeed_ = settings.noiseSpeed;
	noiseSeed_ = settings.noiseSeed;
	dissolveMaskIndex_ = settings.dissolveMaskIndex;
	dissolveThreshold_ = settings.dissolveThreshold;
	dissolveEdgeWidth_ = settings.dissolveEdgeWidth;
	dissolveEdgeColor_[0] = settings.dissolveEdgeColor.x;
	dissolveEdgeColor_[1] = settings.dissolveEdgeColor.y;
	dissolveEdgeColor_[2] = settings.dissolveEdgeColor.z;
	dissolveEdgeColor_[3] = settings.dissolveEdgeColor.w;
	outlineLuminanceEnabled_ = settings.outlineLuminanceEnabled;
	outlineDepthEnabled_ = settings.outlineDepthEnabled;
	outlineLuminanceWeight_ = settings.outlineLuminanceWeight;
	outlineDepthWeight_ = settings.outlineDepthWeight;
	outlineThreshold_ = settings.outlineThreshold;
	outlineSoftness_ = settings.outlineSoftness;
	outlineThickness_ = settings.outlineThickness;
	outlineColor_[0] = settings.outlineColor.x;
	outlineColor_[1] = settings.outlineColor.y;
	outlineColor_[2] = settings.outlineColor.z;
	outlineColor_[3] = settings.outlineColor.w;
	underwaterTintColor_[0] = settings.underwaterTintColor.x;
	underwaterTintColor_[1] = settings.underwaterTintColor.y;
	underwaterTintColor_[2] = settings.underwaterTintColor.z;
	underwaterTintColor_[3] = settings.underwaterTintColor.w;
	underwaterIntensity_ = settings.underwaterIntensity;
	underwaterFogDensity_ = settings.underwaterFogDensity;
	underwaterDistortion_ = settings.underwaterDistortion;
	waterRefractionTintColor_[0] = settings.waterRefractionTintColor.x;
	waterRefractionTintColor_[1] = settings.waterRefractionTintColor.y;
	waterRefractionTintColor_[2] = settings.waterRefractionTintColor.z;
	waterRefractionTintColor_[3] = settings.waterRefractionTintColor.w;
	waterRefractionStrength_ = settings.waterRefractionStrength;
	waterRefractionEdgeSoftness_ = settings.waterRefractionEdgeSoftness;
	waterRefractionTintStrength_ = settings.waterRefractionTintStrength;
}

void Game::ApplyRuntimePostProcessSettings() {
	if (!sceneManager_ || !executionContext_ || !executionContext_->IsPlaying()) {
		appliedRuntimePostProcessGeneration_ = static_cast<uint64_t>(-1);
		appliedRuntimePostProcessInstanceId_ = kInvalidSceneInstanceId;
		return;
	}
	ScenePostProcessSettings settings{};
	uint64_t generation = 0;
	if (!sceneManager_->TryGetActiveRuntimePostProcessSettings(
		settings, generation
	)) {
		return;
	}
	const SceneInstanceId instanceId = sceneManager_->GetActiveSceneInstanceId();
	if (
		instanceId == appliedRuntimePostProcessInstanceId_ &&
		generation == appliedRuntimePostProcessGeneration_
	) {
		return;
	}
	ApplyPostProcessSettings(settings);
	appliedRuntimePostProcessInstanceId_ = instanceId;
	appliedRuntimePostProcessGeneration_ = generation;
}

void Game::StorePostProcessSettingsToDocument() {
	if (!editorSession_ || !editorSession_->IsEditing()) {
		return;
	}

	SceneDocument& document = editorSession_->GetEditDocument();
	const std::string sourceKey = GetActivePostProcessSourceKey();
	if (sourceKey != appliedPostProcessSourceKey_) {
		ApplyPostProcessSettings(document.GetPostProcessSettings());
		appliedPostProcessRevision_ = document.GetRevision();
		appliedPostProcessSourceKey_ = sourceKey;
		return;
	}
	const ScenePostProcessSettings settings = CapturePostProcessSettings();
	if (EqualPostProcessSettings(settings, document.GetPostProcessSettings())) {
		appliedPostProcessRevision_ = document.GetRevision();
		appliedPostProcessSourceKey_ = sourceKey;
		return;
	}

	document.SetPostProcessSettings(settings);
	appliedPostProcessRevision_ = document.GetRevision();
	appliedPostProcessSourceKey_ = sourceKey;
}

std::string Game::GetActivePostProcessSourceKey() const {
	if (!executionContext_) {
		return {};
	}
	if (executionContext_->IsEditing()) {
		return "edit:" + executionContext_->GetActiveSceneId();
	}
	if (sceneManager_) {
		const SceneInstanceId instanceId =
			sceneManager_->GetActiveSceneInstanceId();
		if (instanceId != kInvalidSceneInstanceId) {
			return "runtime-instance:" + std::to_string(instanceId);
		}
	}
	return "runtime:" + executionContext_->GetActiveSceneId();
}

void Game::DrawModelPreview() {
	if (
		!imguiManager_ ||
		!modelPreviewRenderTarget_ ||
		!modelPreviewCamera_ ||
		!modelPreviewObject_
	) {
		return;
	}
	if (editorSession_ && !editorSession_->IsEditing()) {
		return;
	}

	std::string modelPath;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float zoom = 1.0f;
	if (!imguiManager_->GetModelPreviewRequest(modelPath, yaw, pitch, zoom)) {
		return;
	}

	if (modelPath != modelPreviewPath_) {
		ModelManager* modelManager = ModelManager::GetInstance();
		modelManager->LoadModel(modelPath);
		Model* model = modelManager->FindModel(modelPath);
		if (!model) {
			return;
		}

		modelPreviewObject_->SetModel(model);
		modelPreviewObject_->SetScale({ 1.0f, 1.0f, 1.0f });
		Vector3 boundsMin{};
		Vector3 boundsMax{};
		if (model->GetLocalBounds(boundsMin, boundsMax)) {
			const Vector3 center = {
				(boundsMin.x + boundsMax.x) * 0.5f,
				(boundsMin.y + boundsMax.y) * 0.5f,
				(boundsMin.z + boundsMax.z) * 0.5f
			};
			const Vector3 halfExtent = {
				(boundsMax.x - boundsMin.x) * 0.5f,
				(boundsMax.y - boundsMin.y) * 0.5f,
				(boundsMax.z - boundsMin.z) * 0.5f
			};
			const float radius = (std::max)(
				std::sqrt(
					halfExtent.x * halfExtent.x +
					halfExtent.y * halfExtent.y +
					halfExtent.z * halfExtent.z
				),
				0.01f
			);
			modelPreviewObject_->SetTranslate({
				-center.x,
				-center.y,
				-center.z
			});
			modelPreviewFitDistance_ =
				radius / std::tan(modelPreviewCamera_->GetFovY() * 0.5f) * 1.25f;
		} else {
			modelPreviewObject_->SetTranslate({ 0.0f, 0.0f, 0.0f });
			modelPreviewFitDistance_ = 5.0f;
		}
		modelPreviewPath_ = modelPath;
	}

	modelPreviewCamera_->SetOrbitTarget({ 0.0f, 0.0f, 0.0f });
	modelPreviewCamera_->SetOrbitAngle(yaw, pitch);
	modelPreviewCamera_->SetOrbitDistance(
		(std::max)(modelPreviewFitDistance_ * zoom, 0.02f)
	);
	modelPreviewCamera_->UpdatePreviewMatrices();
	modelPreviewObject_->Update();

	modelPreviewRenderTarget_->Begin();
	srvManager_->PreDraw();
	Object3dCommon::GetInstance()->SetCommonRenderState();
	modelPreviewObject_->Draw();
	modelPreviewRenderTarget_->End();

	imguiManager_->SetModelPreviewTexture(
		modelPath,
		modelPreviewRenderTarget_->GetSrvGpuHandle(),
		modelPreviewRenderTarget_->GetWidth(),
		modelPreviewRenderTarget_->GetHeight()
	);
}

void Game::DrawPrefabPreview() {
	if (!imguiManager_ || !prefabPreviewRenderer_) {
		return;
	}
	if (editorSession_ && !editorSession_->IsEditing()) {
		return;
	}

	PrefabPreviewRequest request{};
	if (
		!imguiManager_->GetPrefabPreviewRequest(request) ||
		!request.document
	) {
		return;
	}

	PrefabPreviewRenderer::OverlayOptions overlayOptions{};
	overlayOptions.selectedEntityId = request.selectedEntityId;
	overlayOptions.showSkeleton = request.showSkeleton;
	overlayOptions.showJointAxes = request.showJointAxes;
	overlayOptions.showColliders = request.showColliders;
	overlayOptions.showCombatVolumes = request.showCombatVolumes;
	overlayOptions.isolateSelectedCollider = request.isolateSelectedCollider;
	overlayOptions.showGrid = request.showGrid;
	prefabPreviewRenderer_->Render(
		request.assetPath,
		*request.document,
		request.ghostDocument,
		request.width,
		request.height,
		request.yaw,
		request.pitch,
		request.zoom,
		request.framingSerial,
		overlayOptions
	);
	imguiManager_->SetPrefabPreviewTexture(
		request.assetPath,
		request.revision,
		prefabPreviewRenderer_->GetTexture(),
		prefabPreviewRenderer_->GetWidth(),
		prefabPreviewRenderer_->GetHeight(),
		prefabPreviewRenderer_->GetViewMatrix(),
		prefabPreviewRenderer_->GetProjectionMatrix()
	);
}

void Game::Finalize() {
	if (Input* input = Input::GetInstance()) {
		input->SetCursorCapture(false);
	}
	delete startupErrorScreen_;
	startupErrorScreen_ = nullptr;

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_) {
		imguiManager_->SetEditorSession(nullptr);
		imguiManager_->SetSceneCatalog(nullptr);
		imguiManager_->SetSceneManager(nullptr);
		imguiManager_->SetSceneTemplateRegistry(nullptr);
	}
#endif
	delete visualStudioLocator_;
	visualStudioLocator_ = nullptr;
	delete projectCreationService_;
	projectCreationService_ = nullptr;
	delete projectSolutionGenerationService_;
	projectSolutionGenerationService_ = nullptr;
	delete projectRegistry_;
	projectRegistry_ = nullptr;
	projectLauncherInitialized_ = false;
	pendingCreateOpenSolution_ = false;
	pendingCreateVisualStudioInstanceId_.clear();
	delete modelPreviewObject_;
	modelPreviewObject_ = nullptr;
	delete prefabPreviewRenderer_;
	prefabPreviewRenderer_ = nullptr;
	delete modelPreviewCamera_;
	modelPreviewCamera_ = nullptr;
	delete modelPreviewRenderTarget_;
	modelPreviewRenderTarget_ = nullptr;

	delete bloomRenderer_;
	bloomRenderer_ = nullptr;

	delete fullscreenCopy_;
	fullscreenCopy_ = nullptr;

	delete sceneRenderTarget_;
	sceneRenderTarget_ = nullptr;

	delete motionBlurHistoryRenderTarget_;
	motionBlurHistoryRenderTarget_ = nullptr;

	for (SceneRenderTarget*& renderTarget : postProcessRenderTargets_) {
		delete renderTarget;
		renderTarget = nullptr;
	}
	delete textOverlayRenderTarget_;
	textOverlayRenderTarget_ = nullptr;
	delete foregroundComposeRenderTarget_;
	foregroundComposeRenderTarget_ = nullptr;

	sceneManager_ = nullptr;
	executionContext_ = nullptr;
	editorSession_ = nullptr;
	sceneAssetService_ = nullptr;
	sceneTemplateRegistry_ = nullptr;
	delete editorBootstrap_;
	editorBootstrap_ = nullptr;
	delete runtimeBootstrap_;
	runtimeBootstrap_ = nullptr;
	delete sceneCatalog_;
	sceneCatalog_ = nullptr;

	// 基底クラスの終了処理
	Framework::Finalize();
}

int Game::GetEnabledPostEffectCount() const {
	return
		static_cast<int>(grayscaleEnabled_) +
		static_cast<int>(vignetteEnabled_) +
		static_cast<int>(boxBlurEnabled_) +
		static_cast<int>(gaussianBlurEnabled_) +
		static_cast<int>(depthOfFieldEnabled_) +
		static_cast<int>(radialBlurEnabled_) +
		static_cast<int>(noiseEnabled_) +
		static_cast<int>(dissolveEnabled_) +
		static_cast<int>(outlineEnabled_) +
		static_cast<int>(underwaterEnabled_) +
		static_cast<int>(waterRefractionEnabled_) +
		static_cast<int>(pixelationEnabled_) +
		static_cast<int>(chromaticAberrationEnabled_) +
		static_cast<int>(motionBlurEnabled_);
}

void Game::InvalidateMotionBlurHistory() {
	motionBlurHistoryValid_ = false;
}
