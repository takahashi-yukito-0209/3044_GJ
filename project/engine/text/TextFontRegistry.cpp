// 役割: DirectWriteのSystem font列挙とResource fontの専用collection生成を実装する。
#include "TextFontRegistry.h"

#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"

#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
	using Microsoft::WRL::ComPtr;
	using Path = std::filesystem::path;

	std::wstring Lowercase(const std::wstring& value) {
		std::wstring result = value;
		std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
			return static_cast<wchar_t>(std::towlower(c));
		});
		return result;
	}

	bool SamePathComponent(const Path& left, const Path& right) {
		return Lowercase(left.wstring()) == Lowercase(right.wstring());
	}

	bool IsWithin(const Path& root, const Path& candidate) {
		Path normalizedRoot = root.lexically_normal();
		Path normalizedCandidate = candidate.lexically_normal();
		auto rootIt = normalizedRoot.begin();
		auto candidateIt = normalizedCandidate.begin();
		for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt) {
			if (candidateIt == normalizedCandidate.end() ||
				!SamePathComponent(*rootIt, *candidateIt)) {
				return false;
			}
		}
		return true;
	}

	bool IsDotDotComponent(const Path& path) {
		for (const Path& component : path) {
			if (component == "..") {
				return true;
			}
		}
		return false;
	}

	bool IsFontExtension(const Path& path) {
		std::wstring extension = Lowercase(path.extension().wstring());
		return extension == L".ttf" || extension == L".otf" ||
			extension == L".ttc";
	}

	Path NormalizeResourcePath(const std::string& resourcePath) {
		if (resourcePath.empty()) {
			return {};
		}
		const Path requested = StringUtility::ToPath(resourcePath);
		if (requested.empty() || requested.is_absolute() ||
			requested.has_root_name() || IsDotDotComponent(requested)) {
			return {};
		}
		const Path normalized = requested.lexically_normal();
		auto it = normalized.begin();
		if (it == normalized.end() || !SamePathComponent(*it, Path("resources"))) {
			return {};
		}
		++it;
		if (it == normalized.end() || !SamePathComponent(*it, Path("fonts"))) {
			return {};
		}
		++it;
		if (it == normalized.end()) {
			return {};
		}
		return normalized;
	}

	std::string FormatHRESULT(HRESULT result) {
		std::ostringstream stream;
		stream << "HRESULT 0x" << std::uppercase << std::hex
			<< static_cast<unsigned long>(result);
		return stream.str();
	}

	std::wstring ReadLocalizedFamilyName(
		IDWriteLocalizedStrings* names,
		const wchar_t* locale
	) {
		if (!names) {
			return {};
		}
		UINT32 index = 0;
		BOOL exists = FALSE;
		if (FAILED(names->FindLocaleName(locale, &index, &exists)) || !exists) {
			return {};
		}
		UINT32 length = 0;
		if (FAILED(names->GetStringLength(index, &length))) {
			return {};
		}
		std::wstring value(length + 1, L'\0');
		if (FAILED(names->GetString(index, value.data(), length + 1))) {
			return {};
		}
		value.resize(length);
		return value;
	}

	std::wstring ReadFamilyName(IDWriteFontFamily* family) {
		if (!family) {
			return {};
		}
		ComPtr<IDWriteLocalizedStrings> names;
		if (FAILED(family->GetFamilyNames(&names))) {
			return {};
		}
		std::wstring value = ReadLocalizedFamilyName(names.Get(), L"ja-jp");
		if (value.empty()) {
			value = ReadLocalizedFamilyName(names.Get(), L"en-us");
		}
		if (value.empty() && names->GetCount() > 0) {
			UINT32 length = 0;
			if (SUCCEEDED(names->GetStringLength(0, &length))) {
				value.assign(length + 1, L'\0');
				if (FAILED(names->GetString(0, value.data(), length + 1))) {
					value.clear();
				} else {
					value.resize(length);
				}
			}
		}
		return value;
	}

	struct SystemFamilyContext {
		std::vector<std::string>* values = nullptr;
	};

	int CALLBACK EnumSystemFamily(
		const LOGFONTW* logFont,
		const TEXTMETRICW*,
		DWORD,
		LPARAM parameter
	) {
		if (!logFont || !parameter || logFont->lfFaceName[0] == L'@') {
			return 1;
		}
		auto* context = reinterpret_cast<SystemFamilyContext*>(parameter);
		context->values->push_back(StringUtility::ToUtf8(logFont->lfFaceName));
		return 1;
	}

	std::vector<std::string> EnumerateSystemFamilies() {
		std::vector<std::string> values;
		HDC deviceContext = GetDC(nullptr);
		if (!deviceContext) {
			return values;
		}
		LOGFONTW logFont{};
		logFont.lfCharSet = DEFAULT_CHARSET;
		SystemFamilyContext context{ &values };
		EnumFontFamiliesExW(
			deviceContext,
			&logFont,
			EnumSystemFamily,
			reinterpret_cast<LPARAM>(&context),
			0
		);
		ReleaseDC(nullptr, deviceContext);
		std::sort(values.begin(), values.end(), [](const std::string& left, const std::string& right) {
			const std::string leftLower = [&left] {
				std::string result = left;
				std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
					return static_cast<char>(std::tolower(c));
				});
				return result;
			}();
			const std::string rightLower = [&right] {
				std::string result = right;
				std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
					return static_cast<char>(std::tolower(c));
				});
				return result;
			}();
			return leftLower == rightLower ? left < right : leftLower < rightLower;
		});
		values.erase(std::unique(values.begin(), values.end(), [](const std::string& left, const std::string& right) {
			std::string leftLower = left;
			std::string rightLower = right;
			std::transform(leftLower.begin(), leftLower.end(), leftLower.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			std::transform(rightLower.begin(), rightLower.end(), rightLower.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return leftLower == rightLower;
		}), values.end());
		return values;
	}

	std::vector<std::string> EnumerateResourcePaths() {
		std::vector<std::string> values;
		const Path projectRoot = EditableResourcePath::FindProjectRoot();
		if (projectRoot.empty()) {
			return values;
		}
		const Path fontRoot = projectRoot / "resources" / "fonts";
		std::error_code error;
		if (!std::filesystem::is_directory(fontRoot, error) || error) {
			return values;
		}
		const Path canonicalRoot = std::filesystem::weakly_canonical(fontRoot, error);
		if (error) {
			return values;
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
				std::filesystem::is_regular_file(status) && IsFontExtension(path)) {
				const Path canonicalFile = std::filesystem::weakly_canonical(path, error);
				if (!error && IsWithin(canonicalRoot, canonicalFile)) {
					const Path relative = std::filesystem::relative(path, projectRoot, error);
					if (!error) {
						values.push_back(StringUtility::ToUtf8(relative.lexically_normal()));
					}
				}
			}
			iterator.increment(error);
		}
		std::sort(values.begin(), values.end());
		values.erase(std::unique(values.begin(), values.end()), values.end());
		return values;
	}
}

struct TextFontResource::Impl {
	Microsoft::WRL::ComPtr<IDWriteFactory3> factory;
	Microsoft::WRL::ComPtr<IDWriteFontCollection1> collection;
	std::vector<std::string> families;
};

TextFontResource::TextFontResource(std::unique_ptr<Impl> impl)
	: impl_(std::move(impl)) {}

TextFontResource::~TextFontResource() = default;

const std::vector<std::string>& TextFontResource::GetFamilies() const {
	static const std::vector<std::string> empty;
	return impl_ ? impl_->families : empty;
}

IDWriteFactory3* TextFontResource::GetFactory() const {
	return impl_ ? impl_->factory.Get() : nullptr;
}

IDWriteFontCollection1* TextFontResource::GetCollection() const {
	return impl_ ? impl_->collection.Get() : nullptr;
}

struct TextFontRegistry::Impl {
	struct CacheEntry {
		std::weak_ptr<const TextFontResource> resource;
		std::string cacheKey;
		std::string diagnostic;
	};
	uint64_t generation = 1;
	bool systemFamiliesReady = false;
	bool resourcePathsReady = false;
	std::vector<std::string> systemFamilies;
	std::vector<std::string> resourcePaths;
	std::unordered_map<std::string, CacheEntry> cache;
};

TextFontRegistry::TextFontRegistry()
	: impl_(std::make_unique<Impl>()) {}

TextFontRegistry::~TextFontRegistry() = default;

TextFontRegistry& TextFontRegistry::GetInstance() {
	static TextFontRegistry registry;
	return registry;
}

const std::vector<std::string>& TextFontRegistry::GetSystemFamilies() {
	if (!impl_->systemFamiliesReady) {
		impl_->systemFamilies = EnumerateSystemFamilies();
		impl_->systemFamiliesReady = true;
	}
	return impl_->systemFamilies;
}

const std::vector<std::string>& TextFontRegistry::GetResourcePaths() {
	if (!impl_->resourcePathsReady) {
		impl_->resourcePaths = EnumerateResourcePaths();
		impl_->resourcePathsReady = true;
	}
	return impl_->resourcePaths;
}

uint64_t TextFontRegistry::GetGeneration() const {
	return impl_->generation;
}

void TextFontRegistry::Refresh() {
	++impl_->generation;
	impl_->systemFamilies.clear();
	impl_->resourcePaths.clear();
	impl_->systemFamiliesReady = false;
	impl_->resourcePathsReady = false;
	impl_->cache.clear();
}

TextFontResolution TextFontRegistry::AcquireResource(
	const std::string& resourcePath
) {
	TextFontResolution result{};
	const Path relativePath = NormalizeResourcePath(resourcePath);
	if (relativePath.empty() || !IsFontExtension(relativePath)) {
		result.cacheKey = resourcePath + "|generation=" + std::to_string(impl_->generation);
		result.diagnostic = "Resource font path must be a relative .ttf, .otf, or .ttc under resources/fonts.";
		return result;
	}
	const Path projectRoot = EditableResourcePath::FindProjectRoot();
	if (projectRoot.empty()) {
		result.cacheKey = resourcePath + "|generation=" + std::to_string(impl_->generation);
		result.diagnostic = "Project root could not be resolved.";
		return result;
	}
	const Path fontRoot = projectRoot / "resources" / "fonts";
	std::error_code pathError;
	const Path canonicalRoot = std::filesystem::weakly_canonical(fontRoot, pathError);
	const Path filePath = EditableResourcePath::Resolve(relativePath);
	const Path canonicalFile = std::filesystem::weakly_canonical(filePath, pathError);
	if (pathError) {
		result.cacheKey = resourcePath + "|path-error|generation=" + std::to_string(impl_->generation);
		result.diagnostic = "Resource font path could not be canonicalized.";
		return result;
	}
	if (!IsWithin(canonicalRoot, canonicalFile)) {
		result.cacheKey = resourcePath + "|generation=" + std::to_string(impl_->generation);
		result.diagnostic = "Resource font path resolves outside resources/fonts.";
		return result;
	}
	const std::string normalizedPath = StringUtility::ToUtf8(relativePath.lexically_normal());
	std::error_code error;
	const bool exists = std::filesystem::is_regular_file(filePath, error) && !error;
	const uintmax_t fileSize = exists ? std::filesystem::file_size(filePath, error) : 0;
	const auto modified = exists ? std::filesystem::last_write_time(filePath, error) : std::filesystem::file_time_type{};
	if (error || !exists || fileSize == 0) {
		result.cacheKey = normalizedPath + "|missing|generation=" + std::to_string(impl_->generation);
		result.diagnostic = "Resource font file is missing or empty.";
		impl_->cache[normalizedPath] = { {}, result.cacheKey, result.diagnostic };
		return result;
	}
	result.cacheKey = normalizedPath + "|" + std::to_string(fileSize) + "|" +
		std::to_string(modified.time_since_epoch().count()) + "|generation=" +
		std::to_string(impl_->generation);
	if (const auto found = impl_->cache.find(normalizedPath); found != impl_->cache.end() &&
		found->second.cacheKey == result.cacheKey) {
		result.resource = found->second.resource.lock();
		result.diagnostic = found->second.diagnostic;
		return result;
	}
	try {
		ComPtr<IDWriteFactory3> factory;
		HRESULT hr = DWriteCreateFactory(
			DWRITE_FACTORY_TYPE_ISOLATED,
			__uuidof(IDWriteFactory3),
			reinterpret_cast<IUnknown**>(factory.GetAddressOf())
		);
		if (FAILED(hr)) {
			throw std::runtime_error("Create factory failed: " + FormatHRESULT(hr));
		}
		ComPtr<IDWriteFontFile> fontFile;
		hr = factory->CreateFontFileReference(filePath.c_str(), nullptr, &fontFile);
		if (FAILED(hr)) {
			throw std::runtime_error("Create font file reference failed: " + FormatHRESULT(hr));
		}
		BOOL supported = FALSE;
		DWRITE_FONT_FILE_TYPE fileType{};
		DWRITE_FONT_FACE_TYPE faceType{};
		UINT32 faceCount = 0;
		hr = fontFile->Analyze(&supported, &fileType, &faceType, &faceCount);
		if (FAILED(hr) || !supported || faceCount == 0) {
			throw std::runtime_error("Font file analysis failed: " + FormatHRESULT(hr));
		}
		ComPtr<IDWriteFontSetBuilder> builder;
		hr = factory->CreateFontSetBuilder(&builder);
		if (FAILED(hr)) {
			throw std::runtime_error("Create font set builder failed: " + FormatHRESULT(hr));
		}
		for (UINT32 index = 0; index < faceCount; ++index) {
			ComPtr<IDWriteFontFaceReference> faceReference;
			hr = factory->CreateFontFaceReference(
				fontFile.Get(), index, DWRITE_FONT_SIMULATIONS_NONE, &faceReference
			);
			if (FAILED(hr) || !faceReference || FAILED(builder->AddFontFaceReference(faceReference.Get()))) {
				throw std::runtime_error("Create font face reference failed: " + FormatHRESULT(hr));
			}
		}
		ComPtr<IDWriteFontSet> fontSet;
		hr = builder->CreateFontSet(&fontSet);
		if (FAILED(hr)) {
			throw std::runtime_error("Create font set failed: " + FormatHRESULT(hr));
		}
		ComPtr<IDWriteFontCollection1> collection;
		hr = factory->CreateFontCollectionFromFontSet(fontSet.Get(), &collection);
		if (FAILED(hr)) {
			throw std::runtime_error("Create font collection failed: " + FormatHRESULT(hr));
		}
		auto impl = std::make_unique<TextFontResource::Impl>();
		impl->factory = factory;
		impl->collection = collection;
		for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index) {
			ComPtr<IDWriteFontFamily> family;
			if (FAILED(collection->GetFontFamily(index, &family))) {
				continue;
			}
			const std::wstring familyName = ReadFamilyName(family.Get());
			if (!familyName.empty()) {
				impl->families.push_back(StringUtility::ToUtf8(familyName));
			}
		}
		if (impl->families.empty()) {
			throw std::runtime_error("Font collection contains no family.");
		}
		auto resource = std::shared_ptr<const TextFontResource>(
			new TextFontResource(std::move(impl))
		);
		result.resource = resource;
		impl_->cache[normalizedPath] = { resource, result.cacheKey, {} };
		return result;
	} catch (const std::exception& exception) {
		result.diagnostic = exception.what();
		impl_->cache[normalizedPath] = { {}, result.cacheKey, result.diagnostic };
		return result;
	}
}
