// 役割: resources/fonts のフォントをアプリケーション実行中だけ Windows に登録する。
#pragma once

namespace ResourceFontInstaller {

// resources/fonts 配下の .ttf/.otf/.ttc をプロセス専用フォントとして登録する。
// 管理者権限や OS への永続的なインストールは必要としない。
void Install();

// Install で登録したプロセス専用フォントを解除する。
void Uninstall();

} // namespace ResourceFontInstaller
