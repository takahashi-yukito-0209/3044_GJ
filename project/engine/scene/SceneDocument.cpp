// 役割: SceneDocumentのJSON入出力、Hierarchy操作、Component検証を実装する。
#include "SceneDocument.h"
#include "PrefabAssetRegistry.h"
#include "SceneDocumentMigrator.h"
#include "SceneEntityQuery.h"
#include "SceneTransformResolver.h"
#include "SceneValidator.h"
#include "../math/Matrix4x4.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <Windows.h>

#include "../../externals/nlohmann/json.hpp"

namespace {
	using json = nlohmann::json;
	using SceneEntityQuery::FindComponent;
	using SceneTransformResolver::ResolveSceneWorldMatrix;

	json SceneInputExpressionToJson(const SceneInputExpression& expression) {
		json groups = json::array();
		for (const SceneInputGroup& group : expression.groups) {
			json terms = json::array();
			for (const SceneInputTerm& term : group.terms) {
				terms.push_back({
					{ "input", term.input },
					{ "phase", term.phase }
				});
			}
			groups.push_back({
				{ "mode", group.mode },
				{ "terms", std::move(terms) }
			});
		}
		return {
			{ "mode", expression.mode },
			{ "groups", std::move(groups) }
		};
	}

	std::string FirstSceneInputExpressionTerm(
		const std::optional<SceneInputExpression>& expression
	) {
		if (!expression) {
			return {};
		}
		for (const SceneInputGroup& group : expression->groups) {
			if (!group.terms.empty()) {
				return group.terms.front().input;
			}
		}
		return {};
	}

	SceneInputExpression ReadSceneInputExpression(const json& source) {
		SceneInputExpression expression{};
		if (!source.is_object()) {
			expression.mode = "__INVALID_MODE__";
			return expression;
		}
		if (const auto mode = source.find("mode"); mode != source.end()) {
			expression.mode = mode->is_string()
				? mode->get<std::string>()
				: "__INVALID_MODE__";
		}
		const auto groups = source.find("groups");
		if (groups == source.end()) {
			return expression;
		}
		if (!groups->is_array()) {
			expression.groups.push_back({ "__INVALID_MODE__", {} });
			return expression;
		}
		for (const json& groupValue : *groups) {
			SceneInputGroup group{};
			if (!groupValue.is_object()) {
				group.mode = "__INVALID_MODE__";
				group.terms.push_back({ "__INVALID_INPUT__", "__INVALID_PHASE__" });
				expression.groups.push_back(std::move(group));
				continue;
			}
			if (const auto mode = groupValue.find("mode");
				mode != groupValue.end()) {
				group.mode = mode->is_string()
					? mode->get<std::string>()
					: "__INVALID_MODE__";
			}
			const auto terms = groupValue.find("terms");
			if (terms == groupValue.end()) {
				expression.groups.push_back(std::move(group));
				continue;
			}
			if (!terms->is_array()) {
				group.terms.push_back({ "__INVALID_INPUT__", "__INVALID_PHASE__" });
				expression.groups.push_back(std::move(group));
				continue;
			}
			for (const json& termValue : *terms) {
				SceneInputTerm term{};
				if (!termValue.is_object()) {
					term.input = "__INVALID_INPUT__";
					term.phase = "__INVALID_PHASE__";
					group.terms.push_back(std::move(term));
					continue;
				}
				if (const auto input = termValue.find("input");
					input != termValue.end()) {
					term.input = input->is_string()
						? input->get<std::string>()
						: "__INVALID_INPUT__";
				}
				if (const auto phase = termValue.find("phase");
					phase != termValue.end()) {
					term.phase = phase->is_string()
						? phase->get<std::string>()
						: "__INVALID_PHASE__";
				}
				group.terms.push_back(std::move(term));
			}
			expression.groups.push_back(std::move(group));
		}
		return expression;
	}

	std::vector<SceneFishingHookRankDefinition> BuildLegacyFishingHookRanks(
		const std::vector<float>& scoreMultipliers,
		const std::vector<Vector4>& colors
	) {
		std::vector<SceneFishingHookRankDefinition> ranks;
		ranks.reserve(10);
		for (size_t index = 0; index < 10; ++index) {
			SceneFishingHookRankDefinition rank{};
			rank.id = "rank_" + std::to_string(index + 1);
			rank.displayName = "Rank " + std::to_string(index + 1);
			if (index < scoreMultipliers.size()) {
				rank.scoreMultiplier = scoreMultipliers[index];
			}
			if (index < colors.size()) {
				rank.color = colors[index];
			}
			ranks.push_back(std::move(rank));
		}
		return ranks;
	}

	const std::vector<SceneFishingHookRankDefinition>& ResolveFishingHookRanksForSave(
		const SceneComponent& component,
		std::vector<SceneFishingHookRankDefinition>& legacyFallback
	) {
		if (!component.fishingHookRanks.empty()) {
			return component.fishingHookRanks;
		}
		legacyFallback = BuildLegacyFishingHookRanks(
			component.fishingHookTierScoreMultipliers,
			component.fishingHookMultiplierColors
		);
		return legacyFallback;
	}

	bool IsPrefabDocumentPath(const std::string& filePath) {
		std::string fileName = StringUtility::ToUtf8(
			StringUtility::ToPath(filePath).filename()
		);
		std::transform(
			fileName.begin(),
			fileName.end(),
			fileName.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			}
		);
		return fileName.ends_with(".prefab.json");
	}

	uint64_t MakeVariantLocalId(
		const std::string& assetId,
		const std::string& salt
	) {
		// Variant追加要素をBaseの連番IDから分離し、Base更新後の衝突を避ける。
		uint64_t hash = 1469598103934665603ull;
		for (unsigned char character : assetId + ":" + salt) {
			hash ^= character;
			hash *= 1099511628211ull;
		}
		return
			(hash & 0x0fffffffffffffffull) |
			(uint64_t{ 1 } << 63);
	}

	bool HasPrefabAssetLink(const SceneEntity& entity) {
		return
			!entity.prefabLinks.empty() ||
			!entity.prefabAssetId.empty() ||
			!entity.prefabSourcePath.empty();
	}

	void EnsurePrefabLinkStack(SceneEntity& entity) {
		if (
			!entity.prefabLinks.empty() ||
			(
				entity.prefabAssetId.empty() &&
				entity.prefabSourcePath.empty() &&
				entity.prefabInstanceRootId == 0 &&
				entity.prefabLocalId == 0
			)
		) {
			return;
		}
		entity.prefabLinks.push_back({
			entity.prefabAssetId,
			entity.prefabSourcePath,
			entity.prefabInstanceRootId,
			entity.prefabLocalId
		});
	}

	void SynchronizeActivePrefabLink(SceneEntity& entity) {
		if (entity.prefabLinks.empty()) {
			entity.prefabAssetId.clear();
			entity.prefabSourcePath.clear();
			entity.prefabInstanceRootId = 0;
			entity.prefabLocalId = 0;
			return;
		}
		const ScenePrefabLink& active = entity.prefabLinks.front();
		entity.prefabAssetId = active.assetId;
		entity.prefabSourcePath = active.sourcePath;
		entity.prefabInstanceRootId = active.instanceRootId;
		entity.prefabLocalId = active.localId;
	}

	const ScenePrefabLink* FindPrefabLink(
		const SceneEntity& entity,
		uint64_t instanceRootId
	) {
		const auto found = std::find_if(
			entity.prefabLinks.begin(),
			entity.prefabLinks.end(),
			[instanceRootId](const ScenePrefabLink& link) {
				return link.instanceRootId == instanceRootId;
			}
		);
		return found == entity.prefabLinks.end() ? nullptr : &(*found);
	}

	ScenePrefabLink* FindPrefabLink(
		SceneEntity& entity,
		uint64_t instanceRootId
	) {
		EnsurePrefabLinkStack(entity);
		return const_cast<ScenePrefabLink*>(FindPrefabLink(
			static_cast<const SceneEntity&>(entity),
			instanceRootId
		));
	}

	size_t FindPrefabLinkDepth(
		const SceneEntity& entity,
		uint64_t instanceRootId
	) {
		const auto found = std::find_if(
			entity.prefabLinks.begin(),
			entity.prefabLinks.end(),
			[instanceRootId](const ScenePrefabLink& link) {
				return link.instanceRootId == instanceRootId;
			}
		);
		return found == entity.prefabLinks.end()
			? entity.prefabLinks.size()
			: static_cast<size_t>(found - entity.prefabLinks.begin());
	}

	size_t FindPrefabLinkInsertionDepth(
		const SceneEntity& entity,
		const SceneEntity& instanceRoot,
		uint64_t targetRootId
	) {
		size_t insertionDepth = 0;
		for (const ScenePrefabLink& outerLink : instanceRoot.prefabLinks) {
			if (outerLink.instanceRootId == targetRootId) {
				break;
			}
			if (
				insertionDepth < entity.prefabLinks.size() &&
				entity.prefabLinks[insertionDepth].instanceRootId ==
					outerLink.instanceRootId
			) {
				++insertionDepth;
			}
		}
		return insertionDepth;
	}

	void SetPrefabLink(
		SceneEntity& entity,
		const ScenePrefabLink& link
	) {
		EnsurePrefabLinkStack(entity);
		ScenePrefabLink* existing = FindPrefabLink(
			entity,
			link.instanceRootId
		);
		if (existing) {
			*existing = link;
		} else {
			entity.prefabLinks.insert(entity.prefabLinks.begin(), link);
		}
		SynchronizeActivePrefabLink(entity);
	}

	void SetPrefabLinkAtDepth(
		SceneEntity& entity,
		const ScenePrefabLink& link,
		size_t depth
	) {
		EnsurePrefabLinkStack(entity);
		ScenePrefabLink* existing = FindPrefabLink(
			entity,
			link.instanceRootId
		);
		if (existing) {
			*existing = link;
		} else {
			const size_t insertionDepth = (std::min)(
				depth,
				entity.prefabLinks.size()
			);
			entity.prefabLinks.insert(
				entity.prefabLinks.begin() + insertionDepth,
				link
			);
		}
		SynchronizeActivePrefabLink(entity);
	}

	void RemovePrefabLink(SceneEntity& entity, uint64_t instanceRootId) {
		EnsurePrefabLinkStack(entity);
		entity.prefabLinks.erase(
			std::remove_if(
				entity.prefabLinks.begin(),
				entity.prefabLinks.end(),
				[instanceRootId](const ScenePrefabLink& link) {
					return link.instanceRootId == instanceRootId;
				}
			),
			entity.prefabLinks.end()
		);
		SynchronizeActivePrefabLink(entity);
	}

	void RemapPrefabLinkRoots(
		SceneEntity& entity,
		const std::unordered_map<uint64_t, uint64_t>& idMap
	) {
		EnsurePrefabLinkStack(entity);
		for (ScenePrefabLink& link : entity.prefabLinks) {
			const auto remapped = idMap.find(link.instanceRootId);
			if (remapped != idMap.end()) {
				link.instanceRootId = remapped->second;
			}
		}
		SynchronizeActivePrefabLink(entity);
	}

	std::string ResolvePrefabAssetPath(const SceneEntity& entity) {
		return PrefabAssetRegistry::ResolvePath(
			entity.prefabAssetId,
			entity.prefabSourcePath
		);
	}

	std::string ResolvePrefabAssetPath(const ScenePrefabLink& link) {
		return PrefabAssetRegistry::ResolvePath(
			link.assetId,
			link.sourcePath
		);
	}

	bool MatchesPrefabInstanceLink(
		const SceneEntity& entity,
		uint64_t rootId,
		const ScenePrefabLink& rootLink
	) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (!entityLink) {
			return false;
		}
		return rootLink.assetId.empty()
			? entityLink->sourcePath == rootLink.sourcePath
			: entityLink->assetId == rootLink.assetId;
	}

	json VectorToJson(const Vector3& value) {
		return json::array({ value.x, value.y, value.z });
	}

	json VectorToJson(const Vector2& value) {
		return json::array({ value.x, value.y });
	}

	json VectorToJson(const Vector4& value) {
		return json::array({ value.x, value.y, value.z, value.w });
	}

	json QuaternionToJson(const Quaternion& value) {
		return json::array({ value.x, value.y, value.z, value.w });
	}

	Vector3 JsonToVector(const json& value, const Vector3& fallback) {
		if (!value.is_array() || value.size() != 3) {
			return fallback;
		}
		return {
			value[0].get<float>(),
			value[1].get<float>(),
			value[2].get<float>()
		};
	}

	Vector2 JsonToVector(const json& value, const Vector2& fallback) {
		if (!value.is_array() || value.size() != 2) {
			return fallback;
		}
		return { value[0].get<float>(), value[1].get<float>() };
	}

	Vector4 JsonToVector(const json& value, const Vector4& fallback) {
		if (!value.is_array() || value.size() != 4) {
			return fallback;
		}
		return {
			value[0].get<float>(), value[1].get<float>(),
			value[2].get<float>(), value[3].get<float>()
		};
	}

	json PostProcessToJson(const ScenePostProcessSettings& settings) {
		return {
			{ "bloomEnabled", settings.bloomEnabled },
			{ "baseExposure", settings.baseExposure },
			{ "toneMapMode", settings.toneMapMode },
			{ "bloomThreshold", settings.bloomThreshold },
			{ "bloomSoftKnee", settings.bloomSoftKnee },
			{ "bloomIntensity", settings.bloomIntensity },
			{ "bloomBlurIterations", settings.bloomBlurIterations },
			{ "bloomDownsampleScale", settings.bloomDownsampleScale },
			{ "bloomBlurRadius", settings.bloomBlurRadius },
			{ "grayscaleEnabled", settings.grayscaleEnabled },
			{ "vignetteEnabled", settings.vignetteEnabled },
			{ "boxBlurEnabled", settings.boxBlurEnabled },
			{ "gaussianBlurEnabled", settings.gaussianBlurEnabled },
			{ "depthOfFieldEnabled", settings.depthOfFieldEnabled },
			{ "motionBlurEnabled", settings.motionBlurEnabled },
			{ "motionBlurStrength", settings.motionBlurStrength },
			{ "motionBlurSamples", settings.motionBlurSamples },
			{ "motionBlurMaxRadius", settings.motionBlurMaxRadius },
			{ "radialBlurEnabled", settings.radialBlurEnabled },
			{ "noiseEnabled", settings.noiseEnabled },
			{ "dissolveEnabled", settings.dissolveEnabled },
			{ "outlineEnabled", settings.outlineEnabled },
			{ "underwaterEnabled", settings.underwaterEnabled },
			{ "waterRefractionEnabled", settings.waterRefractionEnabled },
			{ "pixelationEnabled", settings.pixelationEnabled },
			{ "pixelationBlockSize", settings.pixelationBlockSize },
			{ "chromaticAberrationEnabled", settings.chromaticAberrationEnabled },
			{ "chromaticAberrationCenter", VectorToJson(settings.chromaticAberrationCenter) },
			{ "chromaticAberrationIntensity", settings.chromaticAberrationIntensity },
			{ "chromaticAberrationFalloff", settings.chromaticAberrationFalloff },
			{ "vignetteScale", settings.vignetteScale },
			{ "vignettePower", settings.vignettePower },
			{ "vignetteIntensity", settings.vignetteIntensity },
			{ "boxBlurKernelSize", settings.boxBlurKernelSize },
			{ "boxBlurStrength", settings.boxBlurStrength },
			{ "gaussianBlurKernelSize", settings.gaussianBlurKernelSize },
			{ "gaussianBlurSigma", settings.gaussianBlurSigma },
			{ "gaussianBlurStrength", settings.gaussianBlurStrength },
			{ "depthOfFieldFocusDistance", settings.depthOfFieldFocusDistance },
			{ "depthOfFieldFocusRange", settings.depthOfFieldFocusRange },
			{ "depthOfFieldBlurStrength", settings.depthOfFieldBlurStrength },
			{ "depthOfFieldNearStrength", settings.depthOfFieldNearStrength },
			{ "depthOfFieldFarStrength", settings.depthOfFieldFarStrength },
			{ "depthOfFieldMaxRadius", settings.depthOfFieldMaxRadius },
			{ "radialBlurCenter", VectorToJson(settings.radialBlurCenter) },
			{ "radialBlurWidth", settings.radialBlurWidth },
			{ "radialBlurSamples", settings.radialBlurSamples },
			{ "noiseAnimate", settings.noiseAnimate },
			{ "noiseAmount", settings.noiseAmount },
			{ "noiseScale", settings.noiseScale },
			{ "noiseSpeed", settings.noiseSpeed },
			{ "noiseSeed", settings.noiseSeed },
			{ "dissolveMaskIndex", settings.dissolveMaskIndex },
			{ "dissolveThreshold", settings.dissolveThreshold },
			{ "dissolveEdgeWidth", settings.dissolveEdgeWidth },
			{ "dissolveEdgeColor", VectorToJson(settings.dissolveEdgeColor) },
			{ "outlineLuminanceEnabled", settings.outlineLuminanceEnabled },
			{ "outlineDepthEnabled", settings.outlineDepthEnabled },
			{ "outlineLuminanceWeight", settings.outlineLuminanceWeight },
			{ "outlineDepthWeight", settings.outlineDepthWeight },
			{ "outlineThreshold", settings.outlineThreshold },
			{ "outlineSoftness", settings.outlineSoftness },
			{ "outlineThickness", settings.outlineThickness },
			{ "outlineColor", VectorToJson(settings.outlineColor) },
			{ "underwaterTintColor", VectorToJson(settings.underwaterTintColor) },
			{ "underwaterIntensity", settings.underwaterIntensity },
			{ "underwaterFogDensity", settings.underwaterFogDensity },
			{ "underwaterDistortion", settings.underwaterDistortion },
			{ "waterRefractionTintColor", VectorToJson(settings.waterRefractionTintColor) },
			{ "waterRefractionStrength", settings.waterRefractionStrength },
			{ "waterRefractionEdgeSoftness", settings.waterRefractionEdgeSoftness },
			{ "waterRefractionTintStrength", settings.waterRefractionTintStrength }
		};
	}

	ScenePostProcessSettings PostProcessFromJson(
		const json& source,
		const ScenePostProcessSettings& fallback
	) {
		if (!source.is_object()) {
			return fallback;
		}

		ScenePostProcessSettings settings = fallback;
		settings.bloomEnabled = source.value("bloomEnabled", settings.bloomEnabled);
		settings.baseExposure = source.value("baseExposure", settings.baseExposure);
		settings.toneMapMode = source.value("toneMapMode", settings.toneMapMode);
		settings.bloomThreshold = source.value("bloomThreshold", settings.bloomThreshold);
		settings.bloomSoftKnee = source.value("bloomSoftKnee", settings.bloomSoftKnee);
		settings.bloomIntensity = source.value("bloomIntensity", settings.bloomIntensity);
		settings.bloomBlurIterations = source.value("bloomBlurIterations", settings.bloomBlurIterations);
		settings.bloomDownsampleScale = source.value("bloomDownsampleScale", settings.bloomDownsampleScale);
		settings.bloomBlurRadius = source.value("bloomBlurRadius", settings.bloomBlurRadius);
		settings.grayscaleEnabled = source.value("grayscaleEnabled", settings.grayscaleEnabled);
		settings.vignetteEnabled = source.value("vignetteEnabled", settings.vignetteEnabled);
		settings.boxBlurEnabled = source.value("boxBlurEnabled", settings.boxBlurEnabled);
		settings.gaussianBlurEnabled = source.value("gaussianBlurEnabled", settings.gaussianBlurEnabled);
		settings.depthOfFieldEnabled = source.value("depthOfFieldEnabled", settings.depthOfFieldEnabled);
		settings.motionBlurEnabled = source.value("motionBlurEnabled", settings.motionBlurEnabled);
		settings.motionBlurStrength = std::clamp(
			source.value("motionBlurStrength", settings.motionBlurStrength), 0.0f, 1.0f
		);
		settings.motionBlurSamples = std::clamp(
			source.value("motionBlurSamples", settings.motionBlurSamples), 2, 32
		);
		settings.motionBlurMaxRadius = std::clamp(
			source.value("motionBlurMaxRadius", settings.motionBlurMaxRadius), 0.0f, 64.0f
		);
		settings.radialBlurEnabled = source.value("radialBlurEnabled", settings.radialBlurEnabled);
		settings.noiseEnabled = source.value("noiseEnabled", settings.noiseEnabled);
		settings.dissolveEnabled = source.value("dissolveEnabled", settings.dissolveEnabled);
		settings.outlineEnabled = source.value("outlineEnabled", settings.outlineEnabled);
		settings.underwaterEnabled = source.value("underwaterEnabled", settings.underwaterEnabled);
		settings.waterRefractionEnabled = source.value("waterRefractionEnabled", settings.waterRefractionEnabled);
		settings.pixelationEnabled = source.value("pixelationEnabled", settings.pixelationEnabled);
		settings.pixelationBlockSize = std::clamp(
			source.value("pixelationBlockSize", settings.pixelationBlockSize),
			1,
			64
		);
		settings.chromaticAberrationEnabled = source.value("chromaticAberrationEnabled", settings.chromaticAberrationEnabled);
		if (source.contains("chromaticAberrationCenter")) settings.chromaticAberrationCenter = JsonToVector(source.at("chromaticAberrationCenter"), settings.chromaticAberrationCenter);
		settings.chromaticAberrationIntensity = source.value("chromaticAberrationIntensity", settings.chromaticAberrationIntensity);
		settings.chromaticAberrationFalloff = source.value("chromaticAberrationFalloff", settings.chromaticAberrationFalloff);
		settings.vignetteScale = source.value("vignetteScale", settings.vignetteScale);
		settings.vignettePower = source.value("vignettePower", settings.vignettePower);
		settings.vignetteIntensity = source.value("vignetteIntensity", settings.vignetteIntensity);
		settings.boxBlurKernelSize = source.value("boxBlurKernelSize", settings.boxBlurKernelSize);
		settings.boxBlurStrength = source.value("boxBlurStrength", settings.boxBlurStrength);
		settings.gaussianBlurKernelSize = source.value("gaussianBlurKernelSize", settings.gaussianBlurKernelSize);
		settings.gaussianBlurSigma = source.value("gaussianBlurSigma", settings.gaussianBlurSigma);
		settings.gaussianBlurStrength = source.value("gaussianBlurStrength", settings.gaussianBlurStrength);
		settings.depthOfFieldFocusDistance = source.value("depthOfFieldFocusDistance", settings.depthOfFieldFocusDistance);
		settings.depthOfFieldFocusRange = source.value("depthOfFieldFocusRange", settings.depthOfFieldFocusRange);
		settings.depthOfFieldBlurStrength = source.value("depthOfFieldBlurStrength", settings.depthOfFieldBlurStrength);
		settings.depthOfFieldNearStrength = source.value("depthOfFieldNearStrength", settings.depthOfFieldNearStrength);
		settings.depthOfFieldFarStrength = source.value("depthOfFieldFarStrength", settings.depthOfFieldFarStrength);
		settings.depthOfFieldMaxRadius = source.value("depthOfFieldMaxRadius", settings.depthOfFieldMaxRadius);
		if (source.contains("radialBlurCenter")) {
			settings.radialBlurCenter = JsonToVector(
				source.at("radialBlurCenter"),
				settings.radialBlurCenter
			);
		}
		settings.radialBlurWidth = source.value("radialBlurWidth", settings.radialBlurWidth);
		settings.radialBlurSamples = source.value("radialBlurSamples", settings.radialBlurSamples);
		settings.noiseAnimate = source.value("noiseAnimate", settings.noiseAnimate);
		settings.noiseAmount = source.value("noiseAmount", settings.noiseAmount);
		settings.noiseScale = source.value("noiseScale", settings.noiseScale);
		settings.noiseSpeed = source.value("noiseSpeed", settings.noiseSpeed);
		settings.noiseSeed = source.value("noiseSeed", settings.noiseSeed);
		settings.dissolveMaskIndex = source.value("dissolveMaskIndex", settings.dissolveMaskIndex);
		settings.dissolveThreshold = source.value("dissolveThreshold", settings.dissolveThreshold);
		settings.dissolveEdgeWidth = source.value("dissolveEdgeWidth", settings.dissolveEdgeWidth);
		if (source.contains("dissolveEdgeColor")) {
			settings.dissolveEdgeColor = JsonToVector(
				source.at("dissolveEdgeColor"),
				settings.dissolveEdgeColor
			);
		}
		settings.outlineLuminanceEnabled = source.value("outlineLuminanceEnabled", settings.outlineLuminanceEnabled);
		settings.outlineDepthEnabled = source.value("outlineDepthEnabled", settings.outlineDepthEnabled);
		settings.outlineLuminanceWeight = source.value("outlineLuminanceWeight", settings.outlineLuminanceWeight);
		settings.outlineDepthWeight = source.value("outlineDepthWeight", settings.outlineDepthWeight);
		settings.outlineThreshold = source.value("outlineThreshold", settings.outlineThreshold);
		settings.outlineSoftness = source.value("outlineSoftness", settings.outlineSoftness);
		settings.outlineThickness = source.value("outlineThickness", settings.outlineThickness);
		if (source.contains("outlineColor")) {
			settings.outlineColor = JsonToVector(
				source.at("outlineColor"),
				settings.outlineColor
			);
		}
		if (source.contains("underwaterTintColor")) {
			settings.underwaterTintColor = JsonToVector(
				source.at("underwaterTintColor"),
				settings.underwaterTintColor
			);
		}
		settings.underwaterIntensity = source.value("underwaterIntensity", settings.underwaterIntensity);
		settings.underwaterFogDensity = source.value("underwaterFogDensity", settings.underwaterFogDensity);
		settings.underwaterDistortion = source.value("underwaterDistortion", settings.underwaterDistortion);
		if (source.contains("waterRefractionTintColor")) {
			settings.waterRefractionTintColor = JsonToVector(
				source.at("waterRefractionTintColor"),
				settings.waterRefractionTintColor
			);
		}
		settings.waterRefractionStrength = source.value("waterRefractionStrength", settings.waterRefractionStrength);
		settings.waterRefractionEdgeSoftness = source.value("waterRefractionEdgeSoftness", settings.waterRefractionEdgeSoftness);
		settings.waterRefractionTintStrength = source.value("waterRefractionTintStrength", settings.waterRefractionTintStrength);

		settings.baseExposure = (std::max)(0.01f, settings.baseExposure);
		settings.toneMapMode = std::clamp(settings.toneMapMode, 0, 1);
		settings.bloomThreshold = (std::max)(0.0f, settings.bloomThreshold);
		settings.bloomSoftKnee = std::clamp(settings.bloomSoftKnee, 0.0f, 1.0f);
		settings.bloomIntensity = (std::max)(0.0f, settings.bloomIntensity);
		settings.bloomBlurIterations = std::clamp(settings.bloomBlurIterations, 0, 12);
		settings.bloomDownsampleScale = std::clamp(settings.bloomDownsampleScale, 1, 8);
		settings.bloomBlurRadius = (std::max)(0.0f, settings.bloomBlurRadius);
		settings.boxBlurKernelSize = settings.boxBlurKernelSize == 5 ? 5 : 3;
		settings.gaussianBlurKernelSize = settings.gaussianBlurKernelSize == 5 ? 5 : 3;
		settings.radialBlurSamples = std::clamp(settings.radialBlurSamples, 2, 32);
		settings.dissolveMaskIndex = std::clamp(settings.dissolveMaskIndex, 0, 1);
		settings.underwaterIntensity = std::clamp(settings.underwaterIntensity, 0.0f, 1.0f);
		settings.underwaterFogDensity = (std::max)(0.0f, settings.underwaterFogDensity);
		settings.underwaterDistortion = (std::max)(0.0f, settings.underwaterDistortion);
		settings.waterRefractionStrength =
			std::clamp(settings.waterRefractionStrength, 0.0f, 0.12f);
		settings.waterRefractionEdgeSoftness =
			std::clamp(settings.waterRefractionEdgeSoftness, 0.0f, 2.0f);
		settings.waterRefractionTintStrength =
			std::clamp(settings.waterRefractionTintStrength, 0.0f, 1.0f);
		return settings;
	}

	json DebugSettingsToJson(const SceneDebugSettings& settings) {
		return {
			{ "showCameraDirection", settings.showCameraDirection },
			{ "showColliders", settings.showColliders },
			{ "showCameraPath", settings.showCameraPath },
			{ "showCameraPathPointCameraDirection", settings.showCameraPathPointCameraDirection },
			{ "showSkeleton", settings.showSkeleton },
			{ "showJointNames", settings.showJointNames },
			{ "showJointAxes", settings.showJointAxes },
			{ "jointRadius", settings.jointRadius },
			{ "jointAxisLength", settings.jointAxisLength }
		};
	}

	SceneDebugSettings DebugSettingsFromJson(
		const json& source,
		const SceneDebugSettings& fallback
	) {
		if (!source.is_object()) {
			return fallback;
		}

		SceneDebugSettings settings = fallback;
		settings.showCameraDirection = source.value(
			"showCameraDirection",
			settings.showCameraDirection
		);
		settings.showColliders = source.value(
			"showColliders",
			settings.showColliders
		);
		settings.showCameraPath = source.value(
			"showCameraPath",
			settings.showCameraPath
		);
		settings.showCameraPathPointCameraDirection = source.value(
			"showCameraPathPointCameraDirection",
			settings.showCameraPathPointCameraDirection
		);
		settings.showSkeleton = source.value(
			"showSkeleton",
			settings.showSkeleton
		);
		settings.showJointNames = source.value(
			"showJointNames",
			settings.showJointNames
		);
		settings.showJointAxes = source.value(
			"showJointAxes",
			settings.showJointAxes
		);
		settings.jointRadius = (std::max)(
			source.value("jointRadius", settings.jointRadius),
			0.001f
		);
		settings.jointAxisLength = (std::max)(
			source.value("jointAxisLength", settings.jointAxisLength),
			0.001f
		);
		return settings;
	}

	Quaternion JsonToQuaternion(
		const json& value,
		const Quaternion& fallback
	) {
		if (!value.is_array() || value.size() != 4) {
			return fallback;
		}
		return Normalize({
			value[0].get<float>(), value[1].get<float>(),
			value[2].get<float>(), value[3].get<float>()
		});
	}

	json LightingSettingsToJson(const SceneLightingSettings& settings) {
		return {
			{ "shadowMapSize", settings.shadowMapSize }
		};
	}

	SceneLightingSettings LightingSettingsFromJson(
		const json& source,
		const SceneLightingSettings& fallback
	) {
		if (!source.is_object()) {
			return fallback;
		}

		SceneLightingSettings settings = fallback;
		const uint32_t requested = source.value(
			"shadowMapSize",
			settings.shadowMapSize
		);
		settings.shadowMapSize = requested <= 1024
			? 1024
			: requested <= 2048 ? 2048 : 4096;
		return settings;
	}

	Vector3 NormalizeDirectionVector(
		const Vector3& value,
		const Vector3& fallback
	) {
		const float length = std::sqrt(
			value.x * value.x +
			value.y * value.y +
			value.z * value.z
		);
		if (length <= 0.000001f) {
			return fallback;
		}
		return {
			value.x / length,
			value.y / length,
			value.z / length
		};
	}

	void NormalizeTeamSettings(SceneTeamSettings& team) {
		if (team.name.empty()) {
			team.name = "Team";
		}
		auto normalizeForwardAxis = [](std::string& axis) {
			if (
				axis != "+Z" &&
				axis != "-Z" &&
				axis != "+X" &&
				axis != "-X" &&
				axis != "+Y" &&
				axis != "-Y"
			) {
				axis = "+Z";
			}
		};
		normalizeForwardAxis(team.agentForwardAxis);
		team.agentMinSpeed = (std::max)(team.agentMinSpeed, 0.0f);
		team.agentMaxSpeed = (std::max)(team.agentMaxSpeed, team.agentMinSpeed);
		team.agentTurnSpeed = (std::max)(team.agentTurnSpeed, 0.0f);
		team.agentWanderStrength =
			(std::max)(team.agentWanderStrength, 0.0f);
		team.agentWanderChangeInterval =
			(std::max)(team.agentWanderChangeInterval, 0.0f);
		team.agentWanderDirectionRange = std::clamp(
			team.agentWanderDirectionRange,
			0.0f,
			3.14159265359f
		);
		team.agentWanderVerticalRange = std::clamp(
			team.agentWanderVerticalRange,
			0.0f,
			1.0f
		);
		team.agentRandomSeed = (std::max)(team.agentRandomSeed, 0);
		team.agentFlockDecisionInterval =
			(std::max)(team.agentFlockDecisionInterval, 0.0f);
		team.agentFlockAcceleration =
			(std::max)(team.agentFlockAcceleration, 0.0f);
		team.agentFlockTurnRate =
			(std::max)(team.agentFlockTurnRate, 0.0f);
		team.agentMemberCenterFollow =
			(std::max)(team.agentMemberCenterFollow, 0.0f);
		team.agentMemberJitterStrength =
			(std::max)(team.agentMemberJitterStrength, 0.0f);
		team.agentMemberJitterFrequency =
			(std::max)(team.agentMemberJitterFrequency, 0.0f);
		team.agentMemberJitterUpdateInterval =
			(std::max)(team.agentMemberJitterUpdateInterval, 0.0f);
		team.agentMemberJitterFollowSpeed =
			(std::max)(team.agentMemberJitterFollowSpeed, 0.0f);
		team.agentMemberSpeedVariation = std::clamp(
			team.agentMemberSpeedVariation,
			0.0f,
			1.0f
		);
		team.agentMemberLeashDistance =
			(std::max)(team.agentMemberLeashDistance, 0.0f);
		team.agentMemberLeashStrength =
			(std::max)(team.agentMemberLeashStrength, 0.0f);
		team.agentMemberCatchupSpeed =
			(std::max)(team.agentMemberCatchupSpeed, 0.0f);
		team.agentMemberSeparationUpdateInterval =
			(std::max)(team.agentMemberSeparationUpdateInterval, 0.0f);
		team.agentMemberSeparationBlend = std::clamp(
			team.agentMemberSeparationBlend,
			0.0f,
			1.0f
		);
		if (!std::isfinite(team.agentMemberMinimumDistance) ||
			team.agentMemberMinimumDistance < 0.0f) {
			team.agentMemberMinimumDistance = 0.0f;
		}
		if (!std::isfinite(team.agentFormationCapsuleRadius) ||
			team.agentFormationCapsuleRadius < 0.0f) {
			team.agentFormationCapsuleRadius = 0.0f;
		}
		if (!std::isfinite(team.agentFormationCapsuleHalfSegmentLength) ||
			team.agentFormationCapsuleHalfSegmentLength < 0.0f) {
			team.agentFormationCapsuleHalfSegmentLength = 0.0f;
		}
		team.agentTeamHeadingDirection = NormalizeDirectionVector(
			team.agentTeamHeadingDirection,
			{ 0.0f, 0.0f, 1.0f }
		);
		team.agentTeamHeadingWeight =
			(std::max)(team.agentTeamHeadingWeight, 0.0f);
		team.agentTeamHeadingFollowSpeed =
			(std::max)(team.agentTeamHeadingFollowSpeed, 0.0f);
		team.agentTeamRotationWeight =
			std::clamp(team.agentTeamRotationWeight, 0.0f, 1.0f);
		team.agentTeamRotationFollowSpeed =
			(std::max)(team.agentTeamRotationFollowSpeed, 0.0f);
		team.agentRotationFollowSpeed =
			(std::max)(team.agentRotationFollowSpeed, 0.0f);
		team.agentPitchFromVerticalVelocity =
			(std::max)(team.agentPitchFromVerticalVelocity, 0.0f);
		team.agentBankingStrength =
			(std::max)(team.agentBankingStrength, 0.0f);
		team.agentSchoolingUpdateInterval =
			(std::max)(team.agentSchoolingUpdateInterval, 0.0f);
		team.agentSchoolingUpdateJitter =
			(std::max)(team.agentSchoolingUpdateJitter, 0.0f);
		team.agentNeighborLimit = (std::max)(team.agentNeighborLimit, 0);
		team.agentSchoolingBlend =
			std::clamp(team.agentSchoolingBlend, 0.0f, 1.0f);
		team.agentSeparationRadius =
			(std::max)(team.agentSeparationRadius, 0.0f);
		team.agentAlignmentRadius =
			(std::max)(team.agentAlignmentRadius, 0.0f);
		team.agentCohesionRadius =
			(std::max)(team.agentCohesionRadius, 0.0f);
		team.agentSeparationWeight =
			(std::max)(team.agentSeparationWeight, 0.0f);
		team.agentAlignmentWeight =
			(std::max)(team.agentAlignmentWeight, 0.0f);
		team.agentCohesionWeight =
			(std::max)(team.agentCohesionWeight, 0.0f);
	}

	json TeamToJson(const SceneTeamSettings& team) {
		return {
			{ "name", team.name },
			{ "agentBehaviorOverride", team.agentBehaviorOverride },
			{ "agentGroupName", team.agentGroupName },
			{ "agentMinSpeed", team.agentMinSpeed },
			{ "agentMaxSpeed", team.agentMaxSpeed },
			{ "agentTurnSpeed", team.agentTurnSpeed },
			{ "agentWanderStrength", team.agentWanderStrength },
			{ "agentWanderChangeInterval", team.agentWanderChangeInterval },
			{ "agentWanderDirectionRange", team.agentWanderDirectionRange },
			{ "agentWanderVerticalRange", team.agentWanderVerticalRange },
			{ "agentRandomizeSeedOnPlay", team.agentRandomizeSeedOnPlay },
			{ "agentRandomSeed", team.agentRandomSeed },
			{ "agentUseLeaderStartPosition", team.agentUseLeaderStartPosition },
			{ "agentLeaderStartPosition",
				VectorToJson(team.agentLeaderStartPosition) },
			{ "agentFlockDecisionInterval", team.agentFlockDecisionInterval },
			{ "agentFlockAcceleration", team.agentFlockAcceleration },
			{ "agentFlockTurnRate", team.agentFlockTurnRate },
			{ "agentMemberCenterFollow", team.agentMemberCenterFollow },
			{ "agentMemberJitterStrength", team.agentMemberJitterStrength },
			{ "agentMemberJitterFrequency", team.agentMemberJitterFrequency },
			{ "agentMemberJitterUpdateInterval",
				team.agentMemberJitterUpdateInterval },
			{ "agentMemberJitterFollowSpeed",
				team.agentMemberJitterFollowSpeed },
			{ "agentMemberSpeedVariation", team.agentMemberSpeedVariation },
			{ "agentMemberLeashDistance", team.agentMemberLeashDistance },
			{ "agentMemberLeashStrength", team.agentMemberLeashStrength },
			{ "agentMemberCatchupSpeed", team.agentMemberCatchupSpeed },
			{ "agentMemberSeparationUpdateInterval",
				team.agentMemberSeparationUpdateInterval },
			{ "agentMemberSeparationBlend", team.agentMemberSeparationBlend },
		{ "agentMemberMinimumDistance", team.agentMemberMinimumDistance },
		{ "agentFormationCapsuleEnabled", team.agentFormationCapsuleEnabled },
		{ "agentFormationCapsuleScaleWithActiveMembers",
			team.agentFormationCapsuleScaleWithActiveMembers },
		{ "agentFormationCapsuleRadius", team.agentFormationCapsuleRadius },
			{ "agentFormationCapsuleHalfSegmentLength",
				team.agentFormationCapsuleHalfSegmentLength },
			{ "agentUseTeamHeading", team.agentUseTeamHeading },
			{ "agentTeamHeadingFromAverage",
				team.agentTeamHeadingFromAverage },
			{ "agentTeamHeadingDirection",
				VectorToJson(team.agentTeamHeadingDirection) },
			{ "agentTeamHeadingWeight", team.agentTeamHeadingWeight },
			{ "agentTeamHeadingFollowSpeed",
				team.agentTeamHeadingFollowSpeed },
			{ "agentUseTeamRotation", team.agentUseTeamRotation },
			{ "agentTeamRotationWeight", team.agentTeamRotationWeight },
			{ "agentTeamRotationFollowSpeed",
				team.agentTeamRotationFollowSpeed },
			{ "agentAlignForwardToVelocity",
				team.agentAlignForwardToVelocity },
			{ "agentForwardAxis", team.agentForwardAxis },
			{ "agentRotateAxisX", team.agentRotateAxisX },
			{ "agentRotateAxisY", team.agentRotateAxisY },
			{ "agentRotateAxisZ", team.agentRotateAxisZ },
			{ "agentRotationFollowSpeed", team.agentRotationFollowSpeed },
			{ "agentPitchFromVerticalVelocity",
				team.agentPitchFromVerticalVelocity },
			{ "agentBankingStrength", team.agentBankingStrength },
			{ "agentSchooling", team.agentSchooling },
			{ "agentSchoolingUpdateInterval",
				team.agentSchoolingUpdateInterval },
			{ "agentSchoolingUpdateJitter",
				team.agentSchoolingUpdateJitter },
			{ "agentNeighborLimit", team.agentNeighborLimit },
			{ "agentSchoolingBlend", team.agentSchoolingBlend },
			{ "agentSeparationRadius", team.agentSeparationRadius },
			{ "agentAlignmentRadius", team.agentAlignmentRadius },
			{ "agentCohesionRadius", team.agentCohesionRadius },
			{ "agentSeparationWeight", team.agentSeparationWeight },
			{ "agentAlignmentWeight", team.agentAlignmentWeight },
			{ "agentCohesionWeight", team.agentCohesionWeight },
			{ "agentVisualColor", VectorToJson(team.agentVisualColor) },
			{ "agentEnableLighting", team.agentEnableLighting }
		};
	}

	SceneTeamSettings TeamFromJson(const json& source) {
		SceneTeamSettings team{};
		if (!source.is_object()) {
			return team;
		}
		team.name = source.value("name", team.name);
		team.agentBehaviorOverride = source.value(
			"agentBehaviorOverride",
			team.agentBehaviorOverride
		);
		team.agentGroupName = source.value("agentGroupName", team.agentGroupName);
		team.agentMinSpeed = source.value("agentMinSpeed", team.agentMinSpeed);
		team.agentMaxSpeed = source.value("agentMaxSpeed", team.agentMaxSpeed);
		team.agentTurnSpeed = source.value("agentTurnSpeed", team.agentTurnSpeed);
		team.agentWanderStrength = source.value(
			"agentWanderStrength",
			team.agentWanderStrength
		);
		team.agentWanderChangeInterval = source.value(
			"agentWanderChangeInterval",
			team.agentWanderChangeInterval
		);
		team.agentWanderDirectionRange = source.value(
			"agentWanderDirectionRange",
			team.agentWanderDirectionRange
		);
		team.agentWanderVerticalRange = source.value(
			"agentWanderVerticalRange",
			team.agentWanderVerticalRange
		);
		team.agentRandomizeSeedOnPlay = source.value(
			"agentRandomizeSeedOnPlay",
			team.agentRandomizeSeedOnPlay
		);
		team.agentRandomSeed = source.value(
			"agentRandomSeed",
			team.agentRandomSeed
		);
		team.agentUseLeaderStartPosition = source.value(
			"agentUseLeaderStartPosition",
			team.agentUseLeaderStartPosition
		);
		if (source.contains("agentLeaderStartPosition")) {
			team.agentLeaderStartPosition = JsonToVector(
				source.at("agentLeaderStartPosition"),
				team.agentLeaderStartPosition
			);
		}
		team.agentFlockDecisionInterval = source.value(
			"agentFlockDecisionInterval",
			team.agentFlockDecisionInterval
		);
		team.agentFlockAcceleration = source.value(
			"agentFlockAcceleration",
			team.agentFlockAcceleration
		);
		team.agentFlockTurnRate = source.value(
			"agentFlockTurnRate",
			team.agentFlockTurnRate
		);
		team.agentMemberCenterFollow = source.value(
			"agentMemberCenterFollow",
			team.agentMemberCenterFollow
		);
		team.agentMemberJitterStrength = source.value(
			"agentMemberJitterStrength",
			team.agentMemberJitterStrength
		);
		team.agentMemberJitterFrequency = source.value(
			"agentMemberJitterFrequency",
			team.agentMemberJitterFrequency
		);
		team.agentMemberJitterUpdateInterval = source.value(
			"agentMemberJitterUpdateInterval",
			team.agentMemberJitterUpdateInterval
		);
		team.agentMemberJitterFollowSpeed = source.value(
			"agentMemberJitterFollowSpeed",
			team.agentMemberJitterFollowSpeed
		);
		team.agentMemberSpeedVariation = source.value(
			"agentMemberSpeedVariation",
			team.agentMemberSpeedVariation
		);
		team.agentMemberLeashDistance = source.value(
			"agentMemberLeashDistance",
			team.agentMemberLeashDistance
		);
		team.agentMemberLeashStrength = source.value(
			"agentMemberLeashStrength",
			team.agentMemberLeashStrength
		);
		team.agentMemberCatchupSpeed = source.value(
			"agentMemberCatchupSpeed",
			team.agentMemberCatchupSpeed
		);
		team.agentMemberSeparationUpdateInterval = source.value(
			"agentMemberSeparationUpdateInterval",
			team.agentMemberSeparationUpdateInterval
		);
		team.agentMemberSeparationBlend = source.value(
			"agentMemberSeparationBlend",
			team.agentMemberSeparationBlend
		);
		team.agentMemberMinimumDistance = source.value(
			"agentMemberMinimumDistance",
			team.agentMemberMinimumDistance
		);
		team.agentFormationCapsuleEnabled = source.value(
			"agentFormationCapsuleEnabled",
			team.agentFormationCapsuleEnabled
		);
		team.agentFormationCapsuleScaleWithActiveMembers = source.value(
			"agentFormationCapsuleScaleWithActiveMembers",
			team.agentFormationCapsuleScaleWithActiveMembers
		);
		team.agentFormationCapsuleRadius = source.value(
			"agentFormationCapsuleRadius",
			team.agentFormationCapsuleRadius
		);
		team.agentFormationCapsuleHalfSegmentLength = source.value(
			"agentFormationCapsuleHalfSegmentLength",
			team.agentFormationCapsuleHalfSegmentLength
		);
		team.agentUseTeamHeading = source.value(
			"agentUseTeamHeading",
			team.agentUseTeamHeading
		);
		team.agentTeamHeadingFromAverage = source.value(
			"agentTeamHeadingFromAverage",
			team.agentTeamHeadingFromAverage
		);
		if (source.contains("agentTeamHeadingDirection")) {
			team.agentTeamHeadingDirection = JsonToVector(
				source.at("agentTeamHeadingDirection"),
				team.agentTeamHeadingDirection
			);
		}
		team.agentTeamHeadingWeight = source.value(
			"agentTeamHeadingWeight",
			team.agentTeamHeadingWeight
		);
		team.agentTeamHeadingFollowSpeed = source.value(
			"agentTeamHeadingFollowSpeed",
			team.agentTeamHeadingFollowSpeed
		);
		team.agentUseTeamRotation = source.value(
			"agentUseTeamRotation",
			team.agentUseTeamRotation
		);
		team.agentTeamRotationWeight = source.value(
			"agentTeamRotationWeight",
			team.agentTeamRotationWeight
		);
		team.agentTeamRotationFollowSpeed = source.value(
			"agentTeamRotationFollowSpeed",
			team.agentTeamRotationFollowSpeed
		);
		team.agentAlignForwardToVelocity = source.value(
			"agentAlignForwardToVelocity",
			team.agentAlignForwardToVelocity
		);
		team.agentForwardAxis = source.value(
			"agentForwardAxis",
			team.agentForwardAxis
		);
		team.agentRotateAxisX = source.value(
			"agentRotateAxisX",
			team.agentRotateAxisX
		);
		team.agentRotateAxisY = source.value(
			"agentRotateAxisY",
			team.agentRotateAxisY
		);
		team.agentRotateAxisZ = source.value(
			"agentRotateAxisZ",
			team.agentRotateAxisZ
		);
		team.agentRotationFollowSpeed = source.value(
			"agentRotationFollowSpeed",
			team.agentRotationFollowSpeed
		);
		team.agentPitchFromVerticalVelocity = source.value(
			"agentPitchFromVerticalVelocity",
			team.agentPitchFromVerticalVelocity
		);
		team.agentBankingStrength = source.value(
			"agentBankingStrength",
			team.agentBankingStrength
		);
		team.agentSchooling = source.value("agentSchooling", team.agentSchooling);
		team.agentSchoolingUpdateInterval = source.value(
			"agentSchoolingUpdateInterval",
			team.agentSchoolingUpdateInterval
		);
		team.agentSchoolingUpdateJitter = source.value(
			"agentSchoolingUpdateJitter",
			team.agentSchoolingUpdateJitter
		);
		team.agentNeighborLimit = source.value(
			"agentNeighborLimit",
			team.agentNeighborLimit
		);
		team.agentSchoolingBlend = source.value(
			"agentSchoolingBlend",
			team.agentSchoolingBlend
		);
		team.agentSeparationRadius = source.value(
			"agentSeparationRadius",
			team.agentSeparationRadius
		);
		team.agentAlignmentRadius = source.value(
			"agentAlignmentRadius",
			team.agentAlignmentRadius
		);
		team.agentCohesionRadius = source.value(
			"agentCohesionRadius",
			team.agentCohesionRadius
		);
		team.agentSeparationWeight = source.value(
			"agentSeparationWeight",
			team.agentSeparationWeight
		);
		team.agentAlignmentWeight = source.value(
			"agentAlignmentWeight",
			team.agentAlignmentWeight
		);
		team.agentCohesionWeight = source.value(
			"agentCohesionWeight",
			team.agentCohesionWeight
		);
		if (source.contains("agentVisualColor")) {
			team.agentVisualColor = JsonToVector(
				source.at("agentVisualColor"),
				team.agentVisualColor
			);
		}
		team.agentEnableLighting = source.value(
			"agentEnableLighting",
			team.agentEnableLighting
		);
		NormalizeTeamSettings(team);
		return team;
	}

	std::string MakeUniqueTeamName(
		const std::vector<SceneTeamSettings>& teams,
		const std::string& requestedName,
		const std::string& ignoredName = {}
	) {
		const std::string baseName = requestedName.empty()
			? std::string("Team")
			: requestedName;
		std::string candidate = baseName;
		uint32_t suffix = 2;
		auto exists = [&](const std::string& name) {
			if (!ignoredName.empty() && name == ignoredName) {
				return false;
			}
			return std::any_of(
				teams.begin(),
				teams.end(),
				[&](const SceneTeamSettings& team) {
					return team.name == name;
				}
			);
		};
		while (exists(candidate)) {
			candidate = baseName + " " + std::to_string(suffix++);
		}
		return candidate;
	}

	json StatsToJson(const std::vector<SceneStatDefinition>& stats) {
		json result = json::array();
		for (const SceneStatDefinition& stat : stats) {
			result.push_back({
				{ "id", stat.id },
				{ "displayName", stat.displayName },
				{ "min", stat.minValue },
				{ "max", stat.maxValue },
				{ "initial", stat.initialValue }
			});
		}
		return result;
	}

	json EventActionsToJson(const std::vector<SceneEventAction>& actions) {
		json result = json::array();
		for (const SceneEventAction& action : actions) {
			result.push_back({
				{ "type", action.type },
				{ "targetEntityId", action.targetEntityId },
				{ "targetEntityName", action.targetEntityName },
				{ "statId", action.statId },
				{ "statOperation", action.statOperation },
				{ "value", action.value },
				{ "active", action.active },
				{ "sceneId", action.sceneId },
				{ "prefabPath", action.prefabPath },
				{ "prefabParentToTarget", action.prefabParentToTarget },
				{ "prefabUseTargetTransform", action.prefabUseTargetTransform },
				{ "stateName", action.stateName },
				{ "postProcessManagerEntityId", action.postProcessManagerEntityId },
				{ "postProcessManagerEntityName", action.postProcessManagerEntityName },
				{ "postProcessProfileId", action.postProcessProfileId },
				{ "textMotionClipId", action.textMotionClipId }
			});
		}
		return result;
	}

	json EventsToJson(const std::vector<SceneEventBinding>& bindings) {
		json result = json::array();
		for (const SceneEventBinding& binding : bindings) {
			json bindingValue = {
				{ "triggerType", binding.triggerType },
				{ "triggerKey", binding.inputExpression
					? FirstSceneInputExpressionTerm(binding.inputExpression)
					: binding.triggerKey },
				{ "targetEntityId", binding.targetEntityId },
				{ "targetEntityName", binding.targetEntityName },
				{ "statId", binding.statId },
				{ "statComparison", binding.statComparison },
				{ "statValue", binding.statValue },
				{ "targetPosition", VectorToJson(binding.targetPosition) },
				{ "radius", binding.radius },
				{ "triggerOnce", binding.triggerOnce },
				{ "cooldown", binding.cooldown },
				{ "textMotionClipId", binding.textMotionClipId },
				{ "actions", EventActionsToJson(binding.actions) }
			};
			if (binding.inputExpression) {
				bindingValue["inputExpression"] =
					SceneInputExpressionToJson(*binding.inputExpression);
			}
			result.push_back(std::move(bindingValue));
		}
		return result;
	}

	json PrefabAnimationsToJson(
		const std::vector<ScenePrefabAnimationClip>& clips
	) {
		json result = json::array();
		for (const ScenePrefabAnimationClip& clip : clips) {
			json tracks = json::array();
			for (const SceneAnimationTrack& track : clip.tracks) {
				json keyframes = json::array();
				for (const SceneAnimationKeyframe& keyframe : track.keyframes) {
					json keyframeValue = {
						{ "time", keyframe.time },
						{ "value", VectorToJson(keyframe.value) }
					};
					if (!keyframe.easingToNext.empty()) {
						keyframeValue["easingToNext"] = keyframe.easingToNext;
					}
					if (
						keyframe.positionBulge.x != 0.0f ||
						keyframe.positionBulge.y != 0.0f ||
						keyframe.positionBulge.z != 0.0f
					) {
						keyframeValue["positionBulge"] = VectorToJson(
							keyframe.positionBulge
						);
					}
					keyframes.push_back(std::move(keyframeValue));
				}
				tracks.push_back({
					{ "targetEntityId", track.targetEntityId },
					{ "targetEntityName", track.targetEntityName },
					{ "property", track.property },
					{ "easing", track.easing },
					{ "keyframes", std::move(keyframes) }
				});
			}
			result.push_back({
				{ "name", clip.name },
				{ "duration", clip.duration },
				{ "loop", clip.loop },
				{ "playOnStart", clip.playOnStart },
				{ "tracks", std::move(tracks) }
			});
		}
		return result;
	}

	json StateMachineStatesToJson(
		const std::vector<SceneStateDefinition>& states
	) {
		json result = json::array();
		for (const SceneStateDefinition& state : states) {
			json parameters = json::array();
			for (const SceneStateParameter& parameter : state.parameters) {
				parameters.push_back({
					{ "name", parameter.name },
					{ "type", parameter.type },
					{ "floatValue", parameter.floatValue },
					{ "intValue", parameter.intValue },
					{ "boolValue", parameter.boolValue },
					{ "stringValue", parameter.stringValue },
					{ "entityId", parameter.entityId },
					{ "entityName", parameter.entityName }
				});
			}
			result.push_back({
				{ "name", state.name },
				{ "actionId", state.actionId },
				{ "parameters", std::move(parameters) }
			});
		}
		return result;
	}

	json PostProcessProfilesToJson(
		const std::vector<ScenePostProcessProfile>& profiles
	) {
		json result = json::array();
		for (const ScenePostProcessProfile& profile : profiles) {
			json automations = json::array();
			for (const ScenePostProcessAutomation& automation :
				profile.automations) {
				automations.push_back({
					{ "parameter", "DissolveThreshold" },
					{ "startValue", automation.startValue },
					{ "endValue", automation.endValue },
					{ "duration", automation.duration },
					{ "playback", "OneShot" },
					{ "easing", "Linear" }
				});
			}
			result.push_back({
				{ "id", profile.id },
				{ "label", profile.label },
				{ "settings", PostProcessToJson(profile.settings) },
				{ "automations", std::move(automations) }
			});
		}
		return result;
	}

	json AttackDefinitionsToJson(const std::vector<SceneAttackDefinition>& attacks) {
		json result = json::array();
		for (const SceneAttackDefinition& attack : attacks) {
			json windows = json::array();
			for (const SceneAttackHitWindow& window : attack.hitWindows) {
				json serializedWindow = {
					{ "startTime", window.startTime }, { "endTime", window.endTime },
					{ "hitBoxEntityId", window.hitBoxEntityId },
					{ "hitBoxEntityName", window.hitBoxEntityName }
				};
				if (window.payloadSource == "HitBox") {
					serializedWindow["payloadSource"] = "HitBox";
				} else {
					serializedWindow = {
						{ "startTime", window.startTime }, { "endTime", window.endTime },
						{ "hitBoxEntityId", window.hitBoxEntityId },
						{ "hitBoxEntityName", window.hitBoxEntityName },
						{ "payloadSource", "WindowLegacy" },
					{ "damage", window.damage }, { "poiseDamage", window.poiseDamage },
					{ "knockback", window.knockback },
					{ "verticalKnockback", window.verticalKnockback },
					{ "hitStopDuration", window.hitStopDuration },
					{ "reactionTag", window.reactionTag },
					{ "knockbackDirectionMode", window.knockbackDirectionMode },
					{ "knockbackLocalDirection", VectorToJson(window.knockbackLocalDirection) },
					{ "hitPolicy", window.hitPolicy },
					{ "targetCooldown", window.targetCooldown }
					};
					if (window.overrideHitBoxHalfSize) {
						serializedWindow["overrideHitBoxHalfSize"] = true;
						serializedWindow["hitBoxHalfSize"] = VectorToJson(window.hitBoxHalfSize);
					}
				}
				windows.push_back(std::move(serializedWindow));
			}
			json effects = json::array();
			for (const SceneAttackEffectEvent& effect : attack.effectEvents) {
				effects.push_back({
					{ "time", effect.time },
					{ "particleEffectPath", effect.particleEffectPath },
					{ "spawnEntityId", effect.spawnEntityId },
					{ "spawnEntityName", effect.spawnEntityName },
					{ "localOffset", VectorToJson(effect.localOffset) },
					{ "groundPrefabPath", effect.groundPrefabPath },
					{ "groundProbeDistance", effect.groundProbeDistance },
					{ "groundPrefabLifetime", effect.groundPrefabLifetime },
					{ "groundEffectType", effect.groundEffectType },
					{ "groundCrackRadius", effect.groundCrackRadius },
					{ "groundCrackPrimaryBranchCount", effect.groundCrackPrimaryBranchCount },
					{ "groundCrackSegmentsPerBranch", effect.groundCrackSegmentsPerBranch },
					{ "groundCrackBranchProbability", effect.groundCrackBranchProbability },
					{ "groundCrackWidth", effect.groundCrackWidth },
					{ "groundCrackLifetime", effect.groundCrackLifetime },
					{ "groundCrackSurfaceOffset", effect.groundCrackSurfaceOffset }
				});
			}
			result.push_back({
				{ "name", attack.name }, { "animation", attack.animation },
				{ "animationTargetEntityId", attack.animationTargetEntityId },
				{ "animationTargetEntityName", attack.animationTargetEntityName },
				{ "hitBoxEntityId", attack.hitBoxEntityId }, { "hitBoxEntityName", attack.hitBoxEntityName },
				{ "windup", attack.windup }, { "activeTime", attack.activeTime }, { "recovery", attack.recovery },
				{ "forwardDistance", attack.forwardDistance }, { "sideDistance", attack.sideDistance },
				{ "motionEasing", attack.motionEasing }, { "facingMode", attack.facingMode },
				{ "facingTargetEntityId", attack.facingTargetEntityId },
				{ "facingTargetEntityName", attack.facingTargetEntityName },
				{ "facingRotateAngle", attack.facingRotateAngle },
				{ "loopEnabled", attack.loopEnabled }, { "loopMaxCount", attack.loopMaxCount },
				{ "loopSafetyTimeout", attack.loopSafetyTimeout },
				{ "hitWindows", std::move(windows) },
				{ "effectEvents", std::move(effects) }
			});
		}
		return result;
	}

	uint64_t NextComponentLocalId(const SceneEntity& entity) {
		std::unordered_set<uint64_t> usedIds;
		for (const SceneComponent& component : entity.components) {
			if (component.localId != 0) {
				usedIds.insert(component.localId);
			}
		}
		uint64_t nextId = 1;
		while (usedIds.contains(nextId)) {
			++nextId;
		}
		return nextId;
	}

	bool EnsureComponentLocalIds(SceneEntity& entity) {
		std::unordered_set<uint64_t> assignedIds;
		uint64_t nextId = 1;
		bool changed = false;
		for (SceneComponent& component : entity.components) {
			if (
				component.localId != 0 &&
				assignedIds.insert(component.localId).second
			) {
				continue;
			}
			while (assignedIds.contains(nextId)) {
				++nextId;
			}
			component.localId = nextId;
			assignedIds.insert(nextId);
			++nextId;
			changed = true;
		}
		return changed;
	}

	json ComponentToJson(const SceneComponent& component) {
		json result = {
			{ "localId", component.localId },
			{ "type", component.type },
			{ "enabled", component.enabled }
		};
		if (component.type == "MeshRenderer") {
			result["modelPath"] = component.modelPath;
			result["cullMode"] = component.meshCullMode;
			result["visualRotation"] = VectorToJson(component.meshVisualRotation);
			result["environmentReflectionOverride"] =
				component.meshEnvironmentReflectionOverride;
			result["environmentReflectionIntensity"] =
				component.meshEnvironmentReflectionIntensity;
			json materialOverrides = json::array();
			for (const SceneMeshMaterialOverride& override :
				component.meshMaterialOverrides) {
				materialOverrides.push_back({
					{ "materialName", override.materialName },
					{ "enabled", override.enabled },
					{ "colorOverrideEnabled", override.colorOverrideEnabled },
					{ "color", VectorToJson(override.color) },
					{ "texturePath", override.texturePath }
				});
			}
			result["materialOverrides"] = std::move(materialOverrides);
		} else if (component.type == "Environment") {
			result["skyboxEnabled"] = component.environmentSkyboxEnabled;
			result["skyboxPath"] = component.environmentSkyboxPath;
			result["skyboxIntensity"] = component.environmentSkyboxIntensity;
			result["reflectionIntensity"] =
				component.environmentReflectionIntensity;
		} else if (component.type == "SpriteRenderer") {
			result["texturePath"] = component.texturePath;
			result["size"] = VectorToJson(component.spriteSize);
			result["anchor"] = VectorToJson(component.spriteAnchor);
			result["renderSpace"] = component.spriteRenderSpace;
			result["viewportAnchor"] = VectorToJson(component.spriteViewportAnchor);
			result["color"] = VectorToJson(component.spriteColor);
			result["flipX"] = component.spriteFlipX;
			result["flipY"] = component.spriteFlipY;
		} else if (component.type == "TextRenderer") {
			result["text"] = component.textValue;
			result["renderSpace"] = component.textRenderSpace;
			result["fontSource"] = component.textFontSource;
			result["fontResourcePath"] = component.textFontResourcePath;
			result["fontFamily"] = component.textFontFamily;
			result["fontSize"] = component.textFontSize;
			result["fontWeight"] = component.textFontWeight;
			result["fontStyle"] = component.textFontStyle;
			result["color"] = VectorToJson(component.textColor);
			result["opacity"] = component.textOpacity;
			result["horizontalAlignment"] = component.textHorizontalAlignment;
			result["verticalAlignment"] = component.textVerticalAlignment;
			result["wrapMode"] = component.textWrapMode;
			result["overflowMode"] = component.textOverflowMode;
			result["layoutSize"] = VectorToJson(component.textLayoutSize);
			result["characterSpacing"] = component.textCharacterSpacing;
			result["lineSpacing"] = component.textLineSpacing;
			result["outlineEnabled"] = component.textOutlineEnabled;
			result["outlineColor"] = VectorToJson(component.textOutlineColor);
			result["outlineWidth"] = component.textOutlineWidth;
			result["shadowEnabled"] = component.textShadowEnabled;
			result["shadowColor"] = VectorToJson(component.textShadowColor);
			result["shadowOffset"] = VectorToJson(component.textShadowOffset);
			result["viewportAnchor"] = VectorToJson(component.textViewportAnchor);
			result["pivot"] = VectorToJson(component.textPivot);
			result["sortingOrder"] = component.textSortingOrder;
			result["clipEnabled"] = component.textClipEnabled;
			if (component.textHasPlacementProfiles) {
				auto placementToJson = [](const Text2DPlacement& placement) {
					nlohmann::json value{};
					value["position"] = VectorToJson(placement.position);
					value["rotation"] = placement.rotation;
					value["scale"] = VectorToJson(placement.scale);
					value["pivot"] = VectorToJson(placement.pivot);
					value["viewportAnchor"] = VectorToJson(placement.viewportAnchor);
					value["sortingOrder"] = placement.sortingOrder;
					value["clipEnabled"] = placement.clipEnabled;
					return value;
				};
				result["overlayPlacement"] = placementToJson(component.textOverlayPlacement);
				result["scene2DPlacement"] = placementToJson(component.textScene2DPlacement);
			}
		} else if (component.type == "TextMotion") {
			json clips = json::array();
			for (const SceneTextMotionClip& clip : component.textMotionClips) {
				json keyframes = json::array();
				for (const SceneTextMotionKeyframe& keyframe : clip.keyframes) {
					keyframes.push_back({
						{ "timeSeconds", keyframe.timeSeconds },
						{ "positionOffset", VectorToJson(keyframe.positionOffset) },
						{ "rotationOffset", keyframe.rotationOffset },
						{ "scaleMultiplier", VectorToJson(keyframe.scaleMultiplier) },
						{ "opacityMultiplier", keyframe.opacityMultiplier },
						{ "easingToNext", keyframe.easingToNext }
					});
				}
				clips.push_back({
					{ "id", clip.id },
					{ "holdFinalPose", clip.holdFinalPose },
					{ "keyframes", std::move(keyframes) }
				});
			}
			result["clips"] = std::move(clips);
		} else if (component.type == "GameFlowDirector") {
			result["autoStart"] = component.gameFlowAutoStart;
			result["countdownStart"] = component.gameFlowCountdownStart;
			result["countdownStepSeconds"] = component.gameFlowCountdownStepSeconds;
			result["startCueText"] = component.gameFlowStartCueText;
			result["startCueSeconds"] = component.gameFlowStartCueSeconds;
			result["interPhaseDelaySeconds"] = component.gameFlowInterPhaseDelaySeconds;
			result["resultRevealDelaySeconds"] = component.gameFlowResultRevealDelaySeconds;
			result["timerDisplayStepSeconds"] = component.gameFlowTimerDisplayStepSeconds;
			result["timerPrefix"] = component.gameFlowTimerPrefix;
			result["resultPrefix"] = component.gameFlowResultPrefix;
			result["countdownTextEntityId"] = component.gameFlowCountdownTextEntityId;
			result["countdownMotionClipId"] = component.gameFlowCountdownMotionClipId;
			result["phaseTextEntityId"] = component.gameFlowPhaseTextEntityId;
			result["phaseMotionClipId"] = component.gameFlowPhaseMotionClipId;
			result["timerTextEntityId"] = component.gameFlowTimerTextEntityId;
			result["remainingTextEntityId"] = component.gameFlowRemainingTextEntityId;
			result["remainingPrefix"] = component.gameFlowRemainingPrefix;
			result["resultRootEntityId"] = component.gameFlowResultRootEntityId;
			result["resultTimeTextEntityId"] = component.gameFlowResultTimeTextEntityId;
			result["resultMotionClipId"] = component.gameFlowResultMotionClipId;
			json phases = json::array();
			for (const SceneGameFlowPhase& phase : component.gameFlowPhases) {
				json waves = json::array();
				for (const SceneGameFlowWave& wave : phase.waves) {
					waves.push_back({ { "spawnerEntityId", wave.spawnerEntityId }, { "count", wave.count } });
				}
				phases.push_back({ { "id", phase.id }, { "label", phase.label }, { "waves", std::move(waves) } });
			}
			result["phases"] = std::move(phases);
		} else if (component.type == "FishingScoreAttackDirector") {
			result["playerEntityId"] = component.fishingPlayerEntityId;
			result["fishEntityIds"] = component.fishingFishEntityIds;
			result["hookSpawnAreaEntityId"] = component.fishingHookSpawnAreaEntityId;
			result["hookPoolEntityId"] = component.fishingHookPoolEntityId;
			result["waterVolumeEntityId"] = component.fishingWaterVolumeEntityId;
			result["durationSeconds"] = component.fishingDurationSeconds;
			result["maxSelectableFishCount"] = component.fishingMaxSelectableFishCount;
			result["confirmInput"] = component.fishingConfirmInputExpression
				? FirstSceneInputExpressionTerm(component.fishingConfirmInputExpression)
				: component.fishingConfirmInput;
			if (component.fishingConfirmInputExpression) {
				result["confirmInputExpression"] =
					SceneInputExpressionToJson(
						*component.fishingConfirmInputExpression
					);
			}
			result["distanceBandCount"] = component.fishingDistanceBandCount;
			result["hooksPerDistanceBand"] = component.fishingHooksPerDistanceBand;
			result["distanceMultiplierBase"] = component.fishingDistanceMultiplierBase;
			result["distanceMultiplierStep"] = component.fishingDistanceMultiplierStep;
			result["useHookBandSettings"] = component.fishingUseHookBandSettings;
			json hookBands = json::array();
			for (const SceneFishingHookBandSettings& band : component.fishingHookBands) {
				hookBands.push_back({
					{ "distanceMultiplier", band.distanceMultiplier },
					{ "hookCount", band.hookCount },
					{ "hookMultiplierWeights", band.hookMultiplierWeights }
				});
			}
			result["hookBands"] = std::move(hookBands);
			result["hookScoreUnit"] = component.fishingHookScoreUnit;
			result["fishMultiplierBase"] = component.fishingFishMultiplierBase;
			result["fishMultiplierPerAdditionalFish"] =
				component.fishingFishMultiplierPerAdditionalFish;
			std::vector<SceneFishingHookRankDefinition> legacyRanks;
			const std::vector<SceneFishingHookRankDefinition>& ranks =
				ResolveFishingHookRanksForSave(component, legacyRanks);
			json hookRanks = json::array();
			std::vector<float> legacyScoreMultipliers;
			json legacyColors = json::array();
			legacyScoreMultipliers.reserve(ranks.size());
			for (const SceneFishingHookRankDefinition& rank : ranks) {
					hookRanks.push_back({
					{ "id", rank.id },
					{ "displayName", rank.displayName },
					{ "modelPath", rank.modelPath },
					{ "iconTexturePath", rank.iconTexturePath },
					{ "scoreMultiplier", rank.scoreMultiplier },
					{ "color", VectorToJson(rank.color) }
				});
				legacyScoreMultipliers.push_back(rank.scoreMultiplier);
				legacyColors.push_back(VectorToJson(rank.color));
			}
			result["hookRanks"] = std::move(hookRanks);
			result["hookTierScoreMultipliers"] = std::move(legacyScoreMultipliers);
			result["hookMultiplierColors"] = std::move(legacyColors);
			result["hookColorEmissiveIntensity"] =
				component.fishingHookColorEmissiveIntensity;
			result["hookLegendVisible"] = component.fishingHookLegendVisible;
			result["hookLegendTitleTextEntityId"] =
				component.fishingHookLegendTitleTextEntityId;
			result["hookLegendTextEntityIds"] = component.fishingHookLegendTextEntityIds;
			result["hookLegendTitle"] = component.fishingHookLegendTitle;
			result["hookLegendPrefix"] = component.fishingHookLegendPrefix;
			result["hookLegendIconEntityIds"] = component.fishingHookLegendIconEntityIds;
			result["hookLegendIconSize"] = VectorToJson(component.fishingHookLegendIconSize);
			result["randomizeSeedOnPlay"] = component.fishingRandomizeSeedOnPlay;
			result["randomSeed"] = component.fishingRandomSeed;
			result["fishCountTextEntityId"] = component.fishingFishCountTextEntityId;
			result["timerTextEntityId"] = component.fishingTimerTextEntityId;
			result["scoreTextEntityId"] = component.fishingScoreTextEntityId;
			result["multiplierTextEntityId"] = component.fishingMultiplierTextEntityId;
			result["resultTextEntityId"] = component.fishingResultTextEntityId;
			result["fishCountPrefix"] = component.fishingFishCountPrefix;
			result["timerPrefix"] = component.fishingTimerPrefix;
			result["scorePrefix"] = component.fishingScorePrefix;
			result["multiplierPrefix"] = component.fishingMultiplierPrefix;
			result["resultPrefix"] = component.fishingResultPrefix;
			result["useFormationCapsuleCollision"] =
				component.fishingUseFormationCapsuleCollision;
			result["formationOutlineVisible"] =
				component.fishingFormationOutlineVisible;
			result["formationOutlineColor"] =
				VectorToJson(component.fishingFormationOutlineColor);
			result["formationOutlineBloomIntensity"] =
				component.fishingFormationOutlineBloomIntensity;
			result["formationOutlineYOffset"] =
				component.fishingFormationOutlineYOffset;
			result["formationOutlineSegments"] =
				component.fishingFormationOutlineSegments;
		} else if (component.type == "FishingHookSpawnArea") {
			result["halfSizeX"] = component.fishingSpawnHalfSizeX;
			result["halfSizeZ"] = component.fishingSpawnHalfSizeZ;
			result["minimumDistance"] = component.fishingSpawnMinimumDistance;
			result["maxSpawnAttempts"] = component.fishingSpawnMaxAttempts;
		} else if (component.type == "FishingHookPool") {
			json entries = json::array();
			for (const SceneFishingHookPoolEntry& entry : component.fishingHookPoolEntries) {
				entries.push_back({
					{ "hookEntityId", entry.hookEntityId },
					{ "weightsByDistanceBand", entry.weightsByDistanceBand }
				});
			}
			result["entries"] = std::move(entries);
		} else if (component.type == "FishingHook") {
			result["baseScore"] = component.fishingHookBaseScore;
		} else if (component.type == "FishingShark") {
			result["radiusX"] = component.fishingSharkRadiusX;
			result["radiusZ"] = component.fishingSharkRadiusZ;
			result["angularSpeed"] = component.fishingSharkAngularSpeed;
			result["initialPhase"] = component.fishingSharkInitialPhase;
			result["penaltyScore"] = component.fishingSharkPenaltyScore;
			result["hitCooldownSeconds"] = component.fishingSharkHitCooldownSeconds;
			result["pathRandomness"] = component.fishingSharkPathRandomness;
			result["wanderMoveSpeed"] = component.fishingSharkWanderMoveSpeed;
			result["wanderMaximumTurnRate"] = component.fishingSharkWanderMaximumTurnRate;
			result["obstacleAvoidanceDistance"] = component.fishingSharkObstacleAvoidanceDistance;
			result["obstacleAvoidanceStrength"] = component.fishingSharkObstacleAvoidanceStrength;
			result["obstacleAvoidanceResponse"] = component.fishingSharkObstacleAvoidanceResponse;
		} else if (component.type == "Camera") {
			result["isMain"] = component.cameraIsMain;
			result["fovY"] = component.cameraFovY;
			result["nearClip"] = component.cameraNearClip;
			result["farClip"] = component.cameraFarClip;
			result["invertYaw"] = component.cameraInvertYaw;
			result["invertPitch"] = component.cameraInvertPitch;
		} else if (component.type == "Light") {
			result["lightType"] = component.lightType;
			result["color"] = VectorToJson(component.lightColor);
			result["intensity"] = component.lightIntensity;
			result["range"] = component.lightRange;
			result["decay"] = component.lightDecay;
			result["innerAngle"] = component.lightSpotInnerAngle;
			result["outerAngle"] = component.lightSpotOuterAngle;
			result["castsShadow"] = component.lightCastsShadow;
			result["shadow"] = {
				{ "bias", component.lightShadowBias },
				{ "normalBias", component.lightShadowNormalBias },
				{ "strength", component.lightShadowStrength },
				{ "distance", component.lightShadowDistance },
				{ "orthographicSize", component.lightShadowOrthographicSize },
				{ "nearClip", component.lightShadowNearClip },
				{ "farClip", component.lightShadowFarClip },
				{ "texelSnap", component.lightShadowTexelSnap }
			};
		} else if (component.type == "MonitorRenderer") {
			result["cameraEntityId"] = component.monitorCameraEntityId;
			result["cameraName"] = component.monitorCameraName;
			result["resolutionPreset"] = component.monitorResolutionPreset;
			result["width"] = component.monitorWidth;
			result["height"] = component.monitorHeight;
			result["hideSelf"] = component.monitorHideSelf;
		} else if (component.type == "CameraSwitcher") {
			result["triggerKey"] = component.cameraSwitchTriggerKey;
			result["wrap"] = component.cameraSwitchWrap;
			json cameras = json::array();
			for (const SceneCameraSwitchEntry& entry :
				component.cameraSwitchEntries) {
				cameras.push_back({
					{ "entityId", entry.cameraEntityId },
					{ "entityName", entry.cameraEntityName }
				});
			}
			result["cameras"] = std::move(cameras);
		} else if (component.type == "ThirdPersonCamera") {
			result["targetEntityId"] = component.thirdPersonTargetEntityId;
			result["targetEntityName"] = component.thirdPersonTargetEntityName;
			result["distance"] = component.thirdPersonDistance;
			result["aimDistance"] = component.thirdPersonAimDistance;
			result["targetOffset"] = VectorToJson(component.thirdPersonTargetOffset);
			result["aimTargetOffset"] =
				VectorToJson(component.thirdPersonAimTargetOffset);
			result["mouseSensitivity"] = component.thirdPersonMouseSensitivity;
			result["minPitch"] = component.thirdPersonMinPitch;
			result["maxPitch"] = component.thirdPersonMaxPitch;
			result["occlusionMargin"] = component.thirdPersonOcclusionMargin;
			result["occlusionMask"] = component.thirdPersonOcclusionMask;
			result["occlusionPullInSmoothTime"] =
				component.thirdPersonOcclusionPullInSmoothTime;
			result["occlusionRecoverySmoothTime"] =
				component.thirdPersonOcclusionRecoverySmoothTime;
			result["positionSmoothTime"] =
				component.thirdPersonPositionSmoothTime;
			result["rotationSmoothTime"] =
				component.thirdPersonRotationSmoothTime;
			result["yawReference"] = component.thirdPersonYawReference;
			result["allowMouseInput"] = component.thirdPersonAllowMouseInput;
			result["occlusionEnabled"] = component.thirdPersonOcclusionEnabled;
			result["aimModeEnabled"] = component.thirdPersonAimModeEnabled;
			result["invertYaw"] = component.thirdPersonInvertYaw;
			result["invertPitch"] = component.thirdPersonInvertPitch;
		} else if (component.type == "Animator") {
			result["playOnStart"] = component.animatorPlayOnStart;
			result["loop"] = component.animatorLoop;
			result["speed"] = component.animatorSpeed;
			result["defaultClip"] = component.animatorDefaultClip;
			result["transitionDuration"] =
				component.animatorTransitionDuration;
			result["blendCurve"] = component.animatorBlendCurve;
		} else if (component.type == "AudioSource") {
			result["clipPath"] = component.audioClipPath;
			result["spatialMode"] = component.audioSpatialMode;
			result["minimumDistance"] = component.audioMinimumDistance;
			result["maximumDistance"] = component.audioMaximumDistance;
			result["stereoAreaWidth"] = component.audioStereoAreaWidth;
			result["bus"] = component.audioBus;
			result["volume"] = component.audioVolume;
			result["pitch"] = component.audioPitch;
			result["loop"] = component.audioLoop;
			result["playOnStart"] = component.audioPlayOnStart;
			result["stopOnDisable"] = component.audioStopOnDisable;
			result["decompressOnLoad"] = component.audioDecompressOnLoad;
			result["streamFromDisk"] = component.audioStreamFromDisk;
			result["persistAcrossScenes"] = component.audioPersistAcrossScenes;
			result["bgmFadeSeconds"] = component.audioBgmFadeSeconds;
		} else if (component.type == "AudioListener") {
			result["mode"] = component.audioListenerMode;
		} else if (component.type == "PhysicsBody") {
			result["bodyType"] = component.physicsBodyType;
			result["mass"] = component.physicsMass;
			result["useGravity"] = component.physicsUseGravity;
			result["gravityScale"] = component.physicsGravityScale;
			result["drag"] = component.physicsDrag;
			result["restitution"] = component.physicsRestitution;
			result["friction"] = component.physicsFriction;
			result["maxFallSpeed"] = component.physicsMaxFallSpeed;
			result["velocity"] = VectorToJson(component.physicsVelocity);
			result["freezePositionX"] = component.physicsFreezePositionX;
			result["freezePositionY"] = component.physicsFreezePositionY;
			result["freezePositionZ"] = component.physicsFreezePositionZ;
		} else if (component.type == "OBBCollider") {
			result["offset"] = VectorToJson(component.colliderOffset);
			result["sizeMultiplier"] =
				VectorToJson(component.colliderSizeMultiplier);
			result["debugColor"] = VectorToJson(component.colliderDebugColor);
			result["shape"] = component.colliderShape;
			result["sphereRadius"] = component.colliderSphereRadius;
			result["debugVisible"] = component.colliderDebugVisible;
			result["debugDrawMode"] = component.colliderDebugDrawMode;
			result["debugSegments"] = component.colliderDebugSegments;
			result["isTrigger"] = component.colliderIsTrigger;
			result["active"] = component.colliderActive;
			result["layer"] = component.colliderLayer;
			result["mask"] = component.colliderMask;
		} else if (component.type == "PlayerBehavior") {
			result["moveSpeed"] = component.playerMoveSpeed;
			result["jumpVelocity"] = component.playerJumpVelocity;
			result["turnResponsiveness"] = component.playerTurnResponsiveness;
			result["dashMultiplier"] = component.playerDashMultiplier;
			result["cameraRelativeMove"] = component.playerCameraRelativeMove;
			result["allowJump"] = component.playerAllowJump;
			result["autoForward"] = component.playerAutoForward;
			result["inputMode"] = component.playerInputMode;
			result["gamepadDeadzone"] = component.playerGamepadDeadzone;
		} else if (component.type == "AgentBehavior") {
			result["behaviorName"] = component.agentBehaviorName;
			result["movementMode"] = component.agentMovementMode;
			result["profileName"] = component.agentProfileName;
			result["groupName"] = component.agentGroupName;
			result["boundsEntityId"] = component.agentBoundsEntityId;
			result["boundsName"] = component.agentBoundsName;
			result["attractorEntityId"] = component.agentAttractorEntityId;
			result["attractorTag"] = component.agentAttractorTag;
			result["useWaterBounds"] = component.agentUseWaterBounds;
			result["minSpeed"] = component.agentMinSpeed;
			result["maxSpeed"] = component.agentMaxSpeed;
			result["turnSpeed"] = component.agentTurnSpeed;
			result["wanderStrength"] = component.agentWanderStrength;
			result["wanderChangeInterval"] = component.agentWanderChangeInterval;
			result["wanderDirectionRange"] = component.agentWanderDirectionRange;
			result["wanderVerticalRange"] = component.agentWanderVerticalRange;
			result["randomizeSeedOnPlay"] = component.agentRandomizeSeedOnPlay;
			result["randomSeed"] = component.agentRandomSeed;
			result["flockDecisionInterval"] = component.agentFlockDecisionInterval;
			result["flockAcceleration"] = component.agentFlockAcceleration;
			result["flockTurnRate"] = component.agentFlockTurnRate;
			result["memberCenterFollow"] = component.agentMemberCenterFollow;
			result["memberJitterStrength"] = component.agentMemberJitterStrength;
			result["memberJitterFrequency"] = component.agentMemberJitterFrequency;
			result["memberJitterUpdateInterval"] =
				component.agentMemberJitterUpdateInterval;
			result["memberJitterFollowSpeed"] =
				component.agentMemberJitterFollowSpeed;
			result["memberSpeedVariation"] = component.agentMemberSpeedVariation;
			result["memberLeashDistance"] = component.agentMemberLeashDistance;
			result["memberLeashStrength"] = component.agentMemberLeashStrength;
			result["memberCatchupSpeed"] = component.agentMemberCatchupSpeed;
			result["memberSeparationUpdateInterval"] =
				component.agentMemberSeparationUpdateInterval;
			result["memberSeparationBlend"] =
				component.agentMemberSeparationBlend;
			result["memberMinimumDistance"] =
				component.agentMemberMinimumDistance;
			result["boundsWeight"] = component.agentBoundsWeight;
			result["useTeamHeading"] = component.agentUseTeamHeading;
			result["teamHeadingFromAverage"] =
				component.agentTeamHeadingFromAverage;
			result["teamHeadingDirection"] =
				VectorToJson(component.agentTeamHeadingDirection);
			result["teamHeadingWeight"] =
				component.agentTeamHeadingWeight;
			result["teamHeadingFollowSpeed"] =
				component.agentTeamHeadingFollowSpeed;
			result["useTeamRotation"] = component.agentUseTeamRotation;
			result["teamRotationWeight"] =
				component.agentTeamRotationWeight;
			result["teamRotationFollowSpeed"] =
				component.agentTeamRotationFollowSpeed;
			result["alignForwardToVelocity"] =
				component.agentAlignForwardToVelocity;
			result["forwardAxis"] = component.agentForwardAxis;
			result["rotateAxisX"] = component.agentRotateAxisX;
			result["rotateAxisY"] = component.agentRotateAxisY;
			result["rotateAxisZ"] = component.agentRotateAxisZ;
			result["rotationFollowSpeed"] =
				component.agentRotationFollowSpeed;
			result["pitchFromVerticalVelocity"] =
				component.agentPitchFromVerticalVelocity;
			result["bankingStrength"] = component.agentBankingStrength;
			result["schooling"] = component.agentSchooling;
			result["schoolingUpdateInterval"] =
				component.agentSchoolingUpdateInterval;
			result["schoolingUpdateJitter"] =
				component.agentSchoolingUpdateJitter;
			result["neighborLimit"] = component.agentNeighborLimit;
			result["schoolingBlend"] = component.agentSchoolingBlend;
			result["separationRadius"] = component.agentSeparationRadius;
			result["alignmentRadius"] = component.agentAlignmentRadius;
			result["cohesionRadius"] = component.agentCohesionRadius;
			result["separationWeight"] = component.agentSeparationWeight;
			result["alignmentWeight"] = component.agentAlignmentWeight;
			result["cohesionWeight"] = component.agentCohesionWeight;
			result["attractorWeight"] = component.agentAttractorWeight;
			result["teamSettingsOverride"] =
				component.agentTeamSettingsOverride;
			result["visualColor"] = VectorToJson(component.agentVisualColor);
			result["enableLighting"] = component.agentEnableLighting;
		} else if (component.type == "AgentAttractor") {
			result["tag"] = component.attractorTag;
			result["targetBehaviorName"] =
				component.attractorTargetBehaviorName;
			result["targetProfileName"] =
				component.attractorTargetProfileName;
			result["radius"] = component.attractorRadius;
			result["strength"] = component.attractorStrength;
			result["visualColor"] =
				VectorToJson(component.attractorVisualColor);
		} else if (component.type == "WaterVolume") {
			result["halfSize"] = VectorToJson(component.waterHalfSize);
			result["offset"] = VectorToJson(component.waterOffset);
			result["surfaceEnabled"] = component.waterSurfaceEnabled;
			result["surfaceBaseColor"] =
				VectorToJson(component.waterSurfaceBaseColor);
			result["surfaceHighlightColor"] =
				VectorToJson(component.waterSurfaceHighlightColor);
			result["surfaceAlpha"] = component.waterSurfaceAlpha;
			result["surfaceWaveScale"] = component.waterSurfaceWaveScale;
			result["surfaceNormalStrength"] =
				component.waterSurfaceNormalStrength;
			result["surfaceFresnelPower"] =
				component.waterSurfaceFresnelPower;
			result["lightShaftEnabled"] =
				component.waterLightShaftEnabled;
			result["lightColor"] = VectorToJson(component.waterLightColor);
			result["lightDirection"] =
				VectorToJson(component.waterLightDirection);
			result["lightIntensity"] = component.waterLightIntensity;
			result["lightDensity"] = component.waterLightDensity;
			result["causticsIntensity"] =
				component.waterLightCausticsIntensity;
			result["causticsScale"] = component.waterLightCausticsScale;
			result["causticsSpeed"] = component.waterLightCausticsSpeed;
			result["breakupStrength"] =
				component.waterLightBreakupStrength;
			result["warpStrength"] = component.waterLightWarpStrength;
			result["noiseScale"] = component.waterLightNoiseScale;
			result["lightSampleCount"] = component.waterLightSampleCount;
			result["moveSpeedMultiplier"] =
				component.waterMoveSpeedMultiplier;
			result["gravityScale"] = component.waterGravityScale;
			result["drag"] = component.waterDrag;
			result["maxFallSpeed"] = component.waterMaxFallSpeed;
			result["swimUpSpeed"] = component.waterSwimUpSpeed;
		} else if (component.type == "EntityReference") {
			result["referenceName"] = component.entityReferenceName;
			result["target"] = {
				{ "sceneId", component.entityReferenceTarget.sceneId },
				{ "instanceKey", component.entityReferenceTarget.instanceKey },
				{ "entityId", component.entityReferenceTarget.entityId }
			};
		} else if (component.type == "SceneTransition") {
			result["targetSceneId"] =
				component.sceneTransitionTargetSceneId;
			result["triggerType"] = component.sceneTransitionTriggerType;
			result["triggerKey"] = component.sceneTransitionTriggerKey;
		} else if (component.type == "CameraPath") {
			result["targetCameraName"] = component.cameraPathTargetCameraName;
			result["triggerType"] = component.cameraPathTriggerType;
			result["triggerKey"] = component.cameraPathTriggerKey;
			result["enterDuration"] = component.cameraPathEnterDuration;
			result["exitDuration"] = component.cameraPathExitDuration;
			result["interpolation"] = component.cameraPathInterpolation;
			result["defaultEasing"] = component.cameraPathDefaultEasing;
			result["returnToPreviousCamera"] =
				component.cameraPathReturnToPreviousCamera;
			result["startFromCurrentCamera"] =
				component.cameraPathStartFromCurrentCamera;
			result["autoCollectChildPoints"] =
				component.cameraPathAutoCollectChildPoints;
		} else if (component.type == "CameraPathPoint") {
			result["durationToNext"] =
				component.cameraPathPointDurationToNext;
			result["easingToNext"] =
				component.cameraPathPointEasingToNext;
		} else if (component.type == "StatSet") {
			result["stats"] = StatsToJson(component.stats);
		} else if (component.type == "EventTrigger") {
			result["bindings"] = EventsToJson(component.eventBindings);
		} else if (component.type == "PostProcessProfileManager") {
			result["profiles"] = PostProcessProfilesToJson(
				component.postProcessProfiles
			);
			result["statusTextEntityId"] =
				component.postProcessStatusTextEntityId;
			result["statusTextEntityName"] =
				component.postProcessStatusTextEntityName;
			result["statusTextPrefix"] =
				component.postProcessStatusTextPrefix;
		} else if (component.type == "PrefabAnimator") {
			result["clips"] = PrefabAnimationsToJson(
				component.prefabAnimationClips
			);
		} else if (component.type == "AttackSet") {
			result["attacks"] = AttackDefinitionsToJson(component.attackDefinitions);
		} else if (component.type == "StateMachine") {
			result["initialState"] = component.stateMachineInitialState;
			result["resetOnDisable"] = component.stateMachineResetOnDisable;
			result["states"] = StateMachineStatesToJson(
				component.stateMachineStates
			);
		} else if (component.type == "Faction") {
			result["name"] = component.factionName;
		} else if (component.type == "HitBox") {
			result["damage"] = component.hitBoxDamage;
			result["poiseDamage"] = component.hitBoxPoiseDamage;
			result["knockback"] = component.hitBoxKnockback;
			result["verticalKnockback"] = component.hitBoxVerticalKnockback;
			result["knockbackDirectionMode"] = component.hitBoxKnockbackDirectionMode;
			result["knockbackLocalDirection"] = VectorToJson(component.hitBoxKnockbackLocalDirection);
			result["hitPolicy"] = component.hitBoxHitPolicy;
			result["targetCooldown"] = component.hitBoxTargetCooldown;
			result["hitStopDuration"] = component.hitBoxHitStopDuration;
			result["reactionTag"] = component.hitBoxReactionTag;
			result["damageStatId"] = component.hitBoxDamageStatId;
			result["poiseStatId"] = component.hitBoxPoiseStatId;
			result["ownerEntityId"] = component.hitBoxOwnerEntityId;
			result["ownerEntityName"] = component.hitBoxOwnerEntityName;
			result["ignoreSameFaction"] = component.hitBoxIgnoreSameFaction;
		} else if (component.type == "HurtBox") {
			result["damageMultiplier"] = component.hurtBoxDamageMultiplier;
			result["healthStatId"] = component.hurtBoxHealthStatId;
			result["statsEntityId"] = component.hurtBoxStatsEntityId;
			result["statsEntityName"] = component.hurtBoxStatsEntityName;
		} else if (component.type == "HitReaction") {
			result["knockbackMultiplier"] =
				component.hitReactionKnockbackMultiplier;
			result["triggerMode"] = component.hitReactionTriggerMode;
			result["minimumPoiseDamage"] =
				component.hitReactionMinimumPoiseDamage;
			result["poiseStatId"] = component.hitReactionPoiseStatId;
			result["poiseRecoveryDelay"] =
				component.hitReactionPoiseRecoveryDelay;
			result["stateName"] = component.hitReactionStateName;
			result["stateDuration"] = component.hitReactionStateDuration;
		} else if (component.type == "DeathPresentation") {
			result["stateName"] = component.deathPresentationStateName;
			result["deactivateDelay"] =
				component.deathPresentationDeactivateDelay;
			result["effectPath"] = component.deathPresentationEffectPath;
		} else if (component.type == "BoneAttachment") {
			result["targetEntityId"] = component.boneAttachmentTargetEntityId;
			result["targetEntityName"] = component.boneAttachmentTargetEntityName;
			result["jointName"] = component.boneAttachmentJointName;
			result["alignmentMode"] = component.boneAttachmentAlignmentMode;
			result["sourceJointName"] = component.boneAttachmentSourceJointName;
			result["inheritScale"] = component.boneAttachmentInheritScale;
		} else if (component.type == "EnemyBehavior") {
			result["targetEntityId"] = component.enemyTargetEntityId;
			result["targetEntityName"] = component.enemyTargetEntityName;
			result["healthStatId"] = component.enemyHealthStatId;
			result["detectionRange"] = component.enemyDetectionRange;
			result["loseRange"] = component.enemyLoseRange;
			result["attackRange"] = component.enemyAttackRange;
			result["moveSpeed"] = component.enemyMoveSpeed;
			result["turnSpeed"] = component.enemyTurnSpeed;
			result["attackCooldown"] = component.enemyAttackCooldown;
			result["attackWindup"] = component.enemyAttackWindup;
			result["attackActiveTime"] = component.enemyAttackActiveTime;
			result["attackRecovery"] = component.enemyAttackRecovery;
			result["attackAnimationClip"] = component.enemyAttackAnimationClip;
			result["attackPrefabAnimationClip"] =
				component.enemyAttackPrefabAnimationClip;
			result["attackHitBoxEntityId"] =
				component.enemyAttackHitBoxEntityId;
			result["attackHitBoxEntityName"] =
				component.enemyAttackHitBoxEntityName;
		} else if (component.type == "EnemySpawner") {
			result["prefabPath"] = component.enemySpawnerPrefabPath;
			result["initialCount"] = component.enemySpawnerInitialCount;
			result["maxAlive"] = component.enemySpawnerMaxAlive;
			result["interval"] = component.enemySpawnerInterval;
			result["radius"] = component.enemySpawnerRadius;
			result["autoStart"] = component.enemySpawnerAutoStart;
		} else if (component.type == "Projectile") {
			result["direction"] = VectorToJson(component.projectileDirection);
			result["speed"] = component.projectileSpeed;
			result["gravity"] = component.projectileGravity;
			result["lifetime"] = component.projectileLifetime;
			result["destroyOnHit"] = component.projectileDestroyOnHit;
			result["homingTargetEntityId"] =
				component.projectileHomingTargetEntityId;
			result["homingTargetEntityName"] =
				component.projectileHomingTargetEntityName;
			result["homingStrength"] = component.projectileHomingStrength;
		}
		return result;
	}

	void ReadStats(
		const json& source,
		std::vector<SceneStatDefinition>& destination
	) {
		if (!source.is_array()) {
			return;
		}
		for (const json& value : source) {
			if (!value.is_object()) {
				continue;
			}
			SceneStatDefinition stat{};
			stat.id = value.value("id", stat.id);
			stat.displayName = value.value("displayName", stat.displayName);
			stat.minValue = value.value("min", stat.minValue);
			stat.maxValue = (std::max)(
				value.value("max", stat.maxValue),
				stat.minValue
			);
			stat.initialValue = std::clamp(
				value.value("initial", stat.initialValue),
				stat.minValue,
				stat.maxValue
			);
			if (!stat.id.empty()) {
				destination.push_back(std::move(stat));
			}
		}
	}
	SceneEventAction ReadEventAction(const json& value) {
		SceneEventAction action{};
		if (!value.is_object()) {
			return action;
		}
		action.type = value.value("type", action.type);
		action.targetEntityId = value.value(
			"targetEntityId", action.targetEntityId
		);
		action.targetEntityName = value.value(
			"targetEntityName", action.targetEntityName
		);
		action.statId = value.value("statId", action.statId);
		action.statOperation = value.value(
			"statOperation", action.statOperation
		);
		action.value = value.value("value", action.value);
		action.active = value.value("active", action.active);
		action.sceneId = value.value("sceneId", action.sceneId);
		action.prefabPath = value.value("prefabPath", action.prefabPath);
		action.prefabParentToTarget = value.value(
			"prefabParentToTarget", action.prefabParentToTarget
		);
		action.prefabUseTargetTransform = value.value(
			"prefabUseTargetTransform", action.prefabUseTargetTransform
		);
		action.stateName = value.value("stateName", action.stateName);
		action.postProcessManagerEntityId = value.value(
			"postProcessManagerEntityId", action.postProcessManagerEntityId
		);
		action.postProcessManagerEntityName = value.value(
			"postProcessManagerEntityName", action.postProcessManagerEntityName
		);
		action.postProcessProfileId = value.value(
			"postProcessProfileId", action.postProcessProfileId
		);
		action.textMotionClipId = value.value(
			"textMotionClipId", action.textMotionClipId
		);
		return action;
	}

	void ReadEvents(
		const json& source,
		std::vector<SceneEventBinding>& destination
	) {
		if (!source.is_array()) {
			return;
		}
		for (const json& value : source) {
			if (!value.is_object()) {
				continue;
			}
			SceneEventBinding binding{};
			binding.triggerType = value.value(
				"triggerType", binding.triggerType
			);
			binding.triggerKey = value.value("triggerKey", binding.triggerKey);
			if (const auto inputExpression = value.find("inputExpression");
				inputExpression != value.end()) {
				binding.inputExpression = ReadSceneInputExpression(*inputExpression);
				binding.triggerKey = FirstSceneInputExpressionTerm(
					binding.inputExpression
				);
			}
			binding.targetEntityId = value.value(
				"targetEntityId", binding.targetEntityId
			);
			binding.targetEntityName = value.value(
				"targetEntityName", binding.targetEntityName
			);
			binding.statId = value.value("statId", binding.statId);
			binding.statComparison = value.value(
				"statComparison", binding.statComparison
			);
			binding.statValue = value.value("statValue", binding.statValue);
			if (value.contains("targetPosition")) {
				binding.targetPosition = JsonToVector(
					value.at("targetPosition"),
					binding.targetPosition
				);
			}
			binding.radius = (std::max)(value.value("radius", binding.radius), 0.0f);
			binding.triggerOnce = value.value(
				"triggerOnce", binding.triggerOnce
			);
			binding.cooldown = (std::max)(
				value.value("cooldown", binding.cooldown),
				0.0f
			);
			binding.textMotionClipId = value.value(
				"textMotionClipId", binding.textMotionClipId
			);
			if (const auto actions = value.find("actions");
				actions != value.end() && actions->is_array()) {
				for (const json& action : *actions) {
					binding.actions.push_back(ReadEventAction(action));
				}
			}
			destination.push_back(std::move(binding));
		}
	}

	void ReadPostProcessProfiles(
		const json& source,
		std::vector<ScenePostProcessProfile>& destination
	) {
		if (!source.is_array()) {
			return;
		}
		for (const json& value : source) {
			if (!value.is_object()) {
				continue;
			}
			ScenePostProcessProfile profile{};
			profile.id = value.value("id", profile.id);
			profile.label = value.value("label", profile.label);
			profile.settings = PostProcessFromJson(
				value.value("settings", json::object()), profile.settings
			);
			const json& automations = value.value(
				"automations", json::array()
			);
			if (automations.is_array()) {
				for (const json& automationValue : automations) {
					if (!automationValue.is_object() ||
						automationValue.value("parameter", std::string{}) !=
							"DissolveThreshold" ||
						automationValue.value("playback", std::string{ "OneShot" }) !=
							"OneShot" ||
						automationValue.value("easing", std::string{ "Linear" }) !=
							"Linear") {
						continue;
					}
					ScenePostProcessAutomation automation{};
					automation.startValue = automationValue.value(
						"startValue", automation.startValue
					);
					automation.endValue = automationValue.value(
						"endValue", automation.endValue
					);
					automation.duration = (std::max)(
						automationValue.value("duration", automation.duration),
						0.001f
					);
					profile.automations.push_back(automation);
				}
			}
			if (!profile.id.empty()) {
				destination.push_back(std::move(profile));
			}
		}
	}

	void ReadPrefabAnimations(
		const json& source,
		std::vector<ScenePrefabAnimationClip>& destination
	) {
		if (!source.is_array()) {
			return;
		}
		for (const json& value : source) {
			if (!value.is_object()) {
				continue;
			}
			ScenePrefabAnimationClip clip{};
			clip.name = value.value("name", clip.name);
			clip.duration = (std::max)(value.value("duration", clip.duration), 0.001f);
			clip.loop = value.value("loop", clip.loop);
			clip.playOnStart = value.value("playOnStart", clip.playOnStart);
			if (const auto tracks = value.find("tracks");
				tracks != value.end() && tracks->is_array()) {
				for (const json& trackValue : *tracks) {
					if (!trackValue.is_object()) {
						continue;
					}
					SceneAnimationTrack track{};
					track.targetEntityId = trackValue.value(
						"targetEntityId", track.targetEntityId
					);
					track.targetEntityName = trackValue.value(
						"targetEntityName", track.targetEntityName
					);
					track.property = trackValue.value("property", track.property);
					track.easing = trackValue.value("easing", track.easing);
					if (const auto keyframes = trackValue.find("keyframes");
						keyframes != trackValue.end() && keyframes->is_array()) {
						for (const json& keyframeValue : *keyframes) {
							if (!keyframeValue.is_object()) {
								continue;
							}
							SceneAnimationKeyframe keyframe{};
							keyframe.time = (std::max)(
								keyframeValue.value("time", keyframe.time),
								0.0f
							);
							if (keyframeValue.contains("value")) {
								keyframe.value = JsonToVector(
									keyframeValue.at("value"),
									keyframe.value
								);
							}
							keyframe.easingToNext = keyframeValue.value(
								"easingToNext",
								keyframe.easingToNext
							);
							if (keyframeValue.contains("positionBulge")) {
								keyframe.positionBulge = JsonToVector(
									keyframeValue.at("positionBulge"),
									keyframe.positionBulge
								);
							}
							track.keyframes.push_back(keyframe);
						}
						std::stable_sort(
							track.keyframes.begin(),
							track.keyframes.end(),
							[](const SceneAnimationKeyframe& left,
								const SceneAnimationKeyframe& right) {
								return left.time < right.time;
							}
						);
					}
					clip.tracks.push_back(std::move(track));
				}
			}
			destination.push_back(std::move(clip));
		}
	}

	void ReadStateMachineStates(
		const json& source,
		std::vector<SceneStateDefinition>& destination
	) {
		if (!source.is_array()) {
			return;
		}
		for (const json& stateValue : source) {
			if (!stateValue.is_object()) {
				continue;
			}
			SceneStateDefinition state{};
			state.name = stateValue.value("name", state.name);
			state.actionId = stateValue.value("actionId", state.actionId);
			if (const auto parameters = stateValue.find("parameters");
				parameters != stateValue.end() && parameters->is_array()) {
				for (const json& parameterValue : *parameters) {
					if (!parameterValue.is_object()) {
						continue;
					}
					SceneStateParameter parameter{};
					parameter.name = parameterValue.value("name", parameter.name);
					parameter.type = parameterValue.value("type", parameter.type);
					parameter.floatValue = parameterValue.value(
						"floatValue", parameter.floatValue
					);
					parameter.intValue = parameterValue.value(
						"intValue", parameter.intValue
					);
					parameter.boolValue = parameterValue.value(
						"boolValue", parameter.boolValue
					);
					parameter.stringValue = parameterValue.value(
						"stringValue", parameter.stringValue
					);
					parameter.entityId = parameterValue.value(
						"entityId", parameter.entityId
					);
					parameter.entityName = parameterValue.value(
						"entityName", parameter.entityName
					);
					state.parameters.push_back(std::move(parameter));
				}
			}
			destination.push_back(std::move(state));
		}
	}

	void ReadAttackDefinitions(const json& source, std::vector<SceneAttackDefinition>& destination) {
		if (!source.is_array()) { return; }
		for (const json& value : source) {
			if (!value.is_object()) { continue; }
			SceneAttackDefinition attack{};
			attack.name = value.value("name", attack.name);
			attack.animation = value.value("animation", attack.animation);
			attack.animationTargetEntityId = value.value("animationTargetEntityId", attack.animationTargetEntityId);
			attack.animationTargetEntityName = value.value("animationTargetEntityName", attack.animationTargetEntityName);
			attack.hitBoxEntityId = value.value("hitBoxEntityId", attack.hitBoxEntityId);
			attack.hitBoxEntityName = value.value("hitBoxEntityName", attack.hitBoxEntityName);
			attack.windup = (std::max)(value.value("windup", attack.windup), 0.0f);
			attack.activeTime = (std::max)(value.value("activeTime", attack.activeTime), 0.0f);
			attack.recovery = (std::max)(value.value("recovery", attack.recovery), 0.0f);
			attack.forwardDistance = value.value("forwardDistance", attack.forwardDistance);
			attack.sideDistance = value.value("sideDistance", attack.sideDistance);
			attack.motionEasing = value.value("motionEasing", attack.motionEasing);
			attack.facingMode = value.value("facingMode", attack.facingMode);
			if (
				attack.facingMode != "InputDirection" &&
				attack.facingMode != "TargetDirection" &&
				attack.facingMode != "RotateByAngle"
			) {
				attack.facingMode = "FixedAtStart";
			}
			attack.facingTargetEntityId = value.value(
				"facingTargetEntityId", attack.facingTargetEntityId
			);
			attack.facingTargetEntityName = value.value(
				"facingTargetEntityName", attack.facingTargetEntityName
			);
			attack.facingRotateAngle = value.value(
				"facingRotateAngle", attack.facingRotateAngle
			);
			attack.loopEnabled = value.value("loopEnabled", attack.loopEnabled);
			attack.loopMaxCount = (std::max)(
				value.value("loopMaxCount", attack.loopMaxCount), 0
			);
			attack.loopSafetyTimeout = (std::max)(
				value.value("loopSafetyTimeout", attack.loopSafetyTimeout), 0.0f
			);
			if (const auto windows = value.find("hitWindows"); windows != value.end() && windows->is_array()) {
				for (const json& windowValue : *windows) {
					if (!windowValue.is_object()) { continue; }
					SceneAttackHitWindow window{};
					window.startTime = (std::max)(windowValue.value("startTime", window.startTime), 0.0f);
					window.endTime = (std::max)(windowValue.value("endTime", window.endTime), window.startTime);
					window.hitBoxEntityId = windowValue.value("hitBoxEntityId", window.hitBoxEntityId);
					window.hitBoxEntityName = windowValue.value("hitBoxEntityName", window.hitBoxEntityName);
					window.payloadSource = windowValue.value("payloadSource", "WindowLegacy");
					if (window.payloadSource != "HitBox") {
						window.payloadSource = "WindowLegacy";
						window.damage = (std::max)(windowValue.value("damage", window.damage), 0.0f);
						window.poiseDamage = (std::max)(windowValue.value("poiseDamage", window.poiseDamage), 0.0f);
						window.knockback = (std::max)(windowValue.value("knockback", window.knockback), 0.0f);
						window.verticalKnockback = (std::max)(
							windowValue.value("verticalKnockback", window.verticalKnockback), 0.0f
						);
						window.overrideHitBoxHalfSize = windowValue.value(
							"overrideHitBoxHalfSize", window.overrideHitBoxHalfSize
						);
						if (window.overrideHitBoxHalfSize && windowValue.contains("hitBoxHalfSize")) {
							window.hitBoxHalfSize = JsonToVector(
								windowValue.at("hitBoxHalfSize"), window.hitBoxHalfSize
							);
							window.hitBoxHalfSize.x = (std::max)(window.hitBoxHalfSize.x, 0.001f);
							window.hitBoxHalfSize.y = (std::max)(window.hitBoxHalfSize.y, 0.001f);
							window.hitBoxHalfSize.z = (std::max)(window.hitBoxHalfSize.z, 0.001f);
						}
						window.hitStopDuration = (std::max)(
							windowValue.value("hitStopDuration", window.hitStopDuration), 0.0f
						);
						window.reactionTag = windowValue.value("reactionTag", window.reactionTag);
						window.knockbackDirectionMode = windowValue.value("knockbackDirectionMode", window.knockbackDirectionMode);
						window.hitPolicy = windowValue.value("hitPolicy", window.hitPolicy);
						if (window.hitPolicy != "OncePerLoop" && window.hitPolicy != "TargetCooldown") {
							window.hitPolicy = "OncePerActivation";
						}
						window.targetCooldown = (std::max)(
							windowValue.value("targetCooldown", window.targetCooldown), 0.0f
						);
						if (windowValue.contains("knockbackLocalDirection")) window.knockbackLocalDirection = JsonToVector(windowValue.at("knockbackLocalDirection"), window.knockbackLocalDirection);
					}
					attack.hitWindows.push_back(std::move(window));
				}
			}
			if (const auto effects = value.find("effectEvents"); effects != value.end() && effects->is_array()) {
				for (const json& effectValue : *effects) {
					if (!effectValue.is_object()) { continue; }
					SceneAttackEffectEvent effect{};
					effect.time = (std::max)(effectValue.value("time", effect.time), 0.0f);
					effect.particleEffectPath = effectValue.value(
						"particleEffectPath", effect.particleEffectPath
					);
					effect.spawnEntityId = effectValue.value(
						"spawnEntityId", effect.spawnEntityId
					);
					effect.spawnEntityName = effectValue.value(
						"spawnEntityName", effect.spawnEntityName
					);
					if (effectValue.contains("localOffset")) {
						effect.localOffset = JsonToVector(
							effectValue.at("localOffset"), effect.localOffset
						);
					}
					effect.groundPrefabPath = effectValue.value(
						"groundPrefabPath", effect.groundPrefabPath
					);
					effect.groundProbeDistance = (std::max)(
						effectValue.value(
							"groundProbeDistance", effect.groundProbeDistance
						),
						0.0f
					);
					effect.groundPrefabLifetime = (std::max)(
						effectValue.value(
							"groundPrefabLifetime", effect.groundPrefabLifetime
						),
						0.0f
					);
					effect.groundEffectType = effectValue.value(
						"groundEffectType",
						effect.groundPrefabPath.empty() ? "None" : "Prefab"
					);
					if (effect.groundEffectType != "Prefab" &&
						effect.groundEffectType != "ProceduralCrack") {
						effect.groundEffectType = "None";
					}
					effect.groundCrackRadius = (std::max)(effectValue.value(
						"groundCrackRadius", effect.groundCrackRadius), 0.0f);
					effect.groundCrackPrimaryBranchCount = (std::min)(
						(std::max)(effectValue.value("groundCrackPrimaryBranchCount", effect.groundCrackPrimaryBranchCount), 1u), 24u);
					effect.groundCrackSegmentsPerBranch = (std::min)(
						(std::max)(effectValue.value("groundCrackSegmentsPerBranch", effect.groundCrackSegmentsPerBranch), 1u), 12u);
					effect.groundCrackBranchProbability = (std::clamp)(effectValue.value(
						"groundCrackBranchProbability", effect.groundCrackBranchProbability), 0.0f, 1.0f);
					effect.groundCrackWidth = (std::max)(effectValue.value(
						"groundCrackWidth", effect.groundCrackWidth), 0.0f);
					effect.groundCrackLifetime = (std::max)(effectValue.value(
						"groundCrackLifetime", effect.groundCrackLifetime), 0.0f);
					effect.groundCrackSurfaceOffset = (std::max)(effectValue.value(
						"groundCrackSurfaceOffset", effect.groundCrackSurfaceOffset), 0.0f);
					attack.effectEvents.push_back(std::move(effect));
				}
			}
			destination.push_back(std::move(attack));
		}
	}

	uint64_t RemapEntityId(
		uint64_t id,
		const std::unordered_map<uint64_t, uint64_t>& idMap,
		bool preserveUnmappedId = false
	) {
		const auto found = idMap.find(id);
		// Prefab変換は外部IDを破棄し、Scene内複製はブランチ外参照を維持する。
		return found == idMap.end()
			? (preserveUnmappedId ? id : 0)
			: found->second;
	}

	void RemapComponentEntityReferences(
		SceneComponent& component,
		const std::unordered_map<uint64_t, uint64_t>& idMap,
		bool preserveUnmappedIds = false
	) {
		component.monitorCameraEntityId = RemapEntityId(
			component.monitorCameraEntityId, idMap, preserveUnmappedIds
		);
		component.thirdPersonTargetEntityId = RemapEntityId(
			component.thirdPersonTargetEntityId, idMap, preserveUnmappedIds
		);
		for (SceneCameraSwitchEntry& entry : component.cameraSwitchEntries) {
			entry.cameraEntityId = RemapEntityId(
				entry.cameraEntityId, idMap, preserveUnmappedIds
			);
		}
		component.agentBoundsEntityId = RemapEntityId(
			component.agentBoundsEntityId, idMap, preserveUnmappedIds
		);
		component.agentAttractorEntityId = RemapEntityId(
			component.agentAttractorEntityId, idMap, preserveUnmappedIds
		);
		if (component.entityReferenceTarget.sceneId.empty()) {
			component.entityReferenceTarget.entityId = RemapEntityId(
				component.entityReferenceTarget.entityId,
				idMap,
				preserveUnmappedIds
			);
		}
		component.hitBoxOwnerEntityId = RemapEntityId(
			component.hitBoxOwnerEntityId, idMap, preserveUnmappedIds
		);
		component.hurtBoxStatsEntityId = RemapEntityId(
			component.hurtBoxStatsEntityId, idMap, preserveUnmappedIds
		);
		for (SceneStateDefinition& state : component.stateMachineStates) {
			for (SceneStateParameter& parameter : state.parameters) {
				parameter.entityId = RemapEntityId(
					parameter.entityId, idMap, preserveUnmappedIds
				);
			}
		}
		component.boneAttachmentTargetEntityId = RemapEntityId(
			component.boneAttachmentTargetEntityId, idMap, preserveUnmappedIds
		);
		component.enemyTargetEntityId = RemapEntityId(
			component.enemyTargetEntityId, idMap, preserveUnmappedIds
		);
		component.enemyAttackHitBoxEntityId = RemapEntityId(
			component.enemyAttackHitBoxEntityId, idMap, preserveUnmappedIds
		);
		component.projectileHomingTargetEntityId = RemapEntityId(
			component.projectileHomingTargetEntityId, idMap, preserveUnmappedIds
		);
		component.fishingPlayerEntityId = RemapEntityId(
			component.fishingPlayerEntityId, idMap, preserveUnmappedIds
		);
		for (uint64_t& fishEntityId : component.fishingFishEntityIds) {
			fishEntityId = RemapEntityId(
				fishEntityId, idMap, preserveUnmappedIds
			);
		}
		component.fishingHookSpawnAreaEntityId = RemapEntityId(
			component.fishingHookSpawnAreaEntityId, idMap, preserveUnmappedIds
		);
		component.fishingHookPoolEntityId = RemapEntityId(
			component.fishingHookPoolEntityId, idMap, preserveUnmappedIds
		);
		component.fishingWaterVolumeEntityId = RemapEntityId(
			component.fishingWaterVolumeEntityId, idMap, preserveUnmappedIds
		);
		component.fishingHookLegendTitleTextEntityId = RemapEntityId(
			component.fishingHookLegendTitleTextEntityId, idMap, preserveUnmappedIds
		);
		for (uint64_t& textEntityId : component.fishingHookLegendTextEntityIds) {
			textEntityId = RemapEntityId(textEntityId, idMap, preserveUnmappedIds);
		}
		for (uint64_t& iconEntityId : component.fishingHookLegendIconEntityIds) {
			iconEntityId = RemapEntityId(iconEntityId, idMap, preserveUnmappedIds);
		}
		for (SceneFishingHookPoolEntry& entry : component.fishingHookPoolEntries) {
			entry.hookEntityId = RemapEntityId(
				entry.hookEntityId, idMap, preserveUnmappedIds
			);
		}
		component.fishingFishCountTextEntityId = RemapEntityId(
			component.fishingFishCountTextEntityId, idMap, preserveUnmappedIds
		);
		component.fishingTimerTextEntityId = RemapEntityId(
			component.fishingTimerTextEntityId, idMap, preserveUnmappedIds
		);
		component.fishingScoreTextEntityId = RemapEntityId(
			component.fishingScoreTextEntityId, idMap, preserveUnmappedIds
		);
		component.fishingMultiplierTextEntityId = RemapEntityId(
			component.fishingMultiplierTextEntityId, idMap, preserveUnmappedIds
		);
		component.fishingResultTextEntityId = RemapEntityId(
			component.fishingResultTextEntityId, idMap, preserveUnmappedIds
		);
		for (SceneEventBinding& binding : component.eventBindings) {
			binding.targetEntityId = RemapEntityId(
				binding.targetEntityId, idMap, preserveUnmappedIds
			);
			for (SceneEventAction& action : binding.actions) {
				action.targetEntityId = RemapEntityId(
					action.targetEntityId, idMap, preserveUnmappedIds
				);
			}
		}
		for (ScenePrefabAnimationClip& clip : component.prefabAnimationClips) {
			for (SceneAnimationTrack& track : clip.tracks) {
				track.targetEntityId = RemapEntityId(
					track.targetEntityId, idMap, preserveUnmappedIds
				);
			}
		}
		for (SceneAttackDefinition& attack : component.attackDefinitions) {
			attack.animationTargetEntityId = RemapEntityId(
				attack.animationTargetEntityId, idMap, preserveUnmappedIds
			);
			attack.hitBoxEntityId = RemapEntityId(
				attack.hitBoxEntityId, idMap, preserveUnmappedIds
			);
			attack.facingTargetEntityId = RemapEntityId(
				attack.facingTargetEntityId, idMap, preserveUnmappedIds
			);
			for (SceneAttackHitWindow& window : attack.hitWindows) {
				window.hitBoxEntityId = RemapEntityId(
					window.hitBoxEntityId, idMap, preserveUnmappedIds
				);
			}
			for (SceneAttackEffectEvent& effect : attack.effectEvents) {
				effect.spawnEntityId = RemapEntityId(
					effect.spawnEntityId, idMap, preserveUnmappedIds
				);
			}
		}
	}

	std::vector<SceneComponent> ComponentsFromJson(const json& source) {
		std::vector<SceneComponent> components;
		if (!source.is_array()) {
			return components;
		}
		for (const json& value : source) {
			SceneComponent component{};
			if (value.is_string()) {
				component.type = value.get<std::string>();
			} else if (value.is_object()) {
				component.localId = value.value("localId", uint64_t{});
				component.type = value.value("type", std::string{});
				component.enabled = value.value("enabled", true);
				component.modelPath = value.value("modelPath", std::string{});
				component.meshCullMode = value.value(
					"cullMode",
					component.meshCullMode
				);
				if (component.type == "MeshRenderer" && value.contains("visualRotation")) {
					component.meshVisualRotation = JsonToVector(
						value.at("visualRotation"),
						component.meshVisualRotation
					);
				}
				component.meshEnvironmentReflectionOverride = value.value(
					"environmentReflectionOverride",
					component.meshEnvironmentReflectionOverride
				);
				component.meshEnvironmentReflectionIntensity = value.value(
					"environmentReflectionIntensity",
					component.meshEnvironmentReflectionIntensity
				);
				if (const auto overrides = value.find("materialOverrides");
					overrides != value.end() && overrides->is_array()) {
					for (const json& item : *overrides) {
						if (!item.is_object()) {
							continue;
						}
						SceneMeshMaterialOverride override{};
						override.materialName = item.value(
							"materialName", std::string{}
						);
						override.enabled = item.value("enabled", false);
						override.colorOverrideEnabled = item.value(
							"colorOverrideEnabled", false
						);
						override.texturePath = item.value(
							"texturePath", std::string{}
						);
						if (item.contains("color")) {
							override.color = JsonToVector(
								item.at("color"), override.color
							);
						}
						if (!override.materialName.empty()) {
							component.meshMaterialOverrides.push_back(
								std::move(override)
							);
						}
					}
				}
				component.environmentSkyboxEnabled = value.value(
					"skyboxEnabled",
					component.environmentSkyboxEnabled
				);
				component.environmentSkyboxPath = value.value(
					"skyboxPath",
					component.environmentSkyboxPath
				);
				component.environmentSkyboxIntensity = value.value(
					"skyboxIntensity",
					component.environmentSkyboxIntensity
				);
				component.environmentReflectionIntensity = value.value(
					"reflectionIntensity",
					component.environmentReflectionIntensity
				);
				component.texturePath = value.value("texturePath", std::string{});
				if (value.contains("size")) {
					component.spriteSize = JsonToVector(
						value.at("size"),
						component.spriteSize
					);
				}
				if (value.contains("anchor")) {
					component.spriteAnchor = JsonToVector(
						value.at("anchor"),
						component.spriteAnchor
					);
				}
				component.spriteRenderSpace = value.value(
					"renderSpace", component.spriteRenderSpace
				);
				if (value.contains("viewportAnchor")) {
					component.spriteViewportAnchor = JsonToVector(
						value.at("viewportAnchor"),
						component.spriteViewportAnchor
					);
				}
				if (value.contains("color")) {
					component.spriteColor = JsonToVector(
						value.at("color"),
						component.spriteColor
					);
				}
				component.spriteFlipX = value.value("flipX", false);
				component.spriteFlipY = value.value("flipY", false);
				if (component.type == "TextRenderer") {
					component.textValue = value.value(
						"text", component.textValue
					);
					component.textRenderSpace = value.value(
						"renderSpace", component.textRenderSpace
					);
					component.textFontSource = value.value(
						"fontSource", component.textFontSource
					);
					component.textFontResourcePath = value.value(
						"fontResourcePath", component.textFontResourcePath
					);
					component.textFontFamily = value.value(
						"fontFamily", component.textFontFamily
					);
					component.textFontSize = std::clamp(
						value.value("fontSize", component.textFontSize),
						1.0f,
						512.0f
					);
					component.textFontWeight = value.value(
						"fontWeight", component.textFontWeight
					);
					component.textFontStyle = value.value(
						"fontStyle", component.textFontStyle
					);
					if (value.contains("color")) {
						component.textColor = JsonToVector(
							value.at("color"), component.textColor
						);
					}
					component.textOpacity = std::clamp(
						value.value("opacity", component.textOpacity),
						0.0f,
						1.0f
					);
					component.textHorizontalAlignment = value.value(
						"horizontalAlignment", component.textHorizontalAlignment
					);
					component.textVerticalAlignment = value.value(
						"verticalAlignment", component.textVerticalAlignment
					);
					component.textWrapMode = value.value(
						"wrapMode", component.textWrapMode
					);
					component.textOverflowMode = value.value(
						"overflowMode", component.textOverflowMode
					);
					if (value.contains("layoutSize")) {
						component.textLayoutSize = JsonToVector(
							value.at("layoutSize"), component.textLayoutSize
						);
					}
					component.textCharacterSpacing = value.value(
						"characterSpacing", component.textCharacterSpacing
					);
					component.textLineSpacing = (std::max)(
						value.value("lineSpacing", component.textLineSpacing),
						0.1f
					);
					component.textOutlineEnabled = value.value(
						"outlineEnabled", component.textOutlineEnabled
					);
					if (value.contains("outlineColor")) {
						component.textOutlineColor = JsonToVector(
							value.at("outlineColor"), component.textOutlineColor
						);
					}
					component.textOutlineWidth = std::clamp(
						value.value("outlineWidth", component.textOutlineWidth),
						0.0f,
						32.0f
					);
					component.textShadowEnabled = value.value(
						"shadowEnabled", component.textShadowEnabled
					);
					if (value.contains("shadowColor")) {
						component.textShadowColor = JsonToVector(
							value.at("shadowColor"), component.textShadowColor
						);
					}
					if (value.contains("shadowOffset")) {
						component.textShadowOffset = JsonToVector(
							value.at("shadowOffset"), component.textShadowOffset
						);
					}
					if (value.contains("viewportAnchor")) {
						component.textViewportAnchor = JsonToVector(
							value.at("viewportAnchor"), component.textViewportAnchor
						);
					}
					if (value.contains("pivot")) {
						component.textPivot = JsonToVector(
							value.at("pivot"), component.textPivot
						);
					}
					component.textSortingOrder = value.value(
						"sortingOrder", component.textSortingOrder
					);
					component.textClipEnabled = value.value(
						"clipEnabled", component.textClipEnabled
					);
					auto readPlacement = [&value](const char* key, Text2DPlacement& placement) {
						if (!value.contains(key) || !value.at(key).is_object()) {
							return false;
						}
						const auto& source = value.at(key);
						if (source.contains("position")) placement.position = JsonToVector(source.at("position"), placement.position);
						placement.rotation = source.value("rotation", placement.rotation);
						if (source.contains("scale")) placement.scale = JsonToVector(source.at("scale"), placement.scale);
						if (source.contains("pivot")) placement.pivot = JsonToVector(source.at("pivot"), placement.pivot);
						if (source.contains("viewportAnchor")) placement.viewportAnchor = JsonToVector(source.at("viewportAnchor"), placement.viewportAnchor);
						placement.sortingOrder = source.value("sortingOrder", placement.sortingOrder);
						placement.clipEnabled = source.value("clipEnabled", placement.clipEnabled);
						return true;
					};
					component.textHasPlacementProfiles =
						readPlacement("overlayPlacement", component.textOverlayPlacement) |
						readPlacement("scene2DPlacement", component.textScene2DPlacement);
				}
				if (component.type == "TextMotion") {
					component.textMotionClips.clear();
					const auto clips = value.find("clips");
					if (clips != value.end() && clips->is_array()) {
						for (const auto& sourceClip : *clips) {
							if (!sourceClip.is_object()) {
								continue;
							}
							SceneTextMotionClip clip{};
							clip.id = sourceClip.value("id", std::string{});
							clip.holdFinalPose = sourceClip.value(
								"holdFinalPose", false
							);
							const auto keyframes = sourceClip.find("keyframes");
							if (keyframes != sourceClip.end() && keyframes->is_array()) {
								for (const auto& sourceKeyframe : *keyframes) {
									if (!sourceKeyframe.is_object()) {
										continue;
									}
									SceneTextMotionKeyframe keyframe{};
									keyframe.timeSeconds = sourceKeyframe.value(
										"timeSeconds", keyframe.timeSeconds
									);
									if (sourceKeyframe.contains("positionOffset")) {
										keyframe.positionOffset = JsonToVector(
											sourceKeyframe.at("positionOffset"),
											keyframe.positionOffset
										);
									}
									keyframe.rotationOffset = sourceKeyframe.value(
										"rotationOffset", keyframe.rotationOffset
									);
									if (sourceKeyframe.contains("scaleMultiplier")) {
										keyframe.scaleMultiplier = JsonToVector(
											sourceKeyframe.at("scaleMultiplier"),
											keyframe.scaleMultiplier
										);
									}
									keyframe.opacityMultiplier = sourceKeyframe.value(
										"opacityMultiplier", keyframe.opacityMultiplier
									);
									keyframe.easingToNext = sourceKeyframe.value(
										"easingToNext", keyframe.easingToNext
									);
									clip.keyframes.push_back(std::move(keyframe));
								}
							}
							component.textMotionClips.push_back(std::move(clip));
						}
					}
				}
				if (component.type == "GameFlowDirector") {
					component.gameFlowAutoStart = value.value("autoStart", component.gameFlowAutoStart);
					component.gameFlowCountdownStart = value.value("countdownStart", component.gameFlowCountdownStart);
					component.gameFlowCountdownStepSeconds = value.value("countdownStepSeconds", component.gameFlowCountdownStepSeconds);
					component.gameFlowStartCueText = value.value("startCueText", component.gameFlowStartCueText);
					component.gameFlowStartCueSeconds = value.value("startCueSeconds", component.gameFlowStartCueSeconds);
					component.gameFlowInterPhaseDelaySeconds = value.value("interPhaseDelaySeconds", component.gameFlowInterPhaseDelaySeconds);
					component.gameFlowResultRevealDelaySeconds = value.value("resultRevealDelaySeconds", component.gameFlowResultRevealDelaySeconds);
					component.gameFlowTimerDisplayStepSeconds = value.value("timerDisplayStepSeconds", component.gameFlowTimerDisplayStepSeconds);
					component.gameFlowTimerPrefix = value.value("timerPrefix", component.gameFlowTimerPrefix);
					component.gameFlowResultPrefix = value.value("resultPrefix", component.gameFlowResultPrefix);
					component.gameFlowCountdownTextEntityId = value.value("countdownTextEntityId", uint64_t{});
					component.gameFlowCountdownMotionClipId = value.value("countdownMotionClipId", std::string{});
					component.gameFlowPhaseTextEntityId = value.value("phaseTextEntityId", uint64_t{});
					component.gameFlowPhaseMotionClipId = value.value("phaseMotionClipId", std::string{});
					component.gameFlowTimerTextEntityId = value.value("timerTextEntityId", uint64_t{});
					component.gameFlowRemainingTextEntityId = value.value("remainingTextEntityId", uint64_t{});
					component.gameFlowRemainingPrefix = value.value("remainingPrefix", component.gameFlowRemainingPrefix);
					component.gameFlowResultRootEntityId = value.value("resultRootEntityId", uint64_t{});
					component.gameFlowResultTimeTextEntityId = value.value("resultTimeTextEntityId", uint64_t{});
					component.gameFlowResultMotionClipId = value.value("resultMotionClipId", std::string{});
					component.gameFlowPhases.clear();
					if (const auto phases = value.find("phases"); phases != value.end() && phases->is_array()) {
						for (const json& sourcePhase : *phases) {
							if (!sourcePhase.is_object()) continue;
							SceneGameFlowPhase phase{};
							phase.id = sourcePhase.value("id", std::string{});
							phase.label = sourcePhase.value("label", std::string{});
							if (const auto waves = sourcePhase.find("waves"); waves != sourcePhase.end() && waves->is_array()) {
								for (const json& sourceWave : *waves) {
									if (!sourceWave.is_object()) continue;
									phase.waves.push_back({ sourceWave.value("spawnerEntityId", uint64_t{}), sourceWave.value("count", 1) });
								}
							}
							component.gameFlowPhases.push_back(std::move(phase));
						}
					}
				}
				if (component.type == "FishingScoreAttackDirector") {
					component.fishingPlayerEntityId = value.value(
						"playerEntityId", component.fishingPlayerEntityId
					);
					component.fishingFishEntityIds = value.value(
						"fishEntityIds", std::vector<uint64_t>{}
					);
					component.fishingHookSpawnAreaEntityId = value.value(
						"hookSpawnAreaEntityId", component.fishingHookSpawnAreaEntityId
					);
					component.fishingHookPoolEntityId = value.value(
						"hookPoolEntityId", component.fishingHookPoolEntityId
					);
					component.fishingWaterVolumeEntityId = value.value(
						"waterVolumeEntityId", component.fishingWaterVolumeEntityId
					);
					component.fishingDurationSeconds = (std::max)(
						value.value("durationSeconds", component.fishingDurationSeconds),
						0.001f
					);
					component.fishingMaxSelectableFishCount = std::clamp(
						value.value("maxSelectableFishCount", component.fishingMaxSelectableFishCount),
						1, (std::numeric_limits<int>::max)()
					);
					component.fishingConfirmInput = value.value(
						"confirmInput", component.fishingConfirmInput
					);
					if (const auto inputExpression = value.find(
						"confirmInputExpression"
					); inputExpression != value.end()) {
						component.fishingConfirmInputExpression =
							ReadSceneInputExpression(*inputExpression);
						component.fishingConfirmInput = FirstSceneInputExpressionTerm(
							component.fishingConfirmInputExpression
						);
					}
					component.fishingDistanceBandCount = (std::max)(
						value.value("distanceBandCount", component.fishingDistanceBandCount),
						1
					);
					component.fishingHooksPerDistanceBand = std::clamp(
						value.value("hooksPerDistanceBand", component.fishingHooksPerDistanceBand),
						1,
						4
					);
					component.fishingDistanceMultiplierBase = (std::max)(
						value.value("distanceMultiplierBase", component.fishingDistanceMultiplierBase),
						0.0f
					);
					component.fishingDistanceMultiplierStep = (std::max)(
						value.value("distanceMultiplierStep", component.fishingDistanceMultiplierStep),
						0.0f
					);
					component.fishingUseHookBandSettings = value.value(
						"useHookBandSettings", component.fishingUseHookBandSettings
					);
					component.fishingHookBands.clear();
					if (const auto hookBands = value.find("hookBands");
						hookBands != value.end() && hookBands->is_array()) {
						for (const json& sourceBand : *hookBands) {
							if (!sourceBand.is_object()) {
								continue;
							}
							SceneFishingHookBandSettings band{};
							band.distanceMultiplier = sourceBand.value(
								"distanceMultiplier", band.distanceMultiplier
							);
							band.hookCount = sourceBand.value(
								"hookCount", band.hookCount
							);
							if (const auto weights = sourceBand.find("hookMultiplierWeights");
								weights != sourceBand.end() && weights->is_array()) {
								for (const json& weight : *weights) {
									if (weight.is_number()) {
										band.hookMultiplierWeights.push_back(weight.get<float>());
									}
								}
							}
							component.fishingHookBands.push_back(std::move(band));
						}
					}
					component.fishingHookScoreUnit = value.value(
						"hookScoreUnit", component.fishingHookScoreUnit
					);
					component.fishingFishMultiplierBase = value.value(
						"fishMultiplierBase", component.fishingFishMultiplierBase
					);
					component.fishingFishMultiplierPerAdditionalFish = value.value(
						"fishMultiplierPerAdditionalFish",
						component.fishingFishMultiplierPerAdditionalFish
					);
					component.fishingHookTierScoreMultipliers = {
						1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
						6.0f, 7.0f, 8.0f, 9.0f, 10.0f
					};
					if (const auto tierScoreMultipliers = value.find("hookTierScoreMultipliers");
						tierScoreMultipliers != value.end() && tierScoreMultipliers->is_array()) {
						component.fishingHookTierScoreMultipliers.clear();
						for (const json& multiplier : *tierScoreMultipliers) {
							if (multiplier.is_number()) {
								component.fishingHookTierScoreMultipliers.push_back(multiplier.get<float>());
							}
						}
					}
					if (const auto colors = value.find("hookMultiplierColors");
						colors != value.end() && colors->is_array()) {
						component.fishingHookMultiplierColors.clear();
						for (const json& color : *colors) {
							component.fishingHookMultiplierColors.push_back(
								JsonToVector(color, Vector4{ 1.0f, 1.0f, 1.0f, 1.0f })
							);
						}
					}
					component.fishingHookRanks.clear();
					if (const auto ranks = value.find("hookRanks");
						ranks != value.end() && ranks->is_array()) {
						std::vector<SceneFishingHookRankDefinition> parsedRanks;
						for (const json& rankValue : *ranks) {
							if (!rankValue.is_object()) {
								continue;
							}
							SceneFishingHookRankDefinition rank{};
							rank.id = rankValue.value("id", std::string{});
							rank.displayName = rankValue.value(
								"displayName", std::string{}
							);
							 rank.modelPath = rankValue.value(
								"modelPath", std::string{}
							);
							rank.iconTexturePath = rankValue.value(
								"iconTexturePath", std::string{}
							);
							rank.scoreMultiplier = rankValue.value(
								"scoreMultiplier", rank.scoreMultiplier
							);
							if (const auto color = rankValue.find("color");
								color != rankValue.end()) {
								rank.color = JsonToVector(*color, rank.color);
							}
							parsedRanks.push_back(std::move(rank));
						}
						if (parsedRanks.size() == 10) {
							component.fishingHookRanks = std::move(parsedRanks);
						}
					}
					if (component.fishingHookRanks.empty()) {
						component.fishingHookRanks = BuildLegacyFishingHookRanks(
							component.fishingHookTierScoreMultipliers,
							component.fishingHookMultiplierColors
						);
					}
					component.fishingHookColorEmissiveIntensity = value.value(
						"hookColorEmissiveIntensity",
						component.fishingHookColorEmissiveIntensity
					);
					component.fishingHookLegendVisible = value.value(
						"hookLegendVisible", component.fishingHookLegendVisible
					);
					component.fishingHookLegendTitleTextEntityId = value.value(
						"hookLegendTitleTextEntityId",
						component.fishingHookLegendTitleTextEntityId
					);
					component.fishingHookLegendTextEntityIds = value.value(
						"hookLegendTextEntityIds", std::vector<uint64_t>{}
					);
					component.fishingHookLegendTitle = value.value(
						"hookLegendTitle", component.fishingHookLegendTitle
					);
					component.fishingHookLegendPrefix = value.value(
						"hookLegendPrefix", component.fishingHookLegendPrefix
					);
					component.fishingHookLegendIconEntityIds = value.value(
						"hookLegendIconEntityIds", std::vector<uint64_t>(10, 0)
					);
					if (value.contains("hookLegendIconSize")) {
						component.fishingHookLegendIconSize = JsonToVector(
							value.at("hookLegendIconSize"),
							component.fishingHookLegendIconSize
						);
					}
					component.fishingRandomizeSeedOnPlay = value.value(
						"randomizeSeedOnPlay", component.fishingRandomizeSeedOnPlay
					);
					component.fishingRandomSeed = value.value(
						"randomSeed", component.fishingRandomSeed
					);
					component.fishingFishCountTextEntityId = value.value(
						"fishCountTextEntityId", component.fishingFishCountTextEntityId
					);
					component.fishingTimerTextEntityId = value.value(
						"timerTextEntityId", component.fishingTimerTextEntityId
					);
					component.fishingScoreTextEntityId = value.value(
						"scoreTextEntityId", component.fishingScoreTextEntityId
					);
					component.fishingMultiplierTextEntityId = value.value(
						"multiplierTextEntityId", component.fishingMultiplierTextEntityId
					);
					component.fishingResultTextEntityId = value.value(
						"resultTextEntityId", component.fishingResultTextEntityId
					);
					component.fishingFishCountPrefix = value.value(
						"fishCountPrefix", component.fishingFishCountPrefix
					);
					component.fishingTimerPrefix = value.value(
						"timerPrefix", component.fishingTimerPrefix
					);
					component.fishingScorePrefix = value.value(
						"scorePrefix", component.fishingScorePrefix
					);
					component.fishingMultiplierPrefix = value.value(
						"multiplierPrefix", component.fishingMultiplierPrefix
					);
					component.fishingResultPrefix = value.value(
						"resultPrefix", component.fishingResultPrefix
					);
					component.fishingUseFormationCapsuleCollision = value.value(
						"useFormationCapsuleCollision",
						component.fishingUseFormationCapsuleCollision
					);
					component.fishingFormationOutlineVisible = value.value(
						"formationOutlineVisible",
						component.fishingFormationOutlineVisible
					);
					if (value.contains("formationOutlineColor")) {
						component.fishingFormationOutlineColor = JsonToVector(
							value.at("formationOutlineColor"),
							component.fishingFormationOutlineColor
						);
					}
					const float outlineBloomIntensity = value.value(
						"formationOutlineBloomIntensity",
						component.fishingFormationOutlineBloomIntensity
					);
					component.fishingFormationOutlineBloomIntensity =
						std::isfinite(outlineBloomIntensity)
							? std::clamp(outlineBloomIntensity, 0.0f, 32.0f)
							: 1.0f;
					component.fishingFormationOutlineYOffset = value.value(
						"formationOutlineYOffset",
						component.fishingFormationOutlineYOffset
					);
					component.fishingFormationOutlineSegments = std::clamp(
						value.value(
							"formationOutlineSegments",
							component.fishingFormationOutlineSegments
						),
						12,
						128
					);
				} else if (component.type == "FishingHookSpawnArea") {
					component.fishingSpawnHalfSizeX = (std::max)(
						value.value("halfSizeX", component.fishingSpawnHalfSizeX),
						0.001f
					);
					component.fishingSpawnHalfSizeZ = (std::max)(
						value.value("halfSizeZ", component.fishingSpawnHalfSizeZ),
						0.001f
					);
					component.fishingSpawnMinimumDistance = (std::max)(
						value.value("minimumDistance", component.fishingSpawnMinimumDistance),
						0.0f
					);
					component.fishingSpawnMaxAttempts = (std::max)(
						value.value("maxSpawnAttempts", component.fishingSpawnMaxAttempts),
						1
					);
				} else if (component.type == "FishingHookPool") {
					component.fishingHookPoolEntries.clear();
					const auto entries = value.find("entries");
					if (entries != value.end() && entries->is_array()) {
						for (const json& sourceEntry : *entries) {
							if (!sourceEntry.is_object()) {
								continue;
							}
							SceneFishingHookPoolEntry entry{};
							entry.hookEntityId = sourceEntry.value(
								"hookEntityId", entry.hookEntityId
							);
							const auto weights = sourceEntry.find(
								"weightsByDistanceBand"
							);
							if (weights != sourceEntry.end() && weights->is_array()) {
								for (const json& weight : *weights) {
									if (weight.is_number()) {
										entry.weightsByDistanceBand.push_back((std::max)(
											weight.get<float>(), 0.0f
										));
									}
								}
							}
							component.fishingHookPoolEntries.push_back(std::move(entry));
						}
					}
				} else if (component.type == "FishingHook") {
					component.fishingHookBaseScore = (std::max)(
						value.value("baseScore", component.fishingHookBaseScore),
						0
					);
				} else if (component.type == "FishingShark") {
					component.fishingSharkRadiusX = (std::max)(
						value.value("radiusX", component.fishingSharkRadiusX),
						0.001f
					);
					component.fishingSharkRadiusZ = (std::max)(
						value.value("radiusZ", component.fishingSharkRadiusZ),
						0.001f
					);
					component.fishingSharkAngularSpeed = value.value(
						"angularSpeed", component.fishingSharkAngularSpeed
					);
					component.fishingSharkInitialPhase = value.value(
						"initialPhase", component.fishingSharkInitialPhase
					);
					component.fishingSharkPenaltyScore = (std::max)(
						value.value("penaltyScore", component.fishingSharkPenaltyScore),
						0
					);
					component.fishingSharkHitCooldownSeconds = (std::max)(
						value.value(
							"hitCooldownSeconds",
							component.fishingSharkHitCooldownSeconds
						),
						0.0f
					);
					component.fishingSharkPathRandomness = (std::clamp)(
						value.value("pathRandomness", component.fishingSharkPathRandomness),
						0.0f,
						1.0f
					);
					component.fishingSharkWanderMoveSpeed = (std::max)(
						value.value(
							"wanderMoveSpeed",
							component.fishingSharkWanderMoveSpeed
						),
						0.0f
					);
					component.fishingSharkWanderMaximumTurnRate = (std::max)(
						value.value(
							"wanderMaximumTurnRate",
							component.fishingSharkWanderMaximumTurnRate
						),
						0.0f
					);
					component.fishingSharkObstacleAvoidanceDistance = (std::max)(
						value.value(
							"obstacleAvoidanceDistance",
							component.fishingSharkObstacleAvoidanceDistance
						),
						0.0f
					);
					component.fishingSharkObstacleAvoidanceStrength = (std::clamp)(
						value.value(
							"obstacleAvoidanceStrength",
							component.fishingSharkObstacleAvoidanceStrength
						),
						0.0f,
						1.0f
					);
					component.fishingSharkObstacleAvoidanceResponse = (std::max)(
						value.value(
							"obstacleAvoidanceResponse",
							component.fishingSharkObstacleAvoidanceResponse
						),
						0.0f
					);
				}
				component.cameraIsMain = value.value("isMain", false);
				component.cameraFovY = value.value("fovY", component.cameraFovY);
				component.cameraNearClip = value.value(
					"nearClip",
					component.cameraNearClip
				);
				component.cameraFarClip = value.value(
					"farClip",
					component.cameraFarClip
				);
				component.cameraInvertYaw = value.value(
					"invertYaw",
					component.cameraInvertYaw
				);
				component.cameraInvertPitch = value.value(
					"invertPitch",
					component.cameraInvertPitch
				);
				if (component.type == "Light") {
					component.lightType = value.value(
						"lightType",
						component.lightType
					);
					if (value.contains("color")) {
						component.lightColor = JsonToVector(
							value.at("color"),
							component.lightColor
						);
					}
					component.lightColor.x = std::clamp(
						component.lightColor.x,
						0.0f,
						1.0f
					);
					component.lightColor.y = std::clamp(
						component.lightColor.y,
						0.0f,
						1.0f
					);
					component.lightColor.z = std::clamp(
						component.lightColor.z,
						0.0f,
						1.0f
					);
					component.lightColor.w = std::clamp(
						component.lightColor.w,
						0.0f,
						1.0f
					);
					component.lightIntensity = (std::max)(
						value.value("intensity", component.lightIntensity),
						0.0f
					);
					component.lightRange = (std::max)(
						value.value("range", component.lightRange),
						0.1f
					);
					component.lightDecay = (std::max)(
						value.value("decay", component.lightDecay),
						0.0f
					);
					component.lightSpotOuterAngle = std::clamp(
						value.value("outerAngle", component.lightSpotOuterAngle),
						1.0f,
						89.0f
					);
					component.lightSpotInnerAngle = std::clamp(
						value.value("innerAngle", component.lightSpotInnerAngle),
						0.0f,
						component.lightSpotOuterAngle
					);
					component.lightCastsShadow = value.value(
						"castsShadow",
						component.lightCastsShadow
					);
					if (value.contains("shadow") && value.at("shadow").is_object()) {
						const json& shadow = value.at("shadow");
						component.lightShadowBias = (std::max)(
							shadow.value("bias", component.lightShadowBias),
							0.0f
						);
						component.lightShadowNormalBias = (std::max)(
							shadow.value("normalBias", component.lightShadowNormalBias),
							0.0f
						);
						component.lightShadowStrength = std::clamp(
							shadow.value("strength", component.lightShadowStrength),
							0.0f,
							1.0f
						);
						component.lightShadowDistance = (std::max)(
							shadow.value("distance", component.lightShadowDistance),
							1.0f
						);
						component.lightShadowOrthographicSize = (std::max)(
							shadow.value(
								"orthographicSize",
								component.lightShadowOrthographicSize
							),
							1.0f
						);
						component.lightShadowNearClip = (std::max)(
							shadow.value("nearClip", component.lightShadowNearClip),
							0.001f
						);
						component.lightShadowFarClip = (std::max)(
							shadow.value("farClip", component.lightShadowFarClip),
							component.lightShadowNearClip + 0.001f
						);
						component.lightShadowTexelSnap = shadow.value(
							"texelSnap",
							component.lightShadowTexelSnap
						);
					}
					if (
						component.lightType != "Directional" &&
						component.lightType != "Point" &&
						component.lightType != "Spot"
					) {
						component.lightType = "Point";
					}
					if (component.lightType == "Point") {
						component.lightCastsShadow = false;
					}
				}
				component.monitorCameraName = value.value(
					"cameraName",
					component.monitorCameraName
				);
				component.monitorCameraEntityId = value.value(
					"cameraEntityId",
					component.monitorCameraEntityId
				);
				component.monitorResolutionPreset = value.value(
					"resolutionPreset",
					component.monitorResolutionPreset
				);
				component.monitorWidth = value.value(
					"width",
					component.monitorWidth
				);
				component.monitorHeight = value.value(
					"height",
					component.monitorHeight
				);
				component.monitorHideSelf = value.value(
					"hideSelf",
					component.monitorHideSelf
				);
				if (component.type == "CameraSwitcher") {
					component.cameraSwitchTriggerKey = value.value(
						"triggerKey",
						component.cameraSwitchTriggerKey
					);
					component.cameraSwitchWrap = value.value(
						"wrap",
						component.cameraSwitchWrap
					);
					if (const auto cameras = value.find("cameras");
						cameras != value.end() && cameras->is_array()) {
						for (const json& camera : *cameras) {
							if (!camera.is_object()) {
								continue;
							}
							SceneCameraSwitchEntry entry{};
							entry.cameraEntityId = camera.value(
								"entityId", entry.cameraEntityId
							);
							entry.cameraEntityName = camera.value(
								"entityName", entry.cameraEntityName
							);
							component.cameraSwitchEntries.push_back(
								std::move(entry)
							);
						}
					}
				}
				component.thirdPersonTargetEntityId = value.value(
					"targetEntityId",
					component.thirdPersonTargetEntityId
				);
				component.thirdPersonTargetEntityName = value.value(
					"targetEntityName",
					component.thirdPersonTargetEntityName
				);
				component.thirdPersonDistance = value.value(
					"distance",
					component.thirdPersonDistance
				);
				component.thirdPersonAimDistance = value.value(
					"aimDistance",
					component.thirdPersonAimDistance
				);
				if (value.contains("targetOffset")) {
					component.thirdPersonTargetOffset = JsonToVector(
						value.at("targetOffset"),
						component.thirdPersonTargetOffset
					);
				}
				if (value.contains("aimTargetOffset")) {
					component.thirdPersonAimTargetOffset = JsonToVector(
						value.at("aimTargetOffset"),
						component.thirdPersonAimTargetOffset
					);
				}
				component.thirdPersonMouseSensitivity = value.value(
					"mouseSensitivity",
					component.thirdPersonMouseSensitivity
				);
				component.thirdPersonMinPitch = value.value(
					"minPitch",
					component.thirdPersonMinPitch
				);
				component.thirdPersonMaxPitch = value.value(
					"maxPitch",
					component.thirdPersonMaxPitch
				);
				component.thirdPersonOcclusionMargin = value.value(
					"occlusionMargin",
					component.thirdPersonOcclusionMargin
				);
				component.thirdPersonOcclusionMask = value.value(
					"occlusionMask",
					component.thirdPersonOcclusionMask
				);
				component.thirdPersonOcclusionPullInSmoothTime = (std::max)(
					value.value(
						"occlusionPullInSmoothTime",
						component.thirdPersonOcclusionPullInSmoothTime
					),
					0.0f
				);
				component.thirdPersonOcclusionRecoverySmoothTime = (std::max)(
					value.value(
						"occlusionRecoverySmoothTime",
						component.thirdPersonOcclusionRecoverySmoothTime
					),
					0.0f
				);
				component.thirdPersonPositionSmoothTime = (std::max)(
					value.value(
						"positionSmoothTime",
						component.thirdPersonPositionSmoothTime
					),
					0.0f
				);
				component.thirdPersonRotationSmoothTime = (std::max)(
					value.value(
						"rotationSmoothTime",
						component.thirdPersonRotationSmoothTime
					),
					0.0f
				);
				component.thirdPersonYawReference = value.value(
					"yawReference",
					component.thirdPersonYawReference
				);
				component.thirdPersonAllowMouseInput = value.value(
					"allowMouseInput",
					component.thirdPersonAllowMouseInput
				);
				component.thirdPersonOcclusionEnabled = value.value(
					"occlusionEnabled",
					component.thirdPersonOcclusionEnabled
				);
				component.thirdPersonAimModeEnabled = value.value(
					"aimModeEnabled",
					component.thirdPersonAimModeEnabled
				);
				component.thirdPersonInvertYaw = value.value(
					"invertYaw",
					component.thirdPersonInvertYaw
				);
				component.thirdPersonInvertPitch = value.value(
					"invertPitch",
					component.thirdPersonInvertPitch
				);
				if (component.type == "Animator") {
					component.animatorPlayOnStart = value.value(
						"playOnStart",
						component.animatorPlayOnStart
					);
					component.animatorLoop = value.value(
						"loop",
						component.animatorLoop
					);
					component.animatorSpeed = value.value(
						"speed",
						component.animatorSpeed
					);
					component.animatorDefaultClip = (std::max)(
						value.value(
							"defaultClip",
							component.animatorDefaultClip
						),
						0
					);
					component.animatorTransitionDuration = (std::max)(
						value.value(
							"transitionDuration",
							component.animatorTransitionDuration
						),
						0.0f
					);
					component.animatorBlendCurve = value.value(
						"blendCurve",
						component.animatorBlendCurve
					);
					if (component.animatorBlendCurve != "Linear") {
						component.animatorBlendCurve = "SmoothStep";
					}
				}
				component.physicsBodyType = value.value(
					"bodyType",
					component.physicsBodyType
				);
				component.physicsMass = value.value(
					"mass",
					component.physicsMass
				);
				component.physicsUseGravity = value.value(
					"useGravity",
					component.physicsUseGravity
				);
				component.physicsGravityScale = value.value(
					"gravityScale",
					component.physicsGravityScale
				);
				component.physicsDrag = value.value(
					"drag",
					component.physicsDrag
				);
				component.physicsRestitution = value.value(
					"restitution",
					component.physicsRestitution
				);
				component.physicsFriction = value.value(
					"friction",
					component.physicsFriction
				);
				component.physicsMaxFallSpeed = value.value(
					"maxFallSpeed",
					component.physicsMaxFallSpeed
				);
				if (value.contains("velocity")) {
					component.physicsVelocity = JsonToVector(
						value.at("velocity"),
						component.physicsVelocity
					);
				}
				component.physicsFreezePositionX = value.value(
					"freezePositionX",
					component.physicsFreezePositionX
				);
				component.physicsFreezePositionY = value.value(
					"freezePositionY",
					component.physicsFreezePositionY
				);
				component.physicsFreezePositionZ = value.value(
					"freezePositionZ",
					component.physicsFreezePositionZ
				);
				if (component.type == "OBBCollider") {
					if (value.contains("offset")) {
						component.colliderOffset = JsonToVector(
							value.at("offset"),
							component.colliderOffset
						);
					}
					if (value.contains("sizeMultiplier")) {
						component.colliderSizeMultiplier = JsonToVector(
							value.at("sizeMultiplier"),
							component.colliderSizeMultiplier
						);
					}
					if (value.contains("debugColor")) {
						component.colliderDebugColor = JsonToVector(
							value.at("debugColor"),
							component.colliderDebugColor
						);
					}
					component.colliderShape = value.value(
						"shape",
						component.colliderShape
					);
					component.colliderSphereRadius = value.value(
						"sphereRadius",
						component.colliderSphereRadius
					);
					component.colliderDebugVisible = value.value(
						"debugVisible",
						component.colliderDebugVisible
					);
					component.colliderDebugDrawMode = value.value(
						"debugDrawMode",
						component.colliderDebugDrawMode
					);
					component.colliderDebugSegments = value.value(
						"debugSegments",
						component.colliderDebugSegments
					);
					component.colliderIsTrigger = value.value(
						"isTrigger", component.colliderIsTrigger
					);
					component.colliderActive = value.value(
						"active", component.colliderActive
					);
					component.colliderLayer = value.value(
						"layer", component.colliderLayer
					);
					component.colliderMask = value.value(
						"mask", component.colliderMask
					);
					if (component.colliderShape != "Sphere") {
						component.colliderShape = "Box";
					}
					if (
						component.colliderDebugDrawMode != "Solid" &&
						component.colliderDebugDrawMode != "WireframeAndSolid"
					) {
						component.colliderDebugDrawMode = "Wireframe";
					}
					component.colliderSphereRadius = (std::max)(
						component.colliderSphereRadius,
						0.001f
					);
					component.colliderDebugSegments = std::clamp(
						component.colliderDebugSegments,
						4,
						64
					);
				}
				component.playerMoveSpeed = value.value(
					"moveSpeed",
					component.playerMoveSpeed
				);
				component.playerJumpVelocity = value.value(
					"jumpVelocity",
					component.playerJumpVelocity
				);
				component.playerTurnResponsiveness = value.value(
					"turnResponsiveness",
					component.playerTurnResponsiveness
				);
				component.playerDashMultiplier = value.value(
					"dashMultiplier",
					component.playerDashMultiplier
				);
				component.playerCameraRelativeMove = value.value(
					"cameraRelativeMove",
					component.playerCameraRelativeMove
				);
				component.playerAllowJump = value.value(
					"allowJump",
					component.playerAllowJump
				);
				component.playerAutoForward = value.value(
					"autoForward",
					component.playerAutoForward
				);
				component.playerInputMode = value.value(
					"inputMode",
					component.playerInputMode
				);
				component.playerGamepadDeadzone = value.value(
					"gamepadDeadzone",
					component.playerGamepadDeadzone
				);
				if (!std::isfinite(component.playerGamepadDeadzone)) {
					component.playerGamepadDeadzone = 0.20f;
				} else {
					component.playerGamepadDeadzone = std::clamp(
						component.playerGamepadDeadzone,
						0.0f,
						0.95f
					);
				}
				component.agentBehaviorName = value.value(
					"behaviorName",
					component.agentBehaviorName
				);
				component.agentMovementMode = value.value(
					"movementMode",
					component.agentMovementMode
				);
				component.agentProfileName = value.value(
					"profileName",
					component.agentProfileName
				);
				component.agentGroupName = value.value(
					"groupName",
					component.agentGroupName
				);
				component.agentBoundsEntityId = value.value(
					"boundsEntityId",
					component.agentBoundsEntityId
				);
				component.agentBoundsName = value.value(
					"boundsName",
					component.agentBoundsName
				);
				component.agentAttractorEntityId = value.value(
					"attractorEntityId",
					component.agentAttractorEntityId
				);
				component.agentAttractorTag = value.value(
					"attractorTag",
					component.agentAttractorTag
				);
				component.agentUseWaterBounds = value.value(
					"useWaterBounds",
					component.agentUseWaterBounds
				);
				component.agentMinSpeed = value.value(
					"minSpeed",
					component.agentMinSpeed
				);
				component.agentMaxSpeed = value.value(
					"maxSpeed",
					component.agentMaxSpeed
				);
				component.agentTurnSpeed = value.value(
					"turnSpeed",
					component.agentTurnSpeed
				);
				component.agentWanderStrength = value.value(
					"wanderStrength",
					component.agentWanderStrength
				);
				component.agentWanderChangeInterval = value.value(
					"wanderChangeInterval",
					component.agentWanderChangeInterval
				);
				component.agentWanderDirectionRange = value.value(
					"wanderDirectionRange",
					component.agentWanderDirectionRange
				);
				component.agentWanderVerticalRange = value.value(
					"wanderVerticalRange",
					component.agentWanderVerticalRange
				);
				component.agentRandomizeSeedOnPlay = value.value(
					"randomizeSeedOnPlay",
					component.agentRandomizeSeedOnPlay
				);
				component.agentRandomSeed = value.value(
					"randomSeed",
					component.agentRandomSeed
				);
				component.agentFlockDecisionInterval = value.value(
					"flockDecisionInterval",
					component.agentFlockDecisionInterval
				);
				component.agentFlockAcceleration = value.value(
					"flockAcceleration",
					component.agentFlockAcceleration
				);
				component.agentFlockTurnRate = value.value(
					"flockTurnRate",
					component.agentFlockTurnRate
				);
				component.agentMemberCenterFollow = value.value(
					"memberCenterFollow",
					component.agentMemberCenterFollow
				);
				component.agentMemberJitterStrength = value.value(
					"memberJitterStrength",
					component.agentMemberJitterStrength
				);
				component.agentMemberJitterFrequency = value.value(
					"memberJitterFrequency",
					component.agentMemberJitterFrequency
				);
				component.agentMemberJitterUpdateInterval = value.value(
					"memberJitterUpdateInterval",
					component.agentMemberJitterUpdateInterval
				);
				component.agentMemberJitterFollowSpeed = value.value(
					"memberJitterFollowSpeed",
					component.agentMemberJitterFollowSpeed
				);
				component.agentMemberSpeedVariation = value.value(
					"memberSpeedVariation",
					component.agentMemberSpeedVariation
				);
				component.agentMemberLeashDistance = value.value(
					"memberLeashDistance",
					component.agentMemberLeashDistance
				);
				component.agentMemberLeashStrength = value.value(
					"memberLeashStrength",
					component.agentMemberLeashStrength
				);
				component.agentMemberCatchupSpeed = value.value(
					"memberCatchupSpeed",
					component.agentMemberCatchupSpeed
				);
				component.agentMemberSeparationUpdateInterval = value.value(
					"memberSeparationUpdateInterval",
					component.agentMemberSeparationUpdateInterval
				);
				component.agentMemberSeparationBlend = value.value(
					"memberSeparationBlend",
					component.agentMemberSeparationBlend
				);
				component.agentMemberMinimumDistance = value.value(
					"memberMinimumDistance",
					component.agentMemberMinimumDistance
				);
				component.agentBoundsWeight = value.value(
					"boundsWeight",
					component.agentBoundsWeight
				);
				component.agentUseTeamHeading = value.value(
					"useTeamHeading",
					component.agentUseTeamHeading
				);
				component.agentTeamHeadingFromAverage = value.value(
					"teamHeadingFromAverage",
					component.agentTeamHeadingFromAverage
				);
				if (value.contains("teamHeadingDirection")) {
					component.agentTeamHeadingDirection = JsonToVector(
						value.at("teamHeadingDirection"),
						component.agentTeamHeadingDirection
					);
				}
				component.agentTeamHeadingWeight = value.value(
					"teamHeadingWeight",
					component.agentTeamHeadingWeight
				);
				component.agentTeamHeadingFollowSpeed = value.value(
					"teamHeadingFollowSpeed",
					component.agentTeamHeadingFollowSpeed
				);
				component.agentUseTeamRotation = value.value(
					"useTeamRotation",
					component.agentUseTeamRotation
				);
				component.agentTeamRotationWeight = value.value(
					"teamRotationWeight",
					component.agentTeamRotationWeight
				);
				component.agentTeamRotationFollowSpeed = value.value(
					"teamRotationFollowSpeed",
					component.agentTeamRotationFollowSpeed
				);
				component.agentAlignForwardToVelocity = value.value(
					"alignForwardToVelocity",
					component.agentAlignForwardToVelocity
				);
				component.agentForwardAxis = value.value(
					"forwardAxis",
					component.agentForwardAxis
				);
				component.agentRotateAxisX = value.value(
					"rotateAxisX",
					component.agentRotateAxisX
				);
				component.agentRotateAxisY = value.value(
					"rotateAxisY",
					component.agentRotateAxisY
				);
				component.agentRotateAxisZ = value.value(
					"rotateAxisZ",
					component.agentRotateAxisZ
				);
				component.agentRotationFollowSpeed = value.value(
					"rotationFollowSpeed",
					component.agentRotationFollowSpeed
				);
				component.agentPitchFromVerticalVelocity = value.value(
					"pitchFromVerticalVelocity",
					component.agentPitchFromVerticalVelocity
				);
				component.agentBankingStrength = value.value(
					"bankingStrength",
					component.agentBankingStrength
				);
				component.agentSchooling = value.value(
					"schooling",
					component.agentSchooling
				);
				component.agentSchoolingUpdateInterval = value.value(
					"schoolingUpdateInterval",
					component.agentSchoolingUpdateInterval
				);
				component.agentSchoolingUpdateJitter = value.value(
					"schoolingUpdateJitter",
					component.agentSchoolingUpdateJitter
				);
				component.agentNeighborLimit = value.value(
					"neighborLimit",
					component.agentNeighborLimit
				);
				component.agentSchoolingBlend = value.value(
					"schoolingBlend",
					component.agentSchoolingBlend
				);
				component.agentSeparationRadius = value.value(
					"separationRadius",
					component.agentSeparationRadius
				);
				component.agentAlignmentRadius = value.value(
					"alignmentRadius",
					component.agentAlignmentRadius
				);
				component.agentCohesionRadius = value.value(
					"cohesionRadius",
					component.agentCohesionRadius
				);
				component.agentSeparationWeight = value.value(
					"separationWeight",
					component.agentSeparationWeight
				);
				component.agentAlignmentWeight = value.value(
					"alignmentWeight",
					component.agentAlignmentWeight
				);
				component.agentCohesionWeight = value.value(
					"cohesionWeight",
					component.agentCohesionWeight
				);
				component.agentAttractorWeight = value.value(
					"attractorWeight",
					component.agentAttractorWeight
				);
				component.agentTeamSettingsOverride = value.value(
					"teamSettingsOverride",
					component.agentTeamSettingsOverride
				);
				if (value.contains("visualColor")) {
					component.agentVisualColor = JsonToVector(
						value.at("visualColor"),
						component.agentVisualColor
					);
					component.attractorVisualColor = JsonToVector(
						value.at("visualColor"),
						component.attractorVisualColor
					);
				}
				component.agentEnableLighting = value.value(
					"enableLighting",
					component.agentEnableLighting
				);
				component.attractorTag = value.value(
					"tag",
					component.attractorTag
				);
				component.attractorTargetBehaviorName = value.value(
					"targetBehaviorName",
					component.attractorTargetBehaviorName
				);
				component.attractorTargetProfileName = value.value(
					"targetProfileName",
					component.attractorTargetProfileName
				);
				component.attractorRadius = value.value(
					"radius",
					component.attractorRadius
				);
				component.attractorStrength = value.value(
					"strength",
					component.attractorStrength
				);
				if (value.contains("halfSize")) {
					component.waterHalfSize = JsonToVector(
						value.at("halfSize"),
						component.waterHalfSize
					);
				}
				if (value.contains("offset")) {
					component.waterOffset = JsonToVector(
						value.at("offset"),
						component.waterOffset
					);
				}
				component.waterSurfaceEnabled = value.value(
					"surfaceEnabled",
					component.waterSurfaceEnabled
				);
				if (value.contains("surfaceBaseColor")) {
					component.waterSurfaceBaseColor = JsonToVector(
						value.at("surfaceBaseColor"),
						component.waterSurfaceBaseColor
					);
				}
				if (value.contains("surfaceHighlightColor")) {
					component.waterSurfaceHighlightColor = JsonToVector(
						value.at("surfaceHighlightColor"),
						component.waterSurfaceHighlightColor
					);
				}
				component.waterSurfaceAlpha = value.value(
					"surfaceAlpha",
					component.waterSurfaceAlpha
				);
				component.waterSurfaceWaveScale = value.value(
					"surfaceWaveScale",
					component.waterSurfaceWaveScale
				);
				component.waterSurfaceNormalStrength = value.value(
					"surfaceNormalStrength",
					component.waterSurfaceNormalStrength
				);
				component.waterSurfaceFresnelPower = value.value(
					"surfaceFresnelPower",
					component.waterSurfaceFresnelPower
				);
				component.waterLightShaftEnabled = value.value(
					"lightShaftEnabled",
					component.waterLightShaftEnabled
				);
				if (value.contains("lightColor")) {
					component.waterLightColor = JsonToVector(
						value.at("lightColor"),
						component.waterLightColor
					);
				}
				if (value.contains("lightDirection")) {
					component.waterLightDirection = JsonToVector(
						value.at("lightDirection"),
						component.waterLightDirection
					);
				}
				component.waterLightIntensity = value.value(
					"lightIntensity",
					component.waterLightIntensity
				);
				component.waterLightDensity = value.value(
					"lightDensity",
					component.waterLightDensity
				);
				component.waterLightCausticsIntensity = value.value(
					"causticsIntensity",
					component.waterLightCausticsIntensity
				);
				component.waterLightCausticsScale = value.value(
					"causticsScale",
					component.waterLightCausticsScale
				);
				component.waterLightCausticsSpeed = value.value(
					"causticsSpeed",
					component.waterLightCausticsSpeed
				);
				component.waterLightBreakupStrength = value.value(
					"breakupStrength",
					component.waterLightBreakupStrength
				);
				component.waterLightWarpStrength = value.value(
					"warpStrength",
					component.waterLightWarpStrength
				);
				component.waterLightNoiseScale = value.value(
					"noiseScale",
					component.waterLightNoiseScale
				);
				component.waterLightSampleCount = value.value(
					"lightSampleCount",
					component.waterLightSampleCount
				);
				component.waterMoveSpeedMultiplier = value.value(
					"moveSpeedMultiplier",
					component.waterMoveSpeedMultiplier
				);
				component.waterGravityScale = value.value(
					"gravityScale",
					component.waterGravityScale
				);
				component.waterDrag = value.value(
					"drag",
					component.waterDrag
				);
				component.waterMaxFallSpeed = value.value(
					"maxFallSpeed",
					component.waterMaxFallSpeed
				);
				component.waterSwimUpSpeed = value.value(
					"swimUpSpeed",
					component.waterSwimUpSpeed
				);
				component.entityReferenceName = value.value(
					"referenceName",
					component.entityReferenceName
				);
				if (
					const auto target = value.find("target");
					target != value.end() && target->is_object()
				) {
					component.entityReferenceTarget.sceneId = target->value(
						"sceneId",
						component.entityReferenceTarget.sceneId
					);
					component.entityReferenceTarget.instanceKey = target->value(
						"instanceKey",
						component.entityReferenceTarget.instanceKey
					);
					component.entityReferenceTarget.entityId = target->value(
						"entityId",
						component.entityReferenceTarget.entityId
					);
				}
				component.sceneTransitionTargetSceneId = value.value(
					"targetSceneId",
					component.sceneTransitionTargetSceneId
				);
				component.sceneTransitionTriggerType = value.value(
					"triggerType",
					component.sceneTransitionTriggerType
				);
				component.sceneTransitionTriggerKey = value.value(
					"triggerKey",
					component.sceneTransitionTriggerKey
				);
				component.cameraPathTargetCameraName = value.value(
					"targetCameraName",
					component.cameraPathTargetCameraName
				);
				component.cameraPathTriggerType = value.value(
					"triggerType",
					component.cameraPathTriggerType
				);
				component.cameraPathTriggerKey = value.value(
					"triggerKey",
					component.cameraPathTriggerKey
				);
				component.cameraPathEnterDuration = value.value(
					"enterDuration",
					component.cameraPathEnterDuration
				);
				component.cameraPathExitDuration = value.value(
					"exitDuration",
					component.cameraPathExitDuration
				);
				component.cameraPathInterpolation = value.value(
					"interpolation",
					component.cameraPathInterpolation
				);
				component.cameraPathDefaultEasing = value.value(
					"defaultEasing",
					component.cameraPathDefaultEasing
				);
				component.cameraPathReturnToPreviousCamera = value.value(
					"returnToPreviousCamera",
					component.cameraPathReturnToPreviousCamera
				);
				component.cameraPathStartFromCurrentCamera = value.value(
					"startFromCurrentCamera",
					component.cameraPathStartFromCurrentCamera
				);
				component.cameraPathAutoCollectChildPoints = value.value(
					"autoCollectChildPoints",
					component.cameraPathAutoCollectChildPoints
				);
				component.cameraPathPointDurationToNext = value.value(
					"durationToNext",
					component.cameraPathPointDurationToNext
				);
				component.cameraPathPointEasingToNext = value.value(
					"easingToNext",
					component.cameraPathPointEasingToNext
				);
				if (component.type == "StatSet" && value.contains("stats")) {
					ReadStats(value.at("stats"), component.stats);
				}
				if (component.type == "EventTrigger" && value.contains("bindings")) {
					ReadEvents(value.at("bindings"), component.eventBindings);
				}
				if (component.type == "AudioSource") {
					component.audioClipPath = value.value("clipPath", component.audioClipPath);
					component.audioSpatialMode = value.value("spatialMode", component.audioSpatialMode);
					component.audioMinimumDistance = value.value("minimumDistance", component.audioMinimumDistance);
					component.audioMaximumDistance = value.value("maximumDistance", component.audioMaximumDistance);
					component.audioStereoAreaWidth = value.value("stereoAreaWidth", component.audioStereoAreaWidth);
					component.audioBus = value.value("bus", component.audioBus);
					component.audioVolume = (std::max)(value.value("volume", component.audioVolume), 0.0f);
					component.audioPitch = (std::max)(value.value("pitch", component.audioPitch), 0.01f);
					component.audioLoop = value.value("loop", component.audioLoop);
					component.audioPlayOnStart = value.value("playOnStart", component.audioPlayOnStart);
					component.audioStopOnDisable = value.value("stopOnDisable", component.audioStopOnDisable);
					component.audioDecompressOnLoad = value.value("decompressOnLoad", component.audioDecompressOnLoad);
					component.audioStreamFromDisk = value.value("streamFromDisk", component.audioStreamFromDisk);
					component.audioPersistAcrossScenes = value.value("persistAcrossScenes", component.audioPersistAcrossScenes);
					component.audioBgmFadeSeconds = (std::max)(
						value.value("bgmFadeSeconds", component.audioBgmFadeSeconds),
						0.0f
					);
					if (
						component.audioSpatialMode != "ThreeD" &&
						component.audioSpatialMode != "ThreeDPointDownmix" &&
						component.audioSpatialMode != "ThreeDStereoArea"
					) {
						component.audioSpatialMode = "TwoD";
					}
				}
				if (component.type == "AudioListener") {
					component.audioListenerMode = value.value("mode", component.audioListenerMode);
					if (
						component.audioListenerMode != "ActiveCamera" &&
						component.audioListenerMode != "Entity"
					) {
						component.audioListenerMode = "Hybrid";
					}
				}
				if (
					component.type == "PostProcessProfileManager" &&
					value.contains("profiles")
				) {
					ReadPostProcessProfiles(
						value.at("profiles"), component.postProcessProfiles
					);
				}
				if (component.type == "PostProcessProfileManager") {
					component.postProcessStatusTextEntityId = value.value(
						"statusTextEntityId", uint64_t{ 0 }
					);
					component.postProcessStatusTextEntityName = value.value(
						"statusTextEntityName", std::string{}
					);
					component.postProcessStatusTextPrefix = value.value(
						"statusTextPrefix", std::string{ "PostEffect: " }
					);
				}
				if (component.type == "PrefabAnimator" && value.contains("clips")) {
					ReadPrefabAnimations(
						value.at("clips"),
						component.prefabAnimationClips
					);
				}
				if (component.type == "StateMachine") {
					component.stateMachineInitialState = value.value(
						"initialState", component.stateMachineInitialState
					);
					component.stateMachineResetOnDisable = value.value(
						"resetOnDisable", component.stateMachineResetOnDisable
					);
					if (value.contains("states")) {
						ReadStateMachineStates(
							value.at("states"),
							component.stateMachineStates
						);
					}
				}
				if (component.type == "Faction") {
					component.factionName = value.value("name", component.factionName);
				}
				if (component.type == "HitBox") {
					component.hitBoxDamage = value.value("damage", component.hitBoxDamage);
					component.hitBoxPoiseDamage = value.value(
						"poiseDamage", component.hitBoxPoiseDamage
					);
					component.hitBoxKnockback = value.value(
						"knockback", component.hitBoxKnockback
					);
					component.hitBoxVerticalKnockback = (std::max)(
						value.value("verticalKnockback", component.hitBoxVerticalKnockback),
						0.0f
					);
					component.hitBoxKnockbackDirectionMode = value.value(
						"knockbackDirectionMode", component.hitBoxKnockbackDirectionMode
					);
					if (value.contains("knockbackLocalDirection")) {
						component.hitBoxKnockbackLocalDirection = JsonToVector(
							value.at("knockbackLocalDirection"), component.hitBoxKnockbackLocalDirection
						);
					}
					component.hitBoxHitPolicy = value.value(
						"hitPolicy", component.hitBoxHitPolicy
					);
					if (
						component.hitBoxHitPolicy != "OncePerLoop" &&
						component.hitBoxHitPolicy != "TargetCooldown"
					) {
						component.hitBoxHitPolicy = "OncePerActivation";
					}
					component.hitBoxTargetCooldown = (std::max)(
						value.value("targetCooldown", component.hitBoxTargetCooldown), 0.0f
					);
					component.hitBoxHitStopDuration = value.value(
						"hitStopDuration", component.hitBoxHitStopDuration
					);
					component.hitBoxReactionTag = value.value(
						"reactionTag", component.hitBoxReactionTag
					);
					component.hitBoxDamageStatId = value.value(
						"damageStatId", component.hitBoxDamageStatId
					);
					component.hitBoxPoiseStatId = value.value(
						"poiseStatId", component.hitBoxPoiseStatId
					);
					component.hitBoxOwnerEntityId = value.value(
						"ownerEntityId", component.hitBoxOwnerEntityId
					);
					component.hitBoxOwnerEntityName = value.value(
						"ownerEntityName", component.hitBoxOwnerEntityName
					);
					component.hitBoxIgnoreSameFaction = value.value(
						"ignoreSameFaction", component.hitBoxIgnoreSameFaction
					);
				}
				if (component.type == "HurtBox") {
					component.hurtBoxDamageMultiplier = value.value(
						"damageMultiplier", component.hurtBoxDamageMultiplier
					);
					component.hurtBoxHealthStatId = value.value(
						"healthStatId", component.hurtBoxHealthStatId
					);
					component.hurtBoxStatsEntityId = value.value(
						"statsEntityId", component.hurtBoxStatsEntityId
					);
					component.hurtBoxStatsEntityName = value.value(
						"statsEntityName", component.hurtBoxStatsEntityName
					);
				}
				if (component.type == "AttackSet" && value.contains("attacks")) {
					ReadAttackDefinitions(value.at("attacks"), component.attackDefinitions);
				}
				if (component.type == "HitReaction") {
					component.hitReactionKnockbackMultiplier = value.value(
						"knockbackMultiplier",
						component.hitReactionKnockbackMultiplier
					);
					component.hitReactionTriggerMode = value.value(
						"triggerMode", component.hitReactionTriggerMode
					);
					if (component.hitReactionTriggerMode != "PoiseBreak") {
						component.hitReactionTriggerMode = "MinimumDamage";
					}
					component.hitReactionMinimumPoiseDamage = value.value(
						"minimumPoiseDamage",
						component.hitReactionMinimumPoiseDamage
					);
					component.hitReactionPoiseStatId = value.value(
						"poiseStatId", component.hitReactionPoiseStatId
					);
					component.hitReactionPoiseRecoveryDelay = (std::max)(
						value.value(
							"poiseRecoveryDelay",
							component.hitReactionPoiseRecoveryDelay
						),
						0.0f
					);
					component.hitReactionStateName = value.value(
						"stateName", component.hitReactionStateName
					);
					component.hitReactionStateDuration = value.value(
						"stateDuration", component.hitReactionStateDuration
					);
				}
				if (component.type == "DeathPresentation") {
					component.deathPresentationStateName = value.value(
						"stateName", component.deathPresentationStateName
					);
					component.deathPresentationDeactivateDelay = value.value(
						"deactivateDelay",
						component.deathPresentationDeactivateDelay
					);
					component.deathPresentationEffectPath = value.value(
						"effectPath", component.deathPresentationEffectPath
					);
				}
				if (component.type == "BoneAttachment") {
					component.boneAttachmentTargetEntityId = value.value(
						"targetEntityId", component.boneAttachmentTargetEntityId
					);
					component.boneAttachmentTargetEntityName = value.value(
						"targetEntityName", component.boneAttachmentTargetEntityName
					);
					component.boneAttachmentJointName = value.value(
						"jointName", component.boneAttachmentJointName
					);
					component.boneAttachmentAlignmentMode = value.value(
						"alignmentMode", component.boneAttachmentAlignmentMode
					);
					component.boneAttachmentSourceJointName = value.value(
						"sourceJointName",
						component.boneAttachmentSourceJointName
					);
					component.boneAttachmentInheritScale = value.value(
						"inheritScale", component.boneAttachmentInheritScale
					);
				}
				if (component.type == "EnemyBehavior") {
					component.enemyTargetEntityId = value.value(
						"targetEntityId", component.enemyTargetEntityId
					);
					component.enemyTargetEntityName = value.value(
						"targetEntityName", component.enemyTargetEntityName
					);
					component.enemyHealthStatId = value.value(
						"healthStatId", component.enemyHealthStatId
					);
					component.enemyDetectionRange = value.value(
						"detectionRange", component.enemyDetectionRange
					);
					component.enemyLoseRange = value.value(
						"loseRange", component.enemyLoseRange
					);
					component.enemyAttackRange = value.value(
						"attackRange", component.enemyAttackRange
					);
					component.enemyMoveSpeed = value.value(
						"moveSpeed", component.enemyMoveSpeed
					);
					component.enemyTurnSpeed = value.value(
						"turnSpeed", component.enemyTurnSpeed
					);
					component.enemyAttackCooldown = value.value(
						"attackCooldown", component.enemyAttackCooldown
					);
					component.enemyAttackWindup = value.value(
						"attackWindup", component.enemyAttackWindup
					);
					component.enemyAttackActiveTime = value.value(
						"attackActiveTime", component.enemyAttackActiveTime
					);
					component.enemyAttackRecovery = value.value(
						"attackRecovery", component.enemyAttackRecovery
					);
					component.enemyAttackAnimationClip = value.value(
						"attackAnimationClip", component.enemyAttackAnimationClip
					);
					component.enemyAttackPrefabAnimationClip = value.value(
						"attackPrefabAnimationClip",
						component.enemyAttackPrefabAnimationClip
					);
					component.enemyAttackHitBoxEntityId = value.value(
						"attackHitBoxEntityId", component.enemyAttackHitBoxEntityId
					);
					component.enemyAttackHitBoxEntityName = value.value(
						"attackHitBoxEntityName",
						component.enemyAttackHitBoxEntityName
					);
				}
				if (component.type == "Projectile") {
					if (value.contains("direction")) {
						component.projectileDirection = JsonToVector(
							value.at("direction"), component.projectileDirection
						);
					}
					component.projectileSpeed = value.value(
						"speed", component.projectileSpeed
					);
					component.projectileGravity = value.value(
						"gravity", component.projectileGravity
					);
					component.projectileLifetime = value.value(
						"lifetime", component.projectileLifetime
					);
					component.projectileDestroyOnHit = value.value(
						"destroyOnHit", component.projectileDestroyOnHit
					);
					component.projectileHomingTargetEntityId = value.value(
						"homingTargetEntityId",
						component.projectileHomingTargetEntityId
					);
					component.projectileHomingTargetEntityName = value.value(
						"homingTargetEntityName",
						component.projectileHomingTargetEntityName
					);
					component.projectileHomingStrength = value.value(
						"homingStrength", component.projectileHomingStrength
					);
				}
				if (component.type == "EnemySpawner") {
					component.enemySpawnerPrefabPath = value.value(
						"prefabPath", component.enemySpawnerPrefabPath
					);
					component.enemySpawnerInitialCount = value.value(
						"initialCount", component.enemySpawnerInitialCount
					);
					component.enemySpawnerMaxAlive = value.value(
						"maxAlive", component.enemySpawnerMaxAlive
					);
					component.enemySpawnerInterval = value.value(
						"interval", component.enemySpawnerInterval
					);
					component.enemySpawnerRadius = value.value(
						"radius", component.enemySpawnerRadius
					);
					component.enemySpawnerAutoStart = value.value(
						"autoStart", component.enemySpawnerAutoStart
					);
				}
			}
			if (!component.type.empty()) {
				if (component.type == "Environment") {
					if (component.environmentSkyboxPath.empty()) {
						component.environmentSkyboxPath =
							"resources/rostock_laage_airport_4k.dds";
					}
					component.environmentSkyboxIntensity =
						(std::max)(0.0f, component.environmentSkyboxIntensity);
					component.environmentReflectionIntensity = std::clamp(
						component.environmentReflectionIntensity,
						0.0f,
						1.0f
					);
				} else if (component.type == "SceneTransition") {
					if (component.sceneTransitionTriggerType != "Key") {
						component.sceneTransitionTriggerType = "Key";
					}
					if (component.sceneTransitionTriggerKey.empty()) {
						component.sceneTransitionTriggerKey = "ENTER";
					}
				} else if (component.type == "AgentBehavior") {
					if (component.agentBehaviorName.empty()) {
						component.agentBehaviorName = "Agent";
					}
					if (
						component.agentMovementMode != "Free3D" &&
						component.agentMovementMode != "GroundXZ"
					) {
						component.agentMovementMode = "Free3D";
					}
					if (component.agentProfileName.empty()) {
						component.agentProfileName = "Default";
					}
					component.agentMinSpeed =
						(std::max)(component.agentMinSpeed, 0.0f);
					component.agentMaxSpeed =
						(std::max)(component.agentMaxSpeed, component.agentMinSpeed);
					component.agentTurnSpeed =
						(std::max)(component.agentTurnSpeed, 0.0f);
					component.agentWanderStrength =
						(std::max)(component.agentWanderStrength, 0.0f);
					component.agentWanderChangeInterval =
						(std::max)(component.agentWanderChangeInterval, 0.0f);
					component.agentWanderDirectionRange = std::clamp(
						component.agentWanderDirectionRange,
						0.0f,
						3.14159265359f
					);
					component.agentWanderVerticalRange = std::clamp(
						component.agentWanderVerticalRange,
						0.0f,
						1.0f
					);
					component.agentRandomSeed =
						(std::max)(component.agentRandomSeed, 0);
					component.agentFlockDecisionInterval =
						(std::max)(component.agentFlockDecisionInterval, 0.0f);
					component.agentFlockAcceleration =
						(std::max)(component.agentFlockAcceleration, 0.0f);
					component.agentFlockTurnRate =
						(std::max)(component.agentFlockTurnRate, 0.0f);
					component.agentMemberCenterFollow =
						(std::max)(component.agentMemberCenterFollow, 0.0f);
					component.agentMemberJitterStrength =
						(std::max)(component.agentMemberJitterStrength, 0.0f);
					component.agentMemberJitterFrequency =
						(std::max)(component.agentMemberJitterFrequency, 0.0f);
					component.agentMemberJitterUpdateInterval =
						(std::max)(component.agentMemberJitterUpdateInterval, 0.0f);
					component.agentMemberJitterFollowSpeed =
						(std::max)(component.agentMemberJitterFollowSpeed, 0.0f);
					component.agentMemberSpeedVariation = std::clamp(
						component.agentMemberSpeedVariation,
						0.0f,
						1.0f
					);
					component.agentMemberLeashDistance =
						(std::max)(component.agentMemberLeashDistance, 0.0f);
					component.agentMemberLeashStrength =
						(std::max)(component.agentMemberLeashStrength, 0.0f);
					component.agentMemberCatchupSpeed =
						(std::max)(component.agentMemberCatchupSpeed, 0.0f);
					component.agentMemberSeparationUpdateInterval =
						(std::max)(
							component.agentMemberSeparationUpdateInterval,
							0.0f
						);
					component.agentMemberSeparationBlend = std::clamp(
						component.agentMemberSeparationBlend,
						0.0f,
						1.0f
					);
					if (!std::isfinite(component.agentMemberMinimumDistance) ||
						component.agentMemberMinimumDistance < 0.0f) {
						component.agentMemberMinimumDistance = 0.0f;
					}
					component.agentBoundsWeight =
						(std::max)(component.agentBoundsWeight, 0.0f);
					component.agentTeamHeadingDirection =
						NormalizeDirectionVector(
							component.agentTeamHeadingDirection,
							{ 0.0f, 0.0f, 1.0f }
						);
					component.agentTeamHeadingWeight =
						(std::max)(component.agentTeamHeadingWeight, 0.0f);
					component.agentTeamHeadingFollowSpeed =
						(std::max)(
							component.agentTeamHeadingFollowSpeed,
							0.0f
						);
					component.agentTeamRotationWeight =
						std::clamp(
							component.agentTeamRotationWeight,
							0.0f,
							1.0f
						);
					component.agentTeamRotationFollowSpeed =
						(std::max)(
							component.agentTeamRotationFollowSpeed,
							0.0f
						);
					if (
						component.agentForwardAxis != "+Z" &&
						component.agentForwardAxis != "-Z" &&
						component.agentForwardAxis != "+X" &&
						component.agentForwardAxis != "-X" &&
						component.agentForwardAxis != "+Y" &&
						component.agentForwardAxis != "-Y"
					) {
						component.agentForwardAxis = "+Z";
					}
					component.agentRotationFollowSpeed =
						(std::max)(component.agentRotationFollowSpeed, 0.0f);
					component.agentPitchFromVerticalVelocity =
						(std::max)(
							component.agentPitchFromVerticalVelocity,
							0.0f
						);
					component.agentBankingStrength =
						(std::max)(component.agentBankingStrength, 0.0f);
					component.agentSchoolingUpdateInterval =
						(std::max)(
							component.agentSchoolingUpdateInterval,
							0.0f
						);
					component.agentSchoolingUpdateJitter =
						(std::max)(component.agentSchoolingUpdateJitter, 0.0f);
					component.agentNeighborLimit =
						(std::max)(component.agentNeighborLimit, 0);
					component.agentSchoolingBlend =
						std::clamp(component.agentSchoolingBlend, 0.0f, 1.0f);
					component.agentSeparationRadius =
						(std::max)(component.agentSeparationRadius, 0.0f);
					component.agentAlignmentRadius =
						(std::max)(component.agentAlignmentRadius, 0.0f);
					component.agentCohesionRadius =
						(std::max)(component.agentCohesionRadius, 0.0f);
					component.agentSeparationWeight =
						(std::max)(component.agentSeparationWeight, 0.0f);
					component.agentAlignmentWeight =
						(std::max)(component.agentAlignmentWeight, 0.0f);
					component.agentCohesionWeight =
						(std::max)(component.agentCohesionWeight, 0.0f);
					component.agentAttractorWeight =
						(std::max)(component.agentAttractorWeight, 0.0f);
				} else if (component.type == "AgentAttractor") {
					if (component.attractorTag.empty()) {
						component.attractorTag = "Default";
					}
					component.attractorRadius =
						(std::max)(component.attractorRadius, 0.0f);
					component.attractorStrength =
						(std::max)(component.attractorStrength, 0.0f);
				}
				const auto duplicate = std::find_if(
					components.begin(),
					components.end(),
					[&component](const SceneComponent& existing) {
						return existing.type == component.type;
					}
				);
				if (duplicate == components.end()) {
					components.push_back(std::move(component));
				}
			}
		}
		return components;
	}

	void SynchronizeLegacyRendererFields(SceneEntity& entity) {
		if (SceneComponent* meshRenderer = FindComponent(entity, "MeshRenderer")) {
			if (meshRenderer->modelPath.empty()) {
				meshRenderer->modelPath = entity.modelPath;
			}
			entity.modelPath = meshRenderer->modelPath;
		}
		if (SceneComponent* spriteRenderer = FindComponent(entity, "SpriteRenderer")) {
			if (spriteRenderer->texturePath.empty() && !entity.spriteTexturePath.empty()) {
				spriteRenderer->texturePath = entity.spriteTexturePath;
				spriteRenderer->spriteSize = entity.spriteSize;
				spriteRenderer->spriteAnchor = entity.spriteAnchor;
				spriteRenderer->spriteColor = entity.spriteColor;
				spriteRenderer->spriteFlipX = entity.spriteFlipX;
				spriteRenderer->spriteFlipY = entity.spriteFlipY;
			}
			entity.spriteTexturePath = spriteRenderer->texturePath;
			entity.spriteSize = spriteRenderer->spriteSize;
			entity.spriteAnchor = spriteRenderer->spriteAnchor;
			entity.spriteColor = spriteRenderer->spriteColor;
			entity.spriteFlipX = spriteRenderer->spriteFlipX;
			entity.spriteFlipY = spriteRenderer->spriteFlipY;
		}
	}

	json EntityOverridePropertiesToJson(
		const SceneEntity& entity,
		bool includeParent,
		bool includeTransform
	) {
		json value = {
			{ "name", entity.name },
			{ "folder", entity.folder },
			{ "folderTeamEnabled", entity.folderTeamEnabled },
			{ "active", entity.active },
			{ "locked", entity.locked }
		};
		if (includeParent) {
			value["parentId"] = entity.parentId;
		}
		if (includeTransform) {
			value["transform"] = {
				{ "scale", VectorToJson(entity.transform.scale) },
				{ "rotation", QuaternionToJson(entity.transform.rotate) },
				{ "translate", VectorToJson(entity.transform.translate) }
			};
		}
		return value;
	}

	std::string EscapeJsonPointerToken(const std::string& token) {
		std::string result;
		result.reserve(token.size());
		for (char character : token) {
			if (character == '~') {
				result += "~0";
			} else if (character == '/') {
				result += "~1";
			} else {
				result += character;
			}
		}
		return result;
	}

	std::string ArrayElementIdentity(const json& value) {
		if (!value.is_object()) {
			return {};
		}
		auto makeIdentity = [&value](const char* key) -> std::string {
			const auto found = value.find(key);
			if (
				found == value.end() ||
				(found->is_object() || found->is_array() || found->is_null())
			) {
				return {};
			}
			return std::string(key) + ":" + found->dump();
		};

		for (const char* key : {
			"id", "materialName", "name", "time", "entityId"
		}) {
			const std::string identity = makeIdentity(key);
			if (!identity.empty()) {
				return identity;
			}
		}

		const std::string targetIdentity = makeIdentity("targetEntityId");
		const std::string propertyIdentity = makeIdentity("property");
		if (!targetIdentity.empty() && !propertyIdentity.empty()) {
			return targetIdentity + "|" + propertyIdentity;
		}
		return {};
	}

	void CollectJsonPropertyDifferences(
		const json& source,
		const json& instance,
		const std::string& propertyPath,
		bool skipComponentIdentity,
		std::vector<std::string>& differences
	) {
		if (source.is_object() && instance.is_object()) {
			std::vector<std::string> keys;
			std::unordered_set<std::string> seenKeys;
			for (auto iterator = source.begin(); iterator != source.end(); ++iterator) {
				keys.push_back(iterator.key());
				seenKeys.insert(iterator.key());
			}
			for (auto iterator = instance.begin(); iterator != instance.end(); ++iterator) {
				if (seenKeys.insert(iterator.key()).second) {
					keys.push_back(iterator.key());
				}
			}
			for (const std::string& key : keys) {
				if (
					propertyPath.empty() &&
					skipComponentIdentity &&
					(key == "localId" || key == "type")
				) {
					continue;
				}
				const std::string childPath =
					propertyPath + "/" + EscapeJsonPointerToken(key);
				const auto sourceValue = source.find(key);
				const auto instanceValue = instance.find(key);
				if (
					sourceValue == source.end() ||
					instanceValue == instance.end()
				) {
					differences.push_back(childPath);
					continue;
				}
				CollectJsonPropertyDifferences(
					*sourceValue,
					*instanceValue,
					childPath,
					false,
					differences
				);
			}
			return;
		}
		if (source.is_array() && instance.is_array()) {
			if (source == instance) {
				return;
			}
			if (source.size() != instance.size()) {
				differences.push_back(propertyPath);
				return;
			}

			std::unordered_set<std::string> sourceIdentities;
			std::unordered_set<std::string> instanceIdentities;
			for (size_t index = 0; index < source.size(); ++index) {
				const std::string sourceIdentity =
					ArrayElementIdentity(source[index]);
				const std::string instanceIdentity =
					ArrayElementIdentity(instance[index]);
				if (
					sourceIdentity.empty() ||
					sourceIdentity != instanceIdentity ||
					!sourceIdentities.insert(sourceIdentity).second ||
					!instanceIdentities.insert(instanceIdentity).second
				) {
					differences.push_back(propertyPath);
					return;
				}
			}

			for (size_t index = 0; index < source.size(); ++index) {
				CollectJsonPropertyDifferences(
					source[index],
					instance[index],
					propertyPath + "/" + std::to_string(index),
					false,
					differences
				);
			}
			return;
		}
		// 数値配列や安定識別子のない配列は1つのPropertyとして扱う。
		if (source != instance) {
			differences.push_back(propertyPath);
		}
	}

	std::string FormatOverridePropertyPath(const std::string& propertyPath) {
		if (propertyPath.empty()) {
			return "Value";
		}
		std::string result;
		for (size_t index = 1; index < propertyPath.size(); ++index) {
			if (propertyPath[index] == '/') {
				result += '.';
			} else if (
				propertyPath[index] == '~' &&
				index + 1 < propertyPath.size()
			) {
				const char escaped = propertyPath[index + 1];
				if (escaped == '0' || escaped == '1') {
					result += escaped == '0' ? '~' : '/';
					++index;
				} else {
					result += propertyPath[index];
				}
			} else {
				result += propertyPath[index];
			}
		}
		return result;
	}

	bool CopyJsonProperty(
		json& target,
		const json& source,
		const std::string& propertyPath
	) {
		try {
			const json::json_pointer pointer(propertyPath);
			target.at(pointer) = source.at(pointer);
			return true;
		}
		catch (const json::exception&) {
			return false;
		}
	}

	bool SetEntityOverrideProperty(
		SceneEntity& entity,
		const std::string& propertyPath,
		const json& value
	) {
		try {
			if (propertyPath == "/name") {
				entity.name = value.get<std::string>();
			} else if (propertyPath == "/parentId") {
				entity.parentId = value.get<uint64_t>();
			} else if (propertyPath == "/folder") {
				entity.folder = value.get<bool>();
			} else if (propertyPath == "/folderTeamEnabled") {
				entity.folderTeamEnabled = value.get<bool>();
			} else if (propertyPath == "/active") {
				entity.active = value.get<bool>();
			} else if (propertyPath == "/locked") {
				entity.locked = value.get<bool>();
			} else if (propertyPath == "/transform/scale") {
				entity.transform.scale = JsonToVector(
					value,
					entity.transform.scale
				);
			} else if (propertyPath == "/transform/rotation") {
				entity.transform.rotate = JsonToQuaternion(
					value,
					entity.transform.rotate
				);
			} else if (propertyPath == "/transform/translate") {
				entity.transform.translate = JsonToVector(
					value,
					entity.transform.translate
				);
			} else {
				return false;
			}
			return true;
		}
		catch (const json::exception&) {
			return false;
		}
	}

	SceneComponent* FindComponentByLocalId(
		SceneEntity& entity,
		uint64_t localId
	) {
		const auto found = std::find_if(
			entity.components.begin(),
			entity.components.end(),
			[localId](const SceneComponent& component) {
				return component.localId == localId;
			}
		);
		return found == entity.components.end() ? nullptr : &(*found);
	}

	const SceneComponent* FindComponentByLocalId(
		const SceneEntity& entity,
		uint64_t localId
	) {
		const auto found = std::find_if(
			entity.components.begin(),
			entity.components.end(),
			[localId](const SceneComponent& component) {
				return component.localId == localId;
			}
		);
		return found == entity.components.end() ? nullptr : &(*found);
	}

	bool ComponentFromJson(
		const json& value,
		SceneComponent& component
	) {
		const std::vector<SceneComponent> parsed = ComponentsFromJson(
			json::array({ value })
		);
		if (parsed.size() != 1 || parsed.front().type.empty()) {
			return false;
		}
		component = parsed.front();
		return true;
	}

	json SceneEntityToJson(const SceneEntity& entity) {
		const SceneComponent* meshRenderer = FindComponent(
			entity,
			"MeshRenderer"
		);
		const SceneComponent* spriteRenderer = FindComponent(
			entity,
			"SpriteRenderer"
		);
		json components = json::array();
		for (const SceneComponent& component : entity.components) {
			components.push_back(ComponentToJson(component));
		}
		json value = {
			{ "id", entity.id },
			{ "parentId", entity.parentId },
			{ "name", entity.name },
			{ "folder", entity.folder },
			{ "folderTeamEnabled", entity.folderTeamEnabled },
			{ "active", entity.active },
			{ "locked", entity.locked },
			{ "team", entity.teamName },
			{ "transform", {
				{ "scale", VectorToJson(entity.transform.scale) },
				{ "rotation", QuaternionToJson(entity.transform.rotate) },
				{ "translate", VectorToJson(entity.transform.translate) }
			} },
			{ "modelPath", meshRenderer
				? meshRenderer->modelPath
				: entity.modelPath },
			{ "sprite", {
				{ "texturePath", spriteRenderer
					? spriteRenderer->texturePath
					: entity.spriteTexturePath },
				{ "size", VectorToJson(spriteRenderer
					? spriteRenderer->spriteSize
					: entity.spriteSize) },
				{ "anchor", VectorToJson(spriteRenderer
					? spriteRenderer->spriteAnchor
					: entity.spriteAnchor) },
				{ "color", VectorToJson(spriteRenderer
					? spriteRenderer->spriteColor
					: entity.spriteColor) },
				{ "flipX", spriteRenderer
					? spriteRenderer->spriteFlipX
					: entity.spriteFlipX },
				{ "flipY", spriteRenderer
					? spriteRenderer->spriteFlipY
					: entity.spriteFlipY }
			} },
			{ "components", std::move(components) }
		};
		if (HasPrefabAssetLink(entity) || entity.prefabInstanceRootId != 0 ||
			entity.prefabLocalId != 0) {
			json links = json::array();
			if (!entity.prefabLinks.empty()) {
				for (const ScenePrefabLink& link : entity.prefabLinks) {
					links.push_back({
						{ "assetId", link.assetId },
						{ "sourcePath", link.sourcePath },
						{ "instanceRootId", link.instanceRootId },
						{ "localId", link.localId }
					});
				}
			} else {
				links.push_back({
					{ "assetId", entity.prefabAssetId },
					{ "sourcePath", entity.prefabSourcePath },
					{ "instanceRootId", entity.prefabInstanceRootId },
					{ "localId", entity.prefabLocalId }
				});
			}
			value["prefab"] = {
				{ "assetId", entity.prefabAssetId },
				{ "sourcePath", entity.prefabSourcePath },
				{ "instanceRootId", entity.prefabInstanceRootId },
				{ "localId", entity.prefabLocalId },
				{ "links", std::move(links) }
			};
		}
		return value;
	}

	bool SceneEntityFromJson(
		const json& source,
		SceneEntity& entity,
		std::string& errorMessage
	) {
		if (!source.is_object()) {
			errorMessage = "Variant contains an invalid Entity entry";
			return false;
		}
		if (source.contains("transform") &&
			!source.at("transform").is_object()) {
			errorMessage = "Variant Entity transform must be an object";
			return false;
		}
		if (source.contains("sprite") && !source.at("sprite").is_object()) {
			errorMessage = "Variant Entity sprite must be an object";
			return false;
		}
		if (source.contains("prefab") && !source.at("prefab").is_object()) {
			errorMessage = "Variant Entity prefab metadata must be an object";
			return false;
		}
		if (!source.contains("components") ||
			!source.at("components").is_array()) {
			errorMessage = "Variant Entity components must be an array";
			return false;
		}

		entity = {};
		entity.id = source.value("id", uint64_t{});
		entity.parentId = source.value("parentId", uint64_t{});
		entity.name = source.value("name", std::string("Entity"));
		entity.folder = source.value("folder", false);
		entity.folderTeamEnabled = source.value("folderTeamEnabled", false);
		entity.active = source.value("active", true);
		entity.locked = source.value("locked", false);
		entity.teamName = source.value("team", std::string{});
		entity.modelPath = source.value("modelPath", std::string{});
		if (source.contains("prefab")) {
			const json& prefab = source.at("prefab");
			entity.prefabAssetId = prefab.value("assetId", std::string{});
			entity.prefabSourcePath = prefab.value("sourcePath", std::string{});
			entity.prefabInstanceRootId = prefab.value(
				"instanceRootId",
				uint64_t{}
			);
			entity.prefabLocalId = prefab.value("localId", uint64_t{});
			if (prefab.contains("links")) {
				if (!prefab.at("links").is_array()) {
					errorMessage = "Variant Entity prefab links must be an array";
					return false;
				}
				for (const json& linkValue : prefab.at("links")) {
					if (!linkValue.is_object()) {
						errorMessage = "Variant Entity contains an invalid Prefab link";
						return false;
					}
					entity.prefabLinks.push_back({
						linkValue.value("assetId", std::string{}),
						linkValue.value("sourcePath", std::string{}),
						linkValue.value("instanceRootId", uint64_t{}),
						linkValue.value("localId", uint64_t{})
					});
				}
			}
			EnsurePrefabLinkStack(entity);
			SynchronizeActivePrefabLink(entity);
		}
		if (source.contains("sprite")) {
			const json& sprite = source.at("sprite");
			entity.spriteTexturePath = sprite.value(
				"texturePath",
				std::string{}
			);
			if (sprite.contains("size")) {
				entity.spriteSize = JsonToVector(
					sprite.at("size"),
					entity.spriteSize
				);
			}
			if (sprite.contains("anchor")) {
				entity.spriteAnchor = JsonToVector(
					sprite.at("anchor"),
					entity.spriteAnchor
				);
			}
			if (sprite.contains("color")) {
				entity.spriteColor = JsonToVector(
					sprite.at("color"),
					entity.spriteColor
				);
			}
			entity.spriteFlipX = sprite.value("flipX", false);
			entity.spriteFlipY = sprite.value("flipY", false);
		}
		entity.components = ComponentsFromJson(source.at("components"));
		if (source.contains("transform")) {
			const json& transform = source.at("transform");
			if (transform.contains("scale")) {
				entity.transform.scale = JsonToVector(
					transform.at("scale"),
					entity.transform.scale
				);
			}
			if (transform.contains("rotation")) {
				entity.transform.rotate = JsonToQuaternion(
					transform.at("rotation"),
					entity.transform.rotate
				);
			}
			if (transform.contains("translate")) {
				entity.transform.translate = JsonToVector(
					transform.at("translate"),
					entity.transform.translate
				);
			}
		}
		SynchronizeLegacyRendererFields(entity);
		return true;
	}

	json BuildJsonPropertyPatch(const json& base, const json& current) {
		std::vector<std::string> differences;
		CollectJsonPropertyDifferences(
			base,
			current,
			{},
			false,
			differences
		);
		json patch = json::array();
		for (const std::string& path : differences) {
			try {
				const json::json_pointer pointer(path);
				patch.push_back({
					{ "path", path },
					{ "value", current.at(pointer) }
				});
			}
			catch (const json::exception&) {
				patch.push_back({
					{ "path", path },
					{ "remove", true }
				});
			}
		}
		return patch;
	}

	bool RemoveJsonProperty(json& target, const std::string& path) {
		if (path.empty() || path.front() != '/') {
			return false;
		}
		const size_t separator = path.find_last_of('/');
		const std::string parentPath = path.substr(0, separator);
		std::string token = path.substr(separator + 1);
		std::string decodedToken;
		for (size_t index = 0; index < token.size(); ++index) {
			if (token[index] == '~' && index + 1 < token.size()) {
				if (token[index + 1] == '0') {
					decodedToken += '~';
					++index;
					continue;
				}
				if (token[index + 1] == '1') {
					decodedToken += '/';
					++index;
					continue;
				}
			}
			decodedToken += token[index];
		}
		try {
			json& parent = parentPath.empty()
				? target
				: target.at(json::json_pointer(parentPath));
			if (parent.is_object()) {
				parent.erase(decodedToken);
				return true;
			}
			if (parent.is_array()) {
				const size_t index = static_cast<size_t>(std::stoull(decodedToken));
				if (index >= parent.size()) {
					return false;
				}
				parent.erase(parent.begin() + index);
				return true;
			}
		}
		catch (const std::exception&) {
			return false;
		}
		return false;
	}

	bool ApplyJsonPropertyPatch(json& target, const json& patch) {
		if (!patch.is_array()) {
			return false;
		}
		try {
			for (const json& operation : patch) {
				if (
					!operation.is_object() ||
					!operation.contains("path") ||
					!operation.at("path").is_string()
				) {
					return false;
				}
				const std::string path =
					operation.at("path").get<std::string>();
				if (operation.value("remove", false)) {
					if (!RemoveJsonProperty(target, path)) {
						return false;
					}
					continue;
				}
				if (!operation.contains("value")) {
					return false;
				}
				const json::json_pointer pointer(
					path
				);
				target[pointer] = operation.at("value");
			}
		}
		catch (const json::exception&) {
			return false;
		}
		return true;
	}

	bool BuildVariantComparisonDocument(
		const SceneDocument& variant,
		SceneDocument& comparison,
		SceneDocument& base,
		uint64_t& comparisonRootId
	) {
		if (!variant.IsPrefabVariant()) {
			return false;
		}
		const std::string basePath = PrefabAssetRegistry::ResolvePath(
			variant.GetVariantBaseAssetId(),
			variant.GetVariantBasePath()
		);
		if (basePath.empty() || !base.Load(basePath) ||
			base.GetAssetId() != variant.GetVariantBaseAssetId()) {
			return false;
		}
		const auto root = std::find_if(
			base.GetEntities().begin(),
			base.GetEntities().end(),
			[](const SceneEntity& entity) { return entity.parentId == 0; }
		);
		if (root == base.GetEntities().end() ||
			!variant.FindEntity(root->id)) {
			return false;
		}

		comparison = variant;
		comparisonRootId = root->id;
		for (SceneEntity& entity : comparison.GetEntities()) {
			if (!base.FindEntity(entity.id)) {
				continue;
			}
			// 比較用のBase境界を最外側へ一時挿入する。既存Nested境界は保持する。
			EnsurePrefabLinkStack(entity);
			entity.prefabLinks.insert(
				entity.prefabLinks.begin(),
				ScenePrefabLink{
					variant.GetVariantBaseAssetId(),
					basePath,
					comparisonRootId,
					entity.id
				}
			);
			SynchronizeActivePrefabLink(entity);
		}
		return true;
	}

	void RestoreVariantPrefabMetadata(
		SceneDocument& result,
		const SceneDocument& original,
		const SceneDocument& base
	) {
		for (SceneEntity& entity : result.GetEntities()) {
			const SceneEntity* metadataSource = original.FindEntity(entity.id);
			if (!metadataSource) {
				metadataSource = base.FindEntity(entity.id);
			}
			if (!metadataSource) {
				continue;
			}
			entity.prefabAssetId = metadataSource->prefabAssetId;
			entity.prefabSourcePath = metadataSource->prefabSourcePath;
			entity.prefabInstanceRootId = metadataSource->prefabInstanceRootId;
			entity.prefabLocalId = metadataSource->prefabLocalId;
			entity.prefabLinks = metadataSource->prefabLinks;
		}
	}

}

void SceneDocument::Clear(const std::string& sceneName) {
	sceneName_ = sceneName;
	assetId_.clear();
	variantBaseAssetId_.clear();
	variantBasePath_.clear();
	variantBaseSnapshot_.reset();
	entities_.clear();
	teams_.clear();
	lightingSettings_ = {};
	postProcessSettings_ = {};
	debugSettings_ = {};
	nextId_ = 1;
	dirty_ = false;
	revision_ = 0;
	lastLoadError_.clear();
	lastSaveError_.clear();
}

bool SceneDocument::Load(const std::string& filePath) {
	lastLoadError_.clear();
	if (LoadInternal(filePath)) {
		return true;
	}

	const std::string primaryError = lastLoadError_;
	const std::string backupPath = filePath + ".bak";
	if (!LoadInternal(backupPath)) {
		lastLoadError_ =
			"Primary: " + primaryError + " | Backup: " + lastLoadError_;
		return false;
	}

	MarkDirty();
	lastLoadError_ = "Recovered from backup: " + backupPath;
	return true;
}

bool SceneDocument::Save(const std::string& filePath) {
	lastSaveError_.clear();
	const bool isPrefabDocument = IsPrefabDocumentPath(filePath);
	if (isPrefabDocument && assetId_.empty()) {
		assetId_ = PrefabAssetRegistry::ReadAssetId(filePath);
		if (assetId_.empty()) {
			assetId_ = PrefabAssetRegistry::CreateAssetId();
		}
	}
	for (SceneEntity& entity : entities_) {
		if (!entity.runtimeOnly) {
			EnsureComponentLocalIds(entity);
		}
	}
	json root;
	root["version"] = SceneDocumentMigrator::kCurrentVersion;
	root["sceneName"] = sceneName_;
	if (!assetId_.empty()) {
		root["assetId"] = assetId_;
	}
	root["lighting"] = LightingSettingsToJson(lightingSettings_);
	root["postProcess"] = PostProcessToJson(postProcessSettings_);
	root["debug"] = DebugSettingsToJson(debugSettings_);
	root["teams"] = json::array();
	for (const SceneTeamSettings& team : teams_) {
		root["teams"].push_back(TeamToJson(team));
	}
	root["entities"] = json::array();

	for (const SceneEntity& entity : entities_) {
		if (entity.runtimeOnly) {
			continue;
		}
		json components = json::array();
		for (const SceneComponent& component : entity.components) {
			components.push_back(ComponentToJson(component));
		}
		const SceneComponent* meshRenderer = FindComponent(entity, "MeshRenderer");
		const SceneComponent* spriteRenderer = FindComponent(entity, "SpriteRenderer");
		const std::string modelPath = meshRenderer
			? meshRenderer->modelPath
			: entity.modelPath;
		const std::string spriteTexturePath = spriteRenderer
			? spriteRenderer->texturePath
			: entity.spriteTexturePath;
		const Vector2 spriteSize = spriteRenderer
			? spriteRenderer->spriteSize
			: entity.spriteSize;
		const Vector2 spriteAnchor = spriteRenderer
			? spriteRenderer->spriteAnchor
			: entity.spriteAnchor;
		const Vector4 spriteColor = spriteRenderer
			? spriteRenderer->spriteColor
			: entity.spriteColor;
		const bool spriteFlipX = spriteRenderer
			? spriteRenderer->spriteFlipX
			: entity.spriteFlipX;
		const bool spriteFlipY = spriteRenderer
			? spriteRenderer->spriteFlipY
			: entity.spriteFlipY;
		json entityValue = {
			{ "id", entity.id },
			{ "parentId", entity.parentId },
			{ "name", entity.name },
			{ "folder", entity.folder },
			{ "folderTeamEnabled", entity.folderTeamEnabled },
			{ "active", entity.active },
			{ "locked", entity.locked },
			{ "team", entity.teamName },
			{ "transform", {
				{ "scale", VectorToJson(entity.transform.scale) },
				{ "rotation", QuaternionToJson(entity.transform.rotate) },
				{ "translate", VectorToJson(entity.transform.translate) }
			} },
			{ "modelPath", modelPath },
			{ "sprite", {
				{ "texturePath", spriteTexturePath },
				{ "size", VectorToJson(spriteSize) },
				{ "anchor", VectorToJson(spriteAnchor) },
				{ "color", VectorToJson(spriteColor) },
				{ "flipX", spriteFlipX },
				{ "flipY", spriteFlipY }
			} },
			{ "components", components }
		};
		if (
			!entity.prefabLinks.empty() ||
			!entity.prefabAssetId.empty() ||
			!entity.prefabSourcePath.empty() ||
			entity.prefabInstanceRootId != 0 ||
			entity.prefabLocalId != 0
		) {
			json prefabLinks = json::array();
			if (!entity.prefabLinks.empty()) {
				for (const ScenePrefabLink& link : entity.prefabLinks) {
					prefabLinks.push_back({
						{ "assetId", link.assetId },
						{ "sourcePath", link.sourcePath },
						{ "instanceRootId", link.instanceRootId },
						{ "localId", link.localId }
					});
				}
			} else {
				prefabLinks.push_back({
					{ "assetId", entity.prefabAssetId },
					{ "sourcePath", entity.prefabSourcePath },
					{ "instanceRootId", entity.prefabInstanceRootId },
					{ "localId", entity.prefabLocalId }
				});
			}
			entityValue["prefab"] = {
				{ "assetId", entity.prefabAssetId },
				{ "sourcePath", entity.prefabSourcePath },
				{ "instanceRootId", entity.prefabInstanceRootId },
				{ "localId", entity.prefabLocalId },
				{ "links", std::move(prefabLinks) }
			};
		}
		root["entities"].push_back(std::move(entityValue));
	}

	if (!variantBaseAssetId_.empty()) {
		// Variantは有効Document全体ではなく、読込時Baseとの差分だけを保存する。
		const std::string basePath = PrefabAssetRegistry::ResolvePath(
			variantBaseAssetId_,
			variantBasePath_
		);
		if (basePath.empty() || basePath == filePath ||
			variantBaseAssetId_ == assetId_) {
			lastSaveError_ =
				"Prefab Variant Base is missing, ambiguous, or self-referencing.";
			return false;
		}
		SceneDocument latestBase;
		if (!latestBase.LoadInternal(basePath) ||
			latestBase.GetAssetId() != variantBaseAssetId_) {
			lastSaveError_ = "Failed to load the latest Prefab Variant Base.";
			return false;
		}
		// 開いた時点のBaseとの差だけを明示Overrideとし、外部Base更新を固定しない。
		const SceneDocument& base = variantBaseSnapshot_
			? *variantBaseSnapshot_
			: latestBase;

		auto settingsToJson = [](const SceneDocument& document) {
			json teams = json::array();
			for (const SceneTeamSettings& team : document.teams_) {
				teams.push_back(TeamToJson(team));
			}
			return json{
				{ "sceneName", document.sceneName_ },
				{ "lighting", LightingSettingsToJson(
					document.lightingSettings_
				) },
				{ "postProcess", PostProcessToJson(
					document.postProcessSettings_
				) },
				{ "debug", DebugSettingsToJson(document.debugSettings_) },
				{ "teams", std::move(teams) }
			};
		};

		std::unordered_map<uint64_t, json> baseEntities;
		std::unordered_map<uint64_t, json> currentEntities;
		for (const SceneEntity& entity : base.entities_) {
			if (!entity.runtimeOnly) {
				baseEntities.emplace(entity.id, SceneEntityToJson(entity));
			}
		}
		for (const json& entityValue : root.at("entities")) {
			currentEntities.emplace(
				entityValue.value("id", uint64_t{}),
				entityValue
			);
		}

		json removedEntityIds = json::array();
		json addedEntities = json::array();
		json entityOverrides = json::array();
		for (const auto& [entityId, baseValue] : baseEntities) {
			const auto currentEntry = currentEntities.find(entityId);
			if (currentEntry == currentEntities.end()) {
				const uint64_t parentId = baseValue.value(
					"parentId",
					uint64_t{}
				);
				if (parentId == 0 || currentEntities.contains(parentId)) {
					removedEntityIds.push_back(entityId);
				}
				continue;
			}

			json baseProperties = baseValue;
			json currentProperties = currentEntry->second;
			const json baseComponents = baseProperties.value(
				"components",
				json::array()
			);
			const json currentComponents = currentProperties.value(
				"components",
				json::array()
			);
			baseProperties.erase("components");
			currentProperties.erase("components");

			json entityOverride = {
				{ "entityId", entityId }
			};
			const json propertyPatch = BuildJsonPropertyPatch(
				baseProperties,
				currentProperties
			);
			if (!propertyPatch.empty()) {
				entityOverride["properties"] = propertyPatch;
			}

			std::unordered_map<uint64_t, json> baseComponentsById;
			std::unordered_map<uint64_t, json> currentComponentsById;
			for (const json& component : baseComponents) {
				baseComponentsById.emplace(
					component.value("localId", uint64_t{}),
					component
				);
			}
			for (const json& component : currentComponents) {
				currentComponentsById.emplace(
					component.value("localId", uint64_t{}),
					component
				);
			}

			json removedComponentIds = json::array();
			json addedComponents = json::array();
			json componentOverrides = json::array();
			for (const auto& [componentId, baseComponent] :
				baseComponentsById) {
				const auto currentComponent =
					currentComponentsById.find(componentId);
				if (
					currentComponent == currentComponentsById.end() ||
					currentComponent->second.value("type", std::string{}) !=
						baseComponent.value("type", std::string{})
				) {
					removedComponentIds.push_back(componentId);
					continue;
				}
				const json componentPatch = BuildJsonPropertyPatch(
					baseComponent,
					currentComponent->second
				);
				if (!componentPatch.empty()) {
					componentOverrides.push_back({
						{ "componentId", componentId },
						{ "properties", componentPatch }
					});
				}
			}
			for (const auto& [componentId, currentComponent] :
				currentComponentsById) {
				const auto baseComponent = baseComponentsById.find(componentId);
				if (
					baseComponent == baseComponentsById.end() ||
					baseComponent->second.value("type", std::string{}) !=
						currentComponent.value("type", std::string{})
				) {
					addedComponents.push_back(currentComponent);
				}
			}
			if (!removedComponentIds.empty()) {
				entityOverride["removedComponentIds"] =
					std::move(removedComponentIds);
			}
			if (!addedComponents.empty()) {
				entityOverride["addedComponents"] = std::move(addedComponents);
			}
			if (!componentOverrides.empty()) {
				entityOverride["componentOverrides"] =
					std::move(componentOverrides);
			}
			if (entityOverride.size() > 1) {
				entityOverrides.push_back(std::move(entityOverride));
			}
		}
		for (const auto& [entityId, currentValue] : currentEntities) {
			if (!baseEntities.contains(entityId)) {
				addedEntities.push_back(currentValue);
			}
		}

		json variant = {
			{ "base", {
				{ "assetId", variantBaseAssetId_ },
				{ "fallbackPath", basePath }
			} },
			{ "rootOverrides", BuildJsonPropertyPatch(
				settingsToJson(base),
				settingsToJson(*this)
			) },
			{ "entityOverrides", std::move(entityOverrides) },
			{ "addedEntities", std::move(addedEntities) },
			{ "removedEntityIds", std::move(removedEntityIds) }
		};
		root = {
			{ "version", SceneDocumentMigrator::kCurrentVersion },
			{ "sceneName", sceneName_ },
			{ "assetId", assetId_ },
			{ "variant", std::move(variant) }
		};
	}

	const std::filesystem::path target = StringUtility::ToPath(filePath);
	std::filesystem::path temporary = target;
	temporary += L".tmp";
	std::filesystem::path backup = target;
	backup += L".bak";
	std::error_code error;
	if (!target.parent_path().empty()) {
		std::filesystem::create_directories(target.parent_path(), error);
		if (error) {
			return false;
		}
	}

	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			return false;
		}
		output << root.dump(2);
		output.flush();
		if (!output.good()) {
			output.close();
			std::filesystem::remove(temporary, error);
			return false;
		}
	}

	const bool savingVariant = !variantBaseAssetId_.empty();
	SceneDocument rebasedVariant;
	if (savingVariant) {
		// 置換前に最新BaseへOverrideを適用し、欠損Target等のRebase競合を検出する。
		if (!rebasedVariant.LoadInternal(StringUtility::ToUtf8(temporary))) {
			lastSaveError_ = "Failed to rebase Prefab Variant: " +
				rebasedVariant.GetLastLoadError();
			std::filesystem::remove(temporary, error);
			return false;
		}
		rebasedVariant.revision_ = revision_ + 1;
		rebasedVariant.MarkClean();
	}

	if (std::filesystem::exists(target, error) && !error) {
		std::filesystem::copy_file(
			target,
			backup,
			std::filesystem::copy_options::overwrite_existing,
			error
		);
		if (error) {
			std::filesystem::remove(temporary, error);
			return false;
		}
	}

	if (!MoveFileExW(
		temporary.c_str(),
		target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
	)) {
		std::filesystem::remove(temporary, error);
		return false;
	}

	if (isPrefabDocument) {
		PrefabAssetRegistry::Invalidate();
	}
	if (savingVariant) {
		*this = std::move(rebasedVariant);
	} else {
		dirty_ = false;
	}
	lastSaveError_.clear();
	return true;
}

SceneEntity& SceneDocument::CreateEntity(
	const std::string& name,
	uint64_t parentId
) {
	SceneEntity entity{};
	if (variantBaseAssetId_.empty()) {
		entity.id = nextId_++;
	} else {
		entity.id = MakeVariantLocalId(
			assetId_,
			std::to_string(nextId_++)
		);
		while (FindEntity(entity.id)) {
			++entity.id;
		}
	}
	entity.parentId = FindEntity(parentId) ? parentId : 0;
	entity.name = name.empty() ? "Entity" : name;
	entities_.push_back(entity);
	MarkDirty();
	return entities_.back();
}

bool SceneDocument::RemoveEntity(uint64_t id) {
	if (!FindEntity(id)) {
		return false;
	}

	std::unordered_set<uint64_t> removeIds{ id };
	bool foundChild = true;
	while (foundChild) {
		foundChild = false;
		for (const SceneEntity& entity : entities_) {
			if (
				removeIds.contains(entity.parentId) &&
				!removeIds.contains(entity.id)
			) {
				removeIds.insert(entity.id);
				foundChild = true;
			}
		}
	}

	const auto oldSize = entities_.size();
	entities_.erase(
		std::remove_if(
			entities_.begin(),
			entities_.end(),
			[&removeIds](const SceneEntity& entity) {
				return removeIds.contains(entity.id);
			}
		),
		entities_.end()
	);
	if (entities_.size() == oldSize) {
		return false;
	}
	MarkDirty();
	return true;
}

uint64_t SceneDocument::DuplicateEntity(uint64_t id) {
	const SceneEntity* source = FindEntity(id);
	if (!source) {
		return 0;
	}

	const std::vector<SceneEntity> sourceEntities = entities_;
	std::unordered_map<uint64_t, uint64_t> duplicatedIds;
	std::function<uint64_t(uint64_t, uint64_t, bool)> duplicateBranch;
	duplicateBranch = [
		this,
		&sourceEntities,
		&duplicatedIds,
		&duplicateBranch
	](
		uint64_t sourceId,
		uint64_t newParentId,
		bool isRoot
	) -> uint64_t {
		const auto found = std::find_if(
			sourceEntities.begin(),
			sourceEntities.end(),
			[sourceId](const SceneEntity& entity) {
				return entity.id == sourceId;
			}
		);
		if (found == sourceEntities.end()) {
			return 0;
		}

		SceneEntity& duplicate = CreateEntity(
			isRoot ? found->name + " Copy" : found->name,
			newParentId
		);
		const uint64_t duplicateId = duplicate.id;
		duplicatedIds.emplace(sourceId, duplicateId);
		duplicate.folder = found->folder;
		duplicate.folderTeamEnabled = found->folderTeamEnabled;
		duplicate.active = found->active;
		duplicate.locked = found->locked;
		duplicate.teamName = found->teamName;
		duplicate.transform = found->transform;
		duplicate.modelPath = found->modelPath;
		duplicate.spriteTexturePath = found->spriteTexturePath;
		duplicate.spriteSize = found->spriteSize;
		duplicate.spriteAnchor = found->spriteAnchor;
		duplicate.spriteColor = found->spriteColor;
		duplicate.spriteFlipX = found->spriteFlipX;
		duplicate.spriteFlipY = found->spriteFlipY;
		duplicate.components = found->components;
		for (const SceneEntity& child : sourceEntities) {
			if (child.parentId == sourceId) {
				duplicateBranch(child.id, duplicateId, false);
			}
		}
		return duplicateId;
	};

	const uint64_t duplicatedRootId = duplicateBranch(
		id,
		source->parentId,
		true
	);
	if (duplicatedRootId == 0) {
		return 0;
	}
	for (const auto& [sourceId, duplicateId] : duplicatedIds) {
		(void)sourceId;
		SceneEntity* duplicate = FindEntity(duplicateId);
		if (!duplicate) {
			continue;
		}
		for (SceneComponent& component : duplicate->components) {
			// 複製ブランチ内のAttachment等だけ新IDへ向け、外部参照は維持する。
			RemapComponentEntityReferences(component, duplicatedIds, true);
		}
	}
	return duplicatedRootId;
}

bool SceneDocument::SaveEntityBranchAsPrefab(
	uint64_t id,
	const std::string& filePath,
	uint64_t sourceInstanceRootId
) const {
	const SceneEntity* root = FindEntity(id);
	if (!root || filePath.empty()) {
		return false;
	}

	std::vector<const SceneEntity*> branch;
	for (const SceneEntity& entity : entities_) {
		if (entity.id == id || IsDescendantOf(entity.id, id)) {
			branch.push_back(&entity);
		}
	}
	if (branch.empty()) {
		return false;
	}
	if (sourceInstanceRootId == 0) {
		sourceInstanceRootId = root->prefabInstanceRootId;
	}
	std::unordered_set<uint64_t> removedBoundaryRoots;
	if (sourceInstanceRootId != 0) {
		for (const ScenePrefabLink& link : root->prefabLinks) {
			removedBoundaryRoots.insert(link.instanceRootId);
			if (link.instanceRootId == sourceInstanceRootId) {
				break;
			}
		}
	}

	std::unordered_map<uint64_t, uint64_t> idMap;
	std::unordered_set<uint64_t> usedLocalIds;
	uint64_t nextLocalId = 1;
	for (const SceneEntity* entity : branch) {
		const ScenePrefabLink* sourceLink = sourceInstanceRootId != 0
			? FindPrefabLink(*entity, sourceInstanceRootId)
			: nullptr;
		uint64_t localId = sourceLink
			? sourceLink->localId
			: sourceInstanceRootId == 0
				? entity->prefabLocalId
				: uint64_t{};
		if (localId == 0 || !usedLocalIds.insert(localId).second) {
			while (usedLocalIds.contains(nextLocalId)) {
				++nextLocalId;
			}
			localId = nextLocalId++;
			usedLocalIds.insert(localId);
		}
		idMap.emplace(entity->id, localId);
	}

	SceneDocument prefab;
	prefab.Clear(root->name);
	for (const SceneEntity* source : branch) {
		SceneEntity entity = *source;
		entity.id = idMap.at(source->id);
		entity.parentId = source->id == id
			? 0
			: RemapEntityId(source->parentId, idMap);
		entity.runtimeOnly = false;
		// 保存対象より外側のInstance境界はAssetへ持ち込まず、内側だけを維持する。
		EnsurePrefabLinkStack(entity);
		if (!removedBoundaryRoots.empty()) {
			entity.prefabLinks.erase(
				std::remove_if(
					entity.prefabLinks.begin(),
					entity.prefabLinks.end(),
					[&removedBoundaryRoots](const ScenePrefabLink& link) {
						return removedBoundaryRoots.contains(
							link.instanceRootId
						);
					}
				),
				entity.prefabLinks.end()
			);
			SynchronizeActivePrefabLink(entity);
		}
		RemapPrefabLinkRoots(entity, idMap);
		entity.teamName.clear();
		for (SceneComponent& component : entity.components) {
			RemapComponentEntityReferences(component, idMap);
		}
		prefab.GetEntities().push_back(std::move(entity));
	}
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(filePath));
	const std::string resolvedFilePath = StringUtility::ToUtf8(resolvedPath);
	const PrefabAssetReference variantBase =
		PrefabAssetRegistry::ReadVariantBase(resolvedFilePath);
	if (!variantBase.assetId.empty()) {
		// Instance全体ApplyでもVariantを通常Prefabへ置き換えず継承を維持する。
		const std::string resolvedVariantBase =
			PrefabAssetRegistry::ResolvePath(variantBase);
		if (resolvedVariantBase.empty()) {
			return false;
		}
		prefab.assetId_ = PrefabAssetRegistry::ReadAssetId(resolvedFilePath);
		prefab.variantBaseAssetId_ = variantBase.assetId;
		prefab.variantBasePath_ = resolvedVariantBase;
	}
	return prefab.Save(resolvedFilePath);
}

bool SceneDocument::SaveAsPrefabVariant(
	const std::string& filePath,
	const std::string& basePrefabPath
) const {
	if (
		filePath.empty() ||
		basePrefabPath.empty() ||
		!IsPrefabDocumentPath(filePath) ||
		!IsPrefabDocumentPath(basePrefabPath)
	) {
		return false;
	}
	const PrefabAssetReference baseReference =
		PrefabAssetRegistry::CreateReference(basePrefabPath);
	const std::string resolvedBasePath =
		PrefabAssetRegistry::ResolvePath(baseReference);
	if (baseReference.assetId.empty() || resolvedBasePath.empty()) {
		return false;
	}
	const std::filesystem::path resolvedTarget =
		EditableResourcePath::ResolveResource(
			StringUtility::ToPath(filePath)
		).lexically_normal();
	const std::filesystem::path resolvedBase =
		EditableResourcePath::ResolveResource(
			StringUtility::ToPath(resolvedBasePath)
		).lexically_normal();
	if (resolvedTarget == resolvedBase) {
		return false;
	}
	SceneDocument baseSnapshot;
	if (!baseSnapshot.LoadInternal(resolvedBasePath) ||
		baseSnapshot.GetAssetId() != baseReference.assetId) {
		return false;
	}

	SceneDocument variant = *this;
	variant.assetId_ = PrefabAssetRegistry::CreateAssetId();
	variant.variantBaseAssetId_ = baseReference.assetId;
	variant.variantBasePath_ = resolvedBasePath;
	variant.variantBaseSnapshot_ =
		std::make_shared<SceneDocument>(std::move(baseSnapshot));
	std::string variantName = StringUtility::ToUtf8(
		resolvedTarget.filename()
	);
	if (variantName.ends_with(".prefab.json")) {
		variantName.resize(variantName.size() - std::string(".prefab.json").size());
	}
	if (!variantName.empty()) {
		variant.sceneName_ = variantName;
	}
	variant.dirty_ = true;
	return variant.Save(StringUtility::ToUtf8(resolvedTarget));
}

bool SceneDocument::RevertPrefabVariantToBase() {
	if (variantBaseAssetId_.empty()) {
		return false;
	}
	const std::string basePath = PrefabAssetRegistry::ResolvePath(
		variantBaseAssetId_,
		variantBasePath_
	);
	if (basePath.empty()) {
		return false;
	}
	SceneDocument base;
	if (!base.LoadInternal(basePath) ||
		base.GetAssetId() != variantBaseAssetId_) {
		return false;
	}
	const std::shared_ptr<const SceneDocument> baseSnapshot =
		std::make_shared<SceneDocument>(base);
	const std::string variantAssetId = assetId_;
	const std::string baseAssetId = variantBaseAssetId_;
	*this = std::move(base);
	assetId_ = variantAssetId;
	variantBaseAssetId_ = baseAssetId;
	variantBasePath_ = basePath;
	variantBaseSnapshot_ = baseSnapshot;
	MarkDirty();
	return true;
}

std::vector<ScenePrefabPropertyOverride>
SceneDocument::CollectPrefabVariantOverrides() const {
	SceneDocument comparison;
	SceneDocument base;
	uint64_t comparisonRootId = 0;
	if (!BuildVariantComparisonDocument(
		*this,
		comparison,
		base,
		comparisonRootId
	)) {
		return {};
	}
	return comparison.CollectPrefabPropertyOverrides(comparisonRootId);
}

bool SceneDocument::ApplyPrefabVariantOverrideToBase(
	const ScenePrefabPropertyOverride& overrideValue
) {
	if (overrideValue.kind == ScenePrefabOverrideKind::AddedEntity) {
		const std::string basePath = PrefabAssetRegistry::ResolvePath(
			variantBaseAssetId_,
			variantBasePath_
		);
		SceneDocument base;
		const SceneEntity* branchRoot = FindEntity(
			overrideValue.instanceEntityId
		);
		if (basePath.empty() || !branchRoot || branchRoot->parentId == 0 ||
			!base.Load(basePath) || base.GetAssetId() != variantBaseAssetId_) {
			return false;
		}
		std::vector<const SceneEntity*> branch;
		std::unordered_set<uint64_t> branchIds;
		for (const SceneEntity& entity : entities_) {
			if (entity.id == branchRoot->id ||
				IsDescendantOf(entity.id, branchRoot->id)) {
				if (base.FindEntity(entity.id)) {
					return false;
				}
				branch.push_back(&entity);
				branchIds.insert(entity.id);
			}
		}
		for (const SceneEntity* entity : branch) {
			if (entity->parentId != 0 &&
				!branchIds.contains(entity->parentId) &&
				!base.FindEntity(entity->parentId)) {
				return false;
			}
		}
		for (const SceneEntity* source : branch) {
			SceneEntity entity = *source;
			entity.runtimeOnly = false;
			base.GetEntities().push_back(std::move(entity));
		}
		base.MarkDirty();
		if (!base.Save(basePath)) {
			return false;
		}
		// Baseへ移した同一ID Branchを次回SaveでVariant差分から除去する。
		MarkDirty();
		return true;
	}

	SceneDocument comparison;
	SceneDocument base;
	uint64_t comparisonRootId = 0;
	if (!BuildVariantComparisonDocument(
		*this,
		comparison,
		base,
		comparisonRootId
	)) {
		return false;
	}
	if (!comparison.ApplyPrefabPropertyOverride(
		comparisonRootId,
		overrideValue
	)) {
		return false;
	}
	// Baseへ吸収された冗長Overrideを次回SaveでVariant JSONから除去する。
	MarkDirty();
	return true;
}

bool SceneDocument::RevertPrefabVariantOverride(
	const ScenePrefabPropertyOverride& overrideValue
) {
	if (overrideValue.kind == ScenePrefabOverrideKind::AddedEntity) {
		const SceneEntity* branchRoot = FindEntity(
			overrideValue.instanceEntityId
		);
		return branchRoot && branchRoot->parentId != 0
			? RemoveEntity(branchRoot->id)
			: false;
	}

	SceneDocument comparison;
	SceneDocument base;
	uint64_t comparisonRootId = 0;
	if (!BuildVariantComparisonDocument(
		*this,
		comparison,
		base,
		comparisonRootId
	) || !comparison.RevertPrefabPropertyOverride(
		comparisonRootId,
		overrideValue
	)) {
		return false;
	}
	// 比較用Linkを永続化しない。復元EntityはBaseのNested境界へ戻す。
	RestoreVariantPrefabMetadata(comparison, *this, base);
	*this = std::move(comparison);
	MarkDirty();
	return true;
}

uint64_t SceneDocument::InstantiatePrefab(
	const std::string& filePath,
	uint64_t parentId,
	bool runtimeOnly
) {
	if (
		filePath.empty() ||
		(parentId != 0 && !FindEntity(parentId))
	) {
		return 0;
	}
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(filePath));
	const std::string sourcePath = StringUtility::ToUtf8(
		EditableResourcePath::ToProjectRelative(resolvedPath)
	);
	if (
		!prefab.Load(StringUtility::ToUtf8(resolvedPath)) ||
		prefab.GetEntities().empty()
	) {
		return 0;
	}
	std::string sourceAssetId = prefab.GetAssetId();
	if (sourceAssetId.empty() && !runtimeOnly) {
		// 旧Prefabは編集時の初回利用でAsset ID付きへ移行する。
		if (!prefab.Save(StringUtility::ToUtf8(resolvedPath))) {
			return 0;
		}
		sourceAssetId = prefab.GetAssetId();
	}
	if (!assetId_.empty()) {
		if (sourceAssetId == assetId_) {
			return 0;
		}
		PrefabAssetReference variantBase =
			PrefabAssetRegistry::ReadVariantBase(sourcePath);
		std::unordered_set<std::string> visitedVariantBases;
		while (!variantBase.assetId.empty()) {
			if (variantBase.assetId == assetId_ ||
				!visitedVariantBases.insert(variantBase.assetId).second) {
				return 0;
			}
			const std::string variantBasePath =
				PrefabAssetRegistry::ResolvePath(variantBase);
			if (variantBasePath.empty()) {
				return 0;
			}
			variantBase = PrefabAssetRegistry::ReadVariantBase(
				variantBasePath
			);
		}
		for (const SceneEntity& source : prefab.GetEntities()) {
			if (std::any_of(
				source.prefabLinks.begin(),
				source.prefabLinks.end(),
				[this](const ScenePrefabLink& link) {
					return !link.assetId.empty() && link.assetId == assetId_;
				}
			)) {
				return 0;
			}
		}
	}
	if (std::count_if(
		prefab.GetEntities().begin(),
		prefab.GetEntities().end(),
		[](const SceneEntity& entity) { return entity.parentId == 0; }
	) != 1) {
		return 0;
	}

	const bool wasDirty = dirty_;
	std::unordered_map<uint64_t, uint64_t> idMap;
	uint64_t rootId = 0;
	for (const SceneEntity& source : prefab.GetEntities()) {
		SceneEntity& created = CreateEntity(source.name);
		idMap.emplace(source.id, created.id);
		if (source.parentId == 0 && rootId == 0) {
			rootId = created.id;
		}
	}

	for (const SceneEntity& source : prefab.GetEntities()) {
		SceneEntity* destination = FindEntity(idMap.at(source.id));
		if (!destination) {
			continue;
		}
		const uint64_t destinationId = destination->id;
		*destination = source;
		destination->id = destinationId;
		destination->parentId = source.parentId == 0
			? parentId
			: RemapEntityId(source.parentId, idMap);
		destination->runtimeOnly = runtimeOnly;
		RemapPrefabLinkRoots(*destination, idMap);
		destination->prefabLinks.insert(
			destination->prefabLinks.begin(),
			ScenePrefabLink{
				sourceAssetId,
				sourcePath,
				rootId,
				source.id
			}
		);
		SynchronizeActivePrefabLink(*destination);
		for (SceneComponent& component : destination->components) {
			RemapComponentEntityReferences(component, idMap);
		}
	}
	if (runtimeOnly && !wasDirty) {
		MarkClean();
	}
	return rootId;
}

uint64_t SceneDocument::FindPrefabInstanceRoot(uint64_t entityId) const {
	const SceneEntity* entity = FindEntity(entityId);
	if (!entity) {
		return 0;
	}
	std::unordered_set<uint64_t> visited;
	const SceneEntity* current = entity;
	while (current && visited.insert(current->id).second) {
		const uint64_t rootId = current->prefabInstanceRootId;
		const SceneEntity* root = FindEntity(rootId);
		if (
			root &&
			root->prefabInstanceRootId == rootId &&
			HasPrefabAssetLink(*root)
		) {
			return rootId;
		}
		current = FindEntity(current->parentId);
	}
	return 0;
}

std::vector<uint64_t> SceneDocument::CollectPrefabInstanceRoots(
	uint64_t entityId
) const {
	std::vector<uint64_t> roots;
	std::unordered_set<uint64_t> addedRoots;
	std::unordered_set<uint64_t> visitedEntities;
	const SceneEntity* current = FindEntity(entityId);
	while (current && visitedEntities.insert(current->id).second) {
		for (const ScenePrefabLink& link : current->prefabLinks) {
			if (!addedRoots.insert(link.instanceRootId).second) {
				continue;
			}
			const SceneEntity* root = FindEntity(link.instanceRootId);
			if (
				root &&
				FindPrefabLink(*root, link.instanceRootId)
			) {
				roots.push_back(link.instanceRootId);
			}
		}
		current = FindEntity(current->parentId);
	}
	std::stable_sort(
		roots.begin(),
		roots.end(),
		[this](uint64_t leftRootId, uint64_t rightRootId) {
			const SceneEntity* leftRoot = FindEntity(leftRootId);
			const SceneEntity* rightRoot = FindEntity(rightRootId);
			const size_t leftDepth = leftRoot
				? FindPrefabLinkDepth(*leftRoot, leftRootId)
				: 0;
			const size_t rightDepth = rightRoot
				? FindPrefabLinkDepth(*rightRoot, rightRootId)
				: 0;
			return leftDepth < rightDepth;
		}
	);
	return roots;
}

std::vector<std::string> SceneDocument::CollectPrefabInstanceOverrides(
	uint64_t rootId
) const {
	std::vector<std::string> overrides;
	const SceneEntity* instanceRoot = FindEntity(rootId);
	const ScenePrefabLink* instanceRootLink = instanceRoot
		? FindPrefabLink(*instanceRoot, rootId)
		: nullptr;
	if (
		!instanceRoot ||
		!instanceRootLink
	) {
		return overrides;
	}

	const std::string sourcePath = ResolvePrefabAssetPath(*instanceRootLink);
	if (sourcePath.empty()) {
		overrides.push_back("Unable to resolve the linked Prefab asset.");
		return overrides;
	}
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(
			StringUtility::ToPath(sourcePath)
		);
	if (!prefab.Load(StringUtility::ToUtf8(resolvedPath))) {
		overrides.push_back("Unable to load the linked Prefab asset.");
		return overrides;
	}

	std::unordered_map<uint64_t, const SceneEntity*> instanceByLocalId;
	std::unordered_map<uint64_t, uint64_t> sceneToLocalId;
	for (const SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (
			entityLink &&
			MatchesPrefabInstanceLink(entity, rootId, *instanceRootLink) &&
			entityLink->localId != 0
		) {
			instanceByLocalId.emplace(entityLink->localId, &entity);
			sceneToLocalId.emplace(entity.id, entityLink->localId);
		}
	}

	auto makeComparableEntity = [](
		const SceneEntity& entity,
		bool includeTransform
	) {
		json components = json::array();
		for (const SceneComponent& component : entity.components) {
			components.push_back(ComponentToJson(component));
		}
		json value = {
			{ "name", entity.name },
			{ "parentId", entity.parentId },
			{ "folder", entity.folder },
			{ "folderTeamEnabled", entity.folderTeamEnabled },
			{ "active", entity.active },
			{ "locked", entity.locked },
			{ "modelPath", entity.modelPath },
			{ "sprite", {
				{ "texturePath", entity.spriteTexturePath },
				{ "size", VectorToJson(entity.spriteSize) },
				{ "anchor", VectorToJson(entity.spriteAnchor) },
				{ "color", VectorToJson(entity.spriteColor) },
				{ "flipX", entity.spriteFlipX },
				{ "flipY", entity.spriteFlipY }
			} },
			{ "components", std::move(components) }
		};
		if (includeTransform) {
			value["transform"] = {
				{ "scale", VectorToJson(entity.transform.scale) },
				{ "rotation", QuaternionToJson(entity.transform.rotate) },
				{ "translate", VectorToJson(entity.transform.translate) }
			};
		}
		return value;
	};

	std::unordered_set<uint64_t> sourceLocalIds;
	for (const SceneEntity& source : prefab.GetEntities()) {
		sourceLocalIds.insert(source.id);
		const auto found = instanceByLocalId.find(source.id);
		if (found == instanceByLocalId.end()) {
			overrides.push_back("Removed Entity: " + source.name);
			continue;
		}

		SceneEntity normalizedInstance = *found->second;
		normalizedInstance.parentId = RemapEntityId(
			normalizedInstance.parentId,
			sceneToLocalId
		);
		for (SceneComponent& component : normalizedInstance.components) {
			RemapComponentEntityReferences(component, sceneToLocalId);
		}
		const bool isRoot = source.parentId == 0;
		if (
			makeComparableEntity(normalizedInstance, !isRoot) !=
			makeComparableEntity(source, !isRoot)
		) {
			overrides.push_back("Modified Entity: " + source.name);
		}
	}

	for (const auto& [localId, instance] : instanceByLocalId) {
		if (!sourceLocalIds.contains(localId)) {
			overrides.push_back("Stale Entity: " + instance->name);
		}
	}
	for (const SceneEntity& entity : entities_) {
		if (
			entity.id != rootId &&
			!MatchesPrefabInstanceLink(
				entity,
				rootId,
				*instanceRootLink
			) &&
			IsDescendantOf(entity.id, rootId)
		) {
			overrides.push_back("Added Entity: " + entity.name);
		}
	}
	return overrides;
}

std::vector<ScenePrefabPropertyOverride>
SceneDocument::CollectPrefabPropertyOverrides(uint64_t rootId) const {
	std::vector<ScenePrefabPropertyOverride> overrides;
	const SceneEntity* instanceRoot = FindEntity(rootId);
	const ScenePrefabLink* instanceRootLink = instanceRoot
		? FindPrefabLink(*instanceRoot, rootId)
		: nullptr;
	if (
		!instanceRoot ||
		!instanceRootLink
	) {
		return overrides;
	}

	const std::string sourcePath = ResolvePrefabAssetPath(*instanceRootLink);
	if (sourcePath.empty()) {
		return overrides;
	}
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(
			StringUtility::ToPath(sourcePath)
		);
	if (!prefab.Load(StringUtility::ToUtf8(resolvedPath))) {
		return overrides;
	}

	std::unordered_map<uint64_t, const SceneEntity*> instanceByLocalId;
	std::unordered_map<uint64_t, uint64_t> sceneToLocalId;
	for (const SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (
			entityLink &&
			MatchesPrefabInstanceLink(entity, rootId, *instanceRootLink) &&
			entityLink->localId != 0
		) {
			instanceByLocalId.emplace(entityLink->localId, &entity);
			sceneToLocalId.emplace(entity.id, entityLink->localId);
		}
	}

	std::unordered_set<uint64_t> sourceLocalIds;
	for (const SceneEntity& source : prefab.GetEntities()) {
		sourceLocalIds.insert(source.id);
	}

	for (const SceneEntity& source : prefab.GetEntities()) {
		const auto instanceEntry = instanceByLocalId.find(source.id);
		if (instanceEntry == instanceByLocalId.end()) {
			const bool parentIsAlsoRemoved =
				source.parentId != 0 &&
				!instanceByLocalId.contains(source.parentId);
			if (!parentIsAlsoRemoved) {
				ScenePrefabPropertyOverride overrideValue{};
				overrideValue.kind = ScenePrefabOverrideKind::RemovedEntity;
				overrideValue.entityLocalId = source.id;
				overrideValue.entityName = source.name;
				overrideValue.label = "Removed Entity: " + source.name;
				overrides.push_back(std::move(overrideValue));
			}
			continue;
		}

		SceneEntity normalizedInstance = *instanceEntry->second;
		normalizedInstance.parentId = RemapEntityId(
			normalizedInstance.parentId,
			sceneToLocalId
		);
		for (SceneComponent& component : normalizedInstance.components) {
			RemapComponentEntityReferences(component, sceneToLocalId);
		}
		EnsureComponentLocalIds(normalizedInstance);

		const bool isRoot = source.parentId == 0;
		std::vector<std::string> propertyDifferences;
		CollectJsonPropertyDifferences(
			EntityOverridePropertiesToJson(source, !isRoot, !isRoot),
			EntityOverridePropertiesToJson(
				normalizedInstance,
				!isRoot,
				!isRoot
			),
			{},
			false,
			propertyDifferences
		);
		for (const std::string& propertyPath : propertyDifferences) {
			ScenePrefabPropertyOverride overrideValue{};
			overrideValue.kind = ScenePrefabOverrideKind::EntityProperty;
			overrideValue.entityLocalId = source.id;
			overrideValue.entityName = source.name;
			overrideValue.propertyPath = propertyPath;
			overrideValue.label = source.name + " > " +
				FormatOverridePropertyPath(propertyPath);
			overrides.push_back(std::move(overrideValue));
		}

		std::unordered_map<uint64_t, const SceneComponent*> sourceComponents;
		std::unordered_map<uint64_t, const SceneComponent*> instanceComponents;
		for (const SceneComponent& component : source.components) {
			sourceComponents.emplace(component.localId, &component);
		}
		for (const SceneComponent& component : normalizedInstance.components) {
			instanceComponents.emplace(component.localId, &component);
		}

		for (const SceneComponent& sourceComponent : source.components) {
			const auto instanceComponentEntry =
				instanceComponents.find(sourceComponent.localId);
			if (
				instanceComponentEntry == instanceComponents.end() ||
				instanceComponentEntry->second->type != sourceComponent.type
			) {
				ScenePrefabPropertyOverride overrideValue{};
				overrideValue.kind = ScenePrefabOverrideKind::RemovedComponent;
				overrideValue.entityLocalId = source.id;
				overrideValue.componentLocalId = sourceComponent.localId;
				overrideValue.entityName = source.name;
				overrideValue.componentType = sourceComponent.type;
				overrideValue.label = source.name +
					" > Removed Component: " + sourceComponent.type;
				overrides.push_back(std::move(overrideValue));
				continue;
			}

			propertyDifferences.clear();
			CollectJsonPropertyDifferences(
				ComponentToJson(sourceComponent),
				ComponentToJson(*instanceComponentEntry->second),
				{},
				true,
				propertyDifferences
			);
			for (const std::string& propertyPath : propertyDifferences) {
				ScenePrefabPropertyOverride overrideValue{};
				overrideValue.kind = ScenePrefabOverrideKind::ComponentProperty;
				overrideValue.entityLocalId = source.id;
				overrideValue.componentLocalId = sourceComponent.localId;
				overrideValue.entityName = source.name;
				overrideValue.componentType = sourceComponent.type;
				overrideValue.propertyPath = propertyPath;
				overrideValue.label = source.name + " > " +
					sourceComponent.type + "." +
					FormatOverridePropertyPath(propertyPath);
				overrides.push_back(std::move(overrideValue));
			}
		}

		for (const SceneComponent& instanceComponent :
			normalizedInstance.components) {
			const auto sourceComponentEntry =
				sourceComponents.find(instanceComponent.localId);
			if (
				sourceComponentEntry != sourceComponents.end() &&
				sourceComponentEntry->second->type == instanceComponent.type
			) {
				continue;
			}
			ScenePrefabPropertyOverride overrideValue{};
			overrideValue.kind = ScenePrefabOverrideKind::AddedComponent;
			overrideValue.entityLocalId = source.id;
			overrideValue.componentLocalId = instanceComponent.localId;
			overrideValue.entityName = source.name;
			overrideValue.componentType = instanceComponent.type;
			overrideValue.label = source.name +
				" > Added Component: " + instanceComponent.type;
			overrides.push_back(std::move(overrideValue));
		}
	}

	for (const auto& [localId, instance] : instanceByLocalId) {
		if (sourceLocalIds.contains(localId)) {
			continue;
		}
		const SceneEntity* parent = FindEntity(instance->parentId);
		const ScenePrefabLink* parentLink = parent
			? FindPrefabLink(*parent, rootId)
			: nullptr;
		const bool parentIsAlsoStale =
			parentLink &&
			MatchesPrefabInstanceLink(
				*parent,
				rootId,
				*instanceRootLink
			) &&
			parentLink->localId != 0 &&
			!sourceLocalIds.contains(parentLink->localId);
		if (parentIsAlsoStale) {
			continue;
		}
		ScenePrefabPropertyOverride overrideValue{};
		overrideValue.kind = ScenePrefabOverrideKind::StaleEntity;
		overrideValue.entityLocalId = localId;
		overrideValue.instanceEntityId = instance->id;
		overrideValue.entityName = instance->name;
		overrideValue.label = "Stale Entity: " + instance->name;
		overrides.push_back(std::move(overrideValue));
	}

	for (const SceneEntity& entity : entities_) {
		if (
			entity.id == rootId ||
			MatchesPrefabInstanceLink(
				entity,
				rootId,
				*instanceRootLink
			) ||
			!IsDescendantOf(entity.id, rootId)
		) {
			continue;
		}
		const SceneEntity* parent = FindEntity(entity.parentId);
		const bool parentIsAlsoAdded =
			parent &&
			parent->id != rootId &&
			!MatchesPrefabInstanceLink(
				*parent,
				rootId,
				*instanceRootLink
			) &&
			IsDescendantOf(parent->id, rootId);
		if (parentIsAlsoAdded) {
			continue;
		}
		ScenePrefabPropertyOverride overrideValue{};
		overrideValue.kind = ScenePrefabOverrideKind::AddedEntity;
		overrideValue.instanceEntityId = entity.id;
		overrideValue.entityName = entity.name;
		overrideValue.label = "Added Entity: " + entity.name;
		overrides.push_back(std::move(overrideValue));
	}
	return overrides;
}

bool SceneDocument::ApplyPrefabPropertyOverride(
	uint64_t rootId,
	const ScenePrefabPropertyOverride& overrideValue
) {
	SceneEntity* instanceRoot = FindEntity(rootId);
	ScenePrefabLink* instanceRootLink = instanceRoot
		? FindPrefabLink(*instanceRoot, rootId)
		: nullptr;
	if (
		!instanceRoot ||
		!instanceRootLink
	) {
		return false;
	}
	const size_t targetLinkDepth = FindPrefabLinkDepth(
		*instanceRoot,
		rootId
	);
	std::unordered_set<uint64_t> outerBoundaryRoots;
	for (const ScenePrefabLink& link : instanceRoot->prefabLinks) {
		outerBoundaryRoots.insert(link.instanceRootId);
		if (link.instanceRootId == rootId) {
			break;
		}
	}

	const std::string sourcePath = ResolvePrefabAssetPath(*instanceRootLink);
	if (sourcePath.empty()) {
		return false;
	}
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(sourcePath));
	if (!prefab.Load(StringUtility::ToUtf8(resolvedPath))) {
		return false;
	}
	const std::string originalAssetId = instanceRootLink->assetId;
	const std::string originalSourcePath = instanceRootLink->sourcePath;
	auto belongsToInstance = [
		rootId,
		originalAssetId,
		originalSourcePath
	](const SceneEntity& entity) {
		const ScenePrefabLink* link = FindPrefabLink(entity, rootId);
		if (!link) {
			return false;
		}
		return originalAssetId.empty()
			? link->sourcePath == originalSourcePath
			: link->assetId == originalAssetId;
	};
	auto savePrefabAndRefreshLink = [&]() {
		if (!prefab.Save(StringUtility::ToUtf8(resolvedPath))) {
			return false;
		}
		bool metadataChanged = false;
		for (SceneEntity& entity : entities_) {
			if (!belongsToInstance(entity)) {
				continue;
			}
			const ScenePrefabLink* currentLink = FindPrefabLink(
				entity,
				rootId
			);
			if (!currentLink) {
				continue;
			}
			metadataChanged |=
				currentLink->assetId != prefab.GetAssetId() ||
				currentLink->sourcePath != sourcePath;
			SetPrefabLinkAtDepth(
				entity,
				ScenePrefabLink{
					prefab.GetAssetId(),
					sourcePath,
					rootId,
					currentLink->localId
				},
				targetLinkDepth
			);
		}
		if (metadataChanged) {
			MarkDirty();
		}
		return true;
	};
	SceneEntity* sourceEntity = prefab.FindEntity(overrideValue.entityLocalId);
	SceneEntity* instanceEntity = nullptr;
	std::unordered_map<uint64_t, uint64_t> sceneToLocalId;
	for (SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (
			belongsToInstance(entity) &&
			entityLink &&
			entityLink->localId != 0
		) {
			sceneToLocalId.emplace(entity.id, entityLink->localId);
			if (entityLink->localId == overrideValue.entityLocalId) {
				instanceEntity = &entity;
			}
		}
	}

	if (overrideValue.kind == ScenePrefabOverrideKind::RemovedEntity) {
		if (
			!sourceEntity ||
			sourceEntity->parentId == 0 ||
			instanceEntity
		) {
			return false;
		}
		if (!prefab.RemoveEntity(sourceEntity->id)) {
			return false;
		}
		return savePrefabAndRefreshLink();
	}

	if (
		overrideValue.kind == ScenePrefabOverrideKind::AddedEntity ||
		overrideValue.kind == ScenePrefabOverrideKind::StaleEntity
	) {
		SceneEntity* branchRoot = FindEntity(overrideValue.instanceEntityId);
		if (
			!branchRoot ||
			branchRoot->id == rootId ||
			branchRoot->parentId == 0
		) {
			return false;
		}
		if (
			overrideValue.kind == ScenePrefabOverrideKind::AddedEntity &&
			!IsDescendantOf(branchRoot->id, rootId)
		) {
			return false;
		}
		if (
			overrideValue.kind == ScenePrefabOverrideKind::StaleEntity &&
			!belongsToInstance(*branchRoot)
		) {
			return false;
		}

		std::vector<SceneEntity*> branch;
		for (SceneEntity& entity : entities_) {
			if (
				entity.id != branchRoot->id &&
				!IsDescendantOf(entity.id, branchRoot->id)
			) {
				continue;
			}
			const bool validAddedEntity =
				overrideValue.kind == ScenePrefabOverrideKind::AddedEntity &&
				entity.prefabLinks.empty();
			const ScenePrefabLink* entityLink = FindPrefabLink(
				entity,
				rootId
			);
			const bool validStaleEntity =
				overrideValue.kind == ScenePrefabOverrideKind::StaleEntity &&
				belongsToInstance(entity) &&
				entityLink &&
				entityLink->localId != 0 &&
				!prefab.FindEntity(entityLink->localId);
			if (!validAddedEntity && !validStaleEntity) {
				// 異なるPrefab境界を含むBranchは全体Applyへ委ねる。
				return false;
			}
			branch.push_back(&entity);
		}
		if (branch.empty()) {
			return false;
		}
		bool componentIdsChanged = false;
		for (SceneEntity* entity : branch) {
			componentIdsChanged |= EnsureComponentLocalIds(*entity);
		}

		std::unordered_set<uint64_t> usedLocalIds;
		for (const SceneEntity& source : prefab.GetEntities()) {
			usedLocalIds.insert(source.id);
		}
		std::unordered_map<uint64_t, uint64_t> branchLocalIds;
		uint64_t nextLocalId = 1;
		for (const SceneEntity* entity : branch) {
			const ScenePrefabLink* entityLink = FindPrefabLink(
				*entity,
				rootId
			);
			uint64_t localId = 0;
			if (
				overrideValue.kind == ScenePrefabOverrideKind::StaleEntity &&
				entityLink
			) {
				localId = entityLink->localId;
			}
			if (localId == 0 || !usedLocalIds.insert(localId).second) {
				while (usedLocalIds.contains(nextLocalId)) {
					++nextLocalId;
				}
				localId = nextLocalId++;
				usedLocalIds.insert(localId);
			}
			branchLocalIds.emplace(entity->id, localId);
		}

		std::unordered_map<uint64_t, uint64_t> allSceneToLocal =
			sceneToLocalId;
		for (const auto& [sceneId, localId] : branchLocalIds) {
			allSceneToLocal[sceneId] = localId;
		}
		for (const SceneEntity* entity : branch) {
			if (
				entity->parentId != 0 &&
				!allSceneToLocal.contains(entity->parentId)
			) {
				return false;
			}
			if (
				entity->parentId != 0 &&
				!branchLocalIds.contains(entity->parentId) &&
				!prefab.FindEntity(allSceneToLocal.at(entity->parentId))
			) {
				return false;
			}
		}

		for (const SceneEntity* source : branch) {
			SceneEntity entity = *source;
			entity.id = branchLocalIds.at(source->id);
			entity.parentId = source->parentId == 0
				? 0
				: allSceneToLocal.at(source->parentId);
			entity.runtimeOnly = false;
			entity.prefabLinks.erase(
				std::remove_if(
					entity.prefabLinks.begin(),
					entity.prefabLinks.end(),
					[&outerBoundaryRoots](const ScenePrefabLink& link) {
						return outerBoundaryRoots.contains(
							link.instanceRootId
						);
					}
				),
				entity.prefabLinks.end()
			);
			SynchronizeActivePrefabLink(entity);
			RemapPrefabLinkRoots(entity, allSceneToLocal);
			entity.teamName.clear();
			for (SceneComponent& component : entity.components) {
				RemapComponentEntityReferences(component, allSceneToLocal);
			}
			prefab.GetEntities().push_back(std::move(entity));
		}
		if (!savePrefabAndRefreshLink()) {
			return false;
		}

		bool metadataChanged = componentIdsChanged;
		for (SceneEntity* entity : branch) {
			const uint64_t localId = branchLocalIds.at(entity->id);
			const ScenePrefabLink* currentLink = FindPrefabLink(
				*entity,
				rootId
			);
			metadataChanged |=
				!currentLink ||
				currentLink->assetId != prefab.GetAssetId() ||
				currentLink->sourcePath != sourcePath ||
				currentLink->localId != localId;
			SetPrefabLinkAtDepth(
				*entity,
				ScenePrefabLink{
					prefab.GetAssetId(),
					sourcePath,
					rootId,
					localId
				},
				FindPrefabLinkInsertionDepth(
					*entity,
					*instanceRoot,
					rootId
				)
			);
		}
		if (metadataChanged) {
			MarkDirty();
		}
		return true;
	}

	if (!sourceEntity || !instanceEntity) {
		return false;
	}

	SceneEntity normalizedInstance = *instanceEntity;
	normalizedInstance.parentId = RemapEntityId(
		normalizedInstance.parentId,
		sceneToLocalId
	);
	for (SceneComponent& component : normalizedInstance.components) {
		RemapComponentEntityReferences(component, sceneToLocalId);
	}
	EnsureComponentLocalIds(normalizedInstance);

	if (overrideValue.kind == ScenePrefabOverrideKind::EntityProperty) {
		const bool isRoot = sourceEntity->parentId == 0;
		if (
			overrideValue.propertyPath == "/parentId" &&
			normalizedInstance.parentId != 0 &&
			!prefab.FindEntity(normalizedInstance.parentId)
		) {
			// 追加Entityを親にする変更は、そのEntityも保存する全体Applyで扱う。
			return false;
		}
		json sourceProperties = EntityOverridePropertiesToJson(
			*sourceEntity,
			!isRoot,
			!isRoot
		);
		const json instanceProperties = EntityOverridePropertiesToJson(
			normalizedInstance,
			!isRoot,
			!isRoot
		);
		if (!CopyJsonProperty(
			sourceProperties,
			instanceProperties,
			overrideValue.propertyPath
		)) {
			return false;
		}
		try {
			const json::json_pointer pointer(overrideValue.propertyPath);
			if (!SetEntityOverrideProperty(
				*sourceEntity,
				overrideValue.propertyPath,
				sourceProperties.at(pointer)
			)) {
				return false;
			}
		}
		catch (const json::exception&) {
			return false;
		}
		if (isRoot && overrideValue.propertyPath == "/name") {
			prefab.SetSceneName(sourceEntity->name);
		}
	} else if (
		overrideValue.kind == ScenePrefabOverrideKind::ComponentProperty
	) {
		SceneComponent* sourceComponent = FindComponentByLocalId(
			*sourceEntity,
			overrideValue.componentLocalId
		);
		const SceneComponent* instanceComponent = FindComponentByLocalId(
			normalizedInstance,
			overrideValue.componentLocalId
		);
		if (
			!sourceComponent ||
			!instanceComponent ||
			sourceComponent->type != instanceComponent->type
		) {
			return false;
		}
		json sourceValue = ComponentToJson(*sourceComponent);
		if (!CopyJsonProperty(
			sourceValue,
			ComponentToJson(*instanceComponent),
			overrideValue.propertyPath
		)) {
			return false;
		}
		SceneComponent replacement{};
		if (!ComponentFromJson(sourceValue, replacement)) {
			return false;
		}
		*sourceComponent = std::move(replacement);
	} else if (overrideValue.kind == ScenePrefabOverrideKind::AddedComponent) {
		const SceneComponent* instanceComponent = FindComponentByLocalId(
			normalizedInstance,
			overrideValue.componentLocalId
		);
		if (!instanceComponent) {
			return false;
		}
		sourceEntity->components.erase(
			std::remove_if(
				sourceEntity->components.begin(),
				sourceEntity->components.end(),
				[&overrideValue](const SceneComponent& component) {
					return component.localId == overrideValue.componentLocalId;
				}
			),
			sourceEntity->components.end()
		);
		if (FindComponent(*sourceEntity, instanceComponent->type.c_str())) {
			return false;
		}
		sourceEntity->components.push_back(*instanceComponent);
	} else if (overrideValue.kind == ScenePrefabOverrideKind::RemovedComponent) {
		const auto oldSize = sourceEntity->components.size();
		sourceEntity->components.erase(
			std::remove_if(
				sourceEntity->components.begin(),
				sourceEntity->components.end(),
				[&overrideValue](const SceneComponent& component) {
					return component.localId == overrideValue.componentLocalId;
				}
			),
			sourceEntity->components.end()
		);
		if (sourceEntity->components.size() == oldSize) {
			return false;
		}
	} else {
		return false;
	}

	SynchronizeLegacyRendererFields(*sourceEntity);
	return savePrefabAndRefreshLink();
}

bool SceneDocument::RevertPrefabPropertyOverride(
	uint64_t rootId,
	const ScenePrefabPropertyOverride& overrideValue
) {
	SceneEntity* instanceRoot = FindEntity(rootId);
	ScenePrefabLink* instanceRootLink = instanceRoot
		? FindPrefabLink(*instanceRoot, rootId)
		: nullptr;
	if (
		!instanceRoot ||
		!instanceRootLink
	) {
		return false;
	}
	const size_t targetLinkDepth = FindPrefabLinkDepth(
		*instanceRoot,
		rootId
	);

	const std::string sourcePath = ResolvePrefabAssetPath(*instanceRootLink);
	if (sourcePath.empty()) {
		return false;
	}
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(sourcePath));
	if (!prefab.Load(StringUtility::ToUtf8(resolvedPath))) {
		return false;
	}
	const std::string originalAssetId = instanceRootLink->assetId;
	const std::string originalSourcePath = instanceRootLink->sourcePath;
	auto belongsToInstance = [
		rootId,
		originalAssetId,
		originalSourcePath
	](const SceneEntity& entity) {
		const ScenePrefabLink* link = FindPrefabLink(entity, rootId);
		if (!link) {
			return false;
		}
		return originalAssetId.empty()
			? link->sourcePath == originalSourcePath
			: link->assetId == originalAssetId;
	};
	auto refreshSceneLink = [&]() {
		bool changed = false;
		for (SceneEntity& entity : entities_) {
			if (!belongsToInstance(entity)) {
				continue;
			}
			const ScenePrefabLink* currentLink = FindPrefabLink(
				entity,
				rootId
			);
			if (!currentLink) {
				continue;
			}
			changed |=
				currentLink->assetId != prefab.GetAssetId() ||
				currentLink->sourcePath != sourcePath;
			SetPrefabLinkAtDepth(
				entity,
				ScenePrefabLink{
					prefab.GetAssetId(),
					sourcePath,
					rootId,
					currentLink->localId
				},
				targetLinkDepth
			);
		}
		return changed;
	};
	const SceneEntity* sourceEntity = prefab.FindEntity(
		overrideValue.entityLocalId
	);
	SceneEntity* instanceEntity = nullptr;
	std::unordered_map<uint64_t, uint64_t> localToSceneId;
	for (SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (
			belongsToInstance(entity) &&
			entityLink &&
			entityLink->localId != 0
		) {
			localToSceneId.emplace(entityLink->localId, entity.id);
			if (entityLink->localId == overrideValue.entityLocalId) {
				instanceEntity = &entity;
			}
		}
	}

	if (
		overrideValue.kind == ScenePrefabOverrideKind::AddedEntity ||
		overrideValue.kind == ScenePrefabOverrideKind::StaleEntity
	) {
		const SceneEntity* branchRoot = FindEntity(overrideValue.instanceEntityId);
		if (
			!branchRoot ||
			branchRoot->id == rootId ||
			!IsDescendantOf(branchRoot->id, rootId)
		) {
			return false;
		}
		const bool validAddedEntity =
			overrideValue.kind == ScenePrefabOverrideKind::AddedEntity &&
			!belongsToInstance(*branchRoot);
		const ScenePrefabLink* branchRootLink = FindPrefabLink(
			*branchRoot,
			rootId
		);
		const bool validStaleEntity =
			overrideValue.kind == ScenePrefabOverrideKind::StaleEntity &&
			belongsToInstance(*branchRoot) &&
			branchRootLink &&
			branchRootLink->localId != 0 &&
			!prefab.FindEntity(branchRootLink->localId);
		if (!validAddedEntity && !validStaleEntity) {
			return false;
		}
		for (const SceneEntity& entity : entities_) {
			if (
				entity.id != branchRoot->id &&
				!IsDescendantOf(entity.id, branchRoot->id)
			) {
				continue;
			}
			const bool validAddedBranchEntity =
				overrideValue.kind == ScenePrefabOverrideKind::AddedEntity &&
				!belongsToInstance(entity);
			const ScenePrefabLink* entityLink = FindPrefabLink(
				entity,
				rootId
			);
			const bool validStaleBranchEntity =
				overrideValue.kind == ScenePrefabOverrideKind::StaleEntity &&
				belongsToInstance(entity) &&
				entityLink &&
				entityLink->localId != 0 &&
				!prefab.FindEntity(entityLink->localId);
			if (!validAddedBranchEntity && !validStaleBranchEntity) {
				return false;
			}
		}
		if (!RemoveEntity(branchRoot->id)) {
			return false;
		}
		refreshSceneLink();
		return true;
	}

	if (overrideValue.kind == ScenePrefabOverrideKind::RemovedEntity) {
		if (
			!sourceEntity ||
			sourceEntity->parentId == 0 ||
			instanceEntity
		) {
			return false;
		}

		std::vector<const SceneEntity*> sourceBranch;
		std::unordered_set<uint64_t> sourceBranchIds;
		for (const SceneEntity& source : prefab.GetEntities()) {
			if (
				source.id == sourceEntity->id ||
				prefab.IsDescendantOf(source.id, sourceEntity->id)
			) {
				sourceBranch.push_back(&source);
				sourceBranchIds.insert(source.id);
			}
		}
		if (sourceBranch.empty()) {
			return false;
		}
		for (const SceneEntity* source : sourceBranch) {
			if (localToSceneId.contains(source->id)) {
				return false;
			}
			if (
				source->parentId != 0 &&
				!sourceBranchIds.contains(source->parentId) &&
				!localToSceneId.contains(source->parentId)
			) {
				return false;
			}
		}

		const bool runtimeOnly = instanceRoot->runtimeOnly;
		for (const SceneEntity* source : sourceBranch) {
			SceneEntity& created = CreateEntity(source->name);
			localToSceneId.emplace(source->id, created.id);
		}
		for (const SceneEntity* source : sourceBranch) {
			SceneEntity* destination = FindEntity(localToSceneId.at(source->id));
			if (!destination) {
				return false;
			}
			const uint64_t destinationId = destination->id;
			*destination = *source;
			destination->id = destinationId;
			destination->parentId = source->parentId == 0
				? 0
				: localToSceneId.at(source->parentId);
			destination->runtimeOnly = runtimeOnly;
			RemapPrefabLinkRoots(*destination, localToSceneId);
			SetPrefabLinkAtDepth(
				*destination,
				ScenePrefabLink{
					prefab.GetAssetId(),
					sourcePath,
					rootId,
					source->id
				},
				0
			);
			for (SceneComponent& component : destination->components) {
				RemapComponentEntityReferences(component, localToSceneId);
			}
			SynchronizeLegacyRendererFields(*destination);
		}
		refreshSceneLink();
		MarkDirty();
		return true;
	}

	if (!sourceEntity || !instanceEntity) {
		return false;
	}

	if (overrideValue.kind == ScenePrefabOverrideKind::EntityProperty) {
		const bool isRoot = sourceEntity->parentId == 0;
		const json sourceProperties = EntityOverridePropertiesToJson(
			*sourceEntity,
			!isRoot,
			!isRoot
		);
		try {
			const json::json_pointer pointer(overrideValue.propertyPath);
			json sourceValue = sourceProperties.at(pointer);
			if (overrideValue.propertyPath == "/parentId") {
				const uint64_t localParentId = sourceValue.get<uint64_t>();
				if (localParentId == 0) {
					sourceValue = uint64_t{};
				} else {
					const auto parentEntry = localToSceneId.find(localParentId);
					if (parentEntry == localToSceneId.end()) {
						return false;
					}
					sourceValue = parentEntry->second;
				}
			}
			if (!SetEntityOverrideProperty(
				*instanceEntity,
				overrideValue.propertyPath,
				sourceValue
			)) {
				return false;
			}
		}
		catch (const json::exception&) {
			return false;
		}
	} else if (
		overrideValue.kind == ScenePrefabOverrideKind::ComponentProperty
	) {
		const SceneComponent* sourceComponent = FindComponentByLocalId(
			*sourceEntity,
			overrideValue.componentLocalId
		);
		SceneComponent* instanceComponent = FindComponentByLocalId(
			*instanceEntity,
			overrideValue.componentLocalId
		);
		if (
			!sourceComponent ||
			!instanceComponent ||
			sourceComponent->type != instanceComponent->type
		) {
			return false;
		}
		SceneComponent sourceInScene = *sourceComponent;
		RemapComponentEntityReferences(sourceInScene, localToSceneId);
		json instanceValue = ComponentToJson(*instanceComponent);
		if (!CopyJsonProperty(
			instanceValue,
			ComponentToJson(sourceInScene),
			overrideValue.propertyPath
		)) {
			return false;
		}
		SceneComponent replacement{};
		if (!ComponentFromJson(instanceValue, replacement)) {
			return false;
		}
		*instanceComponent = std::move(replacement);
	} else if (overrideValue.kind == ScenePrefabOverrideKind::AddedComponent) {
		const auto oldSize = instanceEntity->components.size();
		instanceEntity->components.erase(
			std::remove_if(
				instanceEntity->components.begin(),
				instanceEntity->components.end(),
				[&overrideValue](const SceneComponent& component) {
					return component.localId == overrideValue.componentLocalId;
				}
			),
			instanceEntity->components.end()
		);
		if (instanceEntity->components.size() == oldSize) {
			return false;
		}
	} else if (overrideValue.kind == ScenePrefabOverrideKind::RemovedComponent) {
		const SceneComponent* sourceComponent = FindComponentByLocalId(
			*sourceEntity,
			overrideValue.componentLocalId
		);
		if (!sourceComponent) {
			return false;
		}
		const SceneComponent* duplicateType = FindComponent(
			*instanceEntity,
			sourceComponent->type.c_str()
		);
		if (
			duplicateType &&
			duplicateType->localId != overrideValue.componentLocalId
		) {
			return false;
		}
		instanceEntity->components.erase(
			std::remove_if(
				instanceEntity->components.begin(),
				instanceEntity->components.end(),
				[&overrideValue](const SceneComponent& component) {
					return component.localId == overrideValue.componentLocalId;
				}
			),
			instanceEntity->components.end()
		);
		SceneComponent replacement = *sourceComponent;
		RemapComponentEntityReferences(replacement, localToSceneId);
		instanceEntity->components.push_back(std::move(replacement));
	} else {
		return false;
	}

	SynchronizeLegacyRendererFields(*instanceEntity);
	refreshSceneLink();
	MarkDirty();
	return true;
}

bool SceneDocument::ApplyPrefabInstance(uint64_t rootId) {
	SceneEntity* root = FindEntity(rootId);
	ScenePrefabLink* rootLink = root
		? FindPrefabLink(*root, rootId)
		: nullptr;
	if (
		!root ||
		!rootLink
	) {
		return false;
	}
	std::vector<SceneEntity*> branch;
	for (SceneEntity& entity : entities_) {
		if (entity.id == rootId || IsDescendantOf(entity.id, rootId)) {
			branch.push_back(&entity);
		}
	}
	std::unordered_map<uint64_t, uint64_t> localIds;
	std::unordered_set<uint64_t> usedLocalIds;
	uint64_t nextLocalId = 1;
	for (SceneEntity* entity : branch) {
		const ScenePrefabLink* entityLink = FindPrefabLink(
			*entity,
			rootId
		);
		uint64_t localId = entityLink ? entityLink->localId : 0;
		if (localId == 0 || !usedLocalIds.insert(localId).second) {
			while (usedLocalIds.contains(nextLocalId)) {
				++nextLocalId;
			}
			localId = nextLocalId++;
			usedLocalIds.insert(localId);
		}
		localIds.emplace(entity->id, localId);
	}
	const std::string sourcePath = ResolvePrefabAssetPath(*rootLink);
	if (sourcePath.empty()) {
		return false;
	}
	if (!SaveEntityBranchAsPrefab(rootId, sourcePath, rootId)) {
		return false;
	}
	const std::string sourceAssetId = PrefabAssetRegistry::ReadAssetId(
		sourcePath
	);

	bool metadataChanged = false;
	for (SceneEntity* entity : branch) {
		const uint64_t localId = localIds.at(entity->id);
		const ScenePrefabLink* currentLink = FindPrefabLink(*entity, rootId);
		metadataChanged |=
			!currentLink ||
			currentLink->assetId != sourceAssetId ||
			currentLink->sourcePath != sourcePath ||
			currentLink->localId != localId;
		SetPrefabLinkAtDepth(
			*entity,
			ScenePrefabLink{ sourceAssetId, sourcePath, rootId, localId },
			FindPrefabLinkInsertionDepth(*entity, *root, rootId)
		);
	}
	if (metadataChanged) {
		MarkDirty();
	}
	return true;
}

bool SceneDocument::RevertPrefabInstance(uint64_t rootId) {
	SceneEntity* instanceRoot = FindEntity(rootId);
	ScenePrefabLink* instanceRootLink = instanceRoot
		? FindPrefabLink(*instanceRoot, rootId)
		: nullptr;
	if (
		!instanceRoot ||
		!instanceRootLink
	) {
		return false;
	}

	const std::string sourcePath = ResolvePrefabAssetPath(*instanceRootLink);
	if (sourcePath.empty()) {
		return false;
	}
	const std::string originalAssetId = instanceRootLink->assetId;
	const std::string originalSourcePath = instanceRootLink->sourcePath;
	auto belongsToInstance = [
		rootId,
		originalAssetId,
		originalSourcePath
	](const SceneEntity& entity) {
		const ScenePrefabLink* link = FindPrefabLink(entity, rootId);
		if (!link) {
			return false;
		}
		return originalAssetId.empty()
			? link->sourcePath == originalSourcePath
			: link->assetId == originalAssetId;
	};
	SceneDocument prefab;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(sourcePath));
	if (
		!prefab.Load(StringUtility::ToUtf8(resolvedPath)) ||
		prefab.GetEntities().empty()
	) {
		return false;
	}
	if (std::count_if(
		prefab.GetEntities().begin(),
		prefab.GetEntities().end(),
		[](const SceneEntity& entity) { return entity.parentId == 0; }
	) != 1) {
		return false;
	}

	const uint64_t preservedParentId = instanceRoot->parentId;
	const QuaternionTransform preservedRootTransform = instanceRoot->transform;
	const bool preservedRuntimeOnly = instanceRoot->runtimeOnly;
	const SceneEntity* prefabRoot = nullptr;
	for (const SceneEntity& source : prefab.GetEntities()) {
		if (source.parentId == 0) {
			prefabRoot = &source;
			break;
		}
	}
	if (!prefabRoot) {
		return false;
	}

	std::unordered_map<uint64_t, uint64_t> localToSceneId;
	for (const SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		if (
			belongsToInstance(entity) &&
			entityLink &&
			entityLink->localId != 0
		) {
			localToSceneId.emplace(entityLink->localId, entity.id);
		}
	}
	localToSceneId[prefabRoot->id] = rootId;

	for (const SceneEntity& source : prefab.GetEntities()) {
		if (localToSceneId.contains(source.id)) {
			continue;
		}
		SceneEntity& created = CreateEntity(source.name);
		localToSceneId.emplace(source.id, created.id);
	}

	std::unordered_set<uint64_t> sourceLocalIds;
	for (const SceneEntity& source : prefab.GetEntities()) {
		sourceLocalIds.insert(source.id);
		SceneEntity* destination = FindEntity(localToSceneId.at(source.id));
		if (!destination) {
			continue;
		}
		std::vector<ScenePrefabLink> preservedOuterLinks;
		if (FindPrefabLink(*destination, rootId)) {
			const size_t destinationDepth = FindPrefabLinkDepth(
				*destination,
				rootId
			);
			preservedOuterLinks.assign(
				destination->prefabLinks.begin(),
				destination->prefabLinks.begin() + destinationDepth
			);
		}
		const uint64_t destinationId = destination->id;
		*destination = source;
		destination->id = destinationId;
		destination->parentId = source.parentId == 0
			? preservedParentId
			: RemapEntityId(source.parentId, localToSceneId);
		destination->runtimeOnly = preservedRuntimeOnly;
		RemapPrefabLinkRoots(*destination, localToSceneId);
		destination->prefabLinks.insert(
			destination->prefabLinks.begin(),
			preservedOuterLinks.begin(),
			preservedOuterLinks.end()
		);
		SetPrefabLinkAtDepth(
			*destination,
			ScenePrefabLink{
				prefab.GetAssetId(),
				sourcePath,
				rootId,
				source.id
			},
			preservedOuterLinks.size()
		);
		for (SceneComponent& component : destination->components) {
			RemapComponentEntityReferences(component, localToSceneId);
		}
		if (source.id == prefabRoot->id) {
			destination->transform = preservedRootTransform;
		}
	}

	std::unordered_set<uint64_t> removeIds;
	for (const SceneEntity& entity : entities_) {
		const ScenePrefabLink* entityLink = FindPrefabLink(entity, rootId);
		const bool removedFromPrefab =
			entity.id != rootId &&
			belongsToInstance(entity) &&
			entityLink &&
			!sourceLocalIds.contains(entityLink->localId);
		const bool addedToInstance =
			entity.id != rootId &&
			!belongsToInstance(entity) &&
			IsDescendantOf(entity.id, rootId);
		if (removedFromPrefab || addedToInstance) {
			removeIds.insert(entity.id);
		}
	}
	for (uint64_t removeId : removeIds) {
		const SceneEntity* entity = FindEntity(removeId);
		if (entity && !removeIds.contains(entity->parentId)) {
			RemoveEntity(removeId);
		}
	}

	MarkDirty();
	return true;
}

bool SceneDocument::UnpackPrefabInstance(uint64_t rootId) {
	const SceneEntity* root = FindEntity(rootId);
	if (!root || !FindPrefabLink(*root, rootId)) {
		return false;
	}
	bool changed = false;
	for (SceneEntity& entity : entities_) {
		if (!FindPrefabLink(entity, rootId)) {
			continue;
		}
		RemovePrefabLink(entity, rootId);
		changed = true;
	}
	if (changed) {
		MarkDirty();
	}
	return changed;
}

bool SceneDocument::SetParent(uint64_t id, uint64_t parentId) {
	SceneEntity* entity = FindEntity(id);
	if (!entity || id == parentId) {
		return false;
	}
	if (parentId != 0 && !FindEntity(parentId)) {
		return false;
	}
	if (parentId != 0 && IsDescendantOf(parentId, id)) {
		return false;
	}
	if (entity->parentId == parentId) {
		return true;
	}
	// 親変更後も見た目の位置を維持するため、変更前のワールド行列を基準にする。
	const Matrix4x4 worldMatrix = ResolveSceneWorldMatrix(*this, *entity);
	Matrix4x4 localMatrix = worldMatrix;
	if (const SceneEntity* newParent = FindEntity(parentId)) {
		const Matrix4x4 parentWorld =
			ResolveSceneWorldMatrix(*this, *newParent);
		if (std::abs(Determinant(parentWorld)) < 0.000001f) {
			return false;
		}
		localMatrix = Multiply(
			worldMatrix,
			Inverse(parentWorld)
		);
	}
	Vector3 localScale{};
	Quaternion localRotate = MakeIdentityQuaternion();
	Vector3 localTranslate{};
	if (!DecomposeAffineMatrix(
		localMatrix,
		localScale,
		localRotate,
		localTranslate
	)) {
		return false;
	}
	entity->parentId = parentId;
	entity->transform.scale = localScale;
	entity->transform.rotate = localRotate;
	entity->transform.translate = localTranslate;
	MarkDirty();
	return true;
}

bool SceneDocument::MoveEntity(uint64_t id, int direction) {
	if (direction == 0) {
		return false;
	}
	const SceneEntity* entity = FindEntity(id);
	if (!entity) {
		return false;
	}
	const uint64_t parentId = entity->parentId;
	std::vector<size_t> siblingIndices;
	for (size_t index = 0; index < entities_.size(); ++index) {
		if (entities_[index].parentId == parentId) {
			siblingIndices.push_back(index);
		}
	}
	const auto sibling = std::find_if(
		siblingIndices.begin(),
		siblingIndices.end(),
		[this, id](size_t index) { return entities_[index].id == id; }
	);
	if (sibling == siblingIndices.end()) {
		return false;
	}
	const std::ptrdiff_t position = std::distance(
		siblingIndices.begin(),
		sibling
	);
	const std::ptrdiff_t targetPosition = position + (direction < 0 ? -1 : 1);
	if (
		targetPosition < 0 ||
		targetPosition >= static_cast<std::ptrdiff_t>(siblingIndices.size())
	) {
		return false;
	}
	std::swap(
		entities_[siblingIndices[position]],
		entities_[siblingIndices[targetPosition]]
	);
	MarkDirty();
	return true;
}

bool SceneDocument::MoveEntityToParent(uint64_t id, uint64_t parentId) {
	if (id == 0 || id == parentId) {
		return false;
	}
	if (parentId != 0 && !FindEntity(parentId)) {
		return false;
	}
	if (parentId != 0 && IsDescendantOf(parentId, id)) {
		return false;
	}

	SceneEntity* entity = FindEntity(id);
	if (!entity) {
		return false;
	}

	const bool parentChanged = entity->parentId != parentId;
	if (parentChanged && !SetParent(id, parentId)) {
		return false;
	}

	const auto sourceIt = std::find_if(
		entities_.begin(),
		entities_.end(),
		[id](const SceneEntity& candidate) {
			return candidate.id == id;
		}
	);
	if (sourceIt == entities_.end()) {
		return false;
	}

	size_t sourceIndex =
		static_cast<size_t>(std::distance(entities_.begin(), sourceIt));
	size_t insertIndex = 0;
	bool foundSibling = false;
	bool siblingAfterSource = false;
	for (size_t index = 0; index < entities_.size(); ++index) {
		if (index != sourceIndex && entities_[index].parentId == parentId) {
			insertIndex = index + 1;
			foundSibling = true;
			if (index > sourceIndex) {
				siblingAfterSource = true;
			}
		}
	}
	if (!foundSibling) {
		insertIndex = entities_.size();
	}
	if (!parentChanged && !siblingAfterSource) {
		return false;
	}

	if (insertIndex == sourceIndex || insertIndex == sourceIndex + 1) {
		if (!parentChanged) {
			return false;
		}
		return true;
	}

	SceneEntity moved = std::move(entities_[sourceIndex]);
	entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
	if (sourceIndex < insertIndex) {
		--insertIndex;
	}
	insertIndex = (std::min)(insertIndex, entities_.size());
	entities_.insert(
		entities_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
		std::move(moved)
	);
	MarkDirty();
	return true;
}

bool SceneDocument::MoveEntityToSibling(
	uint64_t id,
	uint64_t siblingId,
	bool after
) {
	if (id == 0 || siblingId == 0 || id == siblingId) {
		return false;
	}
	SceneEntity* entity = FindEntity(id);
	const SceneEntity* sibling = FindEntity(siblingId);
	if (!entity || !sibling) {
		return false;
	}
	const uint64_t targetParentId = sibling->parentId;
	if (targetParentId != 0 && IsDescendantOf(targetParentId, id)) {
		return false;
	}
	if (entity->parentId != targetParentId) {
		if (!SetParent(id, targetParentId)) {
			return false;
		}
	}

	const auto sourceIt = std::find_if(
		entities_.begin(),
		entities_.end(),
		[id](const SceneEntity& candidate) {
			return candidate.id == id;
		}
	);
	const auto targetIt = std::find_if(
		entities_.begin(),
		entities_.end(),
		[siblingId](const SceneEntity& candidate) {
			return candidate.id == siblingId;
		}
	);
	if (sourceIt == entities_.end() || targetIt == entities_.end()) {
		return false;
	}

	size_t sourceIndex =
		static_cast<size_t>(std::distance(entities_.begin(), sourceIt));
	size_t targetIndex =
		static_cast<size_t>(std::distance(entities_.begin(), targetIt));
	SceneEntity moved = std::move(entities_[sourceIndex]);
	entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
	if (sourceIndex < targetIndex) {
		--targetIndex;
	}
	size_t insertIndex = targetIndex + (after ? 1u : 0u);
	insertIndex = (std::min)(insertIndex, entities_.size());
	entities_.insert(
		entities_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
		std::move(moved)
	);
	MarkDirty();
	return true;
}

SceneTeamSettings& SceneDocument::CreateTeam(const std::string& name) {
	SceneTeamSettings team{};
	team.name = MakeUniqueTeamName(teams_, name);
	NormalizeTeamSettings(team);
	teams_.push_back(std::move(team));
	MarkDirty();
	return teams_.back();
}

bool SceneDocument::RenameTeam(
	const std::string& oldName,
	const std::string& newName
) {
	if (oldName.empty()) {
		return false;
	}
	SceneTeamSettings* team = FindTeam(oldName);
	if (!team) {
		return false;
	}
	const std::string resolvedName = MakeUniqueTeamName(
		teams_,
		newName,
		oldName
	);
	if (resolvedName == oldName) {
		return true;
	}
	team->name = resolvedName;
	for (SceneEntity& entity : entities_) {
		if (entity.teamName == oldName) {
			entity.teamName = resolvedName;
		}
	}
	MarkDirty();
	return true;
}

bool SceneDocument::RemoveTeam(const std::string& name) {
	const auto oldSize = teams_.size();
	teams_.erase(
		std::remove_if(
			teams_.begin(),
			teams_.end(),
			[&](const SceneTeamSettings& team) {
				return team.name == name;
			}
		),
		teams_.end()
	);
	if (teams_.size() == oldSize) {
		return false;
	}
	for (SceneEntity& entity : entities_) {
		if (entity.teamName == name) {
			entity.teamName.clear();
			entity.folderTeamEnabled = false;
		}
	}
	MarkDirty();
	return true;
}

SceneTeamSettings* SceneDocument::FindTeam(const std::string& name) {
	const auto found = std::find_if(
		teams_.begin(),
		teams_.end(),
		[&](const SceneTeamSettings& team) {
			return team.name == name;
		}
	);
	return found == teams_.end() ? nullptr : &(*found);
}

const SceneTeamSettings* SceneDocument::FindTeam(
	const std::string& name
) const {
	const auto found = std::find_if(
		teams_.begin(),
		teams_.end(),
		[&](const SceneTeamSettings& team) {
			return team.name == name;
		}
	);
	return found == teams_.end() ? nullptr : &(*found);
}

std::string SceneDocument::ResolveInheritedFolderTeamName(
	uint64_t entityId
) const {
	const SceneEntity* entity = FindEntity(entityId);
	if (!entity) {
		return {};
	}
	std::unordered_set<uint64_t> visited;
	uint64_t parentId = entity->parentId;
	while (parentId != 0 && visited.insert(parentId).second) {
		const SceneEntity* parent = FindEntity(parentId);
		if (!parent) {
			break;
		}
		if (
			parent->folder &&
			parent->folderTeamEnabled &&
			!parent->teamName.empty() &&
			FindTeam(parent->teamName)
		) {
			return parent->teamName;
		}
		parentId = parent->parentId;
	}
	return {};
}

const SceneTeamSettings* SceneDocument::ResolveEntityTeam(
	const SceneEntity& entity
) const {
	if (!entity.teamName.empty()) {
		if (const SceneTeamSettings* team = FindTeam(entity.teamName)) {
			return team;
		}
	}
	const std::string inheritedName = ResolveInheritedFolderTeamName(entity.id);
	return inheritedName.empty() ? nullptr : FindTeam(inheritedName);
}

bool SceneDocument::AddComponent(uint64_t id, const std::string& type) {
	if (type.empty()) {
		return false;
	}
	SceneEntity* entity = FindEntity(id);
	if (!entity) {
		return false;
	}
	if (entity->folder) {
		return false;
	}
	const bool componentIdsChanged = EnsureComponentLocalIds(*entity);
	const auto found = std::find_if(
		entity->components.begin(),
		entity->components.end(),
		[&type](const SceneComponent& component) {
			return component.type == type;
		}
	);
	if (found != entity->components.end()) {
		bool changed = componentIdsChanged;
		if (type == "MeshRenderer" && found->modelPath.empty()) {
			found->modelPath = entity->modelPath;
			changed = true;
		} else if (type == "MeshRenderer" && found->meshCullMode.empty()) {
			found->meshCullMode = "Back";
			changed = true;
		} else if (type == "MeshRenderer") {
			const float reflectionIntensity = std::clamp(
				found->meshEnvironmentReflectionIntensity,
				0.0f,
				1.0f
			);
			if (
				found->meshEnvironmentReflectionIntensity !=
				reflectionIntensity
			) {
				found->meshEnvironmentReflectionIntensity = reflectionIntensity;
				changed = true;
			}
		} else if (type == "Environment") {
			if (found->environmentSkyboxPath.empty()) {
				found->environmentSkyboxPath = "resources/rostock_laage_airport_4k.dds";
				changed = true;
			}
			if (found->environmentSkyboxIntensity < 0.0f) {
				found->environmentSkyboxIntensity = 1.0f;
				changed = true;
			}
			if (found->environmentReflectionIntensity < 0.0f) {
				found->environmentReflectionIntensity = 0.3f;
				changed = true;
			}
		} else if (type == "SpriteRenderer" && found->texturePath.empty()) {
			found->texturePath = entity->spriteTexturePath;
			found->spriteSize = entity->spriteSize;
			found->spriteAnchor = entity->spriteAnchor;
			found->spriteColor = entity->spriteColor;
			found->spriteFlipX = entity->spriteFlipX;
			found->spriteFlipY = entity->spriteFlipY;
			changed = true;
		} else if (type == "Camera") {
			if (found->cameraNearClip <= 0.0f) {
				found->cameraNearClip = 0.1f;
				changed = true;
			}
			if (found->cameraFarClip <= found->cameraNearClip) {
				found->cameraFarClip = 1000.0f;
				changed = true;
			}
		} else if (type == "Light") {
			if (
				found->lightType != "Directional" &&
				found->lightType != "Point" &&
				found->lightType != "Spot"
			) {
				found->lightType = "Point";
				changed = true;
			}
			const float range = (std::max)(found->lightRange, 0.1f);
			const float decay = (std::max)(found->lightDecay, 0.0f);
			const float intensity = (std::max)(found->lightIntensity, 0.0f);
			const Vector4 color{
				std::clamp(found->lightColor.x, 0.0f, 1.0f),
				std::clamp(found->lightColor.y, 0.0f, 1.0f),
				std::clamp(found->lightColor.z, 0.0f, 1.0f),
				std::clamp(found->lightColor.w, 0.0f, 1.0f)
			};
			const float outerAngle = std::clamp(
				found->lightSpotOuterAngle,
				1.0f,
				89.0f
			);
			const float innerAngle = std::clamp(
				found->lightSpotInnerAngle,
				0.0f,
				outerAngle
			);
			if (
				found->lightRange != range ||
				found->lightDecay != decay ||
				found->lightIntensity != intensity ||
				found->lightSpotOuterAngle != outerAngle ||
				found->lightSpotInnerAngle != innerAngle ||
				found->lightColor.x != color.x ||
				found->lightColor.y != color.y ||
				found->lightColor.z != color.z ||
				found->lightColor.w != color.w
			) {
				found->lightColor = color;
				found->lightRange = range;
				found->lightDecay = decay;
				found->lightIntensity = intensity;
				found->lightSpotOuterAngle = outerAngle;
				found->lightSpotInnerAngle = innerAngle;
				changed = true;
			}
			if (found->lightType == "Point" && found->lightCastsShadow) {
				found->lightCastsShadow = false;
				changed = true;
			}
		} else if (type == "MonitorRenderer") {
			if (found->monitorResolutionPreset.empty()) {
				found->monitorResolutionPreset = "Custom";
				changed = true;
			}
			const uint32_t width = std::clamp<uint32_t>(
				found->monitorWidth,
				64,
				2048
			);
			const uint32_t height = std::clamp<uint32_t>(
				found->monitorHeight,
				64,
				2048
			);
			if (found->monitorWidth != width || found->monitorHeight != height) {
				found->monitorWidth = width;
				found->monitorHeight = height;
				changed = true;
			}
		} else if (type == "CameraSwitcher") {
			if (found->cameraSwitchTriggerKey.empty()) {
				found->cameraSwitchTriggerKey = "F5";
				changed = true;
			}
		} else if (type == "ThirdPersonCamera") {
			if (found->thirdPersonDistance < 0.01f) {
				found->thirdPersonDistance = 0.01f;
				changed = true;
			}
			if (found->thirdPersonAimDistance < 0.01f) {
				found->thirdPersonAimDistance = 0.01f;
				changed = true;
			}
			if (found->thirdPersonMouseSensitivity < 0.0f) {
				found->thirdPersonMouseSensitivity = 0.0f;
				changed = true;
			}
			if (found->thirdPersonMaxPitch < found->thirdPersonMinPitch) {
				std::swap(found->thirdPersonMinPitch, found->thirdPersonMaxPitch);
				changed = true;
			}
			if (found->thirdPersonOcclusionMargin < 0.0f) {
				found->thirdPersonOcclusionMargin = 0.0f;
				changed = true;
			}
			if (found->thirdPersonOcclusionPullInSmoothTime < 0.0f) {
				found->thirdPersonOcclusionPullInSmoothTime = 0.0f;
				changed = true;
			}
			if (found->thirdPersonOcclusionRecoverySmoothTime < 0.0f) {
				found->thirdPersonOcclusionRecoverySmoothTime = 0.0f;
				changed = true;
			}
			if (found->thirdPersonPositionSmoothTime < 0.0f) {
				found->thirdPersonPositionSmoothTime = 0.0f;
				changed = true;
			}
			if (found->thirdPersonRotationSmoothTime < 0.0f) {
				found->thirdPersonRotationSmoothTime = 0.0f;
				changed = true;
			}
			if (
				found->thirdPersonYawReference != "World" &&
				found->thirdPersonYawReference != "Target"
			) {
				found->thirdPersonYawReference = "World";
				changed = true;
			}
		} else if (type == "PhysicsBody") {
			if (found->physicsBodyType.empty()) {
				found->physicsBodyType = "Static";
				changed = true;
			}
			if (found->physicsMass <= 0.0f) {
				found->physicsMass = 1.0f;
				changed = true;
			}
			if (found->physicsMaxFallSpeed <= 0.0f) {
				found->physicsMaxFallSpeed = 100.0f;
				changed = true;
			}
		} else if (type == "FishingScoreAttackDirector") {
			const float outlineYOffset = std::isfinite(
				found->fishingFormationOutlineYOffset
			)
				? found->fishingFormationOutlineYOffset
				: 0.0f;
			const float outlineBloomIntensity = std::isfinite(
				found->fishingFormationOutlineBloomIntensity
			)
				? std::clamp(found->fishingFormationOutlineBloomIntensity, 0.0f, 32.0f)
				: 1.0f;
			const int outlineSegments = std::clamp(
				found->fishingFormationOutlineSegments,
				12,
				128
			);
			const Vector4 outlineColor = {
				std::isfinite(found->fishingFormationOutlineColor.x)
					? std::clamp(found->fishingFormationOutlineColor.x, 0.0f, 1.0f)
					: 0.1f,
				std::isfinite(found->fishingFormationOutlineColor.y)
					? std::clamp(found->fishingFormationOutlineColor.y, 0.0f, 1.0f)
					: 0.9f,
				std::isfinite(found->fishingFormationOutlineColor.z)
					? std::clamp(found->fishingFormationOutlineColor.z, 0.0f, 1.0f)
					: 1.0f,
				std::isfinite(found->fishingFormationOutlineColor.w)
					? std::clamp(found->fishingFormationOutlineColor.w, 0.0f, 1.0f)
					: 1.0f
			};
			if (
				found->fishingFormationOutlineBloomIntensity != outlineBloomIntensity ||
				found->fishingFormationOutlineYOffset != outlineYOffset ||
				found->fishingFormationOutlineSegments != outlineSegments ||
				found->fishingFormationOutlineColor.x != outlineColor.x ||
				found->fishingFormationOutlineColor.y != outlineColor.y ||
				found->fishingFormationOutlineColor.z != outlineColor.z ||
				found->fishingFormationOutlineColor.w != outlineColor.w
			) {
				found->fishingFormationOutlineYOffset = outlineYOffset;
				found->fishingFormationOutlineBloomIntensity = outlineBloomIntensity;
				found->fishingFormationOutlineSegments = outlineSegments;
				found->fishingFormationOutlineColor = outlineColor;
				changed = true;
			}
		} else if (type == "PlayerBehavior") {
			if (found->playerMoveSpeed < 0.0f) {
				found->playerMoveSpeed = 0.0f;
				changed = true;
			}
			if (found->playerJumpVelocity < 0.0f) {
				found->playerJumpVelocity = 0.0f;
				changed = true;
			}
			const float turnResponsiveness = std::clamp(
				found->playerTurnResponsiveness,
				0.0f,
				1.0f
			);
			if (found->playerTurnResponsiveness != turnResponsiveness) {
				found->playerTurnResponsiveness = turnResponsiveness;
				changed = true;
			}
			if (found->playerDashMultiplier < 1.0f) {
				found->playerDashMultiplier = 1.0f;
				changed = true;
			}
		} else if (type == "AgentBehavior") {
			if (found->agentBehaviorName.empty()) {
				found->agentBehaviorName = "Agent";
				changed = true;
			}
			if (found->agentProfileName.empty()) {
				found->agentProfileName = "Default";
				changed = true;
			}
			const float minSpeed = (std::max)(found->agentMinSpeed, 0.0f);
			const float maxSpeed =
				(std::max)(found->agentMaxSpeed, minSpeed);
			if (found->agentMinSpeed != minSpeed) {
				found->agentMinSpeed = minSpeed;
				changed = true;
			}
			if (found->agentMaxSpeed != maxSpeed) {
				found->agentMaxSpeed = maxSpeed;
				changed = true;
			}
			if (found->agentTurnSpeed < 0.0f) {
				found->agentTurnSpeed = 0.0f;
				changed = true;
			}
			if (found->agentWanderStrength < 0.0f) {
				found->agentWanderStrength = 0.0f;
				changed = true;
			}
			if (found->agentBoundsWeight < 0.0f) {
				found->agentBoundsWeight = 0.0f;
				changed = true;
			}
			const Vector3 teamHeadingDirection = NormalizeDirectionVector(
				found->agentTeamHeadingDirection,
				{ 0.0f, 0.0f, 1.0f }
			);
			if (
				found->agentTeamHeadingDirection.x != teamHeadingDirection.x ||
				found->agentTeamHeadingDirection.y != teamHeadingDirection.y ||
				found->agentTeamHeadingDirection.z != teamHeadingDirection.z
			) {
				found->agentTeamHeadingDirection = teamHeadingDirection;
				changed = true;
			}
			if (found->agentTeamHeadingWeight < 0.0f) {
				found->agentTeamHeadingWeight = 0.0f;
				changed = true;
			}
			if (found->agentTeamHeadingFollowSpeed < 0.0f) {
				found->agentTeamHeadingFollowSpeed = 0.0f;
				changed = true;
			}
			const float teamRotationWeight = std::clamp(
				found->agentTeamRotationWeight,
				0.0f,
				1.0f
			);
			if (found->agentTeamRotationWeight != teamRotationWeight) {
				found->agentTeamRotationWeight = teamRotationWeight;
				changed = true;
			}
			if (found->agentTeamRotationFollowSpeed < 0.0f) {
				found->agentTeamRotationFollowSpeed = 0.0f;
				changed = true;
			}
			if (
				found->agentForwardAxis != "+Z" &&
				found->agentForwardAxis != "-Z" &&
				found->agentForwardAxis != "+X" &&
				found->agentForwardAxis != "-X" &&
				found->agentForwardAxis != "+Y" &&
				found->agentForwardAxis != "-Y"
			) {
				found->agentForwardAxis = "+Z";
				changed = true;
			}
			if (found->agentRotationFollowSpeed < 0.0f) {
				found->agentRotationFollowSpeed = 0.0f;
				changed = true;
			}
			if (found->agentPitchFromVerticalVelocity < 0.0f) {
				found->agentPitchFromVerticalVelocity = 0.0f;
				changed = true;
			}
			if (found->agentBankingStrength < 0.0f) {
				found->agentBankingStrength = 0.0f;
				changed = true;
			}
			if (found->agentSchoolingUpdateInterval < 0.0f) {
				found->agentSchoolingUpdateInterval = 0.0f;
				changed = true;
			}
			if (found->agentSchoolingUpdateJitter < 0.0f) {
				found->agentSchoolingUpdateJitter = 0.0f;
				changed = true;
			}
			if (found->agentNeighborLimit < 0) {
				found->agentNeighborLimit = 0;
				changed = true;
			}
			const float schoolingBlend = std::clamp(
				found->agentSchoolingBlend,
				0.0f,
				1.0f
			);
			if (found->agentSchoolingBlend != schoolingBlend) {
				found->agentSchoolingBlend = schoolingBlend;
				changed = true;
			}
			if (found->agentSeparationRadius < 0.0f) {
				found->agentSeparationRadius = 0.0f;
				changed = true;
			}
			if (found->agentAlignmentRadius < 0.0f) {
				found->agentAlignmentRadius = 0.0f;
				changed = true;
			}
			if (found->agentCohesionRadius < 0.0f) {
				found->agentCohesionRadius = 0.0f;
				changed = true;
			}
			if (found->agentSeparationWeight < 0.0f) {
				found->agentSeparationWeight = 0.0f;
				changed = true;
			}
			if (found->agentAlignmentWeight < 0.0f) {
				found->agentAlignmentWeight = 0.0f;
				changed = true;
			}
			if (found->agentCohesionWeight < 0.0f) {
				found->agentCohesionWeight = 0.0f;
				changed = true;
			}
			if (found->agentAttractorWeight < 0.0f) {
				found->agentAttractorWeight = 0.0f;
				changed = true;
			}
		} else if (type == "AgentAttractor") {
			if (found->attractorTag.empty()) {
				found->attractorTag = "Default";
				changed = true;
			}
			if (found->attractorRadius < 0.0f) {
				found->attractorRadius = 0.0f;
				changed = true;
			}
			if (found->attractorStrength < 0.0f) {
				found->attractorStrength = 0.0f;
				changed = true;
			}
		} else if (type == "WaterVolume") {
			const Vector3 halfSize = {
				(std::max)(found->waterHalfSize.x, 0.1f),
				(std::max)(found->waterHalfSize.y, 0.1f),
				(std::max)(found->waterHalfSize.z, 0.1f)
			};
			if (
				found->waterHalfSize.x != halfSize.x ||
				found->waterHalfSize.y != halfSize.y ||
				found->waterHalfSize.z != halfSize.z
			) {
				found->waterHalfSize = halfSize;
				changed = true;
			}
			const float moveMultiplier = std::clamp(
				found->waterMoveSpeedMultiplier,
				0.0f,
				1.0f
			);
			if (found->waterMoveSpeedMultiplier != moveMultiplier) {
				found->waterMoveSpeedMultiplier = moveMultiplier;
				changed = true;
			}
			if (found->waterDrag < 0.0f) {
				found->waterDrag = 0.0f;
				changed = true;
			}
			if (found->waterMaxFallSpeed < 0.0f) {
				found->waterMaxFallSpeed = 0.0f;
				changed = true;
			}
			if (found->waterSwimUpSpeed < 0.0f) {
				found->waterSwimUpSpeed = 0.0f;
				changed = true;
			}
			const float surfaceAlpha = std::clamp(
				found->waterSurfaceAlpha,
				0.0f,
				1.0f
			);
			if (found->waterSurfaceAlpha != surfaceAlpha) {
				found->waterSurfaceAlpha = surfaceAlpha;
				changed = true;
			}
			const float surfaceWaveScale = std::clamp(
				found->waterSurfaceWaveScale,
				0.0f,
				3.0f
			);
			if (found->waterSurfaceWaveScale != surfaceWaveScale) {
				found->waterSurfaceWaveScale = surfaceWaveScale;
				changed = true;
			}
			const float surfaceNormalStrength = std::clamp(
				found->waterSurfaceNormalStrength,
				0.0f,
				2.0f
			);
			if (found->waterSurfaceNormalStrength != surfaceNormalStrength) {
				found->waterSurfaceNormalStrength = surfaceNormalStrength;
				changed = true;
			}
			const float surfaceFresnelPower = std::clamp(
				found->waterSurfaceFresnelPower,
				0.2f,
				8.0f
			);
			if (found->waterSurfaceFresnelPower != surfaceFresnelPower) {
				found->waterSurfaceFresnelPower = surfaceFresnelPower;
				changed = true;
			}
			const float lightIntensity = (std::max)(
				found->waterLightIntensity,
				0.0f
			);
			if (found->waterLightIntensity != lightIntensity) {
				found->waterLightIntensity = lightIntensity;
				changed = true;
			}
			const float lightDensity = (std::max)(
				found->waterLightDensity,
				0.0f
			);
			if (found->waterLightDensity != lightDensity) {
				found->waterLightDensity = lightDensity;
				changed = true;
			}
			const float causticsIntensity = (std::max)(
				found->waterLightCausticsIntensity,
				0.0f
			);
			if (found->waterLightCausticsIntensity != causticsIntensity) {
				found->waterLightCausticsIntensity = causticsIntensity;
				changed = true;
			}
			const float causticsScale = (std::max)(
				found->waterLightCausticsScale,
				0.001f
			);
			if (found->waterLightCausticsScale != causticsScale) {
				found->waterLightCausticsScale = causticsScale;
				changed = true;
			}
			const float breakupStrength = std::clamp(
				found->waterLightBreakupStrength,
				0.0f,
				3.0f
			);
			if (found->waterLightBreakupStrength != breakupStrength) {
				found->waterLightBreakupStrength = breakupStrength;
				changed = true;
			}
			const float warpStrength = std::clamp(
				found->waterLightWarpStrength,
				0.0f,
				3.0f
			);
			if (found->waterLightWarpStrength != warpStrength) {
				found->waterLightWarpStrength = warpStrength;
				changed = true;
			}
			const float noiseScale = (std::max)(
				found->waterLightNoiseScale,
				0.001f
			);
			if (found->waterLightNoiseScale != noiseScale) {
				found->waterLightNoiseScale = noiseScale;
				changed = true;
			}
			const int sampleCount = std::clamp(
				found->waterLightSampleCount,
				4,
				32
			);
			if (found->waterLightSampleCount != sampleCount) {
				found->waterLightSampleCount = sampleCount;
				changed = true;
			}
		} else if (type == "CameraPath") {
			if (found->cameraPathTriggerType.empty()) {
				found->cameraPathTriggerType = "Key";
				changed = true;
			}
			if (found->cameraPathTriggerKey.empty()) {
				found->cameraPathTriggerKey = "C";
				changed = true;
			}
			if (found->cameraPathEnterDuration < 0.0f) {
				found->cameraPathEnterDuration = 0.0f;
				changed = true;
			}
			if (found->cameraPathExitDuration < 0.0f) {
				found->cameraPathExitDuration = 0.0f;
				changed = true;
			}
			if (found->cameraPathInterpolation.empty()) {
				found->cameraPathInterpolation = "Linear";
				changed = true;
			}
			if (found->cameraPathDefaultEasing.empty()) {
				found->cameraPathDefaultEasing = "SmoothStep";
				changed = true;
			}
		} else if (type == "CameraPathPoint") {
			if (found->cameraPathPointDurationToNext < 0.0f) {
				found->cameraPathPointDurationToNext = 0.0f;
				changed = true;
			}
			if (found->cameraPathPointEasingToNext.empty()) {
				found->cameraPathPointEasingToNext = "SmoothStep";
				changed = true;
			}
		}
		if (!found->enabled) {
			found->enabled = true;
			changed = true;
		}
		if (changed) {
			MarkDirty();
		}
		return true;
	}
	SceneComponent component{ type, true };
	if (variantBaseAssetId_.empty()) {
		component.localId = NextComponentLocalId(*entity);
	} else {
		component.localId = MakeVariantLocalId(
			assetId_,
			std::to_string(entity->id) + ":" + type
		);
		while (FindComponentByLocalId(*entity, component.localId)) {
			++component.localId;
		}
	}
	if (type == "MeshRenderer") {
		component.modelPath = entity->modelPath;
		component.meshCullMode = "Back";
		component.meshEnvironmentReflectionOverride = false;
		component.meshEnvironmentReflectionIntensity = 0.3f;
	} else if (type == "Environment") {
		component.environmentSkyboxEnabled = true;
		component.environmentSkyboxPath = "resources/rostock_laage_airport_4k.dds";
		component.environmentSkyboxIntensity = 1.0f;
		component.environmentReflectionIntensity = 0.3f;
	} else if (type == "SpriteRenderer") {
		component.texturePath = entity->spriteTexturePath;
		component.spriteSize = entity->spriteSize;
		component.spriteAnchor = entity->spriteAnchor;
		component.spriteRenderSpace = "Scene2D";
		component.spriteViewportAnchor = { 0.0f, 0.0f };
		component.spriteColor = entity->spriteColor;
		component.spriteFlipX = entity->spriteFlipX;
		component.spriteFlipY = entity->spriteFlipY;
	} else if (type == "TextRenderer") {
		component.textValue = "Text";
		component.textRenderSpace = "ScreenOverlay";
		component.textFontSource = "System";
		component.textFontResourcePath.clear();
		component.textFontFamily = "Yu Gothic UI";
		component.textFontSize = 32.0f;
		component.textFontWeight = "Regular";
		component.textFontStyle = "Normal";
		component.textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		component.textOpacity = 1.0f;
		component.textHorizontalAlignment = "Left";
		component.textVerticalAlignment = "Top";
		component.textWrapMode = "NoWrap";
		component.textOverflowMode = "Overflow";
		component.textLayoutSize = { 0.0f, 0.0f };
		component.textCharacterSpacing = 0.0f;
		component.textLineSpacing = 1.0f;
		component.textOutlineEnabled = false;
		component.textOutlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		component.textOutlineWidth = 2.0f;
		component.textShadowEnabled = false;
		component.textShadowColor = { 0.0f, 0.0f, 0.0f, 0.5f };
		component.textShadowOffset = { 2.0f, 2.0f };
		component.textViewportAnchor = { 0.0f, 0.0f };
		component.textPivot = { 0.0f, 0.0f };
		component.textSortingOrder = 0;
		component.textClipEnabled = false;
	} else if (type == "TextMotion") {
		component.textMotionClips.clear();
	} else if (type == "GameFlowDirector") {
		component.gameFlowPhases.clear();
	} else if (type == "FishingScoreAttackDirector") {
		component.fishingFishEntityIds.clear();
		component.fishingWaterVolumeEntityId = 0;
		component.fishingDurationSeconds = 60.0f;
		component.fishingMaxSelectableFishCount = 5;
		component.fishingConfirmInput = "ENTER";
		component.fishingConfirmInputExpression.reset();
		component.fishingDistanceBandCount = 5;
		component.fishingHooksPerDistanceBand = 2;
		component.fishingDistanceMultiplierBase = 1.0f;
		component.fishingDistanceMultiplierStep = 0.2f;
		component.fishingUseHookBandSettings = false;
		component.fishingHookBands.clear();
		component.fishingHookScoreUnit = 100.0f;
		component.fishingFishMultiplierBase = 1.0f;
		component.fishingFishMultiplierPerAdditionalFish = 1.0f;
		component.fishingHookTierScoreMultipliers = {
			1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
			6.0f, 7.0f, 8.0f, 9.0f, 10.0f
		};
		component.fishingHookMultiplierColors = {
			{ 0.25f, 0.55f, 1.00f, 1.00f },
			{ 0.15f, 0.85f, 1.00f, 1.00f },
			{ 0.20f, 0.95f, 0.55f, 1.00f },
			{ 0.55f, 0.95f, 0.25f, 1.00f },
			{ 0.95f, 0.85f, 0.20f, 1.00f },
			{ 1.00f, 0.58f, 0.15f, 1.00f },
			{ 1.00f, 0.30f, 0.12f, 1.00f },
			{ 1.00f, 0.12f, 0.28f, 1.00f },
			{ 0.85f, 0.18f, 1.00f, 1.00f },
			{ 1.00f, 0.90f, 0.45f, 1.00f }
		};
		component.fishingHookRanks = BuildLegacyFishingHookRanks(
			component.fishingHookTierScoreMultipliers,
			component.fishingHookMultiplierColors
		);
		component.fishingHookColorEmissiveIntensity = 0.35f;
		component.fishingHookLegendVisible = false;
		component.fishingHookLegendTitleTextEntityId = 0;
		component.fishingHookLegendTextEntityIds.clear();
		component.fishingHookLegendTitle = "HOOK BONUS";
		component.fishingHookLegendPrefix = "x";
		component.fishingHookLegendIconEntityIds.assign(10, 0);
		component.fishingHookLegendIconSize = { 32.0f, 32.0f };
		component.fishingRandomizeSeedOnPlay = true;
		component.fishingRandomSeed = 1;
		component.fishingFishCountTextEntityId = 0;
		component.fishingTimerTextEntityId = 0;
		component.fishingScoreTextEntityId = 0;
		component.fishingMultiplierTextEntityId = 0;
		component.fishingResultTextEntityId = 0;
		component.fishingFishCountPrefix = "FISH ";
		component.fishingTimerPrefix = "TIME ";
		component.fishingScorePrefix = "SCORE ";
		component.fishingMultiplierPrefix = "MULTIPLIER ";
		component.fishingResultPrefix = "RESULT ";
		component.fishingUseFormationCapsuleCollision = false;
		component.fishingFormationOutlineVisible = false;
		component.fishingFormationOutlineColor = { 0.1f, 0.9f, 1.0f, 1.0f };
		component.fishingFormationOutlineBloomIntensity = 1.0f;
		component.fishingFormationOutlineYOffset = 0.25f;
		component.fishingFormationOutlineSegments = 48;
	} else if (type == "FishingHookSpawnArea") {
		component.fishingSpawnHalfSizeX = 10.0f;
		component.fishingSpawnHalfSizeZ = 10.0f;
		component.fishingSpawnMinimumDistance = 0.0f;
		component.fishingSpawnMaxAttempts = 16;
	} else if (type == "FishingHookPool") {
		component.fishingHookPoolEntries.clear();
	} else if (type == "FishingHook") {
		component.fishingHookBaseScore = 0;
	} else if (type == "FishingShark") {
		component.fishingSharkRadiusX = 12.0f;
		component.fishingSharkRadiusZ = 18.0f;
		component.fishingSharkAngularSpeed = 0.35f;
		component.fishingSharkInitialPhase = 0.0f;
		component.fishingSharkPenaltyScore = 300;
		component.fishingSharkHitCooldownSeconds = 2.0f;
		component.fishingSharkPathRandomness = 0.2f;
		component.fishingSharkWanderMoveSpeed = 0.0f;
		component.fishingSharkWanderMaximumTurnRate = 1.2f;
		component.fishingSharkObstacleAvoidanceDistance = 8.0f;
		component.fishingSharkObstacleAvoidanceStrength = 0.65f;
		component.fishingSharkObstacleAvoidanceResponse = 4.0f;
	} else if (type == "AudioSource") {
		component.audioClipPath.clear();
		component.audioSpatialMode = "TwoD";
		component.audioMinimumDistance = 1.4f;
		component.audioMaximumDistance = 30.0f;
		component.audioStereoAreaWidth = 1.0f;
		component.audioBus = "SFX";
		component.audioVolume = 1.0f;
		component.audioPitch = 1.0f;
		component.audioLoop = false;
		component.audioPlayOnStart = false;
		component.audioStopOnDisable = true;
		component.audioDecompressOnLoad = true;
		component.audioStreamFromDisk = false;
		component.audioPersistAcrossScenes = false;
		component.audioBgmFadeSeconds = 0.5f;
	} else if (type == "AudioListener") {
		component.audioListenerMode = "Hybrid";
	} else if (type == "Camera") {
		component.cameraIsMain = false;
		component.cameraFovY = 0.45f;
		component.cameraNearClip = 0.1f;
		component.cameraFarClip = 1000.0f;
		component.cameraInvertYaw = false;
		component.cameraInvertPitch = false;
	} else if (type == "Light") {
		component.lightType = "Point";
		component.lightColor = { 1.0f, 0.85f, 0.65f, 1.0f };
		component.lightIntensity = 2.0f;
		component.lightRange = 8.0f;
		component.lightDecay = 1.0f;
		component.lightSpotInnerAngle = 25.0f;
		component.lightSpotOuterAngle = 35.0f;
		component.lightCastsShadow = false;
	} else if (type == "MonitorRenderer") {
		component.monitorCameraEntityId = 0;
		component.monitorCameraName = "";
		component.monitorResolutionPreset = "Square 512";
		component.monitorWidth = 512;
		component.monitorHeight = 512;
		component.monitorHideSelf = true;
	} else if (type == "CameraSwitcher") {
		component.cameraSwitchTriggerKey = "F5";
		component.cameraSwitchWrap = true;
		for (const SceneEntity& candidate : entities_) {
			const bool hasCamera = std::any_of(
				candidate.components.begin(),
				candidate.components.end(),
				[](const SceneComponent& candidateComponent) {
					return candidateComponent.enabled &&
						candidateComponent.type == "Camera";
				}
			);
			if (hasCamera) {
				component.cameraSwitchEntries.push_back({
					candidate.id,
					candidate.name
				});
			}
		}
	} else if (type == "ThirdPersonCamera") {
		component.thirdPersonTargetEntityId = 0;
		component.thirdPersonTargetEntityName.clear();
		component.thirdPersonDistance = 8.0f;
		component.thirdPersonAimDistance = 3.0f;
		component.thirdPersonTargetOffset = { 0.0f, 1.35f, 0.0f };
		component.thirdPersonAimTargetOffset = { 0.0f, 1.55f, 0.0f };
		component.thirdPersonMouseSensitivity = 0.005f;
		component.thirdPersonMinPitch = -1.45f;
		component.thirdPersonMaxPitch = 1.35f;
		component.thirdPersonOcclusionMargin = 0.45f;
		component.thirdPersonOcclusionMask = 0xffffffffu;
		component.thirdPersonOcclusionPullInSmoothTime = 0.04f;
		component.thirdPersonOcclusionRecoverySmoothTime = 0.18f;
		component.thirdPersonPositionSmoothTime = 0.12f;
		component.thirdPersonRotationSmoothTime = 0.08f;
		component.thirdPersonYawReference = "World";
		component.thirdPersonAllowMouseInput = true;
		component.thirdPersonOcclusionEnabled = true;
		component.thirdPersonAimModeEnabled = true;
		component.thirdPersonInvertYaw = false;
		component.thirdPersonInvertPitch = false;
	} else if (type == "Animator") {
		component.animatorPlayOnStart = true;
		component.animatorLoop = true;
		component.animatorSpeed = 1.0f;
		component.animatorDefaultClip = 0;
		component.animatorTransitionDuration = 0.2f;
		component.animatorBlendCurve = "SmoothStep";
	} else if (type == "PhysicsBody") {
		component.physicsBodyType = "Static";
		component.physicsMass = 1.0f;
		component.physicsUseGravity = true;
		component.physicsGravityScale = 1.0f;
		component.physicsDrag = 0.0f;
		component.physicsRestitution = 0.0f;
		component.physicsFriction = 0.0f;
		component.physicsMaxFallSpeed = 100.0f;
		component.physicsVelocity = { 0.0f, 0.0f, 0.0f };
	} else if (type == "PlayerBehavior") {
		component.playerMoveSpeed = 10.8f;
		component.playerJumpVelocity = 37.2f;
		component.playerTurnResponsiveness = 0.018f;
		component.playerDashMultiplier = 1.65f;
		component.playerCameraRelativeMove = true;
		component.playerAllowJump = true;
		component.playerAutoForward = false;
		component.playerInputMode = "KeyboardMouse";
		component.playerGamepadDeadzone = 0.20f;
	} else if (type == "AgentBehavior") {
		component.agentBehaviorName = "Agent";
		component.agentMovementMode = "Free3D";
		component.agentProfileName = "Default";
		component.agentGroupName = "";
		component.agentBoundsEntityId = 0;
		component.agentBoundsName = "";
		component.agentAttractorEntityId = 0;
		component.agentAttractorTag = "";
		component.agentUseWaterBounds = true;
		component.agentMinSpeed = 1.0f;
		component.agentMaxSpeed = 3.0f;
		component.agentTurnSpeed = 2.5f;
		component.agentWanderStrength = 0.8f;
		component.agentBoundsWeight = 3.0f;
		component.agentSchooling = false;
		component.agentSeparationRadius = 1.2f;
		component.agentAlignmentRadius = 4.0f;
		component.agentCohesionRadius = 5.0f;
		component.agentSeparationWeight = 1.8f;
		component.agentAlignmentWeight = 0.8f;
		component.agentCohesionWeight = 0.9f;
		component.agentMemberMinimumDistance = 0.0f;
		component.agentAttractorWeight = 0.0f;
		component.agentVisualColor = { 0.25f, 0.75f, 1.0f, 1.0f };
		component.agentEnableLighting = true;
	} else if (type == "AgentAttractor") {
		component.attractorTag = "Default";
		component.attractorTargetBehaviorName = "";
		component.attractorTargetProfileName = "";
		component.attractorRadius = 6.0f;
		component.attractorStrength = 1.0f;
		component.attractorVisualColor = { 1.0f, 0.35f, 0.45f, 1.0f };
	} else if (type == "WaterVolume") {
		component.waterHalfSize = { 10.0f, 4.0f, 10.0f };
		component.waterOffset = { 0.0f, 0.0f, 0.0f };
		component.waterSurfaceEnabled = true;
		component.waterSurfaceBaseColor = { 0.04f, 0.55f, 0.78f, 1.0f };
		component.waterSurfaceHighlightColor = { 0.42f, 0.95f, 1.20f, 1.0f };
		component.waterSurfaceAlpha = 0.36f;
		component.waterSurfaceWaveScale = 1.0f;
		component.waterSurfaceNormalStrength = 0.75f;
		component.waterSurfaceFresnelPower = 3.0f;
		component.waterLightShaftEnabled = true;
		component.waterLightColor = { 0.55f, 0.90f, 1.15f, 1.0f };
		component.waterLightDirection = { -0.25f, -1.0f, 0.18f };
		component.waterLightIntensity = 0.55f;
		component.waterLightDensity = 0.045f;
		component.waterLightCausticsIntensity = 0.35f;
		component.waterLightCausticsScale = 0.08f;
		component.waterLightCausticsSpeed = 1.0f;
		component.waterLightBreakupStrength = 1.0f;
		component.waterLightWarpStrength = 1.0f;
		component.waterLightNoiseScale = 1.0f;
		component.waterLightSampleCount = 16;
		component.waterMoveSpeedMultiplier = 0.45f;
		component.waterGravityScale = 0.55f;
		component.waterDrag = 4.0f;
		component.waterMaxFallSpeed = 5.0f;
		component.waterSwimUpSpeed = 12.0f;
	} else if (type == "EntityReference") {
		component.entityReferenceName = "Target";
		component.entityReferenceTarget = {};
	} else if (type == "SceneTransition") {
		component.sceneTransitionTargetSceneId = "gameplay";
		component.sceneTransitionTriggerType = "Key";
		component.sceneTransitionTriggerKey = "ENTER";
	} else if (type == "CameraPath") {
		component.cameraPathTargetCameraName = "";
		component.cameraPathTriggerType = "Key";
		component.cameraPathTriggerKey = "C";
		component.cameraPathEnterDuration = 0.5f;
		component.cameraPathExitDuration = 0.5f;
		component.cameraPathInterpolation = "Linear";
		component.cameraPathDefaultEasing = "SmoothStep";
		component.cameraPathReturnToPreviousCamera = true;
		component.cameraPathStartFromCurrentCamera = true;
		component.cameraPathAutoCollectChildPoints = true;
	} else if (type == "CameraPathPoint") {
		component.cameraPathPointDurationToNext = 1.0f;
		component.cameraPathPointEasingToNext = "SmoothStep";
	} else if (type == "StatSet") {
		component.stats.push_back({ "hp", "HP", 0.0f, 100.0f, 100.0f });
	} else if (type == "StateMachine") {
		SceneStateDefinition idle{};
		idle.name = "Idle";
		idle.actionId = "Builtin.Idle";
		SceneStateParameter idleMove{};
		idleMove.name = "MoveState";
		idleMove.type = "String";
		idleMove.stringValue = "Move";
		SceneStateParameter idleAttack{};
		idleAttack.name = "AttackState";
		idleAttack.type = "String";
		idleAttack.stringValue = "Attack";
		SceneStateParameter idleAttackInput{};
		idleAttackInput.name = "AttackInput";
		idleAttackInput.type = "Input";
		idleAttackInput.stringValue = "Mouse Left";
		idle.parameters = { idleMove, idleAttack, idleAttackInput };

		SceneStateDefinition move{};
		move.name = "Move";
		move.actionId = "Builtin.Move";
		SceneStateParameter moveIdle{};
		moveIdle.name = "IdleState";
		moveIdle.type = "String";
		moveIdle.stringValue = "Idle";
		SceneStateParameter moveAttack = idleAttack;
		SceneStateParameter moveAttackInput = idleAttackInput;
		SceneStateParameter moveSpeed{};
		moveSpeed.name = "Speed";
		moveSpeed.type = "Float";
		moveSpeed.floatValue = 6.0f;
		move.parameters = {
			moveIdle, moveAttack, moveAttackInput, moveSpeed
		};

		SceneStateDefinition attack{};
		attack.name = "Attack";
		attack.actionId = "Builtin.MeleeAttack";
		SceneStateParameter returnState{};
		returnState.name = "ReturnState";
		returnState.type = "String";
		returnState.stringValue = "Idle";
		SceneStateParameter hitBox{};
		hitBox.name = "HitBox";
		hitBox.type = "Entity";
		SceneStateParameter animation{};
		animation.name = "Animation";
		animation.type = "String";
		SceneStateParameter animationTarget{};
		animationTarget.name = "AnimationTarget";
		animationTarget.type = "Entity";
		SceneStateParameter windup{};
		windup.name = "Windup";
		windup.type = "Float";
		windup.floatValue = 0.15f;
		SceneStateParameter activeTime{};
		activeTime.name = "ActiveTime";
		activeTime.type = "Float";
		activeTime.floatValue = 0.2f;
		SceneStateParameter recovery{};
		recovery.name = "Recovery";
		recovery.type = "Float";
		recovery.floatValue = 0.35f;
		attack.parameters = {
			returnState, hitBox, animation, animationTarget,
			windup, activeTime, recovery
		};
		component.stateMachineInitialState = "Idle";
		component.stateMachineResetOnDisable = true;
		component.stateMachineStates = { idle, move, attack };
	} else if (type == "EventTrigger") {
		component.eventBindings.push_back({});
	} else if (type == "PrefabAnimator") {
		ScenePrefabAnimationClip clip{};
		SceneAnimationTrack track{};
		track.keyframes = {
			{ 0.0f, {} },
			{ 1.0f, {} }
		};
		clip.tracks.push_back(std::move(track));
		component.prefabAnimationClips.push_back(std::move(clip));
	} else if (type == "AttackSet") {
		component.attackDefinitions.push_back(SceneAttackDefinition{});
	} else if (type == "Faction") {
		component.factionName = "Neutral";
	} else if (type == "HitBox") {
		component.hitBoxDamage = 10.0f;
		component.hitBoxDamageStatId = "hp";
	} else if (type == "HurtBox") {
		component.hurtBoxDamageMultiplier = 1.0f;
		component.hurtBoxHealthStatId = "hp";
	} else if (type == "HitReaction") {
		component.hitReactionKnockbackMultiplier = 1.0f;
		component.hitReactionTriggerMode = "MinimumDamage";
		component.hitReactionMinimumPoiseDamage = 0.0f;
		component.hitReactionPoiseStatId = "poise";
		component.hitReactionPoiseRecoveryDelay = 1.0f;
		component.hitReactionStateName = "Hit";
		component.hitReactionStateDuration = 0.2f;
	} else if (type == "DeathPresentation") {
		component.deathPresentationStateName = "Dead";
		component.deathPresentationDeactivateDelay = 2.0f;
	} else if (type == "EnemyBehavior") {
		component.enemyTargetEntityName = "Player";
	} else if (type == "EnemySpawner") {
		component.enemySpawnerMaxAlive = 10;
		component.enemySpawnerInterval = 1.0f;
		component.enemySpawnerRadius = 3.0f;
	} else if (type == "Projectile") {
		component.projectileDirection = { 0.0f, 0.0f, 1.0f };
	}
	entity->components.push_back(std::move(component));
	if (type == "CameraPath") {
		const uint64_t pathEntityId = entity->id;
		for (uint32_t index = 0; index < 2; ++index) {
			SceneEntity& point = CreateEntity(
				index == 0 ? "Point_00" : "Point_01",
				pathEntityId
			);
			point.transform.translate = {
				0.0f,
				0.0f,
				static_cast<float>(index) * 5.0f
			};
			point.components.push_back(SceneComponent{ "CameraPathPoint", true });
			point.components.back().localId = NextComponentLocalId(point);
			point.components.back().cameraPathPointDurationToNext = 1.0f;
			point.components.back().cameraPathPointEasingToNext = "SmoothStep";
		}
	}
	MarkDirty();
	return true;
}

bool SceneDocument::RemoveComponent(uint64_t id, const std::string& type) {
	SceneEntity* entity = FindEntity(id);
	if (!entity) {
		return false;
	}
	const auto oldSize = entity->components.size();
	entity->components.erase(
		std::remove_if(
			entity->components.begin(),
			entity->components.end(),
			[&type](const SceneComponent& component) {
				return component.type == type;
			}
		),
		entity->components.end()
	);
	if (entity->components.size() == oldSize) {
		return false;
	}
	MarkDirty();
	return true;
}

bool SceneDocument::IsDescendantOf(
	uint64_t id,
	uint64_t potentialAncestorId
) const {
	std::unordered_set<uint64_t> visited;
	const SceneEntity* current = FindEntity(id);
	while (current && current->parentId != 0) {
		if (current->parentId == potentialAncestorId) {
			return true;
		}
		if (!visited.insert(current->id).second) {
			return false;
		}
		current = FindEntity(current->parentId);
	}
	return false;
}

SceneEntity* SceneDocument::FindEntity(uint64_t id) {
	const auto found = std::find_if(
		entities_.begin(),
		entities_.end(),
		[id](const SceneEntity& entity) { return entity.id == id; }
	);
	return found == entities_.end() ? nullptr : &(*found);
}

const SceneEntity* SceneDocument::FindEntity(uint64_t id) const {
	const auto found = std::find_if(
		entities_.begin(),
		entities_.end(),
		[id](const SceneEntity& entity) { return entity.id == id; }
	);
	return found == entities_.end() ? nullptr : &(*found);
}

SceneEntity* SceneDocument::FindEntityByName(const std::string& name) {
	const auto found = std::find_if(
		entities_.begin(),
		entities_.end(),
		[&name](const SceneEntity& entity) { return entity.name == name; }
	);
	return found == entities_.end() ? nullptr : &(*found);
}

const SceneEntity* SceneDocument::FindEntityByName(const std::string& name) const {
	const auto found = std::find_if(
		entities_.begin(),
		entities_.end(),
		[&name](const SceneEntity& entity) { return entity.name == name; }
	);
	return found == entities_.end() ? nullptr : &(*found);
}

bool SceneDocument::LoadInternal(const std::string& filePath) {
	std::ifstream input(StringUtility::ToPath(filePath), std::ios::binary);
	if (!input.is_open()) {
		lastLoadError_ = "Scene file could not be opened: " + filePath;
		return false;
	}

	bool migrated = false;
	try {
		json root = json::parse(input);
		if (root.contains("variant")) {
			// Baseを先に展開し、安定ID単位のOverrideだけを有効Documentへ重ねる。
			if (!root.at("variant").is_object()) {
				lastLoadError_ = "Prefab Variant metadata must be an object";
				return false;
			}
			const json& variant = root.at("variant");
			if (!variant.contains("base") || !variant.at("base").is_object()) {
				lastLoadError_ = "Prefab Variant requires a Base reference";
				return false;
			}
			const PrefabAssetReference baseReference =
				PrefabAssetRegistry::ReadVariantBase(filePath);
			if (baseReference.assetId.empty()) {
				lastLoadError_ = "Prefab Variant Base requires an Asset ID";
				return false;
			}
			const std::string basePath =
				PrefabAssetRegistry::ResolvePath(baseReference);
			if (basePath.empty()) {
				lastLoadError_ =
					"Prefab Variant Base asset is missing or ambiguous";
				return false;
			}

			static thread_local std::vector<std::string> variantLoadStack;
			const std::string resolvedCurrentPath = StringUtility::ToUtf8(
				EditableResourcePath::ResolveResource(
					StringUtility::ToPath(filePath)
				).lexically_normal()
			);
			if (std::find(
				variantLoadStack.begin(),
				variantLoadStack.end(),
				resolvedCurrentPath
			) != variantLoadStack.end()) {
				lastLoadError_ = "Prefab Variant dependency cycle detected";
				return false;
			}
			variantLoadStack.push_back(resolvedCurrentPath);
			struct VariantLoadGuard {
				std::vector<std::string>& stack;
				~VariantLoadGuard() { stack.pop_back(); }
			} variantLoadGuard{ variantLoadStack };

			SceneDocument base;
			if (!base.LoadInternal(basePath)) {
				lastLoadError_ = "Failed to load Prefab Variant Base: " +
					base.GetLastLoadError();
				return false;
			}
			if (base.GetAssetId() != baseReference.assetId) {
				lastLoadError_ =
					"Prefab Variant Base Asset ID does not match";
				return false;
			}
			const std::shared_ptr<const SceneDocument> baseSnapshot =
				std::make_shared<SceneDocument>(base);
			*this = std::move(base);
			assetId_ = root.value("assetId", std::string{});
			if (assetId_.empty()) {
				assetId_ = PrefabAssetRegistry::CreateAssetId();
				migrated = true;
			}
			if (assetId_ == baseReference.assetId) {
				lastLoadError_ =
					"Prefab Variant and Base must use different Asset IDs";
				return false;
			}
			variantBaseAssetId_ = baseReference.assetId;
			variantBasePath_ = basePath;
			variantBaseSnapshot_ = baseSnapshot;

			json rootSettings = {
				{ "sceneName", sceneName_ },
				{ "lighting", LightingSettingsToJson(lightingSettings_) },
				{ "postProcess", PostProcessToJson(postProcessSettings_) },
				{ "debug", DebugSettingsToJson(debugSettings_) },
				{ "teams", json::array() }
			};
			for (const SceneTeamSettings& team : teams_) {
				rootSettings["teams"].push_back(TeamToJson(team));
			}
			if (variant.contains("rootOverrides")) {
				if (!ApplyJsonPropertyPatch(
					rootSettings,
					variant.at("rootOverrides")
				)) {
					lastLoadError_ =
						"Prefab Variant contains invalid Root overrides";
					return false;
				}
			}
			sceneName_ = rootSettings.value("sceneName", sceneName_);
			lightingSettings_ = LightingSettingsFromJson(
				rootSettings.value("lighting", json::object()),
				lightingSettings_
			);
			postProcessSettings_ = PostProcessFromJson(
				rootSettings.value("postProcess", json::object()),
				postProcessSettings_
			);
			debugSettings_ = DebugSettingsFromJson(
				rootSettings.value("debug", json::object()),
				debugSettings_
			);
			if (!rootSettings.contains("teams") ||
				!rootSettings.at("teams").is_array()) {
				lastLoadError_ = "Prefab Variant teams must be an array";
				return false;
			}
			teams_.clear();
			for (const json& teamValue : rootSettings.at("teams")) {
				if (!teamValue.is_object()) {
					lastLoadError_ = "Prefab Variant contains an invalid Team";
					return false;
				}
				teams_.push_back(TeamFromJson(teamValue));
			}

			if (variant.contains("removedEntityIds")) {
				if (!variant.at("removedEntityIds").is_array()) {
					lastLoadError_ =
						"Prefab Variant removedEntityIds must be an array";
					return false;
				}
				std::unordered_set<uint64_t> removedIds;
				for (const json& idValue : variant.at("removedEntityIds")) {
					removedIds.insert(idValue.get<uint64_t>());
				}
				bool foundRemovedChild = true;
				while (foundRemovedChild) {
					foundRemovedChild = false;
					for (const SceneEntity& entity : entities_) {
						if (
							removedIds.contains(entity.parentId) &&
							removedIds.insert(entity.id).second
						) {
							foundRemovedChild = true;
						}
					}
				}
				entities_.erase(
					std::remove_if(
						entities_.begin(),
						entities_.end(),
						[&removedIds](const SceneEntity& entity) {
							return removedIds.contains(entity.id);
						}
					),
					entities_.end()
				);
			}

			if (variant.contains("entityOverrides")) {
				if (!variant.at("entityOverrides").is_array()) {
					lastLoadError_ =
						"Prefab Variant entityOverrides must be an array";
					return false;
				}
				for (const json& overrideValue :
					variant.at("entityOverrides")) {
					if (!overrideValue.is_object()) {
						lastLoadError_ =
							"Prefab Variant contains an invalid Entity override";
						return false;
					}
					const uint64_t entityId = overrideValue.value(
						"entityId",
						uint64_t{}
					);
					SceneEntity* entity = FindEntity(entityId);
					if (!entity) {
						lastLoadError_ =
							"Prefab Variant Entity override target is missing";
						return false;
					}

					if (overrideValue.contains("properties")) {
						json entityJson = SceneEntityToJson(*entity);
						const json components = entityJson.at("components");
						entityJson.erase("components");
						if (!ApplyJsonPropertyPatch(
							entityJson,
							overrideValue.at("properties")
						)) {
							lastLoadError_ =
								"Prefab Variant contains invalid Entity properties";
							return false;
						}
						entityJson["components"] = components;
						SceneEntity replacement{};
						if (!SceneEntityFromJson(
							entityJson,
							replacement,
							lastLoadError_
						) || replacement.id != entityId) {
							return false;
						}
						*entity = std::move(replacement);
					}

					if (overrideValue.contains("removedComponentIds")) {
						if (!overrideValue.at("removedComponentIds").is_array()) {
							lastLoadError_ =
								"Prefab Variant removedComponentIds must be an array";
							return false;
						}
						std::unordered_set<uint64_t> removedComponentIds;
						for (const json& idValue :
							overrideValue.at("removedComponentIds")) {
							removedComponentIds.insert(idValue.get<uint64_t>());
						}
						entity->components.erase(
							std::remove_if(
								entity->components.begin(),
								entity->components.end(),
								[&removedComponentIds](
									const SceneComponent& component
								) {
									return removedComponentIds.contains(
										component.localId
									);
								}
							),
							entity->components.end()
						);
					}

					if (overrideValue.contains("componentOverrides")) {
						if (!overrideValue.at("componentOverrides").is_array()) {
							lastLoadError_ =
								"Prefab Variant componentOverrides must be an array";
							return false;
						}
						for (const json& componentOverride :
							overrideValue.at("componentOverrides")) {
							const uint64_t componentId = componentOverride.value(
								"componentId",
								uint64_t{}
							);
							SceneComponent* component = FindComponentByLocalId(
								*entity,
								componentId
							);
							if (!component ||
								!componentOverride.contains("properties")) {
								lastLoadError_ =
									"Prefab Variant Component override target is missing";
								return false;
							}
							json componentJson = ComponentToJson(*component);
							if (!ApplyJsonPropertyPatch(
								componentJson,
								componentOverride.at("properties")
							)) {
								lastLoadError_ =
									"Prefab Variant contains invalid Component properties";
								return false;
							}
							SceneComponent replacement{};
							if (!ComponentFromJson(componentJson, replacement) ||
								replacement.localId != componentId) {
								lastLoadError_ =
									"Prefab Variant produced an invalid Component";
								return false;
							}
							*component = std::move(replacement);
						}
					}

					if (overrideValue.contains("addedComponents")) {
						if (!overrideValue.at("addedComponents").is_array()) {
							lastLoadError_ =
								"Prefab Variant addedComponents must be an array";
							return false;
						}
						for (const json& componentValue :
							overrideValue.at("addedComponents")) {
							SceneComponent component{};
							if (!ComponentFromJson(componentValue, component) ||
								FindComponentByLocalId(
									*entity,
									component.localId
								) ||
								FindComponent(*entity, component.type.c_str())) {
								lastLoadError_ =
									"Prefab Variant contains a duplicate Component";
								return false;
							}
							entity->components.push_back(std::move(component));
						}
					}
					SynchronizeLegacyRendererFields(*entity);
				}
			}

			if (variant.contains("addedEntities")) {
				if (!variant.at("addedEntities").is_array()) {
					lastLoadError_ =
						"Prefab Variant addedEntities must be an array";
					return false;
				}
				for (const json& entityValue : variant.at("addedEntities")) {
					SceneEntity entity{};
					if (!SceneEntityFromJson(
						entityValue,
						entity,
						lastLoadError_
					) || entity.id == 0 || FindEntity(entity.id)) {
						if (lastLoadError_.empty()) {
							lastLoadError_ =
								"Prefab Variant contains a duplicate Entity ID";
						}
						return false;
					}
					entities_.push_back(std::move(entity));
				}
			}

			std::vector<SceneValidationIssue> issues;
			if (!SceneValidator::ValidateDocument(
				*this,
				nullptr,
				{},
				filePath,
				issues
			)) {
				lastLoadError_ = SceneValidator::FormatIssues(issues);
				return false;
			}
			RebuildNextId();
			ValidateHierarchy();
			dirty_ = migrated;
			if (migrated) {
				++revision_;
			}
			lastLoadError_.clear();
			return true;
		}
		std::string migrationError;
		if (!SceneDocumentMigrator::Migrate(
			root,
			migrated,
			migrationError
		)) {
			lastLoadError_ = migrationError;
			return false;
		}
		if (!root.contains("entities") || !root.at("entities").is_array()) {
			lastLoadError_ = "Scene JSON must contain an entities array";
			return false;
		}
		if (root.contains("lighting") && !root.at("lighting").is_object()) {
			lastLoadError_ = "Scene lighting settings must be an object";
			return false;
		}
		if (root.contains("postProcess") && !root.at("postProcess").is_object()) {
			lastLoadError_ = "Scene postProcess must be an object";
			return false;
		}
		if (root.contains("debug") && !root.at("debug").is_object()) {
			lastLoadError_ = "Scene debug settings must be an object";
			return false;
		}
		if (root.contains("teams") && !root.at("teams").is_array()) {
			lastLoadError_ = "Scene teams must be an array";
			return false;
		}
		if (root.contains("teams")) {
			std::unordered_set<std::string> teamNames;
			for (const json& team : root.at("teams")) {
				if (!team.is_object()) {
					lastLoadError_ = "Scene contains an invalid Team entry";
					return false;
				}
				if (!team.contains("name") || !team.at("name").is_string() ||
					team.at("name").get<std::string>().empty()) {
					lastLoadError_ = "Scene Team requires a name";
					return false;
				}
				const std::string teamName = team.at("name").get<std::string>();
				if (!teamNames.insert(teamName).second) {
					lastLoadError_ = "Duplicate Scene Team name: " + teamName;
					return false;
				}
			}
		}

		sceneName_ = root.value("sceneName", std::string{});
		assetId_ = root.value("assetId", std::string{});
		variantBaseAssetId_.clear();
		variantBasePath_.clear();
		variantBaseSnapshot_.reset();
		entities_.clear();
		teams_.clear();
		lightingSettings_ = LightingSettingsFromJson(
			root.value("lighting", json::object()),
			SceneLightingSettings{}
		);
		postProcessSettings_ = PostProcessFromJson(
			root.value("postProcess", json::object()),
			ScenePostProcessSettings{}
		);
		debugSettings_ = DebugSettingsFromJson(
			root.value("debug", json::object()),
			SceneDebugSettings{}
		);
		if (root.contains("teams") && root.at("teams").is_array()) {
			for (const json& source : root.at("teams")) {
				SceneTeamSettings team = TeamFromJson(source);
				team.name = MakeUniqueTeamName(teams_, team.name);
				teams_.push_back(std::move(team));
			}
		}
		for (const json& source : root.at("entities")) {
			if (!source.is_object()) {
				lastLoadError_ = "Scene contains an invalid Entity entry";
				return false;
			}
			if (source.contains("transform") &&
				!source.at("transform").is_object()) {
				lastLoadError_ = "Entity transform must be an object";
				return false;
			}
			if (source.contains("sprite") && !source.at("sprite").is_object()) {
				lastLoadError_ = "Entity sprite must be an object";
				return false;
			}
			if (source.contains("prefab") && !source.at("prefab").is_object()) {
				lastLoadError_ = "Entity prefab metadata must be an object";
				return false;
			}
			if (
				source.contains("prefab") &&
				source.at("prefab").contains("links") &&
				!source.at("prefab").at("links").is_array()
			) {
				lastLoadError_ = "Entity prefab links must be an array";
				return false;
			}
			if (source.contains("components")) {
				if (!source.at("components").is_array()) {
					lastLoadError_ = "Entity components must be an array";
					return false;
				}
				for (const json& component : source.at("components")) {
					if (!component.is_object() ||
						!component.contains("type") ||
						!component.at("type").is_string() ||
						component.at("type").get<std::string>().empty()) {
						lastLoadError_ =
							"Entity contains an invalid Component entry";
						return false;
					}
				}
			}
			SceneEntity entity{};
			entity.id = source.value("id", uint64_t{});
			entity.parentId = source.value("parentId", uint64_t{});
			entity.name = source.value("name", std::string("Entity"));
			entity.folder = source.value("folder", false);
			entity.folderTeamEnabled = source.value(
				"folderTeamEnabled",
				false
			);
			entity.active = source.value("active", true);
			entity.locked = source.value("locked", false);
			if (source.contains("prefab")) {
				const json& prefab = source.at("prefab");
				entity.prefabAssetId = prefab.value(
					"assetId",
					std::string{}
				);
				entity.prefabSourcePath = prefab.value(
					"sourcePath",
					std::string{}
				);
				entity.prefabInstanceRootId = prefab.value(
					"instanceRootId",
					uint64_t{}
				);
				entity.prefabLocalId = prefab.value(
					"localId",
					uint64_t{}
				);
				if (prefab.contains("links")) {
					for (const json& linkValue : prefab.at("links")) {
						if (!linkValue.is_object()) {
							lastLoadError_ =
								"Entity contains an invalid Prefab link";
							return false;
						}
						ScenePrefabLink link{};
						link.assetId = linkValue.value(
							"assetId",
							std::string{}
						);
						link.sourcePath = linkValue.value(
							"sourcePath",
							std::string{}
						);
						link.instanceRootId = linkValue.value(
							"instanceRootId",
							uint64_t{}
						);
						link.localId = linkValue.value(
							"localId",
							uint64_t{}
						);
						entity.prefabLinks.push_back(std::move(link));
					}
				}
				if (entity.prefabLinks.empty()) {
					EnsurePrefabLinkStack(entity);
					if (!entity.prefabLinks.empty()) {
						migrated = true;
					}
				} else {
					SynchronizeActivePrefabLink(entity);
				}
			}
			entity.teamName = source.value("team", std::string{});
			entity.modelPath = source.value("modelPath", std::string{});
			if (source.contains("sprite") && source.at("sprite").is_object()) {
				const json& sprite = source.at("sprite");
				entity.spriteTexturePath = sprite.value(
					"texturePath",
					std::string{}
				);
				if (sprite.contains("size")) {
					entity.spriteSize = JsonToVector(sprite.at("size"), entity.spriteSize);
				}
				if (sprite.contains("anchor")) {
					entity.spriteAnchor = JsonToVector(sprite.at("anchor"), entity.spriteAnchor);
				}
				if (sprite.contains("color")) {
					entity.spriteColor = JsonToVector(sprite.at("color"), entity.spriteColor);
				}
				entity.spriteFlipX = sprite.value("flipX", false);
				entity.spriteFlipY = sprite.value("flipY", false);
			}
			if (source.contains("components")) {
				entity.components = ComponentsFromJson(source.at("components"));
			}
			// 旧Component IDの補完はメモリ内だけで終わらせず、次回保存対象にする。
			migrated |= EnsureComponentLocalIds(entity);
			if (source.contains("transform")) {
				const json& transform = source.at("transform");
				if (transform.contains("scale")) {
					entity.transform.scale = JsonToVector(
						transform.at("scale"),
						entity.transform.scale
					);
				}
				if (transform.contains("rotation")) {
					entity.transform.rotate = JsonToQuaternion(
						transform.at("rotation"),
						entity.transform.rotate
					);
				} else if (transform.contains("rotate")) {
					entity.transform.rotate = MakeQuaternionFromEuler(
						JsonToVector(transform.at("rotate"), Vector3{})
					);
				}
				if (transform.contains("translate")) {
					entity.transform.translate = JsonToVector(
						transform.at("translate"),
						entity.transform.translate
					);
				}
			}
			SynchronizeLegacyRendererFields(entity);
			entities_.push_back(std::move(entity));
		}
		std::unordered_map<std::string, std::string> assetIdsByPath;
		for (SceneEntity& entity : entities_) {
			EnsurePrefabLinkStack(entity);
			for (ScenePrefabLink& link : entity.prefabLinks) {
				if (!link.assetId.empty() || link.sourcePath.empty()) {
					continue;
				}
				auto [entry, inserted] = assetIdsByPath.try_emplace(
					link.sourcePath
				);
				if (inserted) {
					entry->second = PrefabAssetRegistry::ReadAssetId(
						link.sourcePath
					);
				}
				if (!entry->second.empty()) {
					link.assetId = entry->second;
					migrated = true;
				}
			}
			SynchronizeActivePrefabLink(entity);
		}
	}
	catch (const json::exception& exception) {
		assetId_.clear();
		variantBaseAssetId_.clear();
		variantBasePath_.clear();
		variantBaseSnapshot_.reset();
		entities_.clear();
		teams_.clear();
		lastLoadError_ = "Scene JSON is invalid: ";
		lastLoadError_ += exception.what();
		return false;
	}

	std::vector<SceneValidationIssue> issues;
	if (!SceneValidator::ValidateDocument(
		*this,
		nullptr,
		{},
		filePath,
		issues
	)) {
		lastLoadError_ = SceneValidator::FormatIssues(issues);
		assetId_.clear();
		variantBaseAssetId_.clear();
		variantBasePath_.clear();
		variantBaseSnapshot_.reset();
		entities_.clear();
		teams_.clear();
		return false;
	}
	RebuildNextId();
	ValidateHierarchy();
	dirty_ = migrated;
	if (migrated) {
		++revision_;
	}
	lastLoadError_.clear();
	return true;
}

void SceneDocument::RebuildNextId() {
	nextId_ = 1;
	for (const SceneEntity& entity : entities_) {
		nextId_ = (std::max)(nextId_, entity.id + 1);
	}
}

void SceneDocument::ValidateHierarchy() {
	for (SceneTeamSettings& team : teams_) {
		NormalizeTeamSettings(team);
	}
	for (SceneEntity& entity : entities_) {
		if (!entity.teamName.empty() && !FindTeam(entity.teamName)) {
			entity.teamName.clear();
		}
		if (!entity.folder) {
			entity.folderTeamEnabled = false;
		}
		if (entity.folder) {
			entity.components.clear();
			entity.modelPath.clear();
			entity.spriteTexturePath.clear();
			if (entity.teamName.empty()) {
				entity.folderTeamEnabled = false;
			}
		}
		if (
			entity.parentId == entity.id ||
			(entity.parentId != 0 && !FindEntity(entity.parentId))
		) {
			entity.parentId = 0;
			continue;
		}

		std::unordered_set<uint64_t> visited{ entity.id };
		const SceneEntity* parent = FindEntity(entity.parentId);
		while (parent) {
			if (!visited.insert(parent->id).second) {
				entity.parentId = 0;
				break;
			}
			parent = FindEntity(parent->parentId);
		}
	}
}
