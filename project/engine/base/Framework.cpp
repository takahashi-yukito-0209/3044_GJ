// 役割: Frameworkのフレーム更新と終了判定を実装する。
#include "Framework.h"

#include "../utility/Logger.h"
#include "../text/ResourceFontInstaller.h"
#include "D3DResourceLeadChecker.h"
#include "WinApp.h"
#include "DirectXCommon.h"
#include "ImGuiManager.h"
#include "../io/Input.h"
#include "../3d/SrvManager.h"
#include "../3d/ModelManager.h"
#include "../3d/Object3dCommon.h"
#include "../2d/TextureManager.h"
#include "../2d/SpriteCommon.h"
#include "../particle/ParticleCommon.h"
#include "../particle/ParticleManager.h"
#include "../audio/Audio.h"
#include "../audio/MediaFoundationRuntime.h"
#include "../debug/DebugRenderer.h"
#include "../scene/AbstractSceneFactory.h"

#include <algorithm>

void Framework::Run() {
	// ゲームの初期化
	Initialize();

	while (true) // ゲームループ
	{
		// 毎フレーム更新
		Update();

		// 終了リクエストが来たら抜ける
		if (IsEndRequest()) {
			break;
		}

		// 描画
		Draw();
	}

	// ゲームの終了
	Finalize();
}

void Framework::Initialize() {
	Logger::Initialize();
	ResourceFontInstaller::Install();

	checker_ = new D3DResourceLeadChecker();

	winApp_ = new WinApp();
	winApp_->Initialize();
	if (ImGuiManager::LoadStartFullscreenSetting()) {
		winApp_->SetFullscreen(true);
	}

	dxCommon_ = new DirectXCommon();
	dxCommon_->Initialize(winApp_);

	ShowWindow(winApp_->GetHwnd(), SW_SHOW);
	srvManager_ = new SrvManager();
	srvManager_->Initialize(dxCommon_);

	Object3dCommon::GetInstance()->Initialize(dxCommon_);
	TextureManager::GetInstance()->Initialize(dxCommon_, srvManager_);
	ModelManager::GetInstance()->Initialize(dxCommon_);
	SpriteCommon::GetInstance()->Initialize(dxCommon_);
	particleCommon_ = ParticleCommon::GetInstance();
	particleCommon_->Initialize(dxCommon_);
	ParticleManager::GetInstance()->Initialize(particleCommon_, srvManager_);

	input_ = Input::GetInstance();
	input_->GetInstance()->Initialize(winApp_);

	DebugRenderer::GetInstance()->Initialize(dxCommon_);

#if defined(_DEBUG) || defined(DEVELOPMENT)
	imguiManager_ = new ImGuiManager();
	imguiManager_->Initialize(winApp_, dxCommon_, srvManager_);
#endif

	// Decoderより先にMFを開始し、Audio終了後までProcess lifetimeを維持する。
	mediaFoundationRuntime_ = new MediaFoundationRuntime();
	if (!mediaFoundationRuntime_->Initialize()) {
		Logger::Log(
			"Media Foundation: " + mediaFoundationRuntime_->GetLastError() + "\n"
		);
	}
	audio_ = new Audio();
	audio_->Initialize(mediaFoundationRuntime_);

	endRequest_ = false;
	deltaTime_ = 1.0f / 60.0f;
	previousFrameTime_ = std::chrono::steady_clock::now();
}

void Framework::Update() {
	const auto now = std::chrono::steady_clock::now();
	deltaTime_ = std::clamp(
		std::chrono::duration<float>(now - previousFrameTime_).count(),
		0.0f,
		0.1f
	);
	previousFrameTime_ = now;

	if (winApp_->ProcessMessage()) {
		endRequest_ = true;
		return;
	}

	// Voice callbackは通知だけを行い、PCMとSource Voiceの破棄はMain Threadへ集約する。
	if (audio_) {
		audio_->Update(deltaTime_);
	}

	input_->Update();
	if (input_->TriggerKey(DIK_F11)) {
		winApp_->ToggleFullscreen();

		dxCommon_->ResizeSwapChain(
			winApp_->GetClientWidth(),
			winApp_->GetClientHeight()
		);
	}
#if defined(_DEBUG) || defined(DEVELOPMENT)
	imguiManager_->BeginFrame();
#endif
}
void Framework::Finalize() {
	delete sceneFactory_;
	sceneFactory_ = nullptr;

	DebugRenderer::GetInstance()->Finalize();

#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (imguiManager_) {
		imguiManager_->Finalize();
	}
#endif

	if (audio_) {
		audio_->Finalize();
	}
	if (mediaFoundationRuntime_) {
		mediaFoundationRuntime_->Finalize();
	}

	TextureManager::GetInstance()->Finalize();
	ModelManager::GetInstance()->Finalize();
	SpriteCommon::DeleteInstance();
	ParticleManager::GetInstance()->Reset();
	ParticleManager::DeleteInstance();

	if (particleCommon_) {
		particleCommon_->ResetState();
	}
	ParticleCommon::DeleteInstance();
	particleCommon_ = nullptr;

	delete audio_;
	audio_ = nullptr;
	delete mediaFoundationRuntime_;
	mediaFoundationRuntime_ = nullptr;

#if defined(_DEBUG) || defined(DEVELOPMENT)
	delete imguiManager_;
	imguiManager_ = nullptr;
#endif

	delete srvManager_;
	srvManager_ = nullptr;

	Input::GetInstance()->Finalize();

	delete dxCommon_;
	dxCommon_ = nullptr;

	if (winApp_) {
		winApp_->Finalize();
	}
	delete winApp_;
	winApp_ = nullptr;

	delete checker_;
	checker_ = nullptr;

	ResourceFontInstaller::Uninstall();
	Logger::Finalize();
}
