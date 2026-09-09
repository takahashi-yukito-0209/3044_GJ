// 役割: resources/fonts を走査し、GDI から使用できるプロセス専用フォントとして登録する。
#include "ResourceFontInstaller.h"

#include "../utility/EditableResourcePath.h"
#include "../utility/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace {
	using Path = std::filesystem::path;

	std::vector<Path>& InstalledFontPaths() {
		static std::vector<Path> paths;
		return paths;
	}

	bool IsFontFile(const Path& path) {
		std::wstring extension = path.extension().wstring();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
			return static_cast<wchar_t>(std::towlower(character));
		});
		return extension == L".ttf" || extension == L".otf" || extension == L".ttc";
	}

	std::vector<Path> FindResourceFonts() {
		std::vector<Path> paths;
		const Path projectRoot = EditableResourcePath::FindProjectRoot();
		if (projectRoot.empty()) {
			return paths;
		}
		const Path fontRoot = projectRoot / "resources" / "fonts";
		std::error_code error;
		if (!std::filesystem::is_directory(fontRoot, error) || error) {
			return paths;
		}

		std::filesystem::recursive_directory_iterator iterator(
			fontRoot,
			std::filesystem::directory_options::skip_permission_denied,
			error
		);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			const Path path = iterator->path();
			const auto status = std::filesystem::symlink_status(path, error);
			if (!error && !std::filesystem::is_symlink(status) &&
				std::filesystem::is_regular_file(status) && IsFontFile(path)) {
				paths.push_back(path);
			}
			iterator.increment(error);
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}
}

void ResourceFontInstaller::Install() {
	std::vector<Path>& installedPaths = InstalledFontPaths();
	if (!installedPaths.empty()) {
		return;
	}

	for (const Path& path : FindResourceFonts()) {
		if (AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) == 0) {
			Logger::Log(L"Resource font registration failed: " + path.wstring() + L"\n");
			continue;
		}
		installedPaths.push_back(path);
	}
}

void ResourceFontInstaller::Uninstall() {
	std::vector<Path>& installedPaths = InstalledFontPaths();
	for (auto iterator = installedPaths.rbegin(); iterator != installedPaths.rend(); ++iterator) {
		RemoveFontResourceExW(iterator->c_str(), FR_PRIVATE, nullptr);
	}
	installedPaths.clear();
}
