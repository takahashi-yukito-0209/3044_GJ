// 役割: Win32ウィンドウ手続きとメッセージ取得を実装する。
#include "WinApp.h"
#include "../externals/imgui/imgui.h"
#include "../externals/imgui/imgui_impl_dx12.h"
#include "../externals/imgui/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ウィンドウプロシージャ
LRESULT CALLBACK WinApp::WindowProc(HWND hwnd, UINT msg,
							WPARAM wparam, LPARAM lparam){
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if(ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)){
		return 0;
	}
#endif
	// メッセージに応じてゲーム固有の処理を行う
	switch(msg){
		// ウィンドウが破棄された
		case WM_DESTROY:
			// OSに対して、アプリの終了を伝える
			PostQuitMessage(0);
			return 0;
	}

	// 標準のメッセージ処理を行う
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

void WinApp::Initialize(){

	SetProcessDpiAwarenessContext(
		DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
	);
	CoInitializeEx(0, COINIT_MULTITHREADED);

	// ウィンドウプロシージャ
	wc.lpfnWndProc = WindowProc;
	// ウィンドウクラス名（なんでも良い）
	wc.lpszClassName = L"CG2WindowClass";
	// インスタンスハンドル
	wc.hInstance = GetModuleHandle(nullptr);
	// カーソル
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

	// ウィンドウクラスを登録する
	RegisterClass(&wc);


	//ウィンドウサイズを表す構造体にクライアント領域を入れる
	RECT wrc = { 0,0,kClientWidth,kClientHeight };

	//クライアント領域を元に実際のサイズにwrcを変更してもらう
	AdjustWindowRect(&wrc, WS_OVERLAPPEDWINDOW, false);

	// ウィンドウの生成
	hwnd = CreateWindow(
		wc.lpszClassName,      // 利用するクラス名
		L"3044_ツナになりたい",                // タイトルバーの文字（何でも良い）
		WS_OVERLAPPEDWINDOW,   // よく見るウィンドウスタイル
		CW_USEDEFAULT,         // 表示X座標（Windowsに任せる）
		CW_USEDEFAULT,         // 表示Y座標（WindowsOSに任せる）
		wrc.right - wrc.left,  // ウィンドウ横幅
		wrc.bottom - wrc.top,  // ウィンドウ縦幅
		nullptr,               // 親ウィンドウハンドル
		nullptr,               // メニューハンドル
		wc.hInstance,          // インスタンスハンドル
		nullptr);              // オプション


}

void WinApp::Finalize(){
	CloseWindow(hwnd);
	CoUninitialize();
}

bool WinApp::ProcessMessage(){
	MSG msg{};

	while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)){
		if(msg.message == WM_QUIT){
			return true;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return false;
}

uint32_t WinApp::GetClientWidth() const {
	RECT clientRect{};
	GetClientRect(hwnd, &clientRect);
	return static_cast<uint32_t>(clientRect.right - clientRect.left);
}

uint32_t WinApp::GetClientHeight() const {
	RECT clientRect{};
	GetClientRect(hwnd, &clientRect);
	return static_cast<uint32_t>(clientRect.bottom - clientRect.top);
}

void WinApp::ToggleFullscreen() {
	SetFullscreen(!isFullscreen_);
}

void WinApp::SetFullscreen(bool fullscreen) {
	if (isFullscreen_ == fullscreen || hwnd == nullptr) {
		return;
	}

	if (fullscreen) {
		// 元のウィンドウ状態を保存
		GetWindowRect(hwnd, &windowedRect_);
		windowedStyle_ = GetWindowLongPtr(hwnd, GWL_STYLE);

		// 今いるモニターのサイズを取得
		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(MONITORINFO);

		HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		GetMonitorInfo(monitor, &monitorInfo);

		// 枠なしウィンドウに変更
		SetWindowLongPtr(
			hwnd,
			GWL_STYLE,
			windowedStyle_ & ~WS_OVERLAPPEDWINDOW
		);

		SetWindowPos(
			hwnd,
			HWND_TOP,
			monitorInfo.rcMonitor.left,
			monitorInfo.rcMonitor.top,
			monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
			monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED
		);

		isFullscreen_ = true;
	}
	else {
		// 元のウィンドウスタイルへ戻す
		SetWindowLongPtr(hwnd, GWL_STYLE, windowedStyle_);

		SetWindowPos(
			hwnd,
			nullptr,
			windowedRect_.left,
			windowedRect_.top,
			windowedRect_.right - windowedRect_.left,
			windowedRect_.bottom - windowedRect_.top,
			SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
		);

		isFullscreen_ = false;
	}
}
