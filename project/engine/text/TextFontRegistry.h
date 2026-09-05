// 役割: System font一覧とresources/fonts内のResource font leaseを所有する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDWriteFactory3;
struct IDWriteFontCollection1;

class TextFontResource {
public:
	~TextFontResource();
	TextFontResource(const TextFontResource&) = delete;
	TextFontResource& operator=(const TextFontResource&) = delete;

	const std::vector<std::string>& GetFamilies() const;
	IDWriteFactory3* GetFactory() const;
	IDWriteFontCollection1* GetCollection() const;

private:
	struct Impl;
	explicit TextFontResource(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
	friend class TextFontRegistry;
};

struct TextFontResolution {
	std::shared_ptr<const TextFontResource> resource;
	std::string cacheKey;
	std::string diagnostic;
};

class TextFontRegistry {
public:
	static TextFontRegistry& GetInstance();
	~TextFontRegistry();
	TextFontRegistry(const TextFontRegistry&) = delete;
	TextFontRegistry& operator=(const TextFontRegistry&) = delete;

	const std::vector<std::string>& GetSystemFamilies();
	const std::vector<std::string>& GetResourcePaths();
	TextFontResolution AcquireResource(const std::string& resourcePath);
	uint64_t GetGeneration() const;
	void Refresh();

private:
	TextFontRegistry();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
