// 役割: ImGuiエディタ各ウィンドウの描画、入力、シーン編集操作を実装する。
#include "ImGuiManager.h"
#include "EditorComponentCatalog.h"
#include "PostProcessSettingsEditor.h"

#include <cassert>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

#include "WinApp.h"
#include "DirectXCommon.h"
#include "../3d/SrvManager.h"
#include "../2d/TextureManager.h"
#include "../2d/TextureFormat.h"
#include "../3d/ModelManager.h"
#include "../3d/Model.h"
#include "../3d/ModelFormat.h"
#include "../3d/Object3dCommon.h"
#include "../3d/Camera.h"
#include "../debug/DebugRenderer.h"
#include "../io/Input.h"
#include "../math/Matrix4x4.h"
#include "../math/Math.h"
#include "../particle/ParticleEffectResource.h"
#include "../particle/ParticleManager.h"
#include "../scene/EditorSession.h"
#include "../scene/PrefabAssetRegistry.h"
#include "../scene/PrefabEditorSession.h"
#include "../scene/SceneCatalog.h"
#include "../scene/SceneDocument.h"
#include "../scene/SceneEntityQuery.h"
#include "../scene/SceneInputKey.h"
#include "../scene/SceneManager.h"
#include "../scene/ScenePrefabAnimationEvaluator.h"
#include "../scene/SceneTemplateRegistry.h"
#include "../scene/SceneTransformResolver.h"
#include "../scene/SceneValidator.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"
#include "../utility/SystemPerformanceMonitor.h"

#include "../../externals/imgui/imgui.h"
#include "../../externals/imgui/imgui_internal.h"
#include "../../externals/imgui/imgui_impl_win32.h"
#include "../../externals/imgui/imgui_impl_dx12.h"
#include "../../externals/ImGuizmo/ImGuizmo.h"
#include "../../externals/nlohmann/json.hpp"

namespace {
	using json = nlohmann::json;
	using SceneEntityQuery::FindComponent;
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveSceneWorldMatrix;

	bool DrawSceneInputCombo(
		const char* label,
		std::string& inputName,
		EditorLanguage language
	) {
		bool changed = false;
		const char* preview = inputName.empty()
			? SelectEditorText(language, "選択...", "Select...")
			: inputName.c_str();
		if (!ImGui::BeginCombo(label, preview)) {
			return false;
		}
		ImGui::SeparatorText(SelectEditorText(language, "マウス", "Mouse"));
		for (const SceneInputMouseDefinition& mouse : kSceneInputMouseDefinitions) {
			if (ImGui::Selectable(mouse.name, inputName == mouse.name)) {
				inputName = mouse.name;
				changed = true;
			}
		}
		ImGui::SeparatorText(SelectEditorText(language, "キーボード", "Keyboard"));
		for (const SceneInputKeyDefinition& key : kSceneInputKeyDefinitions) {
			if (ImGui::Selectable(key.name, inputName == key.name)) {
				inputName = key.name;
				changed = true;
			}
		}
		ImGui::SeparatorText(SelectEditorText(language, "コントローラー", "Controller"));
		for (const SceneInputGamepadDefinition& gamepad :
			kSceneInputGamepadDefinitions) {
			if (ImGui::Selectable(
				gamepad.name,
				inputName == gamepad.name
			)) {
				inputName = gamepad.name;
				changed = true;
			}
		}
		ImGui::EndCombo();
		return changed;
	}

	std::string FirstInputExpressionTerm(
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

	std::string InputExpressionSummary(
		const std::optional<SceneInputExpression>& expression,
		const std::string& legacyInput,
		EditorLanguage language
	) {
		if (!expression) {
			return legacyInput.empty()
				? SelectEditorText(language, "未設定", "Not set")
				: legacyInput;
		}
		if (expression->groups.empty()) {
			return SelectEditorText(language, "入力なし", "No input");
		}
		std::string result;
		const char* rootOperator = expression->mode == "All" ? " AND " : " OR ";
		for (size_t groupIndex = 0; groupIndex < expression->groups.size(); ++groupIndex) {
			if (groupIndex != 0) {
				result += rootOperator;
			}
			const SceneInputGroup& group = expression->groups[groupIndex];
			if (expression->groups.size() > 1) {
				result += "(";
			}
			const char* groupOperator = group.mode == "All" ? " AND " : " OR ";
			for (size_t termIndex = 0; termIndex < group.terms.size(); ++termIndex) {
				if (termIndex != 0) {
					result += groupOperator;
				}
				const SceneInputTerm& term = group.terms[termIndex];
				result += term.input.empty()
					? SelectEditorText(language, "未設定", "Not set")
					: term.input;
				if (term.phase == "Held") {
					result += SelectEditorText(language, "（押下中）", " (Held)");
				}
			}
			if (expression->groups.size() > 1) {
				result += ")";
			}
		}
		return result;
	}

	bool DrawSceneInputExpressionEditor(
		const char* label,
		std::optional<SceneInputExpression>& expression,
		std::string& legacyInput,
		EditorLanguage language
	) {
		bool changed = false;
		ImGui::PushID(label);
		ImGui::Text("%s: %s", label, InputExpressionSummary(
			expression, legacyInput, language
		).c_str());
		if (!expression) {
			if (ImGui::SmallButton(SelectEditorText(
				language,
				"複数入力を編集###MaterializeInputExpression",
				"Edit Multiple Inputs###MaterializeInputExpression"
			))) {
				SceneInputExpression materialized{};
				SceneInputGroup group{};
				group.terms.push_back({ legacyInput, "Pressed" });
				materialized.groups.push_back(std::move(group));
				expression = std::move(materialized);
				changed = true;
			}
			ImGui::PopID();
			return changed;
		}

		if (ImGui::BeginCombo(
			SelectEditorText(language, "グループ間###InputExpressionRootMode", "Between Groups###InputExpressionRootMode"),
			expression->mode.c_str()
		)) {
			for (const char* mode : { "Any", "All" }) {
				if (ImGui::Selectable(mode, expression->mode == mode)) {
					expression->mode = mode;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		for (size_t groupIndex = 0; groupIndex < expression->groups.size(); ++groupIndex) {
			SceneInputGroup& group = expression->groups[groupIndex];
			ImGui::PushID(static_cast<int>(groupIndex));
			if (ImGui::TreeNodeEx(
				"InputGroup",
				ImGuiTreeNodeFlags_DefaultOpen,
				SelectEditorText(language, "グループ %zu", "Group %zu"),
				groupIndex + 1
			)) {
				if (ImGui::BeginCombo(
					SelectEditorText(language, "条件###InputExpressionGroupMode", "Terms###InputExpressionGroupMode"),
					group.mode.c_str()
				)) {
					for (const char* mode : { "Any", "All" }) {
						if (ImGui::Selectable(mode, group.mode == mode)) {
							group.mode = mode;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
				int removeTermIndex = -1;
				for (size_t termIndex = 0; termIndex < group.terms.size(); ++termIndex) {
					SceneInputTerm& term = group.terms[termIndex];
					ImGui::PushID(static_cast<int>(termIndex));
					changed |= DrawSceneInputCombo(
						SelectEditorText(language, "入力###InputExpressionTerm", "Input###InputExpressionTerm"),
						term.input,
						language
					);
					if (ImGui::BeginCombo(
						SelectEditorText(language, "状態###InputExpressionPhase", "Phase###InputExpressionPhase"),
						term.phase.c_str()
					)) {
						for (const char* phase : { "Pressed", "Held" }) {
							if (ImGui::Selectable(phase, term.phase == phase)) {
								term.phase = phase;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					if (ImGui::SmallButton(SelectEditorText(
						language, "削除###RemoveInputTerm", "Remove###RemoveInputTerm"
					))) {
						removeTermIndex = static_cast<int>(termIndex);
					}
					ImGui::PopID();
				}
				if (removeTermIndex >= 0) {
					group.terms.erase(group.terms.begin() + removeTermIndex);
					changed = true;
				}
				if (ImGui::SmallButton(SelectEditorText(
					language, "条件を追加###AddInputTerm", "Add Term###AddInputTerm"
				))) {
					group.terms.push_back({ {}, "Pressed" });
					changed = true;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (ImGui::SmallButton(SelectEditorText(
			language, "グループを追加###AddInputGroup", "Add Group###AddInputGroup"
		))) {
			expression->groups.push_back({});
			changed = true;
		}
		if (changed) {
			legacyInput = FirstInputExpressionTerm(expression);
		}
		ImGui::PopID();
		return changed;
	}

	void EnsureFishingHookRanks(SceneComponent& component) {
		if (component.fishingHookRanks.size() == 10) {
			return;
		}
		const std::vector<SceneFishingHookRankDefinition> previous =
			std::move(component.fishingHookRanks);
		component.fishingHookRanks.clear();
		component.fishingHookRanks.reserve(10);
		for (size_t index = 0; index < 10; ++index) {
			SceneFishingHookRankDefinition rank{};
			if (index < previous.size()) {
				rank = previous[index];
			} else {
				rank.id = "rank_" + std::to_string(index + 1);
				rank.displayName = "Rank " + std::to_string(index + 1);
				if (index < component.fishingHookTierScoreMultipliers.size()) {
					rank.scoreMultiplier =
						component.fishingHookTierScoreMultipliers[index];
				}
				if (index < component.fishingHookMultiplierColors.size()) {
					rank.color = component.fishingHookMultiplierColors[index];
				}
			}
			if (rank.id.empty()) {
				rank.id = "rank_" + std::to_string(index + 1);
			}
			if (rank.displayName.empty()) {
				rank.displayName = "Rank " + std::to_string(index + 1);
			}
			component.fishingHookRanks.push_back(std::move(rank));
		}
	}

	bool IsSameRotation(const Quaternion& left, const Quaternion& right) {
		const Quaternion normalizedLeft = Normalize(left);
		const Quaternion normalizedRight = Normalize(right);
		const float dot =
			normalizedLeft.x * normalizedRight.x +
			normalizedLeft.y * normalizedRight.y +
			normalizedLeft.z * normalizedRight.z +
			normalizedLeft.w * normalizedRight.w;
		return std::abs(dot) >= 0.999999f;
	}

	bool HasNonUniformScale(const Matrix4x4& matrix) {
		Vector3 scale{};
		Quaternion rotate = MakeIdentityQuaternion();
		Vector3 translate{};
		if (!DecomposeAffineMatrix(matrix, scale, rotate, translate)) {
			return true;
		}
		const Vector3 absoluteScale{
			std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)
		};
		constexpr float epsilon = 0.0001f;
		return
			std::abs(absoluteScale.x - absoluteScale.y) > epsilon ||
			std::abs(absoluteScale.y - absoluteScale.z) > epsilon;
	}

	std::string PathToUtf8(const std::filesystem::path& path) {
		return StringUtility::ToUtf8(path);
	}

	std::filesystem::path PathFromUtf8(const std::string& path) {
		return StringUtility::ToPath(path);
	}

	bool IsAudioAssetExtension(const std::string& extension) {
		return extension == ".wav" || extension == ".mp3" ||
			extension == ".aac" || extension == ".m4a";
	}

	bool IsAudioAssetPath(const std::filesystem::path& path) {
		std::string extension = path.extension().string();
		std::transform(
			extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			}
		);
		return IsAudioAssetExtension(extension);
	}

	void CopyTextBuffer(
		char* destination,
		size_t destinationSize,
		const std::string& source
	) {
		strncpy_s(destination, destinationSize, source.c_str(), _TRUNCATE);
	}

	bool InputTextString(const char* label, std::string& value) {
		char buffer[256]{};
		CopyTextBuffer(buffer, sizeof(buffer), value);
		if (!ImGui::InputText(label, buffer, sizeof(buffer))) {
			return false;
		}
		value = buffer;
		return true;
	}

	bool InputTextMultilineString(const char* label, std::string& value) {
		char buffer[2048]{};
		CopyTextBuffer(buffer, sizeof(buffer), value);
		if (!ImGui::InputTextMultiline(
			label,
			buffer,
			sizeof(buffer),
			ImVec2(-1.0f, ImGui::GetTextLineHeight() * 5.0f)
		)) {
			return false;
		}
		value = buffer;
		return true;
	}

	std::string BuildEntityHierarchyLabel(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		std::vector<std::string> names;
		const SceneEntity* current = &entity;
		while (current) {
			names.push_back(current->name.empty() ? "Entity" : current->name);
			current = current->parentId != 0
				? document.FindEntity(current->parentId)
				: nullptr;
		}
		std::reverse(names.begin(), names.end());
		std::string result;
		for (const std::string& name : names) {
			if (!result.empty()) {
				result += " / ";
			}
			result += name;
		}
		return result;
	}

	std::vector<std::string> CollectEntityJointNames(
		const SceneEntity& entity
	) {
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		if (!meshRenderer || meshRenderer->modelPath.empty()) {
			return {};
		}
		ModelManager* modelManager = ModelManager::GetInstance();
		if (!modelManager) {
			return {};
		}
		modelManager->LoadModel(meshRenderer->modelPath);
		const Model* model = modelManager->FindModel(meshRenderer->modelPath);
		if (!model) {
			return {};
		}

		std::vector<std::string> jointNames;
		std::function<void(const Model::Node&)> collectNode;
		collectNode = [&](const Model::Node& node) {
			if (
				!node.name.empty() &&
				std::find(jointNames.begin(), jointNames.end(), node.name) ==
					jointNames.end()
			) {
				jointNames.push_back(node.name);
			}
			for (const Model::Node& child : node.children) {
				collectNode(child);
			}
		};
		collectNode(model->GetRootNode());
		return jointNames;
	}

	bool DrawJointNameCombo(
		const char* label,
		const std::vector<std::string>& jointNames,
		std::string& selectedJointName
	) {
		const std::string preview = selectedJointName.empty()
			? "Select Bone..."
			: selectedJointName;
		bool changed = false;
		if (ImGui::BeginCombo(label, preview.c_str())) {
			for (size_t index = 0; index < jointNames.size(); ++index) {
				const std::string& jointName = jointNames[index];
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable(
					jointName.c_str(), selectedJointName == jointName
				)) {
					selectedJointName = jointName;
					changed = true;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	std::string SceneAssetFileStem(const SceneDescriptor& descriptor) {
		const std::string path = descriptor.assetPath.empty()
			? descriptor.filePath
			: descriptor.assetPath;
		std::string fileName = StringUtility::ToUtf8(
			StringUtility::ToPath(path).filename()
		);
		constexpr const char* suffix = ".scene.json";
		if (fileName.ends_with(suffix)) {
			fileName.erase(fileName.size() - std::strlen(suffix));
		}
		return fileName;
	}

	std::string BuildSceneAssetPath(const char* fileStem) {
		std::string fileName = fileStem;
		if (!fileName.ends_with(".scene.json")) {
			fileName += ".scene.json";
		}
		return "resources/scenes/" + fileName;
	}

	std::filesystem::path GetProjectResourceRoot();

	bool ReadBinaryFile(
		const std::filesystem::path& path,
		std::vector<uint8_t>& output
	) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input.is_open()) {
			return false;
		}
		const std::streampos size = input.tellg();
		if (size <= 0) {
			return false;
		}
		output.resize(static_cast<size_t>(size));
		input.seekg(0, std::ios::beg);
		input.read(
			reinterpret_cast<char*>(output.data()),
			static_cast<std::streamsize>(output.size())
		);
		return input.gcount() == static_cast<std::streamsize>(output.size());
	}

	bool LoadFirstAvailableFont(
		const std::vector<std::filesystem::path>& candidates,
		std::vector<uint8_t>& output
	) {
		for (const std::filesystem::path& candidate : candidates) {
			std::error_code error;
			if (!std::filesystem::exists(candidate, error)) {
				continue;
			}
			if (ReadBinaryFile(candidate, output)) {
				return true;
			}
		}
		return false;
	}

	void AddProjectAssetGlyphs(ImFontGlyphRangesBuilder& builder) {
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			GetProjectResourceRoot(),
			std::filesystem::directory_options::skip_permission_denied,
			error
		);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			builder.AddText(PathToUtf8(iterator->path().filename()).c_str());
			iterator.increment(error);
		}
	}

	bool IsModelAssetPath(const std::filesystem::path& path) {
		return ModelFormat::IsBasicModelPath(path);
	}

	bool IsTextureAssetPath(const std::filesystem::path& path) {
		return TextureFormat::IsSupportedTexturePath(path);
	}

	bool IsPrefabAssetPath(const std::filesystem::path& path) {
		return PathToUtf8(path.filename()).ends_with(".prefab.json");
	}

	bool TryParsePrefabAssetReference(
		const json& value,
		PrefabAssetReference& reference
	) {
		if (value.is_string()) {
			reference = PrefabAssetRegistry::CreateReference(
				value.get<std::string>()
			);
		} else if (value.is_object()) {
			reference.assetId = value.value("assetId", std::string{});
			reference.fallbackPath = value.value(
				"fallbackPath",
				std::string{}
			);
			if (reference.assetId.empty()) {
				reference = PrefabAssetRegistry::CreateReference(
					reference.fallbackPath
				);
			}
		} else {
			return false;
		}

		const std::string resolvedPath =
			PrefabAssetRegistry::ResolvePath(reference);
		const std::string& validationPath = resolvedPath.empty()
			? reference.fallbackPath
			: resolvedPath;
		if (!IsPrefabAssetPath(PathFromUtf8(validationPath))) {
			return false;
		}
		if (!resolvedPath.empty()) {
			reference.fallbackPath = resolvedPath;
		}
		return true;
	}

	json PrefabAssetReferenceToJson(
		const PrefabAssetReference& source
	) {
		PrefabAssetReference reference = source;
		if (reference.assetId.empty()) {
			reference = PrefabAssetRegistry::CreateReference(
				reference.fallbackPath
			);
		}
		const std::string resolvedPath =
			PrefabAssetRegistry::ResolvePath(reference);
		if (!resolvedPath.empty()) {
			reference.fallbackPath = resolvedPath;
		}
		return {
			{ "assetId", reference.assetId },
			{ "fallbackPath", reference.fallbackPath }
		};
	}

	bool ContainsPrefabAssetReference(
		const std::vector<PrefabAssetReference>& references,
		const PrefabAssetReference& candidate
	) {
		return std::any_of(
			references.begin(),
			references.end(),
			[&candidate](const PrefabAssetReference& reference) {
				return PrefabAssetRegistry::IsSameAsset(
					reference,
					candidate
				);
			}
		);
	}

	bool ContainsCaseInsensitive(
		const std::string& text,
		const std::string& search
	) {
		if (search.empty()) {
			return true;
		}
		auto toLower = [](const std::string& value) {
			std::string result = value;
			std::transform(
				result.begin(),
				result.end(),
				result.begin(),
				[](unsigned char character) {
					return static_cast<char>(std::tolower(character));
				}
			);
			return result;
		};
		return toLower(text).find(toLower(search)) != std::string::npos;
	}

	std::filesystem::path GetProjectResourceRoot() {
		return EditableResourcePath::Resolve("resources");
	}

	constexpr char kEditorSettingsPath[] = "editor_settings.json";

	const char* GetAudioSpatialModeDisplayName(const std::string& mode) {
		if (mode == "ThreeD") return "ThreeD Point";
		if (mode == "ThreeDPointDownmix") return "ThreeD Point Downmix";
		if (mode == "ThreeDStereoArea") return "ThreeD Stereo Area";
		return "TwoD Stereo";
	}

	bool IsThreeDAudioSpatialMode(const std::string& mode) {
		return mode == "ThreeD" || mode == "ThreeDPointDownmix" ||
			mode == "ThreeDStereoArea";
	}

	void DrawAudioSpatialClipCompatibilityWarning(
		EditorLanguage language,
		const SceneComponent& component
	) {
		if (!IsThreeDAudioSpatialMode(component.audioSpatialMode) ||
			component.audioClipPath.empty() || component.audioStreamFromDisk) {
			return;
		}
		const std::filesystem::path resolvedPath =
			EditableResourcePath::ResolveResource(PathFromUtf8(component.audioClipPath));
		std::error_code filesystemError;
		if (!IsAudioAssetPath(resolvedPath) ||
			!std::filesystem::is_regular_file(resolvedPath, filesystemError)) {
			return;
		}
		Audio* audio = Audio::GetInstance();
		if (!audio || !audio->CanProbeAudioFileMetadata()) {
			return;
		}
		AudioFileMetadata metadata{};
		std::string metadataError;
		if (!audio->TryGetAudioFileMetadata(
			component.audioClipPath.c_str(), metadata, &metadataError
		)) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				language == EditorLanguage::Japanese
					? "Audio metadataを読み取れません: %s"
					: "Audio metadata could not be read: %s",
				metadataError.c_str()
			);
			return;
		}
		const uint32_t requiredChannels = component.audioSpatialMode == "ThreeD" ? 1 : 2;
		if (metadata.channelCount == requiredChannels) {
			return;
		}

		const ImVec4 warningColor(0.95f, 0.65f, 0.25f, 1.0f);
		if (component.audioSpatialMode == "ThreeD" && metadata.channelCount == 2) {
			ImGui::TextColored(
				warningColor,
				SelectEditorText(
					language,
					"このClipは%u chです。ThreeD Pointはモノラルのみです。ThreeD Point DownmixまたはThreeD Stereo Areaを選択してください。",
					"This clip has %u channels. ThreeD Point requires mono. Use ThreeD Point Downmix or ThreeD Stereo Area."
				),
				metadata.channelCount
			);
			return;
		}
		if (metadata.channelCount == 1) {
			ImGui::TextColored(
				warningColor,
				SelectEditorText(
					language,
					"このClipはモノラルです。%sはステレオを必要とします。ThreeD Pointを選択してください。",
					"This clip is mono. %s requires stereo. Use ThreeD Point."
				),
				GetAudioSpatialModeDisplayName(component.audioSpatialMode)
			);
			return;
		}
		ImGui::TextColored(
			warningColor,
			SelectEditorText(
				language,
				"このClipは%u chです。%sはステレオを必要とします。",
				"This clip has %u channels. %s requires stereo."
			),
			metadata.channelCount,
			GetAudioSpatialModeDisplayName(component.audioSpatialMode)
		);
	}

	const char* LocalizedComponentWidgetLabel(
		EditorLanguage language,
		const char* english
	) {
		if (language == EditorLanguage::English) {
			return english;
		}
		static constexpr std::pair<const char*, const char*> labels[] = {
			{ "Model", "モデル" }, { "Texture", "テクスチャ" },
			{ "Materials", "マテリアル" }, { "Override", "上書き" },
			{ "Override Color", "色を上書き" }, { "Color", "色" },
			{ "Reset Preview", "Previewをリセット" }, { "Clear Model", "モデルを解除" },
			{ "Drop Model Here", "モデルをここへドロップ" },
			{ "Clear Texture", "テクスチャを解除" },
			{ "Drop Texture Here", "テクスチャをここへドロップ" },
			{ "Cull Mode", "カリング" }, { "Reflection Intensity", "反射の強さ" },
			{ "Override Environment Reflection", "Environment反射を上書き" },
			{ "Skybox Enabled", "Skyboxを有効化" }, { "Skybox DDS", "Skybox DDS" },
			{ "Skybox Intensity", "Skyboxの強さ" }, { "Drop DDS Skybox Here", "DDS Skyboxをここへドロップ" },
			{ "Size", "サイズ" },
			{ "Anchor", "アンカー" }, { "Flip X", "X反転" }, { "Flip Y", "Y反転" },
			{ "Text", "テキスト" }, { "Font Family", "フォント" },
			{ "Font Size", "フォントサイズ" }, { "Render Space", "描画空間" },
			{ "Weight", "太さ" }, { "Style", "スタイル" },
			{ "Opacity", "不透明度" }, { "Horizontal Align", "横方向の配置" },
			{ "Vertical Align", "縦方向の配置" }, { "Wrap", "折り返し" },
			{ "Overflow", "はみ出し" }, { "Layout Size", "レイアウトサイズ" },
			{ "Character Spacing", "文字間隔" }, { "Line Spacing", "行間" },
			{ "Outline", "アウトライン" }, { "Outline Color", "アウトライン色" },
			{ "Outline Width", "アウトライン幅" }, { "Shadow", "影" },
			{ "Shadow Color", "影の色" }, { "Shadow Offset", "影のオフセット" },
			{ "Light Type", "Lightの種類" }, { "Intensity", "強さ" },
			{ "Range", "範囲" }, { "Decay", "減衰" }, { "Direction", "方向" },
			{ "Inner Angle", "内側の角度" }, { "Outer Angle", "外側の角度" },
			{ "Cast Shadow", "影を描画" }, { "Shadow Bias", "影のBias" },
			{ "Normal Bias", "Normal Bias" }, { "Shadow Strength", "影の濃さ" },
			{ "Shadow Distance", "影の距離" }, { "Orthographic Size", "正射影サイズ" },
			{ "Shadow Near Clip", "影の近クリップ" }, { "Shadow Far Clip", "影の遠クリップ" },
			{ "Texel Snap", "Texel Snap" }, { "Shadow Map Size", "Shadow Mapサイズ" },
			{ "Main Camera", "Main Camera" }, { "FOV Y", "視野角 Y" },
			{ "Near Clip", "近クリップ" }, { "Far Clip", "遠クリップ" },
			{ "Target Camera Name", "対象Camera名" }, { "Camera Entity", "Camera Entity" },
			{ "Resolution Preset", "解像度プリセット" }, { "Width", "幅" },
			{ "Height", "高さ" }, { "Hide Self In View", "表示内で自身を隠す" },
			{ "Repair Camera ID", "Camera IDを修復" },
			{ "Switch Key", "切替キー" }, { "Wrap To First", "先頭へ戻る" },
			{ "Camera", "Camera" }, { "Add Camera", "Cameraを追加" },
			{ "Target Entity", "対象Entity" }, { "Allow Mouse Input", "マウス入力を許可" },
			{ "Yaw Reference", "Yawの基準" }, { "Distance", "距離" },
			{ "Aim Distance", "照準時の距離" }, { "Aim Mode Enabled", "照準Modeを有効化" },
			{ "Target Offset", "対象オフセット" }, { "Aim Target Offset", "照準対象オフセット" },
			{ "Mouse Sensitivity", "マウス感度" }, { "Min Pitch", "最小Pitch" },
			{ "Max Pitch", "最大Pitch" }, { "Occlusion Margin", "遮蔽マージン" },
			{ "Occlusion Enabled", "遮蔽を有効化" }, { "Occlusion Layer Mask", "遮蔽Layer Mask" },
			{ "Target Camera Entity", "対象Camera Entity" }, { "Trigger Type", "Triggerの種類" },
			{ "Trigger Key", "Triggerキー" }, { "Enter Duration", "開始時間" },
			{ "Exit Duration", "終了時間" }, { "Interpolation", "補間" },
			{ "Default Easing", "既定Easing" }, { "Return To Previous Camera", "前のCameraへ戻る" },
			{ "Start From Current Camera", "現在のCameraから開始" },
			{ "Auto Collect Child Points", "子Pointを自動収集" }, { "Select", "選択" },
			{ "Add Point", "Pointを追加" }, { "Half Size", "半分のサイズ" },
			{ "Offset", "オフセット" }, { "Surface Enabled", "Surfaceを有効化" },
			{ "Base Color", "基本色" }, { "Highlight Color", "ハイライト色" },
			{ "Surface Alpha", "Surfaceの透明度" }, { "Wave Scale", "波の大きさ" },
			{ "Normal Strength", "Normalの強さ" }, { "Fresnel Power", "Fresnelの強さ" },
			{ "Light Shafts", "光芒" }, { "Light Color", "Lightの色" },
			{ "Light Direction", "Lightの方向" }, { "Light Intensity", "Lightの強さ" },
			{ "Light Density", "Lightの密度" }, { "Caustics Intensity", "Causticsの強さ" },
			{ "Caustics Scale", "Causticsの大きさ" }, { "Caustics Speed", "Causticsの速度" },
			{ "Breakup Strength", "崩れの強さ" }, { "Warp Strength", "歪みの強さ" },
			{ "Noise Scale", "ノイズの大きさ" }, { "Raymarch Samples", "Raymarch回数" },
			{ "Move Speed Multiplier", "移動速度倍率" }, { "Gravity Scale", "重力倍率" },
			{ "Area Width", "Area Width" },
			{ "Drag", "抵抗" }, { "Max Fall Speed", "最大落下速度" }, { "Swim Up Speed", "上昇速度" }
			, { "Collider Active", "Colliderを有効化" }, { "Is Trigger", "Triggerとして扱う" }
			, { "Collision Layer", "Collision Layer" }, { "Collision Mask", "Collision Mask" }
			, { "Shape", "形状" }, { "Radius", "半径" }, { "Size Multiplier", "サイズ倍率" }
			, { "Debug Visible", "Debug表示" }, { "Draw Mode", "描画Mode" }
			, { "Debug Segments", "Debug分割数" }, { "Debug Color", "Debug色" }
			, { "Damage", "ダメージ" }, { "Poise Damage", "Poiseダメージ" }
			, { "Knockback", "ノックバック" }, { "Vertical Knockback", "垂直ノックバック" }
			, { "Hit Stop Duration", "Hit Stop時間" }, { "Damage Multiplier", "ダメージ倍率" }
			, { "Knockback Multiplier", "ノックバック倍率" }, { "Mass", "質量" }
			, { "Use Gravity", "重力を使用" }, { "Restitution", "反発" }
			, { "Friction", "摩擦" }, { "Velocity", "速度" }
			, { "Move Speed", "移動速度" }, { "Jump Velocity", "ジャンプ速度" }
			, { "Turn Responsiveness", "旋回の反応性" }, { "Dash Multiplier", "Dash倍率" }
			, { "Camera Relative Move", "Camera基準で移動" }, { "Allow Jump", "ジャンプを許可" }
			, { "Reaction Tag", "Reaction Tag" }, { "Damage Stat", "Damage Stat" }
			, { "Poise Stat", "Poise Stat" }, { "Owner Entity Id", "所有Entity ID" }
			, { "Owner Entity Name", "所有Entity名" }, { "Ignore Same Faction", "同じFactionを無視" }
			, { "Health Stat", "Health Stat" }, { "Stats Entity Id", "Stats Entity ID" }
			, { "Stats Entity Name", "Stats Entity名" }, { "Target Entity Id", "対象Entity ID" }
			, { "Target Entity Name", "対象Entity名" }, { "Detection Range", "検出範囲" }
			, { "Lose Range", "追跡解除範囲" }, { "Attack Range", "攻撃範囲" }
			, { "Turn Speed", "旋回速度" }, { "Attack Cooldown", "攻撃間隔" }
			, { "Attack Windup", "攻撃開始時間" }, { "Attack Active Time", "攻撃有効時間" }
			, { "Attack Recovery", "攻撃復帰時間" }, { "Initial Count", "初期数" }
			, { "Max Alive", "最大生存数" }, { "Respawn Interval", "再出現間隔" }
			, { "Spawn Radius", "出現半径" }, { "Auto Start", "自動開始" }
			, { "Id", "ID" }, { "Display Name", "表示名" }
			, { "Min", "最小値" }, { "Max", "最大値" }, { "Initial", "初期値" }
			, { "Initial State", "初期State" }, { "Reset On Disable", "無効化時にリセット" }
			, { "Name", "名前" }, { "Action Id", "Action ID" }, { "Built-in Action", "組み込みAction" }
			, { "Parameter Name", "Parameter名" }, { "Type", "種類" }, { "Value", "値" }
			, { "Enemy Prefab", "Enemy Prefab" }, { "Local Direction", "ローカル方向" }
			, { "Speed", "速度" }, { "Gravity", "重力" }, { "Lifetime", "寿命" }
			, { "Destroy On Hit", "Hit時に削除" }, { "Homing Strength", "追尾の強さ" }
			, { "Attack Animation Clip", "攻撃Animation Clip" }
			, { "Attack Prefab Animation Clip", "攻撃Prefab Animation Clip" }
			, { "Attack HitBox Entity Id", "攻撃HitBox Entity ID" }
			, { "Attack HitBox Entity Name", "攻撃HitBox Entity名" }
			, { "Homing Target Entity Id", "追尾対象Entity ID" }
			, { "Homing Target Entity Name", "追尾対象Entity名" }
			, { "Faction", "Faction" }, { "Knockback Multiplier", "ノックバック倍率" }
			, { "Reaction Trigger", "Reaction Trigger" }, { "Poise Recovery Delay", "Poise回復待機時間" }
			, { "Minimum Poise Damage", "最小Poiseダメージ" }, { "Hit State", "Hit State" }
			, { "Death State", "Death State" }, { "Deactivate Delay", "無効化までの時間" }
			, { "Death Effect Path", "死亡Effectパス" }
			, { "Behavior", "Behavior" }, { "Profile", "Profile" }, { "Movement Mode", "移動Mode" }
			, { "Separation Radius", "分離半径" }, { "Separation Weight", "分離の強さ" }
			, { "Neighbor Limit", "近傍数の上限" }, { "Group", "Group" }
			, { "Min Speed", "最小速度" }, { "Max Speed", "最大速度" }
			, { "Wander Strength", "Wanderの強さ" }, { "Random Seed", "乱数Seed" }
			, { "Align Forward To Velocity", "進行方向へ前方を揃える" }
			, { "Forward Axis", "前方Axis" }, { "Rotation Follow Speed", "回転追従速度" }
			, { "Use Water Bounds", "Water Boundsを使用" }, { "Bounds Weight", "Boundsの強さ" }
			, { "Attractor Weight", "Attractorの強さ" }, { "Schooling", "Schooling" }
			, { "Visual Color", "表示色" }, { "Enable Lighting", "Lightingを有効化" }
			, { "Tag", "Tag" }, { "Target Behavior", "対象Behavior" }, { "Target Profile", "対象Profile" }
			, { "Wander Change Interval", "Wander変更間隔" }, { "Wander Direction Range", "Wander方向範囲" }
			, { "Wander Vertical Range", "Wander垂直範囲" }, { "Randomize Seed On Play", "Play時にSeedをランダム化" }
			, { "Flock Decision Interval", "Flock判断間隔" }, { "Flock Acceleration", "Flock加速度" }
			, { "Flock Max Turn Rate", "Flock最大旋回速度" }, { "Return Strength", "復帰の強さ" }
			, { "Max Distance", "最大距離" }, { "Use Team Heading", "Team Headingを使用" }
			, { "Team Heading Direction", "Team Heading方向" }, { "Team Heading Weight", "Team Headingの強さ" }
			, { "Team Heading Follow Speed", "Team Heading追従速度" }, { "Pitch From Vertical Velocity", "垂直速度からPitchを設定" }
			, { "Banking Strength", "Bankingの強さ" }, { "Bounds Entity Id", "Bounds Entity ID" }
			, { "Bounds Name", "Bounds名" }, { "Attractor Entity Id", "Attractor Entity ID" }
			, { "Attractor Tag", "Attractor Tag" }, { "Schooling Update Interval", "Schooling更新間隔" }
			, { "Schooling Blend", "Schoolingの混合" }, { "Alignment Radius", "整列半径" }
			, { "Cohesion Radius", "結合半径" }, { "Alignment Weight", "整列の強さ" }
			, { "Cohesion Weight", "結合の強さ" }, { "Strength", "強さ" }
			, { "Override Team Agent Settings", "Team Agent設定を上書き" }
			, { "Jitter Strength", "Jitterの強さ" }, { "Jitter Frequency", "Jitter頻度" }
			, { "Jitter Update Interval", "Jitter更新間隔" }, { "Jitter Follow Speed", "Jitter追従速度" }
			, { "Leash Strength", "Leashの強さ" }, { "Catchup Speed", "追いつき速度" }
			, { "Separation Update Interval", "分離更新間隔" }, { "Separation Blend", "分離の混合" }
			, { "Rotate X", "Xを回転" }, { "Rotate Y", "Yを回転" }, { "Rotate Z", "Zを回転" }
			, { "Schooling Update Jitter", "Schooling更新の揺らぎ" }
			, { "Reference Name", "参照名" }, { "Target Scene Id", "対象Scene ID" }
			, { "Target Instance Key", "対象Instance Key" }, { "Target Scene", "対象Scene" }
			, { "Trigger Key", "Triggerキー" }, { "Play On Start", "開始時に再生" }
			, { "Loop", "繰り返す" }, { "Default Clip Index", "既定Clip番号" }
			, { "Transition Duration", "切替時間" }, { "Blend Curve", "Blend Curve" }
			, { "Clip Name", "Clip名" }, { "Duration", "時間" }
			, { "Property", "Property" }, { "Easing", "Easing" }, { "Time", "時間" }
			, { "Active Value", "Active値" }, { "Target Bone", "対象Bone" }
			, { "Alignment Mode", "配置Mode" }, { "Weapon Bone", "Weapon Bone" }
			, { "Label", "表示名" }, { "Copy Scene Baseline", "Sceneの基準値をコピー" }
			, { "Animate Dissolve Threshold", "Dissolve Thresholdをアニメーション" }
			, { "Automation Start", "Automation開始値" }, { "Automation End", "Automation終了値" }
			, { "Automation Duration", "Automation時間" }
			, { "Trigger", "Trigger" }, { "Camera Path", "Camera Path" }
			, { "Stat Id", "Stat ID" }, { "Comparison", "比較" }, { "Compare Value", "比較値" }
			, { "Target Position", "対象位置" }, { "Key", "キー" }
			, { "Trigger Once", "一度だけ発火" }, { "Cooldown", "Cooldown" }
			, { "Type", "種類" }, { "Action Target Entity Id", "Action対象Entity ID" }
			, { "Action Target Entity Name", "Action対象Entity名" }, { "Action Stat Id", "Action Stat ID" }
			, { "Operation", "操作" }, { "Prefab Path", "Prefabパス" }
			, { "Parent To Target", "対象の子にする" }, { "Spawn At Target Transform", "対象TransformでSpawn" }
			, { "State Name", "State名" }, { "Scene Id", "Scene ID" }
			, { "Manager", "Manager" }, { "Profile", "Profile" }
			, { "Name", "名前" }, { "Animation", "Animation" }, { "Windup", "攻撃開始時間" }
			, { "Active Time", "有効時間" }, { "Recovery", "復帰時間" }
			, { "Forward Distance", "前方移動距離" }, { "Side Distance", "横移動距離" }
			, { "Facing", "向き" }, { "Facing Target", "向きの対象Entity" }
			, { "Dedicated HitBox", "専用HitBox" }, { "Hit Policy", "Hit判定方式" }
			, { "Direction Mode", "方向Mode" }, { "Time", "時間" }
			, { "Particle Effect Path", "Particle Effectパス" }, { "Spawn Entity", "Spawn Entity" }
			, { "Euler Value (Radians)", "Euler値（Radians）" }
			, { "Inherit Bone Scale", "Bone Scaleを継承" }
			, { "Loop Enabled", "Loopを有効化" }
			, { "Loop Max Count (0 = Unlimited)", "Loop最大回数（0 = 無制限）" }
			, { "Loop Safety Timeout", "Loop安全Timeout" }
			, { "Start", "開始" }, { "End", "終了" }
			, { "HitBox", "HitBox" }
			, { "Override HitBox Half Size", "HitBox半サイズを上書き" }
			, { "HitBox Half Size", "HitBox半サイズ" }
			, { "Target Cooldown", "対象Cooldown" }
			, { "Local Direction", "ローカル方向" }
			, { "Ground Effect Type", "Ground Effectの種類" }
			, { "Ground Probe Distance", "Ground Probe距離" }
			, { "Ground Prefab Path", "Ground Prefabパス" }
			, { "Ground Prefab Lifetime", "Ground Prefabの寿命" }
			, { "Crack Radius", "Crack半径" }
			, { "Primary Branch Count", "主Branch数" }
			, { "Segments Per Branch", "BranchあたりのSegment数" }
			, { "Branch Probability", "Branch確率" }
			, { "Crack Width", "Crack幅" }
			, { "Crack Lifetime", "Crackの寿命" }
			, { "Crack Surface Offset", "CrackのSurface Offset" }
			, { "Local Offset", "ローカルオフセット" }
			, { "Target", "対象" }
			, { "Duration To Next", "次のPointまでの時間" }
			, { "Easing To Next", "次のPointへのEasing" }
			, { "Default Clip", "既定Clip" }
			, { "Body Type", "Bodyの種類" }
			, { "Freeze Position", "位置を固定" }
		};
		for (const auto& [source, localized] : labels) {
			if (std::strcmp(source, english) == 0) {
				static thread_local std::string label;
				label = localized;
				label += "###";
				label += english;
				return label.c_str();
			}
		}
		return english;
	}

	bool MatchesEditorComponentSearch(
		const EditorComponentDefinition& definition,
		const std::string& search
	) {
		if (search.empty()) {
			return true;
		}
		std::string searchable = std::string(definition.type) + " " +
			definition.japaneseName + " " + definition.englishName + " " +
			definition.japaneseDescription + " " +
			definition.englishDescription;
		return ContainsCaseInsensitive(searchable, search);
	}

	std::string MakeComponentFoldoutKey(
		const std::string& sceneId,
		uint64_t entityId,
		const std::string& componentType
	) {
		return sceneId + "/" + std::to_string(entityId) + "/" + componentType;
	}

	std::string GetProjectResourcePath(const std::string& path) {
		return PathToUtf8(EditableResourcePath::ToProjectRelative(
			EditableResourcePath::ResolveResource(PathFromUtf8(path))
		));
	}

	std::string GetModelPathRelativeToResources(const std::string& fullPath) {
		const std::string projectPath = GetProjectResourcePath(fullPath);
		const std::string prefix = "resources/";
		if (projectPath.rfind(prefix, 0) == 0) {
			return projectPath.substr(prefix.length());
		}
		return projectPath;
	}

	std::vector<std::string> CollectModelAssetPaths() {
		std::vector<std::string> paths;
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			GetProjectResourceRoot(),
			std::filesystem::directory_options::skip_permission_denied,
			error
		);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			if (iterator->is_regular_file(error) && IsModelAssetPath(iterator->path())) {
				paths.push_back(GetModelPathRelativeToResources(
					PathToUtf8(iterator->path())
				));
			}
			iterator.increment(error);
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}

	std::vector<std::string> CollectTextureAssetPaths() {
		std::vector<std::string> paths;
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			GetProjectResourceRoot(),
			std::filesystem::directory_options::skip_permission_denied,
			error
		);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			if (iterator->is_regular_file(error) && IsTextureAssetPath(iterator->path())) {
				paths.push_back(GetProjectResourcePath(
					PathToUtf8(iterator->path())
				));
			}
			iterator.increment(error);
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}

	std::vector<std::string> CollectAudioAssetPaths() {
		std::vector<std::string> paths;
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			GetProjectResourceRoot(),
			std::filesystem::directory_options::skip_permission_denied,
			error
		);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end) {
			if (iterator->is_regular_file(error) && IsAudioAssetPath(iterator->path())) {
				paths.push_back(GetProjectResourcePath(PathToUtf8(iterator->path())));
			}
			iterator.increment(error);
		}
		std::sort(paths.begin(), paths.end());
		paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
		return paths;
	}

	Vector3 TransformCoord(const Vector3& value, const Matrix4x4& matrix) {
		const float x =
			value.x * matrix.m[0][0] +
			value.y * matrix.m[1][0] +
			value.z * matrix.m[2][0] +
			matrix.m[3][0];
		const float y =
			value.x * matrix.m[0][1] +
			value.y * matrix.m[1][1] +
			value.z * matrix.m[2][1] +
			matrix.m[3][1];
		const float z =
			value.x * matrix.m[0][2] +
			value.y * matrix.m[1][2] +
			value.z * matrix.m[2][2] +
			matrix.m[3][2];
		const float w =
			value.x * matrix.m[0][3] +
			value.y * matrix.m[1][3] +
			value.z * matrix.m[2][3] +
			matrix.m[3][3];
		const float inverseW = std::abs(w) > 0.000001f ? 1.0f / w : 1.0f;
		return { x * inverseW, y * inverseW, z * inverseW };
	}

	bool IntersectRayAabb(
		const Vector3& rayOrigin,
		const Vector3& rayDirection,
		const Vector3& boundsMin,
		const Vector3& boundsMax,
		float& outDistance
	) {
		float tMin = 0.0f;
		float tMax = (std::numeric_limits<float>::max)();
		const float origin[3] = {
			rayOrigin.x,
			rayOrigin.y,
			rayOrigin.z
		};
		const float direction[3] = {
			rayDirection.x,
			rayDirection.y,
			rayDirection.z
		};
		const float minimum[3] = {
			boundsMin.x,
			boundsMin.y,
			boundsMin.z
		};
		const float maximum[3] = {
			boundsMax.x,
			boundsMax.y,
			boundsMax.z
		};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			if (std::abs(direction[axis]) < 0.000001f) {
				if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
					return false;
				}
				continue;
			}
			float t1 = (minimum[axis] - origin[axis]) / direction[axis];
			float t2 = (maximum[axis] - origin[axis]) / direction[axis];
			if (t1 > t2) {
				std::swap(t1, t2);
			}
			tMin = (std::max)(tMin, t1);
			tMax = (std::min)(tMax, t2);
			if (tMin > tMax) {
				return false;
			}
		}
		outDistance = tMin;
		return true;
	}

	bool IntersectRaySphere(
		const Vector3& rayOrigin,
		const Vector3& rayDirection,
		const Vector3& center,
		float radius,
		float& outDistance
	) {
		const Vector3 originToCenter = Math::Subtract(rayOrigin, center);
		const float projected = Math::Dot(originToCenter, rayDirection);
		const float squaredDistance = Math::Dot(
			originToCenter,
			originToCenter
		);
		const float discriminant =
			projected * projected - (squaredDistance - radius * radius);
		if (discriminant < 0.0f) {
			return false;
		}
		outDistance = -projected - std::sqrt(discriminant);
		if (outDistance < 0.0f) {
			outDistance = -projected + std::sqrt(discriminant);
		}
		return outDistance >= 0.0f;
	}

	float GetMaxWorldAxisScale(const Matrix4x4& matrix) {
		return (std::max)(
			Math::Length({ matrix.m[0][0], matrix.m[0][1], matrix.m[0][2] }),
			(std::max)(
				Math::Length({ matrix.m[1][0], matrix.m[1][1], matrix.m[1][2] }),
				Math::Length({ matrix.m[2][0], matrix.m[2][1], matrix.m[2][2] })
			)
		);
	}

	bool IsLowPriorityPickTarget(
		const SceneEntity& entity,
		const SceneComponent& meshRenderer,
		const Vector3& localMin,
		const Vector3& localMax
	) {
		std::string name = entity.name;
		std::string modelPath = meshRenderer.modelPath;
		std::transform(name.begin(), name.end(), name.begin(), ::tolower);
		std::transform(
			modelPath.begin(),
			modelPath.end(),
			modelPath.begin(),
			::tolower
		);
		if (
			name.find("terrain") != std::string::npos ||
			modelPath.find("terrain") != std::string::npos
		) {
			return true;
		}

		const Vector3 extent = {
			std::abs((localMax.x - localMin.x) * entity.transform.scale.x),
			std::abs((localMax.y - localMin.y) * entity.transform.scale.y),
			std::abs((localMax.z - localMin.z) * entity.transform.scale.z)
		};
		const float footprint = extent.x * extent.z;
		return footprint > 2500.0f || (extent.x > 80.0f && extent.z > 80.0f);
	}

}

ImGuiManager* ImGuiManager::instance = nullptr;

ImGuiManager* ImGuiManager::GetInstance() {
	return instance;
}

bool ImGuiManager::ConsumeOpenSceneRequest(
	std::string& sceneId,
	bool& discardUnsavedChanges
) {
	if (requestedSceneId_.empty()) {
		return false;
	}
	sceneId = std::move(requestedSceneId_);
	requestedSceneId_.clear();
	discardUnsavedChanges = requestedSceneDiscardUnsavedChanges_;
	requestedSceneDiscardUnsavedChanges_ = false;
	return true;
}

bool ImGuiManager::ConsumeSceneAssetRequest(SceneAssetRequest& request) {
	if (!sceneAssetRequestPending_) {
		return false;
	}
	request = std::move(requestedSceneAsset_);
	requestedSceneAsset_ = {};
	sceneAssetRequestPending_ = false;
	return true;
}

bool ImGuiManager::ConsumeSceneInstanceRequest(SceneInstanceRequest& request) {
	if (!sceneInstanceRequestPending_) {
		return false;
	}
	request = std::move(requestedSceneInstance_);
	requestedSceneInstance_ = {};
	sceneInstanceRequestPending_ = false;
	return true;
}

bool ImGuiManager::ConsumeStartSceneRequest(std::string& sceneId) {
	if (!startSceneRequestPending_) {
		return false;
	}
	sceneId = std::move(requestedStartSceneId_);
	requestedStartSceneId_.clear();
	startSceneRequestPending_ = false;
	return true;
}

bool ImGuiManager::ConsumeStartupModeRequest(
	SceneBuildConfiguration& configuration,
	SceneStartupMode& mode
) {
	if (!startupModeRequestPending_) {
		return false;
	}
	configuration = requestedStartupConfiguration_;
	mode = requestedStartupMode_;
	startupModeRequestPending_ = false;
	return true;
}

void ImGuiManager::NotifySceneAssetOperationResult(
	bool success,
	const std::string& message
) {
	if (success) {
		InvalidateProjectCache();
		return;
	}
	sceneAssetErrorMessage_ = message.empty()
		? "Scene asset operation failed."
		: message;
	sceneAssetErrorPopupRequested_ = true;
}

void ImGuiManager::NotifySceneInstanceOperationResult(
	bool success,
	const std::string& message
) {
	sceneInstanceOperationSucceeded_ = success;
	sceneInstanceStatusMessage_ = message;
}

void ImGuiManager::NotifyProjectSettingsResult(
	bool success,
	const std::string& message
) {
	if (success) {
		return;
	}
	projectSettingsErrorMessage_ = message.empty()
		? "Project Settings could not be saved."
		: message;
	projectSettingsErrorPopupRequested_ = true;
}

void ImGuiManager::NotifyEditSceneOpened() {
	selectedEntityId_ = 0;
	selectedEntityIds_.clear();
	ResetComponentPicker(sceneComponentPicker_);
	hierarchySelectionAnchorId_ = 0;
	hierarchyObservedEntityId_ = 0;
	hierarchyRenameEntityId_ = 0;
	hierarchyRevealRequested_ = false;
	revealInspectorRequested_ = false;
}

bool ImGuiManager::sceneViewInputActive_ = false;

bool ImGuiManager::LoadStartFullscreenSetting() {
	std::string text;
	if (!EditableResourcePath::ReadText(kEditorSettingsPath, text)) {
		return false;
	}

	try {
		const json settings = json::parse(text);
		return settings.contains("startFullscreen") &&
			settings["startFullscreen"].is_boolean() &&
			settings["startFullscreen"].get<bool>();
	} catch (const json::exception&) {
		return false;
	}
}

void ImGuiManager::Initialize(WinApp* winApp, DirectXCommon* dxCommon, SrvManager* srvManager){
	instance = this;
	assert(winApp);
	assert(dxCommon);
	assert(srvManager);

	winApp_ = winApp;
	dxCommon_ = dxCommon;
	srvManager_ = srvManager;
	sceneViewWidth_ = dxCommon_->GetClientWidth();
	sceneViewHeight_ = dxCommon_->GetClientHeight();
	prefabEditorSession_ = new PrefabEditorSession();
	prefabAnimationPreviewDocument_ = new SceneDocument();
	playerCombatPreviewDocument_ = new SceneDocument();
	prefabHitBoxGhostDocument_ = new SceneDocument();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	// 旧PrefabはEditor起動時に一度だけIDを付与し、アクセス履歴を即時安定化する。
	PrefabAssetRegistry::MigrateMissingAssetIds();
	LoadEditorSettings();

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	const float dpiScale =
		static_cast<float>(GetDpiForWindow(winApp_->GetHwnd())) / 96.0f;
	ConfigureEditorFont(io, dpiScale);
	style.ScaleAllSizes(dpiScale);
	style.WindowRounding = 0.0f;
	style.ChildRounding = 0.0f;
	style.FrameRounding = 2.0f;
	style.PopupRounding = 2.0f;
	style.TabRounding = 2.0f;
	style.WindowBorderSize = 1.0f;
	style.FrameBorderSize = 0.0f;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.11f, 0.12f, 1.0f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.075f, 0.08f, 0.09f, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.15f, 1.0f);
	style.Colors[ImGuiCol_Tab] = ImVec4(0.09f, 0.095f, 0.105f, 1.0f);
	style.Colors[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.34f, 0.48f, 1.0f);
	style.Colors[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.24f, 0.33f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.25f, 0.32f, 1.0f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.36f, 0.48f, 1.0f);
	style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.24f, 0.54f, 0.82f, 0.7f);

	ImGui_ImplWin32_Init(winApp_->GetHwnd());

	ImGui_ImplDX12_InitInfo initInfo{};
	initInfo.Device = dxCommon_->GetDevice();
	initInfo.CommandQueue = dxCommon_->GetCommandQueue();
	initInfo.NumFramesInFlight = dxCommon_->GetSwapChainResourcesNum();
	initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	initInfo.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	initInfo.SrvDescriptorHeap = srvManager_->GetDescriptorHeap();
	initInfo.UserData = srvManager_;

	initInfo.SrvDescriptorAllocFn =
		[](ImGui_ImplDX12_InitInfo* info,
		   D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
		   D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle){
			   SrvManager* srvManager = static_cast<SrvManager*>(info->UserData);
			   assert(srvManager);
			   assert(srvManager->CanAllocate());

			   uint32_t index = srvManager->Allocate();
			   *out_cpu_handle = srvManager->GetCPUDescriptorHandle(index);
			   *out_gpu_handle = srvManager->GetGPUDescriptorHandle(index);
		};

	initInfo.SrvDescriptorFreeFn =
		[](ImGui_ImplDX12_InitInfo*,
		   D3D12_CPU_DESCRIPTOR_HANDLE,
		   D3D12_GPU_DESCRIPTOR_HANDLE){
			   // 今のSrvManagerには解放機能が無いので何もしない
		};

	ImGui_ImplDX12_Init(&initInfo);
}

void ImGuiManager::ConfigureEditorFont(ImGuiIO& io, float dpiScale) {
	const std::filesystem::path fontDirectory =
		EditableResourcePath::Resolve("resources/fonts");
	const std::vector<std::filesystem::path> msGothicFontCandidates = {
		fontDirectory / "msgothic.ttc",
		L"C:\\Windows\\Fonts\\msgothic.ttc"
	};
	const std::vector<std::filesystem::path> yuGothicFontCandidates = {
		fontDirectory / "YuGothM.ttc",
		fontDirectory / "YuGothR.ttc",
		L"C:\\Windows\\Fonts\\YuGothM.ttc",
		L"C:\\Windows\\Fonts\\YuGothR.ttc"
	};
	const std::vector<std::filesystem::path> meiryoFontCandidates = {
		fontDirectory / "meiryo.ttc",
		L"C:\\Windows\\Fonts\\meiryo.ttc"
	};
	const std::vector<std::filesystem::path> bizUdGothicFontCandidates = {
		fontDirectory / "BIZ-UDGothicR.ttc",
		L"C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc"
	};
	const std::vector<std::filesystem::path> windowsJapaneseFallbackCandidates = {
		fontDirectory / "msgothic.ttc",
		L"C:\\Windows\\Fonts\\msgothic.ttc",
		fontDirectory / "YuGothM.ttc",
		fontDirectory / "YuGothR.ttc",
		L"C:\\Windows\\Fonts\\YuGothM.ttc",
		L"C:\\Windows\\Fonts\\YuGothR.ttc",
		fontDirectory / "meiryo.ttc",
		L"C:\\Windows\\Fonts\\meiryo.ttc",
		fontDirectory / "BIZ-UDGothicR.ttc",
		L"C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc"
	};
	const std::vector<std::filesystem::path> cascadiaFontCandidates = {
		fontDirectory / "CascadiaMono.ttf",
		fontDirectory / "CascadiaCode.ttf",
		L"C:\\Windows\\Fonts\\CascadiaMono.ttf",
		L"C:\\Windows\\Fonts\\CascadiaCode.ttf"
	};
	const std::vector<std::filesystem::path> japaneseFallbackCandidates = {
		fontDirectory / "NotoSansCJKjp-Regular.otf",
		fontDirectory / "NotoSansJP-Regular.ttf",
		fontDirectory / "NotoSansJP-VF.ttf",
		L"C:\\Windows\\Fonts\\NotoSansJP-VF.ttf",
		L"C:\\Windows\\Fonts\\YuGothM.ttc",
		L"C:\\Windows\\Fonts\\meiryo.ttc",
		L"C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc",
		L"C:\\Windows\\Fonts\\msgothic.ttc"
	};

	editorBaseFontData_.clear();
	editorJapaneseFontData_.clear();
	editorGlyphRanges_.clear();

	ImFontGlyphRangesBuilder glyphBuilder;
	glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
	AddProjectAssetGlyphs(glyphBuilder);
	glyphBuilder.BuildRanges(&editorGlyphRanges_);

	ImFontConfig fontConfig{};
	fontConfig.SizePixels = editorFontSize_ * dpiScale;
	fontConfig.OversampleH = 1;
	fontConfig.OversampleV = 1;
	fontConfig.FontDataOwnedByAtlas = false;

	const std::vector<std::filesystem::path>* selectedCandidates = nullptr;
	bool selectedFontContainsJapanese = true;
	switch (editorFontPreset_) {
	case EditorFontPreset::MsGothic:
		selectedCandidates = &msGothicFontCandidates;
		break;
	case EditorFontPreset::YuGothicUi:
		selectedCandidates = &yuGothicFontCandidates;
		break;
	case EditorFontPreset::Meiryo:
		selectedCandidates = &meiryoFontCandidates;
		break;
	case EditorFontPreset::BizUdGothic:
		selectedCandidates = &bizUdGothicFontCandidates;
		break;
	case EditorFontPreset::CascadiaMonoWithJapanese:
		selectedCandidates = &cascadiaFontCandidates;
		selectedFontContainsJapanese = false;
		break;
	case EditorFontPreset::ImGuiDefaultWithJapanese:
		selectedFontContainsJapanese = false;
		break;
	}

	ImFont* editorFont = nullptr;
	if (selectedCandidates &&
		LoadFirstAvailableFont(*selectedCandidates, editorBaseFontData_)) {
		editorFont = io.Fonts->AddFontFromMemoryTTF(
			editorBaseFontData_.data(),
			static_cast<int>(editorBaseFontData_.size()),
			fontConfig.SizePixels,
			&fontConfig,
			editorGlyphRanges_.Data
		);
	}

	// 選択Fontが無い環境でも、日英を単一書体で表示できるWindows Fontを優先する。
	if (!editorFont && editorFontPreset_ != EditorFontPreset::ImGuiDefaultWithJapanese &&
		LoadFirstAvailableFont(windowsJapaneseFallbackCandidates, editorBaseFontData_)) {
		selectedFontContainsJapanese = true;
		editorFont = io.Fonts->AddFontFromMemoryTTF(
			editorBaseFontData_.data(),
			static_cast<int>(editorBaseFontData_.size()),
			fontConfig.SizePixels,
			&fontConfig,
			editorGlyphRanges_.Data
		);
	}

	if (!editorFont) {
		selectedFontContainsJapanese = false;
		editorFont = io.Fonts->AddFontDefault(&fontConfig);
	}

	if (editorFont && !selectedFontContainsJapanese &&
		LoadFirstAvailableFont(japaneseFallbackCandidates, editorJapaneseFontData_)) {
		ImFontConfig mergeConfig = fontConfig;
		mergeConfig.MergeMode = true;
		mergeConfig.DstFont = editorFont;
		io.Fonts->AddFontFromMemoryTTF(
			editorJapaneseFontData_.data(),
			static_cast<int>(editorJapaneseFontData_.size()),
			fontConfig.SizePixels,
			&mergeConfig,
			editorGlyphRanges_.Data
		);
	}

	if (editorFont) {
		io.FontDefault = editorFont;
	}
}

void ImGuiManager::BeginFrame(){
	ImGuiIO& io = ImGui::GetIO();
	Input* input = Input::GetInstance();
	const bool altHeld = input &&
		(input->PushKey(DIK_LMENU) || input->PushKey(DIK_RMENU));
	const bool blockEditorMouse =
		editorSession_ && editorSession_->IsPlaying() && !altHeld;
	if (blockEditorMouse) {
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	} else {
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	}
	ApplyPendingEditorFont();
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();

	CreateDockSpace();
}

void ImGuiManager::LoadEditorSettings() {
	std::string text;
	if (!EditableResourcePath::ReadText(kEditorSettingsPath, text)) {
		return;
	}

	try {
		const json settings = json::parse(text);
		if (settings.contains("language") && settings["language"].is_string()) {
			editorLanguage_ = ParseEditorLanguage(
				settings["language"].get<std::string>()
			);
		}
		const std::string preset = settings.value(
			"fontPreset",
			"msGothic"
		);
		if (preset == "yuGothicUi" || preset == "unifiedCjk") {
			editorFontPreset_ = EditorFontPreset::YuGothicUi;
		} else if (preset == "meiryo") {
			editorFontPreset_ = EditorFontPreset::Meiryo;
		} else if (preset == "bizUdGothic") {
			editorFontPreset_ = EditorFontPreset::BizUdGothic;
		} else if (preset == "imguiDefaultWithJapanese") {
			editorFontPreset_ = EditorFontPreset::ImGuiDefaultWithJapanese;
		} else if (preset == "cascadiaMonoWithJapanese" ||
			preset == "cascadiaMonoWithCjk") {
			editorFontPreset_ = EditorFontPreset::CascadiaMonoWithJapanese;
		} else {
			// 旧既定のoriginalWithCjkも、新しい日英単一書体の既定へ移行する。
			editorFontPreset_ = EditorFontPreset::MsGothic;
		}
		if (settings.contains("fontSize") && settings["fontSize"].is_number()) {
			editorFontSize_ = std::clamp(
				settings["fontSize"].get<float>(),
				10.0f,
				22.0f
			);
		}
		if (
			settings.contains("startFullscreen") &&
			settings["startFullscreen"].is_boolean()
		) {
			startFullscreen_ = settings["startFullscreen"].get<bool>();
		}
		if (
			settings.contains("sceneGridVisible") &&
			settings["sceneGridVisible"].is_boolean()
		) {
			sceneGridVisible_ = settings["sceneGridVisible"].get<bool>();
		}
		if (
			settings.contains("prefabGridVisible") &&
			settings["prefabGridVisible"].is_boolean()
		) {
			prefabGridVisible_ = settings["prefabGridVisible"].get<bool>();
		}
		if (
			settings.contains("sceneAxisVisible") &&
			settings["sceneAxisVisible"].is_boolean()
		) {
			sceneAxisVisible_ = settings["sceneAxisVisible"].get<bool>();
		}
		if (
			settings.contains("prefabAxisVisible") &&
			settings["prefabAxisVisible"].is_boolean()
		) {
			prefabAxisVisible_ = settings["prefabAxisVisible"].get<bool>();
		}
		if (
			settings.contains("componentFoldouts") &&
			settings["componentFoldouts"].is_object()
		) {
			componentFoldoutStates_.clear();
			for (const auto& [key, value] : settings["componentFoldouts"].items()) {
				if (value.is_boolean()) {
					componentFoldoutStates_[key] = value.get<bool>();
				}
			}
		}
		if (
			settings.contains("inspectorFoldouts") &&
			settings["inspectorFoldouts"].is_object()
		) {
			inspectorFoldoutStates_.clear();
			for (const auto& [key, value] : settings["inspectorFoldouts"].items()) {
				if (value.is_boolean()) {
					inspectorFoldoutStates_[key] = value.get<bool>();
				}
			}
		}
		componentInspectorMode_ = ComponentInspectorMode::Detailed;
		if (
			settings.contains("componentInspectorMode") &&
			settings["componentInspectorMode"].is_string() &&
			settings["componentInspectorMode"].get<std::string>() == "simple"
		) {
			componentInspectorMode_ = ComponentInspectorMode::Simple;
		}
		if (
			settings.contains("favoriteComponentTypes") &&
			settings["favoriteComponentTypes"].is_array()
		) {
			favoriteComponentTypes_.clear();
			for (const json& value : settings["favoriteComponentTypes"]) {
				if (!value.is_string()) {
					continue;
				}
				const std::string type = value.get<std::string>();
				if (FindEditorComponentDefinition(type)) {
					favoriteComponentTypes_.insert(type);
				}
			}
		}
		if (
			settings.contains("recentPrefabs") &&
			settings["recentPrefabs"].is_array()
		) {
			recentPrefabReferences_.clear();
			for (const json& value : settings["recentPrefabs"]) {
				PrefabAssetReference reference{};
				if (!TryParsePrefabAssetReference(value, reference)) {
					continue;
				}
				if (!ContainsPrefabAssetReference(
					recentPrefabReferences_,
					reference
				)) {
					recentPrefabReferences_.push_back(std::move(reference));
				}
				if (recentPrefabReferences_.size() >= 12) {
					break;
				}
			}
		}
		if (
			settings.contains("favoritePrefabs") &&
			settings["favoritePrefabs"].is_array()
		) {
			favoritePrefabReferences_.clear();
			for (const json& value : settings["favoritePrefabs"]) {
				PrefabAssetReference reference{};
				if (!TryParsePrefabAssetReference(value, reference)) {
					continue;
				}
				if (!ContainsPrefabAssetReference(
					favoritePrefabReferences_,
					reference
				)) {
					favoritePrefabReferences_.push_back(std::move(reference));
				}
			}
		}
	} catch (const json::exception&) {
		// 壊れたエディタ設定は無視し、既定値で起動する。
	}
}

void ImGuiManager::SaveEditorSettings() const {
	const char* preset = "msGothic";
	if (editorFontPreset_ == EditorFontPreset::YuGothicUi) {
		preset = "yuGothicUi";
	} else if (editorFontPreset_ == EditorFontPreset::Meiryo) {
		preset = "meiryo";
	} else if (editorFontPreset_ == EditorFontPreset::BizUdGothic) {
		preset = "bizUdGothic";
	} else if (editorFontPreset_ == EditorFontPreset::ImGuiDefaultWithJapanese) {
		preset = "imguiDefaultWithJapanese";
	} else if (editorFontPreset_ == EditorFontPreset::CascadiaMonoWithJapanese) {
		preset = "cascadiaMonoWithJapanese";
	}

	json componentFoldouts = json::object();
	for (const auto& [key, open] : componentFoldoutStates_) {
		componentFoldouts[key] = open;
	}
	json inspectorFoldouts = json::object();
	for (const auto& [key, open] : inspectorFoldoutStates_) {
		inspectorFoldouts[key] = open;
	}
	std::vector<std::string> favoriteComponentTypes(
		favoriteComponentTypes_.begin(),
		favoriteComponentTypes_.end()
	);
	std::sort(
		favoriteComponentTypes.begin(),
		favoriteComponentTypes.end()
	);
	json favoriteComponentTypeValues = json::array();
	for (const std::string& type : favoriteComponentTypes) {
		favoriteComponentTypeValues.push_back(type);
	}
	json recentPrefabs = json::array();
	for (const PrefabAssetReference& reference : recentPrefabReferences_) {
		recentPrefabs.push_back(PrefabAssetReferenceToJson(reference));
	}
	std::vector<PrefabAssetReference> favoritePrefabReferences =
		favoritePrefabReferences_;
	std::sort(
		favoritePrefabReferences.begin(),
		favoritePrefabReferences.end(),
		[](const PrefabAssetReference& left, const PrefabAssetReference& right) {
			return PrefabAssetRegistry::ResolvePath(left) <
				PrefabAssetRegistry::ResolvePath(right);
		}
	);
	json favoritePrefabs = json::array();
	for (const PrefabAssetReference& reference : favoritePrefabReferences) {
		favoritePrefabs.push_back(PrefabAssetReferenceToJson(reference));
	}

	const json settings = {
		{ "language", ToEditorLanguageSettingValue(editorLanguage_) },
		{ "fontPreset", preset },
		{ "fontSize", editorFontSize_ },
		{ "startFullscreen", startFullscreen_ },
		{ "sceneGridVisible", sceneGridVisible_ },
		{ "prefabGridVisible", prefabGridVisible_ },
		{ "sceneAxisVisible", sceneAxisVisible_ },
		{ "prefabAxisVisible", prefabAxisVisible_ },
		{ "componentFoldouts", std::move(componentFoldouts) },
		{ "inspectorFoldouts", std::move(inspectorFoldouts) },
		{
			"componentInspectorMode",
			componentInspectorMode_ == ComponentInspectorMode::Simple
				? "simple"
				: "detailed"
		},
		{ "favoriteComponentTypes", std::move(favoriteComponentTypeValues) },
		{ "recentPrefabs", std::move(recentPrefabs) },
		{ "favoritePrefabs", std::move(favoritePrefabs) }
	};
	EditableResourcePath::WriteTextAtomically(
		kEditorSettingsPath,
		settings.dump(2)
	);
}

bool ImGuiManager::DrawPersistentInspectorHeader(
	const std::string& key,
	const char* label,
	bool defaultOpen
) {
	const auto saved = inspectorFoldoutStates_.find(key);
	const bool wasOpen = saved == inspectorFoldoutStates_.end()
		? defaultOpen
		: saved->second;
	ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
	const bool open = ImGui::CollapsingHeader(
		label,
		ImGuiTreeNodeFlags_SpanAvailWidth
	);
	if (open != wasOpen) {
		inspectorFoldoutStates_[key] = open;
		SaveEditorSettings();
	}
	return open;
}

void ImGuiManager::RequestEditorFontRebuild() {
	editorFontRebuildRequested_ = true;
}

void ImGuiManager::ApplyPendingEditorFont() {
	if (!editorFontRebuildRequested_) {
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	io.FontDefault = nullptr;
	io.Fonts->Clear();
	const float dpiScale =
		static_cast<float>(GetDpiForWindow(winApp_->GetHwnd())) / 96.0f;
	ConfigureEditorFont(io, dpiScale);
	editorFontRebuildRequested_ = false;
}

void ImGuiManager::EndFrame(){
	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dxCommon_->GetCommandList());
	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		// Platform WindowはBackend側のSwapChainとCommand Queueで描画する。
		// メインDraw Dataの後に更新し、Dock解除したWindowをアプリ外にも表示する。
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

void ImGuiManager::DrawEditorWorkspace(
	D3D12_GPU_DESCRIPTOR_HANDLE sceneTexture,
	uint32_t textureWidth,
	uint32_t textureHeight,
	const char* sceneName
) {
	prefabKeyboardFocusThisFrame_ = false;
	if (editorSession_) {
		const ImGuiIO& io = ImGui::GetIO();
		const bool mayEditThisFrame =
			ImGui::IsAnyItemActive() ||
			io.WantTextInput ||
			io.MouseDown[ImGuiMouseButton_Left] ||
			io.MouseDown[ImGuiMouseButton_Right] ||
			io.MouseDown[ImGuiMouseButton_Middle] ||
			io.MouseClicked[ImGuiMouseButton_Left] ||
			io.MouseClicked[ImGuiMouseButton_Right] ||
			io.MouseClicked[ImGuiMouseButton_Middle];
		if (mayEditThisFrame) {
			editorSession_->BeginEditFrame();
		}
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin(
		"Scene",
		nullptr,
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse
	);

	const ImVec2 availableSize = ImGui::GetContentRegionAvail();
	sceneViewWidth_ = static_cast<uint32_t>(
		(std::max)(availableSize.x, 1.0f)
	);
	sceneViewHeight_ = static_cast<uint32_t>(
		(std::max)(availableSize.y, 1.0f)
	);

	const ImTextureID textureId =
		static_cast<ImTextureID>(sceneTexture.ptr);
	ImGui::Image(
		ImTextureRef(textureId),
		availableSize,
		ImVec2(0.0f, 0.0f),
		ImVec2(1.0f, 1.0f)
	);
	const bool sceneImageHovered = ImGui::IsItemHovered();
	const ImVec2 sceneMin = ImGui::GetItemRectMin();
	const ImVec2 sceneMax = ImGui::GetItemRectMax();
	sceneViewMinX_ = sceneMin.x;
	sceneViewMinY_ = sceneMin.y;
	sceneViewMaxX_ = sceneMax.x;
	sceneViewMaxY_ = sceneMax.y;
	if (Camera* camera = Object3dCommon::GetInstance()
		? Object3dCommon::GetInstance()->GetDefaultCamera()
		: nullptr) {
		DrawSceneDebugLabels(
			sceneMin,
			sceneMax,
			camera->GetViewProjectionMatrix()
		);
	}
	if (
		editorSession_ &&
		editorSession_->IsEditing() &&
		ImGui::BeginDragDropTarget()
	) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
			"PROJECT_TEXTURE_PATH"
		)) {
			const char* droppedPath = static_cast<const char*>(payload->Data);
			if (droppedPath && droppedPath[0] != '\0') {
				SceneDocument& document = editorSession_->GetEditDocument();
				const std::filesystem::path texturePath = PathFromUtf8(droppedPath);
				std::string entityName = PathToUtf8(texturePath.stem());
				if (entityName.empty()) {
					entityName = "Sprite";
				}
				const std::string baseName = entityName;
				uint32_t suffix = 2;
				while (document.FindEntityByName(entityName)) {
					entityName = baseName + " " + std::to_string(suffix++);
				}
				SceneEntity& entity = document.CreateEntity(entityName);
				entity.spriteTexturePath = GetProjectResourcePath(
					PathToUtf8(texturePath)
				);
				document.AddComponent(entity.id, "SpriteRenderer");
				entity.components.front().texturePath = entity.spriteTexturePath;
				const ImVec2 mouse = ImGui::GetMousePos();
				const float normalizedX = (mouse.x - sceneMin.x) /
					(std::max)(sceneMax.x - sceneMin.x, 1.0f);
				const float normalizedY = (mouse.y - sceneMin.y) /
					(std::max)(sceneMax.y - sceneMin.y, 1.0f);
				entity.transform.translate = {
					normalizedX * static_cast<float>((std::max)(textureWidth, uint32_t{ 1 })),
					normalizedY * static_cast<float>((std::max)(textureHeight, uint32_t{ 1 })),
					0.0f
				};
				if (TextureManager::GetInstance()) {
					TextureManager::GetInstance()->LoadTexture(entity.spriteTexturePath);
					const auto& metadata = TextureManager::GetInstance()->GetMetaData(
						entity.spriteTexturePath
					);
					entity.spriteSize = {
						static_cast<float>(metadata.width),
						static_cast<float>(metadata.height)
					};
					entity.components.front().spriteSize = entity.spriteSize;
				}
				selectedEntityId_ = entity.id;
				selectedProjectFile_.clear();
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
			"PROJECT_MODEL_PATH"
		)) {
			const char* droppedPath = static_cast<const char*>(payload->Data);
			if (droppedPath && droppedPath[0] != '\0') {
				SceneDocument& document = editorSession_->GetEditDocument();
				const std::filesystem::path modelPath = PathFromUtf8(droppedPath);
				std::string entityName = PathToUtf8(modelPath.stem());
				if (entityName.empty()) {
					entityName = "Model";
				}
				const std::string baseName = entityName;
				uint32_t suffix = 2;
				while (document.FindEntityByName(entityName)) {
					entityName = baseName + " " + std::to_string(suffix++);
				}
				SceneEntity& entity = document.CreateEntity(entityName);
				entity.modelPath = GetModelPathRelativeToResources(
					PathToUtf8(modelPath)
				);
				document.AddComponent(entity.id, "MeshRenderer");
				entity.components.front().modelPath = entity.modelPath;
				Object3dCommon* object3dCommon = Object3dCommon::GetInstance();
				if (Camera* camera = object3dCommon
					? object3dCommon->GetDefaultCamera()
					: nullptr) {
					const Matrix4x4& cameraWorld = camera->GetWorldMatrix();
					entity.transform.translate = {
						cameraWorld.m[3][0] + cameraWorld.m[2][0] * 5.0f,
						cameraWorld.m[3][1] + cameraWorld.m[2][1] * 5.0f,
						cameraWorld.m[3][2] + cameraWorld.m[2][2] * 5.0f
					};
				}
				if (ModelManager::GetInstance()) {
					ModelManager::GetInstance()->LoadModel(entity.modelPath);
				}
				selectedEntityId_ = entity.id;
				selectedProjectFile_.clear();
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
			"PROJECT_PREFAB_PATH"
		)) {
			const char* droppedPath = static_cast<const char*>(payload->Data);
			if (droppedPath && droppedPath[0] != '\0') {
				Vector3 dropPosition{};
				const Vector3* rootTranslate = nullptr;
				Camera* camera = Object3dCommon::GetInstance()
					? Object3dCommon::GetInstance()->GetDefaultCamera()
					: nullptr;
				if (camera) {
					const ImVec2 mouse = ImGui::GetMousePos();
					const float width = (std::max)(
						sceneMax.x - sceneMin.x,
						1.0f
					);
					const float height = (std::max)(
						sceneMax.y - sceneMin.y,
						1.0f
					);
					const float ndcX =
						((mouse.x - sceneMin.x) / width) * 2.0f - 1.0f;
					const float ndcY =
						1.0f - ((mouse.y - sceneMin.y) / height) * 2.0f;
					const Matrix4x4 inverseViewProjection = Inverse(
						Multiply(
							camera->GetViewMatrix(),
							camera->GetProjectionMatrix()
						)
					);
					const Vector3 nearPoint = TransformCoord(
						{ ndcX, ndcY, 0.0f },
						inverseViewProjection
					);
					const Vector3 farPoint = TransformCoord(
						{ ndcX, ndcY, 1.0f },
						inverseViewProjection
					);
					const Vector3 rayDirection = Math::Normalize(
						Math::Subtract(farPoint, nearPoint)
					);
					dropPosition = Math::Add(
						nearPoint,
						Math::Multiply(rayDirection, 5.0f)
					);
					rootTranslate = &dropPosition;
				}
				InstantiatePrefabInEditScene(
					droppedPath,
					0,
					rootTranslate
				);
			}
		}
		ImGui::EndDragDropTarget();
	}

	DrawSceneGizmo(
		sceneMin.x,
		sceneMin.y,
		sceneMax.x - sceneMin.x,
		sceneMax.y - sceneMin.y,
		textureWidth,
		textureHeight
	);
	if (sceneAxisVisible_) {
		Camera* camera = Object3dCommon::GetInstance()
			? Object3dCommon::GetInstance()->GetDefaultCamera()
			: nullptr;
		if (camera) {
			DrawWorldAxisIndicator(
				sceneMin,
				sceneMax,
				camera->GetViewMatrix()
			);
		}
	}

	if (
		sceneImageHovered &&
		editorSession_ &&
		editorSession_->IsEditing() &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		!ImGuizmo::IsUsing() &&
		!ImGuizmo::IsOver()
	) {
		const ImVec2 mouse = ImGui::GetMousePos();
		const bool overSceneToolbar =
			mouse.x >= sceneMin.x &&
			mouse.x <= sceneMin.x + 420.0f &&
			mouse.y >= sceneMin.y &&
			mouse.y <= sceneMin.y + 40.0f;
		if (!overSceneToolbar) {
			PickSceneEntity(
				sceneMin.x,
				sceneMin.y,
				sceneMax.x - sceneMin.x,
				sceneMax.y - sceneMin.y
			);
		}
	}
	if (
		editorSession_ &&
		editorSession_->IsEditing() &&
		ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		!ImGui::GetIO().WantTextInput &&
		ImGui::IsKeyPressed(ImGuiKey_F, false)
	) {
		FocusSceneCameraOnSelection();
	}

	sceneViewInputActive_ =
		(sceneImageHovered || ImGui::IsWindowFocused()) &&
		!ImGuizmo::IsUsing() &&
		!ImGuizmo::IsOver();

	ImGui::End();
	ImGui::PopStyleVar();

	if (showHierarchy_) {
		DrawHierarchyWindow(sceneName);
	}
	if (showInspector_) {
		DrawInspectorWindow();
	}
	if (showProject_) {
		DrawProjectWindow();
	}
	if (showConsole_) {
		DrawConsoleWindow();
	}
	if (showFishingScoreAttackConsole_) {
		DrawFishingScoreAttackConsoleWindow();
	}
	if (showInputSettings_) {
		DrawInputSettingsWindow();
	}
	if (showLoadedScenes_) {
		DrawLoadedScenesWindow();
	}
	if (showPrefab_) {
		DrawPrefabWindow();
	}

	if (editorSession_) {
		const bool editingInteractionActive =
			ImGui::IsAnyItemActive() ||
			ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
			ImGuizmo::IsUsing();
		editorSession_->EndEditFrame(!editingInteractionActive);
	}
	HandleEditShortcuts();

	(void)textureWidth;
	(void)textureHeight;
}

void ImGuiManager::SetModelPreviewTexture(
	const std::string& modelPath,
	D3D12_GPU_DESCRIPTOR_HANDLE texture,
	uint32_t width,
	uint32_t height
) {
	modelPreviewRenderedPath_ = modelPath;
	modelPreviewTexture_ = texture;
	modelPreviewWidth_ = (std::max)(width, 1u);
	modelPreviewHeight_ = (std::max)(height, 1u);
}

void ImGuiManager::SetPrefabPreviewTexture(
	const std::string& assetPath,
	uint64_t revision,
	D3D12_GPU_DESCRIPTOR_HANDLE texture,
	uint32_t width,
	uint32_t height,
	const Matrix4x4& viewMatrix,
	const Matrix4x4& projectionMatrix
) {
	prefabPreviewRenderedPath_ = assetPath;
	prefabPreviewRenderedRevision_ = revision;
	prefabPreviewTexture_ = texture;
	prefabPreviewTextureWidth_ = (std::max)(width, 1u);
	prefabPreviewTextureHeight_ = (std::max)(height, 1u);
	prefabPreviewViewMatrix_ = viewMatrix;
	prefabPreviewProjectionMatrix_ = projectionMatrix;
	prefabPreviewCameraValid_ = texture.ptr != 0;
}

const std::vector<std::string>& ImGuiManager::GetCachedModelAssetPaths() {
	if (assetPathCacheDirty_) {
		RefreshAssetPathCache();
	}
	return cachedModelAssetPaths_;
}

const std::vector<std::string>& ImGuiManager::GetCachedTextureAssetPaths() {
	if (assetPathCacheDirty_) {
		RefreshAssetPathCache();
	}
	return cachedTextureAssetPaths_;
}

const std::vector<std::string>& ImGuiManager::GetCachedAudioAssetPaths() {
	if (assetPathCacheDirty_) {
		RefreshAssetPathCache();
	}
	return cachedAudioAssetPaths_;
}

const std::vector<std::string>& ImGuiManager::GetCachedPrefabAssetPaths() {
	if (prefabAssetPathCacheDirty_) {
		RefreshPrefabAssetPathCache();
	}
	return cachedPrefabAssetPaths_;
}

bool ImGuiManager::DrawAudioClipAssetField(
	const char* label,
	std::string& audioClipPath
) {
	const std::filesystem::path currentPath = PathFromUtf8(audioClipPath);
	const std::string preview = audioClipPath.empty()
		? "None"
		: PathToUtf8(currentPath.filename());
	bool changed = false;
	if (ImGui::BeginCombo(label, preview.empty() ? audioClipPath.c_str() : preview.c_str())) {
		if (ImGui::IsWindowAppearing()) {
			audioAssetSearchBuffer_[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##AudioAssetSearch",
			SelectEditorText(editorLanguage_, "音声を検索", "Search audio"),
			audioAssetSearchBuffer_,
			sizeof(audioAssetSearchBuffer_)
		);
		ImGui::Separator();
		if (ImGui::Selectable("None", audioClipPath.empty())) {
			audioClipPath.clear();
			changed = true;
		}
		for (const std::string& candidate : GetCachedAudioAssetPaths()) {
			if (!ContainsCaseInsensitive(candidate, audioAssetSearchBuffer_)) {
				continue;
			}
			if (ImGui::Selectable(candidate.c_str(), audioClipPath == candidate)) {
				audioClipPath = candidate;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	const bool fieldHovered = ImGui::IsItemHovered();
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROJECT_AUDIO_PATH")) {
			const char* droppedPath = static_cast<const char*>(payload->Data);
			if (droppedPath && droppedPath[0] != '\0') {
				const std::filesystem::path resolvedPath =
					EditableResourcePath::ResolveResource(PathFromUtf8(droppedPath)).lexically_normal();
				const std::filesystem::path resourceRoot =
					GetProjectResourceRoot().lexically_normal();
				const std::filesystem::path relativePath =
					resolvedPath.lexically_relative(resourceRoot);
				bool isInsideResources = !relativePath.empty() && !relativePath.is_absolute();
				for (const std::filesystem::path& segment : relativePath) {
					if (segment == "..") {
						isInsideResources = false;
						break;
					}
				}
				std::error_code error;
				if (
					isInsideResources && IsAudioAssetPath(resolvedPath) &&
					std::filesystem::is_regular_file(resolvedPath, error)
				) {
					const std::string normalizedPath = GetProjectResourcePath(droppedPath);
					if (audioClipPath != normalizedPath) {
						audioClipPath = normalizedPath;
						changed = true;
					}
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(audioClipPath.empty());
	if (ImGui::SmallButton(SelectEditorText(
		editorLanguage_,
		"解除###ClearAudioClip",
		"Clear###ClearAudioClip"
	))) {
		audioClipPath.clear();
		changed = true;
	}
	ImGui::EndDisabled();
	if (!audioClipPath.empty() && fieldHovered) {
		ImGui::SetTooltip("%s", audioClipPath.c_str());
	}

	if (!audioClipPath.empty()) {
		const std::filesystem::path resolvedPath =
			EditableResourcePath::ResolveResource(PathFromUtf8(audioClipPath));
		std::error_code error;
		if (!IsAudioAssetPath(resolvedPath)) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"Unsupported audio format"
			);
		} else if (!std::filesystem::is_regular_file(resolvedPath, error)) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"Missing audio asset"
			);
		}
	}
	return changed;
}

void ImGuiManager::RefreshPrefabAssetPathCache() {
	cachedPrefabAssetPaths_.clear();
	std::error_code error;
	std::filesystem::recursive_directory_iterator iterator(
		GetProjectResourceRoot(),
		std::filesystem::directory_options::skip_permission_denied,
		error
	);
	const std::filesystem::recursive_directory_iterator end;
	while (!error && iterator != end) {
		if (
			iterator->is_regular_file(error) &&
			IsPrefabAssetPath(iterator->path())
		) {
			cachedPrefabAssetPaths_.push_back(
				PathToUtf8(iterator->path().lexically_normal())
			);
		}
		iterator.increment(error);
	}
	std::sort(
		cachedPrefabAssetPaths_.begin(),
		cachedPrefabAssetPaths_.end()
	);
	prefabAssetPathCacheDirty_ = false;
}

void ImGuiManager::RecordRecentPrefab(const std::string& filePath) {
	const PrefabAssetReference reference =
		PrefabAssetRegistry::CreateReference(filePath);
	recentPrefabReferences_.erase(
		std::remove_if(
			recentPrefabReferences_.begin(),
			recentPrefabReferences_.end(),
			[&reference](const PrefabAssetReference& recent) {
				return PrefabAssetRegistry::IsSameAsset(recent, reference);
			}
		),
		recentPrefabReferences_.end()
	);
	recentPrefabReferences_.insert(
		recentPrefabReferences_.begin(),
		reference
	);
	if (recentPrefabReferences_.size() > 12) {
		recentPrefabReferences_.resize(12);
	}
	SaveEditorSettings();
}

bool ImGuiManager::IsFavoritePrefab(const std::string& filePath) const {
	return ContainsPrefabAssetReference(
		favoritePrefabReferences_,
		PrefabAssetRegistry::CreateReference(filePath)
	);
}

void ImGuiManager::ToggleFavoritePrefab(const std::string& filePath) {
	ToggleFavoritePrefab(PrefabAssetRegistry::CreateReference(filePath));
}

void ImGuiManager::ToggleFavoritePrefab(
	const PrefabAssetReference& reference
) {
	const auto found = std::find_if(
		favoritePrefabReferences_.begin(),
		favoritePrefabReferences_.end(),
		[&reference](const PrefabAssetReference& favorite) {
			return PrefabAssetRegistry::IsSameAsset(favorite, reference);
		}
	);
	if (found != favoritePrefabReferences_.end()) {
		favoritePrefabReferences_.erase(found);
	} else {
		favoritePrefabReferences_.push_back(reference);
	}
	SaveEditorSettings();
}

void ImGuiManager::RefreshAssetPathCache() {
	cachedModelAssetPaths_ = CollectModelAssetPaths();
	cachedTextureAssetPaths_ = CollectTextureAssetPaths();
	cachedAudioAssetPaths_ = CollectAudioAssetPaths();
	assetPathCacheDirty_ = false;
}

void ImGuiManager::InvalidateProjectCache() {
	assetPathCacheDirty_ = true;
	prefabAssetPathCacheDirty_ = true;
	prefabAssetValidationCompleted_ = false;
	prefabAssetValidationScannedCount_ = 0;
	prefabAssetValidationResults_.clear();
	PrefabAssetRegistry::Invalidate();
	projectDirectoryCacheDirty_ = true;
	projectTreeCacheDirty_ = true;
	cachedProjectFolder_.clear();
}

const std::vector<ImGuiManager::ProjectDirectoryEntry>&
ImGuiManager::GetCachedProjectDirectoryEntries() {
	if (
		projectDirectoryCacheDirty_ ||
		cachedProjectFolder_ != selectedProjectFolder_
	) {
		RefreshProjectDirectoryCache();
	}
	return cachedProjectEntries_;
}

void ImGuiManager::RefreshProjectDirectoryCache() {
	cachedProjectEntries_.clear();
	cachedProjectFolder_ = selectedProjectFolder_;
	projectDirectoryCacheDirty_ = false;

	std::error_code ec;
	const std::filesystem::path selectedFolderPath =
		PathFromUtf8(selectedProjectFolder_);
	if (!std::filesystem::exists(selectedFolderPath, ec)) {
		return;
	}

	for (const auto& entry : std::filesystem::directory_iterator(selectedFolderPath, ec)) {
		const bool isDirectory = entry.is_directory(ec);
		const bool isRegularFile = entry.is_regular_file(ec);
		if (!isDirectory && !isRegularFile) {
			continue;
		}

		ProjectDirectoryEntry cachedEntry{};
		cachedEntry.fileName = PathToUtf8(entry.path().filename());
		cachedEntry.filePath = PathToUtf8(entry.path());
		cachedEntry.extension = entry.path().extension().string();
		std::transform(
			cachedEntry.extension.begin(),
			cachedEntry.extension.end(),
			cachedEntry.extension.begin(),
			::tolower
		);
		cachedEntry.isDirectory = isDirectory;
		cachedEntry.isTexture = !isDirectory && IsTextureAssetPath(entry.path());
		cachedEntry.isModel = !isDirectory && IsModelAssetPath(entry.path());
		cachedEntry.isScene = !isDirectory && sceneCatalog_ &&
			sceneCatalog_->FindByFilePath(cachedEntry.filePath);
		cachedProjectEntries_.push_back(cachedEntry);
	}

	std::sort(
		cachedProjectEntries_.begin(),
		cachedProjectEntries_.end(),
		[](const ProjectDirectoryEntry& left, const ProjectDirectoryEntry& right) {
			if (left.isDirectory != right.isDirectory) {
				return left.isDirectory;
			}
			return left.fileName < right.fileName;
		}
	);
}

ImGuiManager::ProjectDirectoryNode ImGuiManager::BuildProjectDirectoryNode(
	const std::filesystem::path& path
) {
	ProjectDirectoryNode node{};
	node.folderName = PathToUtf8(path.filename());
	if (node.folderName.empty()) {
		node.folderName = PathToUtf8(path);
	}
	node.folderPath = PathToUtf8(path);

	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		return node;
	}

	for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
		if (entry.is_directory(ec)) {
			node.children.push_back(BuildProjectDirectoryNode(entry.path()));
		}
	}
	std::sort(
		node.children.begin(),
		node.children.end(),
		[](const ProjectDirectoryNode& left, const ProjectDirectoryNode& right) {
			return left.folderName < right.folderName;
		}
	);
	return node;
}

void ImGuiManager::RefreshProjectTreeCache() {
	cachedProjectTreeRoot_ = BuildProjectDirectoryNode(
		GetProjectResourceRoot()
	);
	projectTreeCacheDirty_ = false;
}

bool ImGuiManager::GetModelPreviewRequest(
	std::string& modelPath,
	float& yaw,
	float& pitch,
	float& zoom
) const {
	if (prefabEditorSession_ && prefabEditorSession_->IsOpen()) {
		const SceneDocument& prefab = prefabEditorSession_->GetDocument();
		if (const SceneEntity* entity = prefab.FindEntity(prefabSelectedEntityId_)) {
			if (const SceneComponent* meshRenderer =
				FindEnabledComponent(*entity, "MeshRenderer")) {
				if (!meshRenderer->modelPath.empty()) {
					modelPath = meshRenderer->modelPath;
					yaw = modelPreviewYaw_;
					pitch = modelPreviewPitch_;
					zoom = modelPreviewZoom_;
					return true;
				}
			}
		}
	}
	if (!selectedProjectFile_.empty()) {
		const std::filesystem::path selectedPath =
			PathFromUtf8(selectedProjectFile_);
		if (IsModelAssetPath(selectedPath)) {
			modelPath = GetModelPathRelativeToResources(
				PathToUtf8(selectedPath)
			);
			yaw = modelPreviewYaw_;
			pitch = modelPreviewPitch_;
			zoom = modelPreviewZoom_;
			return true;
		}
	}

	if (editorSession_ && selectedEntityId_ != 0) {
		const SceneDocument& document = editorSession_->GetActiveDocument();
		if (const SceneEntity* entity = document.FindEntity(selectedEntityId_)) {
			if (const SceneComponent* meshRenderer =
				FindEnabledComponent(*entity, "MeshRenderer")) {
				if (meshRenderer->modelPath.empty()) {
					return false;
				}
				modelPath = meshRenderer->modelPath;
				yaw = modelPreviewYaw_;
				pitch = modelPreviewPitch_;
				zoom = modelPreviewZoom_;
				return true;
			}
		}
	}

	return false;
}

const SceneDocument& ImGuiManager::GetPrefabStageDocument() const {
	const SceneDocument& sourceDocument = prefabEditorSession_->GetDocument();
	if (
		prefabAnimationPreviewActive_ &&
		prefabAnimationPreviewDocument_ &&
		prefabAnimationPreviewAssetPath_ == prefabEditorSession_->GetFilePath()
	) {
		return *prefabAnimationPreviewDocument_;
	}
	return sourceDocument;
}

void ImGuiManager::RebuildPrefabAnimationPreviewDocument() {
	if (
		!prefabAnimationPreviewActive_ ||
		!prefabAnimationPreviewDocument_ ||
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen()
	) {
		return;
	}

	const SceneDocument& sourceDocument = prefabEditorSession_->GetDocument();
	const SceneEntity* owner = sourceDocument.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	const SceneComponent* animator = owner
		? FindEnabledComponent(*owner, "PrefabAnimator")
		: nullptr;
	if (
		!animator ||
		prefabAnimationPreviewClipIndex_ < 0 ||
		prefabAnimationPreviewClipIndex_ >=
			static_cast<int>(animator->prefabAnimationClips.size())
	) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = false;
		return;
	}

	*prefabAnimationPreviewDocument_ = sourceDocument;
	ScenePrefabAnimationEvaluator::ApplyClip(
		*prefabAnimationPreviewDocument_,
		owner->id,
		animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_],
		prefabAnimationPreviewTime_
	);
	if (prefabAttackPreviewMode_) {
		const SceneComponent* attackSet = FindEnabledComponent(*owner, "AttackSet");
		if (attackSet && prefabAttackPreviewIndex_ >= 0 &&
			prefabAttackPreviewIndex_ < static_cast<int>(attackSet->attackDefinitions.size())) {
			const SceneAttackDefinition& attack =
				attackSet->attackDefinitions[prefabAttackPreviewIndex_];
			std::unordered_set<uint64_t> windowHitBoxIds;
			std::unordered_set<uint64_t> activeHitBoxIds;
			for (const SceneAttackHitWindow& window : attack.hitWindows) {
				SceneEntity* hitBox = window.hitBoxEntityId != 0
					? prefabAnimationPreviewDocument_->FindEntity(window.hitBoxEntityId)
					: nullptr;
				if (!hitBox && !window.hitBoxEntityName.empty()) {
					hitBox = prefabAnimationPreviewDocument_->FindEntityByName(
						window.hitBoxEntityName
					);
				}
				if (!hitBox || !FindComponent(*hitBox, "HitBox")) {
					continue;
				}
				windowHitBoxIds.insert(hitBox->id);
				if (prefabAnimationPreviewTime_ >= window.startTime &&
					prefabAnimationPreviewTime_ < window.endTime) {
					activeHitBoxIds.insert(hitBox->id);
					if (window.payloadSource == "WindowLegacy" &&
						window.overrideHitBoxHalfSize) {
						if (SceneComponent* collider = FindComponent(*hitBox, "OBBCollider");
							collider && collider->enabled && collider->colliderShape == "Box") {
							Vector3 halfSize = window.hitBoxHalfSize;
							halfSize.x = (std::max)(halfSize.x, 0.001f);
							halfSize.y = (std::max)(halfSize.y, 0.001f);
							halfSize.z = (std::max)(halfSize.z, 0.001f);
							collider->colliderSizeMultiplier = halfSize;
						}
					}
				}
			}
			// Sourceを変更せず、Attackが参照するColliderだけをPreview Copyで
			// active/ghost表示へ分ける。選択Entityはこの表示で書き換えない。
			for (uint64_t hitBoxId : windowHitBoxIds) {
				if (SceneEntity* hitBox = prefabAnimationPreviewDocument_->FindEntity(hitBoxId)) {
					hitBox->active = activeHitBoxIds.contains(hitBoxId);
				}
			}
		}
	}
	prefabAnimationPreviewAssetPath_ = prefabEditorSession_->GetFilePath();
	prefabAnimationPreviewSourceRevision_ = sourceDocument.GetRevision();
}

void ImGuiManager::RebuildPlayerCombatPreviewDocument() {
	if (!playerCombatPreviewEnabled_ || !playerCombatPreviewDocument_ ||
		!editorSession_ || !editorSession_->IsEditing() ||
		!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}

	const SceneDocument& source = editorSession_->GetEditDocument();
	const SceneEntity* root = source.FindEntity(playerCombatPreviewRootId_);
	const SceneEntity* weapon = source.FindEntity(playerCombatPreviewWeaponId_);
	if (!root || !weapon ||
		(weapon->id != root->id && !source.IsDescendantOf(weapon->id, root->id))) {
		playerCombatPreviewStatus_ = "Select a Player Root and its PlayerWeapon instance.";
		return;
	}
	const SceneComponent* animator = FindEnabledComponent(*weapon, "PrefabAnimator");
	const SceneComponent* attackSet = FindEnabledComponent(*weapon, "AttackSet");
	const SceneDocument& prefab = prefabEditorSession_->GetDocument();
	const SceneEntity* prefabOwner = prefab.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	const SceneComponent* prefabAnimator = prefabOwner
		? FindEnabledComponent(*prefabOwner, "PrefabAnimator") : nullptr;
	if (!animator || !attackSet || !prefabAnimator ||
		prefabAnimationPreviewClipIndex_ < 0 ||
		prefabAnimationPreviewClipIndex_ >= static_cast<int>(
			prefabAnimator->prefabAnimationClips.size()
		)) {
		playerCombatPreviewStatus_ = "The selected Weapon needs PrefabAnimator, AttackSet, and a Clip.";
		return;
	}
	const std::string& clipName = prefabAnimator->prefabAnimationClips[
		prefabAnimationPreviewClipIndex_
	].name;
	auto sourceClip = std::find_if(
		animator->prefabAnimationClips.begin(),
		animator->prefabAnimationClips.end(),
		[&clipName](const ScenePrefabAnimationClip& candidate) {
			return candidate.name == clipName;
		}
	);
	auto attack = std::find_if(
		attackSet->attackDefinitions.begin(),
		attackSet->attackDefinitions.end(),
		[&clipName](const SceneAttackDefinition& candidate) {
			return candidate.animation == clipName;
		}
	);
	if (sourceClip == animator->prefabAnimationClips.end() ||
		attack == attackSet->attackDefinitions.end()) {
		playerCombatPreviewStatus_ = "The selected title Scene Weapon has no matching Clip or Attack.";
		return;
	}
	if (attack->facingMode != "FixedAtStart") {
		playerCombatPreviewStatus_ = "Combat Rig Preview currently supports Fixed At Start facing only.";
		return;
	}

	*playerCombatPreviewDocument_ = source;
	for (SceneEntity& entity : playerCombatPreviewDocument_->GetEntities()) {
		if (entity.id != root->id &&
			!playerCombatPreviewDocument_->IsDescendantOf(entity.id, root->id)) {
			entity.active = false;
		}
	}
	SceneEntity* previewRoot = playerCombatPreviewDocument_->FindEntity(root->id);
	ScenePrefabAnimationEvaluator::ApplyClip(
		*playerCombatPreviewDocument_, weapon->id, *sourceClip,
		prefabAnimationPreviewTime_
	);
	for (const SceneAttackHitWindow& window : attack->hitWindows) {
		SceneEntity* hitBox = window.hitBoxEntityId != 0
			? playerCombatPreviewDocument_->FindEntity(window.hitBoxEntityId)
			: nullptr;
		if (!hitBox && !window.hitBoxEntityName.empty()) {
			hitBox = playerCombatPreviewDocument_->FindEntityByName(
				window.hitBoxEntityName
			);
		}
		if (hitBox) {
			hitBox->active = prefabAnimationPreviewTime_ >= window.startTime &&
				prefabAnimationPreviewTime_ < window.endTime;
		}
	}
	const float activeDuration = (std::max)(attack->activeTime, 0.0001f);
	const float rawProgress = std::clamp(
		(prefabAnimationPreviewTime_ - attack->windup) / activeDuration,
		0.0f, 1.0f
	);
	float progress = Math::SmoothStep(rawProgress);
	if (attack->motionEasing == "Linear") progress = rawProgress;
	if (attack->motionEasing == "EaseIn") progress = rawProgress * rawProgress * rawProgress;
	if (attack->motionEasing == "EaseOut") progress = Math::EaseOutCubic(rawProgress);
	if (attack->motionEasing == "EaseInOut") {
		progress = rawProgress < 0.5f
			? 4.0f * rawProgress * rawProgress * rawProgress
			: 1.0f - std::pow(-2.0f * rawProgress + 2.0f, 3.0f) * 0.5f;
	}
	if (previewRoot) {
		previewRoot->transform.translate.x += attack->sideDistance * progress;
		previewRoot->transform.translate.z += attack->forwardDistance * progress;
	}
	playerCombatPreviewStatus_.clear();
}

void ImGuiManager::RebuildPrefabHitBoxGhostDocument() {
	if (
		!prefabHitBoxSetupMode_ ||
		!prefabHitBoxGhostVisible_ ||
		!prefabHitBoxGhostDocument_ ||
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen()
	) {
		return;
	}

	const SceneDocument& sourceDocument = prefabEditorSession_->GetDocument();
	const SceneEntity* owner = sourceDocument.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	const SceneComponent* animator = owner
		? FindEnabledComponent(*owner, "PrefabAnimator")
		: nullptr;
	if (
		!animator ||
		prefabAnimationPreviewClipIndex_ < 0 ||
		prefabAnimationPreviewClipIndex_ >=
			static_cast<int>(animator->prefabAnimationClips.size())
	) {
		return;
	}

	*prefabHitBoxGhostDocument_ = sourceDocument;
	ScenePrefabAnimationEvaluator::ApplyClip(
		*prefabHitBoxGhostDocument_,
		owner->id,
		animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_],
		prefabHitBoxGhostTime_
	);
}

bool ImGuiManager::GetPrefabPreviewRequest(PrefabPreviewRequest& request) {
	if (
		!showPrefab_ ||
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen()
	) {
		return false;
	}
	// InspectorはTimelineより後にSource Documentを変更する。ここで最後に
	// Snapshotを作り直し、編集FrameだけAuthoring Poseへ戻る表示を防ぐ。
	RebuildPrefabAnimationPreviewDocument();
	RebuildPrefabHitBoxGhostDocument();
	RebuildPlayerCombatPreviewDocument();

	const SceneDocument& sourceDocument = prefabEditorSession_->GetDocument();
	const bool useCombatRigPreview =
		playerCombatPreviewEnabled_ &&
		playerCombatPreviewDocument_ &&
		editorSession_ &&
		editorSession_->IsEditing() &&
		!prefabHitBoxSetupMode_ &&
		playerCombatPreviewStatus_.empty();
	request.document = useCombatRigPreview
		? playerCombatPreviewDocument_
		: &GetPrefabStageDocument();
	request.ghostDocument = (!useCombatRigPreview &&
		prefabHitBoxSetupMode_ &&
		prefabHitBoxGhostVisible_ &&
		prefabHitBoxGhostDocument_
	) ? prefabHitBoxGhostDocument_ : nullptr;
	request.assetPath = useCombatRigPreview
		? editorSession_->GetSceneFilePath() + "#CombatRig"
		: prefabEditorSession_->GetFilePath();
	request.revision = useCombatRigPreview
		? editorSession_->GetEditDocument().GetRevision()
		: sourceDocument.GetRevision();
	request.yaw = prefabPreviewYaw_;
	request.pitch = prefabPreviewPitch_;
	request.zoom = prefabPreviewZoom_;
	request.width = prefabPreviewRequestedWidth_;
	request.height = prefabPreviewRequestedHeight_;
	request.showSkeleton = prefabPreviewShowSkeleton_;
	request.showJointAxes = prefabPreviewShowJointAxes_;
	request.showColliders = prefabPreviewShowColliders_;
	request.showCombatVolumes = prefabPreviewShowCombatVolumes_;
	request.selectedEntityId = useCombatRigPreview
		? playerCombatPreviewWeaponId_
		: prefabSelectedEntityId_;
	request.isolateSelectedCollider = !useCombatRigPreview && prefabHitBoxSetupMode_;
	request.showGrid = prefabGridVisible_;
	request.framingSerial = prefabPreviewFramingSerial_;
	// Keep the exact final request identity. The renderer returns this one on a
	// later frame, including the composed Combat Rig's source scene identity.
	prefabPreviewRequestedPath_ = request.assetPath;
	prefabPreviewRequestedRevision_ = request.revision;
	prefabPreviewRequestUsesCombatRig_ = useCombatRigPreview;
	return true;
}

void ImGuiManager::SetProjectLauncherView(const ProjectLauncherView& value) {
	projectLauncherView_ = value;
	if (projectLauncherVisualStudioIndex_ >= static_cast<int>(projectLauncherView_.visualStudioInstances.size())) {
		projectLauncherVisualStudioIndex_ = -1;
	}
	if (projectLauncherVisualStudioIndex_ < 0) {
		for (size_t index = 0; index < projectLauncherView_.visualStudioInstances.size(); ++index) {
			if (projectLauncherView_.visualStudioInstances[index].selected) {
				projectLauncherVisualStudioIndex_ = static_cast<int>(index);
				break;
			}
		}
	}
}

bool ImGuiManager::ConsumeProjectLauncherRequest(ProjectLauncherRequest& request) {
	if (!projectLauncherRequestPending_) {
		return false;
	}
	request = std::move(projectLauncherRequest_);
	projectLauncherRequest_ = {};
	projectLauncherRequestPending_ = false;
	return true;
}

bool ImGuiManager::HasUnsavedPrefabChanges() const {
	return prefabEditorSession_ && prefabEditorSession_->IsDirty();
}

bool ImGuiManager::PickSceneEntity(
	float x,
	float y,
	float width,
	float height
) {
	if (
		!editorSession_ ||
		!editorSession_->IsEditing() ||
		width <= 1.0f ||
		height <= 1.0f
	) {
		return false;
	}

	Camera* camera = Object3dCommon::GetInstance()
		? Object3dCommon::GetInstance()->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return false;
	}

	const ImVec2 mouse = ImGui::GetMousePos();
	const float normalizedX = (mouse.x - x) / width;
	const float normalizedY = (mouse.y - y) / height;
	if (
		normalizedX < 0.0f ||
		normalizedX > 1.0f ||
		normalizedY < 0.0f ||
		normalizedY > 1.0f
	) {
		return false;
	}

	const float ndcX = normalizedX * 2.0f - 1.0f;
	const float ndcY = 1.0f - normalizedY * 2.0f;
	const Matrix4x4 inverseViewProjection = Inverse(
		Multiply(camera->GetViewMatrix(), camera->GetProjectionMatrix())
	);
	const Vector3 nearPoint =
		TransformCoord({ ndcX, ndcY, 0.0f }, inverseViewProjection);
	const Vector3 farPoint =
		TransformCoord({ ndcX, ndcY, 1.0f }, inverseViewProjection);
	const Vector3 rayDirection =
		Math::Normalize(Math::Subtract(farPoint, nearPoint));

	SceneDocument& document = editorSession_->GetEditDocument();
	uint64_t bestEntityId = 0;
	float bestDistance = (std::numeric_limits<float>::max)();
	uint64_t bestLowPriorityEntityId = 0;
	float bestLowPriorityDistance = (std::numeric_limits<float>::max)();
	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.locked || !entity.active) {
			continue;
		}
		const SceneComponent* meshRenderer =
			FindEnabledComponent(entity, "MeshRenderer");
		if (!meshRenderer || meshRenderer->modelPath.empty()) {
			continue;
		}
		ModelManager::GetInstance()->LoadModel(meshRenderer->modelPath);
		Model* model = ModelManager::GetInstance()->FindModel(
			meshRenderer->modelPath
		);
		if (!model) {
			continue;
		}
		Vector3 localMin{};
		Vector3 localMax{};
		if (!model->GetLocalBounds(localMin, localMax)) {
			continue;
		}
		const Matrix4x4 inverseWorld =
			Inverse(ResolveSceneWorldMatrix(document, entity));
		const Vector3 localRayOrigin =
			TransformCoord(nearPoint, inverseWorld);
		const Vector3 localRayFar =
			TransformCoord(farPoint, inverseWorld);
		const Vector3 localRayDirection =
			Math::Normalize(Math::Subtract(localRayFar, localRayOrigin));
		float distance = 0.0f;
		if (
			IntersectRayAabb(
				localRayOrigin,
				localRayDirection,
				localMin,
				localMax,
				distance
			)
		) {
			const Vector3 localHit = Math::Add(
				localRayOrigin,
				Math::Multiply(localRayDirection, distance)
			);
			const Vector3 worldHit = TransformCoord(
				localHit,
				ResolveSceneWorldMatrix(document, entity)
			);
			const float worldDistance =
				Math::Length(Math::Subtract(worldHit, nearPoint));
			if (worldDistance >= bestDistance) {
				const bool lowPriority = IsLowPriorityPickTarget(
					entity,
					*meshRenderer,
					localMin,
					localMax
				);
				if (
					lowPriority &&
					worldDistance < bestLowPriorityDistance
				) {
					bestLowPriorityDistance = worldDistance;
					bestLowPriorityEntityId = entity.id;
				}
				continue;
			}
			const bool lowPriority = IsLowPriorityPickTarget(
				entity,
				*meshRenderer,
				localMin,
				localMax
			);
			if (lowPriority) {
				if (worldDistance < bestLowPriorityDistance) {
					bestLowPriorityDistance = worldDistance;
					bestLowPriorityEntityId = entity.id;
				}
			} else {
				bestDistance = worldDistance;
				bestEntityId = entity.id;
			}
		}
	}

	if (bestEntityId == 0) {
		bestEntityId = bestLowPriorityEntityId;
	}

	if (bestEntityId == 0) {
		return false;
	}

	selectedEntityId_ = bestEntityId;
	selectedProjectFile_.clear();
	showInspector_ = true;
	revealInspectorRequested_ = true;
	return true;
}

void ImGuiManager::FocusSceneCameraOnSelection() {
	if (!editorSession_ || !editorSession_->IsEditing()) {
		return;
	}
	Camera* camera = Object3dCommon::GetInstance()
		? Object3dCommon::GetInstance()->GetDefaultCamera()
		: nullptr;
	if (!camera) {
		return;
	}

	SceneDocument& document = editorSession_->GetEditDocument();
	std::vector<uint64_t> targetIds;
	for (uint64_t entityId : selectedEntityIds_) {
		if (document.FindEntity(entityId)) {
			targetIds.push_back(entityId);
		}
	}
	if (targetIds.empty() && selectedEntityId_ != 0) {
		if (document.FindEntity(selectedEntityId_)) {
			targetIds.push_back(selectedEntityId_);
		}
	}
	if (targetIds.empty()) {
		return;
	}

	Vector3 center{};
	std::vector<Vector3> positions;
	positions.reserve(targetIds.size());
	for (uint64_t entityId : targetIds) {
		const SceneEntity* entity = document.FindEntity(entityId);
		if (!entity) {
			continue;
		}
		const Matrix4x4 worldMatrix = ResolveSceneWorldMatrix(document, *entity);
		const Vector3 position = {
			worldMatrix.m[3][0],
			worldMatrix.m[3][1],
			worldMatrix.m[3][2]
		};
		positions.push_back(position);
		center = Math::Add(center, position);
	}
	if (positions.empty()) {
		return;
	}
	center = Math::Multiply(center, 1.0f / static_cast<float>(positions.size()));

	float radius = 0.0f;
	for (const Vector3& position : positions) {
		radius = (std::max)(
			radius,
			Math::Length(Math::Subtract(position, center))
		);
	}
	const Vector3 currentOffset = Math::Subtract(camera->GetTranslate(), center);
	Vector3 viewDirection = Math::Normalize(currentOffset);
	if (Math::Length(viewDirection) < 0.0001f) {
		viewDirection = { 0.0f, 0.35f, -1.0f };
		viewDirection = Math::Normalize(viewDirection);
	}
	const float distance = (std::max)(5.0f, radius * 2.5f + 2.0f);
	if (camera->IsOrbitMode()) {
		camera->SetOrbitTarget(center);
		camera->SetOrbitDistance(distance);
	} else {
		camera->SetLookAt(
			Math::Add(center, Math::Multiply(viewDirection, distance)),
			center
		);
	}
}

void ImGuiManager::DrawSceneGizmo(
	float x,
	float y,
	float width,
	float height,
	uint32_t textureWidth,
	uint32_t textureHeight
) {
	if (
		!editorSession_ ||
		!editorSession_->IsEditing() ||
		width <= 1.0f ||
		height <= 1.0f
	) {
		return;
	}

	if (
		ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		!ImGui::GetIO().WantTextInput &&
		!ImGuizmo::IsUsing()
	) {
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
			gizmoOperation_ = 0;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
			gizmoOperation_ = 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
			gizmoOperation_ = 2;
		}
	}

	ImGui::SetCursorScreenPos(ImVec2(x + 8.0f, y + 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 0.0f));
	ImGui::BeginGroup();
	const char* operationLabels[] = { "W", "E", "R" };
	const char* operationTooltips[] = { "Move (W)", "Rotate (E)", "Scale (R)" };
	for (int operation = 0; operation < 3; ++operation) {
		if (operation > 0) {
			ImGui::SameLine();
		}
		if (gizmoOperation_ == operation) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		if (ImGui::Button(operationLabels[operation], ImVec2(28.0f, 24.0f))) {
			gizmoOperation_ = operation;
		}
		if (gizmoOperation_ == operation) {
			ImGui::PopStyleColor();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", operationTooltips[operation]);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(gizmoLocalMode_ ? "Local" : "World", ImVec2(52.0f, 24.0f))) {
		gizmoLocalMode_ = !gizmoLocalMode_;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Toggle Local / World space");
	}
	ImGui::SameLine();
	ImGui::Checkbox("Snap", &gizmoSnapEnabled_);
	if (gizmoSnapEnabled_) {
		float* snapValue = gizmoOperation_ == 0
			? &gizmoTranslationSnap_
			: gizmoOperation_ == 1
				? &gizmoRotationSnapDegrees_
				: &gizmoScaleSnap_;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(64.0f);
		ImGui::DragFloat(
			"##GizmoSnapValue",
			snapValue,
			gizmoOperation_ == 1 ? 1.0f : 0.05f,
			0.01f,
			0.0f,
			"%.2f"
		);
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Grid", &sceneGridVisible_)) {
		SaveEditorSettings();
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Axis", &sceneAxisVisible_)) {
		SaveEditorSettings();
	}
	ImGui::EndGroup();
	ImGui::PopStyleVar(2);

	if (selectedEntityId_ == 0) {
		return;
	}
	SceneDocument& document = editorSession_->GetEditDocument();
	SceneEntity* entity = document.FindEntity(selectedEntityId_);
	Camera* camera = Object3dCommon::GetInstance()
		? Object3dCommon::GetInstance()->GetDefaultCamera()
		: nullptr;
	if (!entity || !camera) {
		return;
	}
	if (entity->locked) {
		return;
	}

	const SceneComponent* spriteRenderer =
		FindEnabledComponent(*entity, "SpriteRenderer");
	const bool isSprite = spriteRenderer != nullptr;
	const SceneEntity* parentEntity = document.FindEntity(entity->parentId);
	const Matrix4x4 parentWorld = parentEntity
		? ResolveSceneWorldMatrix(document, *parentEntity)
		: MakeIdentity4x4();
	Matrix4x4 worldMatrix{};
	Matrix4x4 viewMatrix{};
	Matrix4x4 projectionMatrix{};
	if (isSprite) {
		const Vector3 spriteRotate =
			MakeEulerFromQuaternion(entity->transform.rotate);
		const Vector3 spriteScale = {
			spriteRenderer->spriteSize.x * entity->transform.scale.x,
			spriteRenderer->spriteSize.y * entity->transform.scale.y,
			1.0f
		};
		worldMatrix = MakeAffineMatrix(
			spriteScale,
			Vector3{ 0.0f, 0.0f, spriteRotate.z },
			Vector3{
				entity->transform.translate.x,
				entity->transform.translate.y,
				0.0f
			}
		);
		if (parentEntity) {
			worldMatrix = Multiply(
				worldMatrix,
				parentWorld
			);
		}
		viewMatrix = MakeIdentity4x4();
		projectionMatrix = MakeOrthographicMatrix(
			0.0f,
			0.0f,
			static_cast<float>((std::max)(textureWidth, uint32_t{ 1 })),
			static_cast<float>((std::max)(textureHeight, uint32_t{ 1 })),
			0.0f,
			100.0f
		);
	} else {
		worldMatrix = ResolveSceneWorldMatrix(document, *entity);
		viewMatrix = camera->GetViewMatrix();
		projectionMatrix = camera->GetProjectionMatrix();
	}

	const ImGuizmo::OPERATION operation = gizmoOperation_ == 0
		? ImGuizmo::TRANSLATE
		: gizmoOperation_ == 1
			? ImGuizmo::ROTATE
			: ImGuizmo::SCALE;
	const ImGuizmo::MODE mode = gizmoLocalMode_
		? ImGuizmo::LOCAL
		: ImGuizmo::WORLD;
	if (
		!gizmoLocalMode_ &&
		gizmoOperation_ != 0 &&
		parentEntity &&
		HasNonUniformScale(parentWorld)
	) {
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(x + 8.0f, y + 38.0f),
			IM_COL32(255, 190, 80, 255),
			"World Rotate/Scale requires a uniformly scaled parent."
		);
		return;
	}
	const float snapValue = gizmoOperation_ == 0
		? gizmoTranslationSnap_
		: gizmoOperation_ == 1
			? gizmoRotationSnapDegrees_
			: gizmoScaleSnap_;
	const float snap[3] = { snapValue, snapValue, snapValue };

	ImGuizmo::SetOrthographic(isSprite);
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(x, y, width, height);
	const bool changed = ImGuizmo::Manipulate(
		&viewMatrix.m[0][0],
		&projectionMatrix.m[0][0],
		operation,
		mode,
		&worldMatrix.m[0][0],
		nullptr,
		gizmoSnapEnabled_ ? snap : nullptr
	);
	if (!changed || !ImGuizmo::IsUsing()) {
		return;
	}

	Matrix4x4 localMatrix = worldMatrix;
	if (parentEntity) {
		localMatrix = Multiply(worldMatrix, Inverse(parentWorld));
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
		return;
	}
	if (isSprite) {
		if (gizmoOperation_ == 0) {
			entity->transform.translate.x = localTranslate.x;
			entity->transform.translate.y = localTranslate.y;
		} else if (gizmoOperation_ == 1) {
			const Vector3 localEuler = MakeEulerFromQuaternion(localRotate);
			entity->transform.rotate = MakeQuaternionFromEuler({
				0.0f, 0.0f, localEuler.z
			});
		} else {
			entity->transform.scale.x = localScale.x /
				(std::max)(spriteRenderer->spriteSize.x, 0.001f);
			entity->transform.scale.y = localScale.y /
				(std::max)(spriteRenderer->spriteSize.y, 0.001f);
		}
	} else {
		if (gizmoOperation_ == 0) {
			entity->transform.translate = localTranslate;
		} else if (gizmoOperation_ == 1) {
			entity->transform.rotate = localRotate;
		} else {
			entity->transform.scale = localScale;
		}
	}
	document.MarkDirty();
}

void ImGuiManager::Finalize(){
	StopAudioPreview();
	delete prefabEditorSession_;
	prefabEditorSession_ = nullptr;
	delete prefabAnimationPreviewDocument_;
	prefabAnimationPreviewDocument_ = nullptr;
	delete playerCombatPreviewDocument_;
	playerCombatPreviewDocument_ = nullptr;
	delete prefabHitBoxGhostDocument_;
	prefabHitBoxGhostDocument_ = nullptr;
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	editorBaseFontData_.clear();
	editorJapaneseFontData_.clear();
	editorGlyphRanges_.clear();
	instance = nullptr;
}

void ImGuiManager::RequestOpenScene(const std::string& sceneId) {
	if (!editorSession_ || !editorSession_->IsEditing() || sceneId.empty() ||
		sceneId == editorSession_->GetEditSceneId()) {
		return;
	}

	sceneSaveFailed_ = false;
	if (editorSession_->GetEditDocument().IsDirty()) {
		pendingSceneId_ = sceneId;
		sceneSwitchPopupRequested_ = true;
		return;
	}

	requestedSceneId_ = sceneId;
	requestedSceneDiscardUnsavedChanges_ = false;
}

void ImGuiManager::QueueSceneAssetRequest(
	const SceneAssetRequest& request
) {
	if (sceneAssetRequestPending_) {
		return;
	}
	requestedSceneAsset_ = request;
	sceneAssetRequestPending_ = true;
}

void ImGuiManager::QueueSceneInstanceRequest(
	const SceneInstanceRequest& request
) {
	if (sceneInstanceRequestPending_) {
		return;
	}
	requestedSceneInstance_ = request;
	sceneInstanceRequestPending_ = true;
}

void ImGuiManager::DrawSceneMenu() {
	if (!ImGui::BeginMenu(SelectEditorText(
		editorLanguage_,
		"Scene###SceneMenu",
		"Scene###SceneMenu"
	))) {
		return;
	}

	const bool canOpenScene =
		editorSession_ && editorSession_->IsEditing() && sceneCatalog_;
	const bool currentSceneClean = canOpenScene &&
		!editorSession_->GetEditDocument().IsDirty();
	const SceneDescriptor* currentScene = canOpenScene
		? sceneCatalog_->Find(editorSession_->GetEditSceneId())
		: nullptr;
	const bool canManageScene = currentSceneClean && currentScene &&
		!sceneAssetRequestPending_;

	if (ImGui::MenuItem(
		SelectEditorText(
			editorLanguage_,
			"新しいScene...###NewSceneMenuItem",
			"New Scene...###NewSceneMenuItem"
		),
		nullptr,
		false,
		canManageScene
	)) {
		CopyTextBuffer(
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_),
			"New Scene"
		);
		CopyTextBuffer(
			sceneAssetIdBuffer_,
			sizeof(sceneAssetIdBuffer_),
			"new_scene"
		);
		CopyTextBuffer(
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_),
			"new_scene"
		);
		sceneTemplateIndex_ = 0;
		createScenePopupRequested_ = true;
	}
	if (ImGui::MenuItem(
		SelectEditorText(
			editorLanguage_,
			"現在のSceneを複製...###DuplicateSceneMenuItem",
			"Duplicate Active Scene...###DuplicateSceneMenuItem"
		),
		nullptr,
		false,
		canManageScene
	)) {
		const std::string duplicateId = currentScene->id + "_copy";
		CopyTextBuffer(
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_),
			currentScene->displayName + " Copy"
		);
		CopyTextBuffer(
			sceneAssetIdBuffer_,
			sizeof(sceneAssetIdBuffer_),
			duplicateId
		);
		CopyTextBuffer(
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_),
			duplicateId
		);
		sceneAssetTargetId_ = currentScene->id;
		duplicateScenePopupRequested_ = true;
	}
	if (ImGui::MenuItem(
		SelectEditorText(
			editorLanguage_,
			"現在のScene名を変更...###RenameSceneMenuItem",
			"Rename Active Scene...###RenameSceneMenuItem"
		),
		nullptr,
		false,
		canManageScene
	)) {
		CopyTextBuffer(
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_),
			currentScene->displayName
		);
		CopyTextBuffer(
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_),
			SceneAssetFileStem(*currentScene)
		);
		sceneAssetTargetId_ = currentScene->id;
		renameScenePopupRequested_ = true;
	}

	if (ImGui::BeginMenu(
		SelectEditorText(
			editorLanguage_,
			"Sceneを削除###DeleteSceneMenu",
			"Delete Scene###DeleteSceneMenu"
		),
		canManageScene
	)) {
		bool hasDeleteCandidate = false;
		for (const SceneDescriptor& scene : sceneCatalog_->GetScenes()) {
			if (scene.id == currentScene->id ||
				scene.id == sceneCatalog_->GetStartSceneId()) {
				continue;
			}
			hasDeleteCandidate = true;
			const std::string label = scene.displayName + "##delete_" + scene.id;
			if (ImGui::MenuItem(label.c_str())) {
				sceneAssetTargetId_ = scene.id;
				deleteScenePopupRequested_ = true;
			}
		}
		if (!hasDeleteCandidate) {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"削除できるSceneはありません",
				"No deletable Scene"
			));
		}
		ImGui::EndMenu();
	}
	if (canOpenScene && !currentSceneClean) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"Scene Assetを操作するには現在のSceneを保存してください。",
			"Save the active Scene to manage Scene assets."
		));
	}

	ImGui::Separator();
	ImGui::MenuItem(
		SelectEditorText(
			editorLanguage_,
			"読み込み済みScene###LoadedScenesMenuItem",
			"Loaded Scenes###LoadedScenesMenuItem"
		),
		nullptr,
		&showLoadedScenes_
	);

	ImGui::Separator();
	ImGui::TextDisabled("%s", SelectEditorText(
		editorLanguage_,
		"Sceneを開く",
		"Open Scene"
	));
	ImGui::BeginDisabled(!canOpenScene);
	if (sceneCatalog_ && editorSession_) {
		for (const SceneDescriptor& scene : sceneCatalog_->GetScenes()) {
			const std::string label = scene.displayName + "##" + scene.id;
			if (ImGui::MenuItem(
				label.c_str(),
				nullptr,
				editorSession_->GetEditSceneId() == scene.id
			)) {
				RequestOpenScene(scene.id);
			}
		}
	}
	ImGui::EndDisabled();
	ImGui::EndMenu();
}

void ImGuiManager::DrawSceneSwitchConfirmation() {
	const char* popupLabel = SelectEditorText(
		editorLanguage_,
		"未保存のScene###UnsavedScenePopup",
		"Unsaved Scene###UnsavedScenePopup"
	);
	if (sceneSwitchPopupRequested_) {
		ImGui::OpenPopup(popupLabel);
		sceneSwitchPopupRequested_ = false;
	}
	if (!ImGui::BeginPopupModal(
		popupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		return;
	}

	const SceneDescriptor* pendingScene = sceneCatalog_
		? sceneCatalog_->Find(pendingSceneId_)
		: nullptr;
	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"現在のSceneには未保存の変更があります。",
		"The current Scene has unsaved changes."
	));
	if (pendingScene) {
		ImGui::Text(
			SelectEditorText(editorLanguage_, "開く: %s", "Open: %s"),
			pendingScene->displayName.c_str()
		);
	}
	if (sceneSaveFailed_) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.25f, 1.0f),
			"%s",
			SelectEditorText(
				editorLanguage_,
				"現在のSceneを保存できませんでした。",
				"The current Scene could not be saved."
			)
		);
	}
	ImGui::Separator();

	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"保存して開く###SaveAndOpenScene",
		"Save and Open###SaveAndOpenScene"
	))) {
		if (editorSession_ && editorSession_->Save()) {
			requestedSceneId_ = pendingSceneId_;
			requestedSceneDiscardUnsavedChanges_ = false;
			pendingSceneId_.clear();
			sceneSaveFailed_ = false;
			ImGui::CloseCurrentPopup();
		} else {
			sceneSaveFailed_ = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"変更を破棄###DiscardSceneChanges",
		"Discard###DiscardSceneChanges"
	))) {
		requestedSceneId_ = pendingSceneId_;
		requestedSceneDiscardUnsavedChanges_ = true;
		pendingSceneId_.clear();
		sceneSaveFailed_ = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"キャンセル###CancelSceneSwitch",
		"Cancel###CancelSceneSwitch"
	))) {
		pendingSceneId_.clear();
		sceneSaveFailed_ = false;
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void ImGuiManager::DrawSceneAssetDialogs() {
	const char* createScenePopupLabel = SelectEditorText(
		editorLanguage_,
		"Sceneを作成###CreateScenePopup",
		"Create Scene###CreateScenePopup"
	);
	if (createScenePopupRequested_) {
		ImGui::OpenPopup(createScenePopupLabel);
		createScenePopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		createScenePopupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "名前###CreateSceneName", "Name###CreateSceneName"),
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_)
		);
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "Scene ID###CreateSceneId", "Scene ID###CreateSceneId"),
			sceneAssetIdBuffer_,
			sizeof(sceneAssetIdBuffer_)
		);
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "ファイル名###CreateSceneFile", "File Name###CreateSceneFile"),
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_)
		);
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"resources/scenes以下へ.scene.jsonとして保存します。",
			"Saved under resources/scenes as .scene.json"
		));

		const std::vector<SceneTemplateDescriptor>* templates =
			sceneTemplateRegistry_
				? &sceneTemplateRegistry_->GetTemplates()
				: nullptr;
		if (templates && !templates->empty()) {
			sceneTemplateIndex_ = std::clamp(
				sceneTemplateIndex_,
				0,
				static_cast<int>(templates->size()) - 1
			);
			const char* preview =
				(*templates)[sceneTemplateIndex_].displayName.c_str();
			if (ImGui::BeginCombo(
				SelectEditorText(
					editorLanguage_,
					"テンプレート###CreateSceneTemplate",
					"Template###CreateSceneTemplate"
				),
				preview
			)) {
				for (int index = 0; index < static_cast<int>(templates->size()); ++index) {
					if (ImGui::Selectable(
						(*templates)[index].displayName.c_str(),
						sceneTemplateIndex_ == index
					)) {
						sceneTemplateIndex_ = index;
					}
				}
				ImGui::EndCombo();
			}
		} else {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"利用できるSceneテンプレートがありません。",
				"No Scene templates are available."
			));
		}

		const bool canSubmit = templates && !templates->empty() &&
			sceneAssetNameBuffer_[0] != '\0' &&
			sceneAssetIdBuffer_[0] != '\0' &&
			sceneAssetFileBuffer_[0] != '\0';
		ImGui::BeginDisabled(!canSubmit);
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"作成###CreateSceneSubmit",
			"Create###CreateSceneSubmit"
		))) {
			SceneAssetRequest request{};
			request.operation = SceneAssetOperation::Create;
			request.sceneId = sceneAssetIdBuffer_;
			request.displayName = sceneAssetNameBuffer_;
			request.assetPath = BuildSceneAssetPath(sceneAssetFileBuffer_);
			request.templateId = (*templates)[sceneTemplateIndex_].id;
			QueueSceneAssetRequest(request);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"キャンセル###CancelCreateScene",
			"Cancel###CancelCreateScene"
		))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	const char* duplicateScenePopupLabel = SelectEditorText(
		editorLanguage_,
		"Sceneを複製###DuplicateScenePopup",
		"Duplicate Scene###DuplicateScenePopup"
	);
	if (duplicateScenePopupRequested_) {
		ImGui::OpenPopup(duplicateScenePopupLabel);
		duplicateScenePopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		duplicateScenePopupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		const SceneDescriptor* source = sceneCatalog_
			? sceneCatalog_->Find(sceneAssetTargetId_)
			: nullptr;
		if (source) {
			ImGui::Text(
				SelectEditorText(editorLanguage_, "複製元: %s", "Source: %s"),
				source->displayName.c_str()
			);
		}
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "名前###DuplicateSceneName", "Name###DuplicateSceneName"),
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_)
		);
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "Scene ID###DuplicateSceneId", "Scene ID###DuplicateSceneId"),
			sceneAssetIdBuffer_,
			sizeof(sceneAssetIdBuffer_)
		);
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "ファイル名###DuplicateSceneFile", "File Name###DuplicateSceneFile"),
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_)
		);
		const bool canSubmit = source && sceneAssetNameBuffer_[0] != '\0' &&
			sceneAssetIdBuffer_[0] != '\0' &&
			sceneAssetFileBuffer_[0] != '\0';
		ImGui::BeginDisabled(!canSubmit);
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"複製###DuplicateSceneSubmit",
			"Duplicate###DuplicateSceneSubmit"
		))) {
			SceneAssetRequest request{};
			request.operation = SceneAssetOperation::Duplicate;
			request.sourceSceneId = sceneAssetTargetId_;
			request.sceneId = sceneAssetIdBuffer_;
			request.displayName = sceneAssetNameBuffer_;
			request.assetPath = BuildSceneAssetPath(sceneAssetFileBuffer_);
			QueueSceneAssetRequest(request);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"キャンセル###CancelDuplicateScene",
			"Cancel###CancelDuplicateScene"
		))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	const char* renameScenePopupLabel = SelectEditorText(
		editorLanguage_,
		"Scene名を変更###RenameScenePopup",
		"Rename Scene###RenameScenePopup"
	);
	if (renameScenePopupRequested_) {
		ImGui::OpenPopup(renameScenePopupLabel);
		renameScenePopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		renameScenePopupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::Text(
			SelectEditorText(editorLanguage_, "Scene ID: %s", "Scene ID: %s"),
			sceneAssetTargetId_.c_str()
		);
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"参照を維持するため、Scene IDは変更しません。",
			"Scene ID remains stable so references do not change."
		));
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "名前###RenameSceneName", "Name###RenameSceneName"),
			sceneAssetNameBuffer_,
			sizeof(sceneAssetNameBuffer_)
		);
		ImGui::InputText(
			SelectEditorText(editorLanguage_, "ファイル名###RenameSceneFile", "File Name###RenameSceneFile"),
			sceneAssetFileBuffer_,
			sizeof(sceneAssetFileBuffer_)
		);
		const bool canSubmit = !sceneAssetTargetId_.empty() &&
			sceneAssetNameBuffer_[0] != '\0' &&
			sceneAssetFileBuffer_[0] != '\0';
		ImGui::BeginDisabled(!canSubmit);
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"名前を変更###RenameSceneSubmit",
			"Rename###RenameSceneSubmit"
		))) {
			SceneAssetRequest request{};
			request.operation = SceneAssetOperation::Rename;
			request.sceneId = sceneAssetTargetId_;
			request.displayName = sceneAssetNameBuffer_;
			request.assetPath = BuildSceneAssetPath(sceneAssetFileBuffer_);
			QueueSceneAssetRequest(request);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"キャンセル###CancelRenameScene",
			"Cancel###CancelRenameScene"
		))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	const char* deleteScenePopupLabel = SelectEditorText(
		editorLanguage_,
		"Sceneを削除###DeleteScenePopup",
		"Delete Scene###DeleteScenePopup"
	);
	if (deleteScenePopupRequested_) {
		ImGui::OpenPopup(deleteScenePopupLabel);
		deleteScenePopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		deleteScenePopupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		const SceneDescriptor* target = sceneCatalog_
			? sceneCatalog_->Find(sceneAssetTargetId_)
			: nullptr;
		ImGui::TextUnformatted(SelectEditorText(
			editorLanguage_,
			"Scene Assetを削除し、Catalogから取り除きますか？",
			"Delete the Scene asset and remove it from the Catalog?"
		));
		if (target) {
			ImGui::Text(
				SelectEditorText(editorLanguage_, "Scene: %s", "Scene: %s"),
				target->displayName.c_str()
			);
			ImGui::Text(
				SelectEditorText(editorLanguage_, "パス: %s", "Path: %s"),
				target->assetPath.c_str()
			);
		}
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"ほかのSceneから参照されている場合は削除できません。",
			"Deletion is rejected when another Scene references it."
		));
		ImGui::BeginDisabled(!target);
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"削除###DeleteSceneSubmit",
			"Delete###DeleteSceneSubmit"
		))) {
			SceneAssetRequest request{};
			request.operation = SceneAssetOperation::Delete;
			request.sceneId = sceneAssetTargetId_;
			QueueSceneAssetRequest(request);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(SelectEditorText(
			editorLanguage_,
			"キャンセル###CancelDeleteScene",
			"Cancel###CancelDeleteScene"
		))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	const char* sceneAssetErrorPopupLabel = SelectEditorText(
		editorLanguage_,
		"Scene Assetエラー###SceneAssetErrorPopup",
		"Scene Asset Error###SceneAssetErrorPopup"
	);
	if (sceneAssetErrorPopupRequested_) {
		ImGui::OpenPopup(sceneAssetErrorPopupLabel);
		sceneAssetErrorPopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		sceneAssetErrorPopupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::TextWrapped("%s", sceneAssetErrorMessage_.c_str());
		if (ImGui::Button("OK###CloseSceneAssetError")) {
			sceneAssetErrorMessage_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void ImGuiManager::DrawSettingsMenu() {
	if (!ImGui::BeginMenu(SelectEditorText(
		editorLanguage_,
		"設定###SettingsMenu",
		"Settings###SettingsMenu"
	))) {
		return;
	}
	if (ImGui::BeginMenu(
		SelectEditorText(
			editorLanguage_,
			"Project###ProjectSettingsMenu",
			"Project###ProjectSettingsMenu"
		),
		sceneCatalog_ != nullptr
	)) {
		if (ImGui::BeginMenu(SelectEditorText(
			editorLanguage_,
			"開始Scene###StartupSceneMenu",
			"Startup Scene###StartupSceneMenu"
		))) {
			for (const SceneDescriptor& scene : sceneCatalog_->GetScenes()) {
				const bool selected =
					scene.id == sceneCatalog_->GetStartSceneId();
				const std::string label = scene.displayName + "##startup_" + scene.id;
				if (ImGui::MenuItem(
					label.c_str(),
					nullptr,
					selected,
					!startSceneRequestPending_
				) && !selected) {
					requestedStartSceneId_ = scene.id;
					startSceneRequestPending_ = true;
				}
			}
			ImGui::Separator();
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"resources/scenes/scenes.jsonへ保存されます。",
				"Saved in resources/scenes/scenes.json"
			));
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(SelectEditorText(
			editorLanguage_,
			"起動モード###StartupModeMenu",
			"Startup Mode###StartupModeMenu"
		))) {
			struct StartupModeEntry {
				const char* label;
				SceneBuildConfiguration configuration;
			};
			const StartupModeEntry entries[] = {
				{ "Debug", SceneBuildConfiguration::Debug },
				{ "Development", SceneBuildConfiguration::Development },
				{ "Release", SceneBuildConfiguration::Release }
			};
			for (const StartupModeEntry& entry : entries) {
				if (!ImGui::BeginMenu(entry.label)) {
					continue;
				}
				const SceneStartupMode currentMode =
					sceneCatalog_->GetStartupMode(entry.configuration);
				const bool editorAllowed =
					entry.configuration != SceneBuildConfiguration::Release;
				if (ImGui::MenuItem(
					SelectEditorText(
						editorLanguage_,
						"Editor###StartupEditorMode",
						"Editor###StartupEditorMode"
					),
					nullptr,
					currentMode == SceneStartupMode::Editor,
					editorAllowed && !startupModeRequestPending_
				)) {
					requestedStartupConfiguration_ = entry.configuration;
					requestedStartupMode_ = SceneStartupMode::Editor;
					startupModeRequestPending_ = true;
				}
				if (ImGui::MenuItem(
					SelectEditorText(
						editorLanguage_,
						"Runtime###StartupRuntimeMode",
						"Runtime###StartupRuntimeMode"
					),
					nullptr,
					currentMode == SceneStartupMode::Runtime,
					!startupModeRequestPending_
				)) {
					requestedStartupConfiguration_ = entry.configuration;
					requestedStartupMode_ = SceneStartupMode::Runtime;
					startupModeRequestPending_ = true;
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"ReleaseはRuntime起動だけに対応しています。",
				"Release supports Runtime startup only."
			));
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}

	ImGui::Separator();

	if (ImGui::BeginMenu(SelectEditorText(
		editorLanguage_,
		"ウィンドウ###WindowsMenu",
		"Windows###WindowsMenu"
	))) {
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "Hierarchy###ShowHierarchyWindow", "Hierarchy###ShowHierarchyWindow"), nullptr, &showHierarchy_);
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "Inspector###ShowInspectorWindow", "Inspector###ShowInspectorWindow"), nullptr, &showInspector_);
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "Project###ShowProjectWindow", "Project###ShowProjectWindow"), nullptr, &showProject_);
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "Prefab###ShowPrefabWindow", "Prefab###ShowPrefabWindow"), nullptr, &showPrefab_);
		bool prefabInspectorVisible = showPrefabInspector_;
		if (ImGui::MenuItem(
			SelectEditorText(
				editorLanguage_,
				"Prefab Inspector###ShowPrefabInspectorWindow",
				"Prefab Inspector###ShowPrefabInspectorWindow"
			),
			nullptr,
			&prefabInspectorVisible
		)) {
			showPrefabInspector_ = prefabInspectorVisible;
			if (showPrefabInspector_) {
				showPrefab_ = true;
				prefabInspectorFocusRequested_ = true;
			}
		}
		if (ImGui::MenuItem(
			SelectEditorText(
				editorLanguage_,
				"Prefab Quick Open###PrefabQuickOpenMenuItem",
				"Prefab Quick Open###PrefabQuickOpenMenuItem"
			),
			"Ctrl+Shift+P"
		)) {
			RequestPrefabQuickOpen();
		}
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "Console###ShowConsoleWindow", "Console###ShowConsoleWindow"), nullptr, &showConsole_);
		ImGui::MenuItem(SelectEditorText(
			editorLanguage_,
			"Fishing Score Attack Console###ShowFishingScoreAttackConsoleWindow",
			"Fishing Score Attack Console###ShowFishingScoreAttackConsoleWindow"
		), nullptr, &showFishingScoreAttackConsole_);
		ImGui::MenuItem(SelectEditorText(
			editorLanguage_,
			"入力設定###ShowInputSettingsWindow",
			"Input Settings###ShowInputSettingsWindow"
		), nullptr, &showInputSettings_);
		ImGui::MenuItem(SelectEditorText(editorLanguage_, "読み込み済みScene###ShowLoadedScenesWindow", "Loaded Scenes###ShowLoadedScenesWindow"), nullptr, &showLoadedScenes_);
		ImGui::Separator();
		if (ImGui::MenuItem(SelectEditorText(
			editorLanguage_,
			"レイアウトをリセット###ResetEditorLayout",
			"Reset Layout###ResetEditorLayout"
		))) {
			resetLayout_ = true;
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(SelectEditorText(
		editorLanguage_,
		"アプリケーション###ApplicationMenu",
		"Application###ApplicationMenu"
	))) {
		if (ImGui::MenuItem(
			SelectEditorText(
				editorLanguage_,
				"全画面で起動###StartFullscreenMenuItem",
				"Start in Fullscreen###StartFullscreenMenuItem"
			),
			nullptr,
			startFullscreen_
		)) {
			startFullscreen_ = !startFullscreen_;
			SaveEditorSettings();
		}
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"次回起動時に適用します。現在はF11で切り替えられます。",
			"Applied on next launch. F11 toggles now."
		));
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu(SelectEditorText(
		editorLanguage_,
		"外観###AppearanceMenu",
		"Appearance###AppearanceMenu"
	))) {
		const std::string languageMenuLabel = std::string(SelectEditorText(
			editorLanguage_,
			"言語",
			"Language"
		)) + "###EditorLanguageMenu";
		if (ImGui::BeginMenu(languageMenuLabel.c_str())) {
			const std::string japaneseLabel = std::string(SelectEditorText(
				editorLanguage_,
				"日本語",
				"Japanese"
			)) + "###EditorLanguageJapanese";
			if (ImGui::MenuItem(
				japaneseLabel.c_str(),
				nullptr,
				editorLanguage_ == EditorLanguage::Japanese
			)) {
				editorLanguage_ = EditorLanguage::Japanese;
				SaveEditorSettings();
			}
			const std::string englishLabel = std::string(SelectEditorText(
				editorLanguage_,
				"英語",
				"English"
			)) + "###EditorLanguageEnglish";
			if (ImGui::MenuItem(
				englishLabel.c_str(),
				nullptr,
				editorLanguage_ == EditorLanguage::English
			)) {
				editorLanguage_ = EditorLanguage::English;
				SaveEditorSettings();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(SelectEditorText(
			editorLanguage_,
			"フォント###FontMenu",
			"Font###FontMenu"
		))) {
			if (ImGui::MenuItem(
				SelectEditorText(
					editorLanguage_,
					"MS Gothic（既定・等幅）###FontMsGothic",
					"MS Gothic (Default, Monospace)###FontMsGothic"
				),
				nullptr,
				editorFontPreset_ == EditorFontPreset::MsGothic
			)) {
				editorFontPreset_ = EditorFontPreset::MsGothic;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			if (ImGui::MenuItem(
				"Yu Gothic UI###FontYuGothicUi",
				nullptr,
				editorFontPreset_ == EditorFontPreset::YuGothicUi
			)) {
				editorFontPreset_ = EditorFontPreset::YuGothicUi;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			if (ImGui::MenuItem(
				"Meiryo###FontMeiryo",
				nullptr,
				editorFontPreset_ == EditorFontPreset::Meiryo
			)) {
				editorFontPreset_ = EditorFontPreset::Meiryo;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			if (ImGui::MenuItem(
				"BIZ UD Gothic###FontBizUdGothic",
				nullptr,
				editorFontPreset_ == EditorFontPreset::BizUdGothic
			)) {
				editorFontPreset_ = EditorFontPreset::BizUdGothic;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			if (ImGui::MenuItem(
				SelectEditorText(
					editorLanguage_,
					"ImGui Default＋日本語###FontImGuiDefault",
					"ImGui Default + Japanese###FontImGuiDefault"
				),
				nullptr,
				editorFontPreset_ == EditorFontPreset::ImGuiDefaultWithJapanese
			)) {
				editorFontPreset_ = EditorFontPreset::ImGuiDefaultWithJapanese;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			if (ImGui::MenuItem(
				SelectEditorText(
					editorLanguage_,
					"Cascadia Mono＋日本語###FontCascadiaMono",
					"Cascadia Mono + Japanese###FontCascadiaMono"
				),
				nullptr,
				editorFontPreset_ == EditorFontPreset::CascadiaMonoWithJapanese
			)) {
				editorFontPreset_ = EditorFontPreset::CascadiaMonoWithJapanese;
				RequestEditorFontRebuild();
				SaveEditorSettings();
			}
			ImGui::EndMenu();
		}

		if (ImGui::DragFloat(
			SelectEditorText(
				editorLanguage_,
				"フォントサイズ###EditorFontSize",
				"Font Size###EditorFontSize"
			),
			&editorFontSize_,
			0.25f,
			10.0f,
			22.0f,
			"%.1f px"
		)) {
			RequestEditorFontRebuild();
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			SaveEditorSettings();
		}
		ImGui::EndMenu();
	}

	ImGui::EndMenu();
}

void ImGuiManager::DrawProjectSettingsDialogs() {
	const char* popupLabel = SelectEditorText(
		editorLanguage_,
		"Project設定エラー###ProjectSettingsErrorPopup",
		"Project Settings Error###ProjectSettingsErrorPopup"
	);
	if (projectSettingsErrorPopupRequested_) {
		ImGui::OpenPopup(popupLabel);
		projectSettingsErrorPopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		popupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::TextWrapped("%s", projectSettingsErrorMessage_.c_str());
		if (ImGui::Button("OK###CloseProjectSettingsError")) {
			projectSettingsErrorMessage_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void ImGuiManager::CreateDockSpace(){
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	const ImGuiWindowFlags windowFlags =
		ImGuiWindowFlags_MenuBar |
		ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNavFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("EditorDockSpace", nullptr, windowFlags);
	ImGui::PopStyleVar(3);

	if (ImGui::BeginMenuBar()) {
		DrawSceneMenu();
		if (ImGui::BeginMenu("Project###ProjectLauncherMenu")) {
			if (ImGui::MenuItem("Project Manager...###ShowProjectLauncher")) {
				showProjectLauncher_ = true;
				QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::Refresh });
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(SelectEditorText(
			editorLanguage_,
			"Prefab###PrefabMenu",
			"Prefab###PrefabMenu"
		))) {
			if (ImGui::MenuItem(SelectEditorText(
				editorLanguage_,
				"Prefab Windowを表示###ShowPrefabWindowMenuItem",
				"Show Prefab Window###ShowPrefabWindowMenuItem"
			))) {
				showPrefab_ = true;
				prefabFocusFramesRemaining_ = 2;
			}
			if (ImGui::MenuItem(SelectEditorText(
				editorLanguage_,
				"Prefab Inspectorを表示###ShowPrefabInspectorMenuItem",
				"Show Prefab Inspector###ShowPrefabInspectorMenuItem"
			))) {
				showPrefab_ = true;
				showPrefabInspector_ = true;
				prefabInspectorFocusRequested_ = true;
			}
			if (ImGui::MenuItem(
				SelectEditorText(
					editorLanguage_,
					"Quick Open...###QuickOpenPrefabMenuItem",
					"Quick Open...###QuickOpenPrefabMenuItem"
				),
				"Ctrl+Shift+P"
			)) {
				RequestPrefabQuickOpen();
			}
			ImGui::EndMenu();
		}
		DrawSettingsMenu();
		DrawPlaybackControls();
		ImGui::EndMenuBar();
	}
	const ImGuiIO& dockSpaceIo = ImGui::GetIO();
	if (
		dockSpaceIo.KeyCtrl &&
		dockSpaceIo.KeyShift &&
		ImGui::IsKeyPressed(ImGuiKey_P, false)
	) {
		RequestPrefabQuickOpen();
	}
	if (prefabQuickOpenPopupRequested_) {
		prefabQuickOpenSearchBuffer_[0] = '\0';
		prefabQuickOpenFocusRequested_ = true;
		ImGui::OpenPopup(SelectEditorText(
			editorLanguage_,
			"Prefab Quick Open###PrefabQuickOpenPopup",
			"Quick Open Prefab###PrefabQuickOpenPopup"
		));
		prefabQuickOpenPopupRequested_ = false;
	}
	DrawPrefabQuickOpenPopup();
	DrawSceneSwitchConfirmation();
	DrawSceneAssetDialogs();
	DrawProjectSettingsDialogs();
	DrawProjectLauncherWindow();

	const ImGuiID dockSpaceId = ImGui::GetID("UnityEditorDockSpaceV2");
	if (
		resetLayout_ ||
		ImGui::DockBuilderGetNode(dockSpaceId) == nullptr
	) {
		BuildDefaultLayout();
		resetLayout_ = false;
	}

	ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f));
	ImGui::End();
}

void ImGuiManager::DrawPlaybackControls() {
	if (!editorSession_) {
		return;
	}

	ImGui::Separator();
	const EditorPlayState state = editorSession_->GetState();
	if (state == EditorPlayState::Edit) {
		if (ImGui::SmallButton("Play")) {
			editorSession_->Play();
		}
	}
	else {
		if (ImGui::SmallButton("Stop")) {
			editorSession_->Stop();
		}
		ImGui::SameLine();
		if (state == EditorPlayState::Paused) {
			if (ImGui::SmallButton("Resume")) {
				editorSession_->Resume();
			}
		}
		else if (ImGui::SmallButton("Pause")) {
			editorSession_->Pause();
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("F1 Play/Stop / F2 Pause");

	ImGui::SameLine();
	ImGui::BeginDisabled(!editorSession_->IsEditing());
	const bool saveRequested = ImGui::SmallButton("Save Scene");
	ImGui::EndDisabled();
	if (editorSession_->IsEditing() && saveRequested) {
		editorSession_->Save();
	}

	ImGui::SameLine();
	ImGui::BeginDisabled(
		!editorSession_->IsEditing() ||
		!editorSession_->CanUndo()
	);
	const bool undoRequested = ImGui::SmallButton("Undo");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(
		!editorSession_->IsEditing() ||
		!editorSession_->CanRedo()
	);
	const bool redoRequested = ImGui::SmallButton("Redo");
	ImGui::EndDisabled();
	if (editorSession_->IsEditing() && undoRequested) {
		editorSession_->Undo();
	}
	if (editorSession_->IsEditing() && redoRequested) {
		editorSession_->Redo();
	}

	ImGui::SameLine();
	if (state == EditorPlayState::Playing) {
		ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.4f, 1.0f), "Playing");
	}
	else if (state == EditorPlayState::Paused) {
		ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "Paused");
	}
	else if (editorSession_->GetEditDocument().IsDirty()) {
		ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "Unsaved");
	}
	else {
		ImGui::TextDisabled("Edit");
	}
}

void ImGuiManager::BuildDefaultLayout() {
	const ImGuiID dockSpaceId = ImGui::GetID("UnityEditorDockSpaceV2");
	const ImGuiViewport* viewport = ImGui::GetMainViewport();

	ImGui::DockBuilderRemoveNode(dockSpaceId);
	ImGui::DockBuilderAddNode(
		dockSpaceId,
		ImGuiDockNodeFlags_DockSpace
	);
	ImGui::DockBuilderSetNodeSize(dockSpaceId, viewport->WorkSize);

	ImGuiID centerId = dockSpaceId;
	const ImGuiID leftId = ImGui::DockBuilderSplitNode(
		centerId,
		ImGuiDir_Left,
		0.22f,
		nullptr,
		&centerId
	);
	const ImGuiID rightId = ImGui::DockBuilderSplitNode(
		centerId,
		ImGuiDir_Right,
		0.51f,
		nullptr,
		&centerId
	);
	const ImGuiID bottomId = ImGui::DockBuilderSplitNode(
		centerId,
		ImGuiDir_Down,
		0.42f,
		nullptr,
		&centerId
	);

	ImGui::DockBuilderDockWindow("Scene", centerId);
	ImGui::DockBuilderDockWindow("Prefab", centerId);
	ImGui::DockBuilderDockWindow("Hierarchy", leftId);
	ImGui::DockBuilderDockWindow("Inspector", rightId);
	ImGui::DockBuilderDockWindow("Prefab Inspector", rightId);
	ImGui::DockBuilderDockWindow("Scene Controls", rightId);
	ImGui::DockBuilderDockWindow("Title Scene", rightId);
	ImGui::DockBuilderDockWindow("Particle Effect Editor", rightId);
	ImGui::DockBuilderDockWindow("Environment", rightId);
	ImGui::DockBuilderDockWindow("Post Process Stack", rightId);
	ImGui::DockBuilderDockWindow("Scene Particles", rightId);
	ImGui::DockBuilderDockWindow("Monitor Debug", bottomId);
	ImGui::DockBuilderDockWindow("Project", bottomId);
	ImGui::DockBuilderDockWindow("Console", bottomId);
	ImGui::DockBuilderDockWindow(
		"Fishing Score Attack Console###FishingScoreAttackConsole",
		rightId
	);
	ImGui::DockBuilderDockWindow(
		"Input Settings###InputSettingsWindow",
		rightId
	);
	ImGui::DockBuilderDockWindow("Loaded Scenes", bottomId);
	ImGui::DockBuilderFinish(dockSpaceId);
}

void ImGuiManager::DrawLoadedScenesWindow() {
	ImGui::Begin("Loaded Scenes", &showLoadedScenes_);

	const bool runtimeMode = editorSession_ && !editorSession_->IsEditing();
	if (!sceneManager_ || !sceneCatalog_) {
		ImGui::TextDisabled("Scene runtime is not available.");
		ImGui::End();
		return;
	}
	if (!runtimeMode) {
		ImGui::TextDisabled("Additive Scene operations are available in Play Mode.");
	}

	const std::vector<const SceneInstance*> loadedScenes =
		sceneManager_->GetLoadedSceneInstances();
	const SceneInstanceId activeInstanceId =
		sceneManager_->GetActiveSceneInstanceId();

	if (ImGui::BeginTable(
		"LoadedSceneInstances",
		5,
		ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_SizingStretchProp
	)) {
		ImGui::TableSetupColumn("Scene");
		ImGui::TableSetupColumn("Instance Key");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Persistent");
		ImGui::TableSetupColumn("Actions");
		ImGui::TableHeadersRow();

		for (const SceneInstance* instance : loadedScenes) {
			if (!instance) {
				continue;
			}
			const SceneInstanceId instanceId = instance->GetId();
			ImGui::PushID(static_cast<int>(instanceId));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(instance->GetSceneId().c_str());
			ImGui::TextDisabled(
				"ID: %llu",
				static_cast<unsigned long long>(instanceId)
			);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(instance->GetInstanceKey().c_str());

			ImGui::TableSetColumnIndex(2);
			const bool isActive = instanceId == activeInstanceId;
			if (isActive) {
				ImGui::TextUnformatted("Active");
			} else {
				ImGui::BeginDisabled(!runtimeMode || sceneInstanceRequestPending_);
				if (ImGui::SmallButton("Set Active")) {
					SceneInstanceRequest request{};
					request.operation = SceneInstanceOperation::SetActive;
					request.instanceId = instanceId;
					QueueSceneInstanceRequest(request);
				}
				ImGui::EndDisabled();
			}

			ImGui::TableSetColumnIndex(3);
			bool persistent = instance->IsPersistent();
			ImGui::BeginDisabled(!runtimeMode || sceneInstanceRequestPending_);
			if (ImGui::Checkbox("##Persistent", &persistent)) {
				SceneInstanceRequest request{};
				request.operation = SceneInstanceOperation::SetPersistent;
				request.instanceId = instanceId;
				request.persistent = persistent;
				QueueSceneInstanceRequest(request);
			}
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(4);
			const bool canUnload = runtimeMode && loadedScenes.size() > 1 &&
				!sceneInstanceRequestPending_;
			ImGui::BeginDisabled(!canUnload);
			if (ImGui::SmallButton("Unload")) {
				SceneInstanceRequest request{};
				request.operation = SceneInstanceOperation::Unload;
				request.instanceId = instanceId;
				QueueSceneInstanceRequest(request);
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Additive Load");
	const std::vector<SceneDescriptor>& catalogScenes = sceneCatalog_->GetScenes();
	static int selectedSceneIndex = 0;
	if (selectedSceneIndex >= static_cast<int>(catalogScenes.size())) {
		selectedSceneIndex = 0;
	}
	const char* selectedLabel = catalogScenes.empty()
		? "No registered Scenes"
		: catalogScenes[selectedSceneIndex].displayName.c_str();
	ImGui::BeginDisabled(!runtimeMode || catalogScenes.empty() ||
		sceneInstanceRequestPending_);
	if (ImGui::BeginCombo("Scene", selectedLabel)) {
		for (int index = 0; index < static_cast<int>(catalogScenes.size()); ++index) {
			const bool selected = index == selectedSceneIndex;
			if (ImGui::Selectable(
				catalogScenes[index].displayName.c_str(),
				selected
			)) {
				selectedSceneIndex = index;
			}
			if (selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::InputText(
		"Instance Key (optional)",
		additiveInstanceKeyBuffer_,
		sizeof(additiveInstanceKeyBuffer_)
	);
	if (ImGui::Button("Load Additive") && !catalogScenes.empty()) {
		SceneInstanceRequest request{};
		request.operation = SceneInstanceOperation::LoadAdditive;
		request.sceneId = catalogScenes[selectedSceneIndex].id;
		request.instanceKey = additiveInstanceKeyBuffer_;
		QueueSceneInstanceRequest(request);
	}
	ImGui::EndDisabled();

	if (!sceneInstanceStatusMessage_.empty()) {
		const ImVec4 color = sceneInstanceOperationSucceeded_
			? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
			: ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		ImGui::TextColored(color, "%s", sceneInstanceStatusMessage_.c_str());
	}

	ImGui::End();
}

void ImGuiManager::DrawHierarchyWindow(const char* sceneName) {
	ImGui::Begin(
		SelectEditorText(
			editorLanguage_,
			"Hierarchy###Hierarchy",
			"Hierarchy###Hierarchy"
		),
		&showHierarchy_
	);
	if (editorSession_) {
		SceneDocument& document = editorSession_->GetActiveDocument();
		for (
			auto it = selectedEntityIds_.begin();
			it != selectedEntityIds_.end();
		) {
			if (!document.FindEntity(*it)) {
				it = selectedEntityIds_.erase(it);
			} else {
				++it;
			}
		}
		if (selectedEntityId_ == 0) {
			selectedEntityIds_.clear();
			hierarchySelectionAnchorId_ = 0;
		} else if (selectedEntityId_ != hierarchyObservedEntityId_) {
			// Scene ViewなどHierarchy外からの選択は単体選択として受け取り、表示位置を追従する。
			selectedEntityIds_.clear();
			selectedEntityIds_.insert(selectedEntityId_);
			hierarchySelectionAnchorId_ = selectedEntityId_;
			hierarchyRevealRequested_ = true;
		}
		ImGui::BeginDisabled(!editorSession_->IsEditing());
		bool createRequested = false;
		bool createFolderRequested = false;
		bool createCameraPathRequested = false;
		uint64_t createParentId = 0;
		if (ImGui::SmallButton("+")) {
			createRequested = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", SelectEditorText(
				editorLanguage_,
				"空のEntityを作成",
				"Create Empty Entity"
			));
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(SelectEditorText(
			editorLanguage_,
			"+ フォルダー###CreateHierarchyFolder",
			"+ Folder###CreateHierarchyFolder"
		))) {
			createFolderRequested = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", SelectEditorText(
				editorLanguage_,
				"フォルダーを作成",
				"Create Folder"
			));
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(SelectEditorText(
			editorLanguage_,
			"+ パス###CreateHierarchyCameraPath",
			"+ Path###CreateHierarchyCameraPath"
		))) {
			createCameraPathRequested = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", SelectEditorText(
				editorLanguage_,
				"子ポイントを2つ持つCameraPathを作成",
				"Create CameraPath with two child points"
			));
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled(
			SelectEditorText(editorLanguage_, "%zu Entity", "%zu entities"),
			document.GetEntities().size()
		);
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##HierarchySearch",
			SelectEditorText(
				editorLanguage_,
				"検索... team:Fish type:Camera is:inactive",
				"Search... team:Fish type:Camera is:inactive"
			),
			hierarchySearchBuffer_,
			sizeof(hierarchySearchBuffer_)
		);
		ImGui::Separator();

		uint64_t removeId = 0;
		uint64_t duplicateId = 0;
		uint64_t reorderId = 0;
		uint64_t reorderTargetId = 0;
		bool reorderAfter = false;
		uint64_t reparentId = 0;
		uint64_t reparentTargetId = 0;
		std::vector<uint64_t> hierarchyDroppedIds;
		uint64_t moveId = 0;
		int moveDirection = 0;
		if (
			editorSession_->IsEditing() &&
			selectedEntityId_ != 0 &&
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			!ImGui::GetIO().WantTextInput
		) {
			const SceneEntity* selectedEntity = document.FindEntity(selectedEntityId_);
			if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
				FocusSceneCameraOnSelection();
			}
			if (selectedEntity && !selectedEntity->locked) {
				if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
					removeId = selectedEntityId_;
				}
				if (
					ImGui::GetIO().KeyCtrl &&
					ImGui::IsKeyPressed(ImGuiKey_D, false)
				) {
					duplicateId = selectedEntityId_;
				}
			}
		}

		auto drawDropLine = [](const ImVec2& start, const ImVec2& end) {
			ImGui::GetWindowDrawList()->AddLine(
				start,
				end,
				IM_COL32(90, 180, 255, 255),
				2.0f
			);
		};
		auto drawDropRect = [](const ImVec2& itemMin, const ImVec2& itemMax) {
			ImGui::GetWindowDrawList()->AddRect(
				itemMin,
				itemMax,
				IM_COL32(90, 180, 255, 255),
				3.0f,
				0,
				2.0f
			);
		};
		auto toLower = [](std::string value) {
			std::transform(
				value.begin(),
				value.end(),
				value.begin(),
				[](unsigned char ch) {
					return static_cast<char>(std::tolower(ch));
				}
			);
			return value;
		};
		const std::string hierarchySearch = toLower(hierarchySearchBuffer_);
		std::vector<std::string> hierarchyFilterTerms;
		for (size_t begin = 0; begin < hierarchySearch.size();) {
			while (
				begin < hierarchySearch.size() &&
				std::isspace(static_cast<unsigned char>(hierarchySearch[begin]))
			) {
				++begin;
			}
			const size_t end = hierarchySearch.find_first_of(" \t\r\n", begin);
			if (begin < hierarchySearch.size()) {
				hierarchyFilterTerms.push_back(
					hierarchySearch.substr(begin, end - begin)
				);
			}
			begin = end == std::string::npos ? hierarchySearch.size() : end + 1;
		}
		const bool searchActive = !hierarchyFilterTerms.empty();
		uint64_t hierarchyDropTargetId = 0;
		bool hierarchyDropAfter = false;
		bool hierarchyDropIntoFolder = false;
		bool hierarchyDropToRoot = false;
		std::string hierarchyPrefabDropPath;
		uint64_t hierarchyPrefabDropParentId = 0;
		if (!editorSession_->IsEditing() || searchActive) {
			hierarchyDragSourceId_ = 0;
			hierarchyDragActive_ = false;
		}
		auto entityNameMatches = [&](const SceneEntity& entity) {
			for (const std::string& term : hierarchyFilterTerms) {
				const size_t separator = term.find(':');
				if (separator == std::string::npos) {
					if (toLower(entity.name).find(term) == std::string::npos) {
						return false;
					}
					continue;
				}

				const std::string key = term.substr(0, separator);
				const std::string value = term.substr(separator + 1);
				if (value.empty()) {
					continue;
				}
				if (key == "team") {
					const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
					if (!team || toLower(team->name).find(value) == std::string::npos) {
						return false;
					}
				} else if (key == "type") {
					bool typeMatches = entity.folder && value == "folder";
					for (const SceneComponent& component : entity.components) {
						if (toLower(component.type) == value) {
							typeMatches = true;
							break;
						}
					}
					if (!typeMatches) {
						return false;
					}
				} else if (key == "is") {
					const bool matches =
						(value == "active" && entity.active) ||
						(value == "inactive" && !entity.active) ||
						(value == "locked" && entity.locked) ||
						(value == "unlocked" && !entity.locked) ||
						(value == "folder" && entity.folder);
					if (!matches) {
						return false;
					}
				} else {
					// 未対応の条件は名前検索として扱い、検索結果が空になる事故を避ける。
					if (toLower(entity.name).find(term) == std::string::npos) {
						return false;
					}
				}
			}
			return true;
		};
		std::function<bool(uint64_t)> entityVisibleInFilter;
		entityVisibleInFilter = [&](uint64_t entityId) {
			const SceneEntity* entity = document.FindEntity(entityId);
			if (!entity) {
				return false;
			}
			if (entityNameMatches(*entity)) {
				return true;
			}
			for (const SceneEntity& child : document.GetEntities()) {
				if (
					child.parentId == entityId &&
					entityVisibleInFilter(child.id)
				) {
					return true;
				}
			}
			return false;
		};
		std::function<bool(uint64_t)> entitySubtreeHasLocked;
		entitySubtreeHasLocked = [&](uint64_t entityId) {
			const SceneEntity* entity = document.FindEntity(entityId);
			if (!entity) {
				return false;
			}
			if (entity->locked) {
				return true;
			}
			for (const SceneEntity& child : document.GetEntities()) {
				if (
					child.parentId == entityId &&
					entitySubtreeHasLocked(child.id)
				) {
					return true;
				}
			}
			return false;
		};
		std::vector<uint64_t> hierarchyFilterOrder;
		std::function<void(uint64_t)> collectFilterOrder;
		collectFilterOrder = [&](uint64_t entityId) {
			if (!entityVisibleInFilter(entityId)) {
				return;
			}
			hierarchyFilterOrder.push_back(entityId);
			for (const SceneEntity& child : document.GetEntities()) {
				if (child.parentId == entityId) {
					collectFilterOrder(child.id);
				}
			}
		};
		for (const SceneEntity& entity : document.GetEntities()) {
			if (entity.parentId == 0) {
				collectFilterOrder(entity.id);
			}
		}
		auto isEntitySelected = [&](uint64_t entityId) {
			return selectedEntityIds_.find(entityId) != selectedEntityIds_.end();
		};
		auto getSelectedRoots = [&]() {
			std::vector<uint64_t> roots;
			for (const SceneEntity& candidate : document.GetEntities()) {
				if (!isEntitySelected(candidate.id)) {
					continue;
				}
				bool selectedAncestor = false;
				for (
					const SceneEntity* parent = document.FindEntity(candidate.parentId);
					parent;
					parent = document.FindEntity(parent->parentId)
				) {
					if (isEntitySelected(parent->id)) {
						selectedAncestor = true;
						break;
					}
				}
				if (!selectedAncestor) {
					roots.push_back(candidate.id);
				}
			}
			return roots;
		};
		auto getDraggedRoots = [&]() {
			if (hierarchyDragSourceId_ == 0) {
				return std::vector<uint64_t>{};
			}
			if (isEntitySelected(hierarchyDragSourceId_)) {
				return getSelectedRoots();
			}
			return std::vector<uint64_t>{ hierarchyDragSourceId_ };
		};
		auto rootsCanBeEdited = [&](const std::vector<uint64_t>& roots) {
			return !roots.empty() && std::all_of(
				roots.begin(),
				roots.end(),
				[&](uint64_t entityId) {
					const SceneEntity* entity = document.FindEntity(entityId);
					return entity && !entity->locked && !entitySubtreeHasLocked(entityId);
				}
			);
		};
			auto selectEntity = [&](uint64_t entityId, bool extend, bool range) {
			if (range && hierarchySelectionAnchorId_ != 0) {
				const auto anchor = std::find(
					hierarchyFilterOrder.begin(),
					hierarchyFilterOrder.end(),
					hierarchySelectionAnchorId_
				);
				const auto target = std::find(
					hierarchyFilterOrder.begin(),
					hierarchyFilterOrder.end(),
					entityId
				);
				if (anchor != hierarchyFilterOrder.end() && target != hierarchyFilterOrder.end()) {
					selectedEntityIds_.clear();
					const auto first = (std::min)(anchor, target);
					const auto last = (std::max)(anchor, target);
					for (auto it = first; it != std::next(last); ++it) {
						selectedEntityIds_.insert(*it);
					}
				} else {
					selectedEntityIds_.clear();
					selectedEntityIds_.insert(entityId);
					hierarchySelectionAnchorId_ = entityId;
				}
			} else if (extend) {
				if (isEntitySelected(entityId)) {
					selectedEntityIds_.erase(entityId);
				} else {
					selectedEntityIds_.insert(entityId);
					hierarchySelectionAnchorId_ = entityId;
				}
			} else {
				selectedEntityIds_.clear();
				selectedEntityIds_.insert(entityId);
				hierarchySelectionAnchorId_ = entityId;
			}
			selectedEntityId_ = selectedEntityIds_.empty()
				? 0
				: entityId;
			selectedProjectFile_.clear();
			showInspector_ = true;
			revealInspectorRequested_ = true;
		};
		auto setSelectedActive = [&](bool active, uint64_t clickedEntityId) {
			const bool applyToSelection = isEntitySelected(clickedEntityId);
			bool changed = false;
			for (SceneEntity& candidate : document.GetEntities()) {
				if ((applyToSelection && isEntitySelected(candidate.id)) ||
					(!applyToSelection && candidate.id == clickedEntityId)) {
					if (candidate.active != active) {
						candidate.active = active;
						changed = true;
					}
				}
			}
			if (changed) {
				document.MarkDirty();
			}
		};
		auto setSelectedLocked = [&](bool locked, uint64_t clickedEntityId) {
			const bool applyToSelection = isEntitySelected(clickedEntityId);
			bool changed = false;
			for (SceneEntity& candidate : document.GetEntities()) {
				if ((applyToSelection && isEntitySelected(candidate.id)) ||
					(!applyToSelection && candidate.id == clickedEntityId)) {
					if (candidate.locked != locked) {
						candidate.locked = locked;
						changed = true;
					}
				}
			}
			if (changed) {
				document.MarkDirty();
			}
		};

		std::function<void(uint64_t)> drawEntity;
		drawEntity = [&](uint64_t entityId) {
			const SceneEntity* entity = document.FindEntity(entityId);
			if (!entity) {
				return;
			}
			if (!entityVisibleInFilter(entityId)) {
				return;
			}
			const size_t childCount = static_cast<size_t>(std::count_if(
				document.GetEntities().begin(),
				document.GetEntities().end(),
				[&](const SceneEntity& candidate) {
					return candidate.parentId == entityId &&
						entityVisibleInFilter(candidate.id);
				}
			));
			const bool hasChildren = childCount > 0;
			const bool editable = editorSession_->IsEditing() && !entity->locked;

			ImGui::PushID(static_cast<int>(entity->id));
			ImGui::BeginDisabled(!editorSession_->IsEditing());
			bool active = entity->active;
			if (ImGui::SmallButton(active ? "V" : "-")) {
				setSelectedActive(!active, entity->id);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", active
					? SelectEditorText(editorLanguage_, "Entityを非表示", "Hide Entity")
					: SelectEditorText(editorLanguage_, "Entityを表示", "Show Entity"));
			}
			ImGui::SameLine();
			bool locked = entity->locked;
			if (ImGui::SmallButton(locked ? "L" : "U")) {
				setSelectedLocked(!locked, entity->id);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", locked
					? SelectEditorText(editorLanguage_, "編集ロックを解除", "Unlock Editing")
					: SelectEditorText(editorLanguage_, "編集をロック", "Lock Editing"));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGuiTreeNodeFlags flags =
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_SpanAvailWidth;
			if (!hasChildren) {
				flags |= ImGuiTreeNodeFlags_Leaf |
					ImGuiTreeNodeFlags_NoTreePushOnOpen;
			}
			if (isEntitySelected(entity->id)) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}
			const std::string folderPrefix = entity->folder
				? "[" + std::string(SelectEditorText(
					editorLanguage_,
					"フォルダー",
					"Folder"
				)) + " " + std::to_string(childCount) + "]  "
				: "";
			const std::string label = entity->locked
				? folderPrefix + entity->name + SelectEditorText(
					editorLanguage_,
					" [ロック中]",
					" [locked]"
				)
				: entity->active
					? folderPrefix + entity->name
					: folderPrefix + entity->name + SelectEditorText(
						editorLanguage_,
						" (非アクティブ)",
						" (inactive)"
					);
			const SceneTeamSettings* effectiveTeam =
				document.ResolveEntityTeam(*entity);
			const std::string labelWithTeam = effectiveTeam
				? label + " {" + effectiveTeam->name + "}"
				: label;
			const bool revealAncestor =
				hierarchyRevealRequested_ &&
				selectedEntityId_ != 0 &&
				document.IsDescendantOf(selectedEntityId_, entity->id);
			if (
				searchActive ||
				revealAncestor ||
				(
					hierarchyAutoOpenFolderId_ == entity->id &&
					ImGui::GetTime() - hierarchyAutoOpenStartTime_ >= 0.55
				)
			) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			}
			if (entity->folder) {
				ImGui::PushStyleVar(
					ImGuiStyleVar_FramePadding,
					ImVec2(ImGui::GetStyle().FramePadding.x, 5.0f)
				);
			}
			const bool renaming = hierarchyRenameEntityId_ == entity->id;
			const bool open = ImGui::TreeNodeEx(
				"##Entity",
				flags,
				"%s",
				renaming ? " " : labelWithTeam.c_str()
			);
			if (entity->folder) {
				ImGui::PopStyleVar();
			}
			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 itemMax = ImGui::GetItemRectMax();
			if (editable && ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload =
					ImGui::AcceptDragDropPayload("PROJECT_PREFAB_PATH")) {
					const char* droppedPath =
						static_cast<const char*>(payload->Data);
					if (droppedPath && droppedPath[0] != '\0') {
						hierarchyPrefabDropPath = droppedPath;
						hierarchyPrefabDropParentId = entity->id;
					}
				}
				ImGui::EndDragDropTarget();
			}
			const ImVec2 nextItemCursor = ImGui::GetCursorScreenPos();
			const bool treeItemClicked =
				ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
			const bool treeItemDoubleClicked =
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
				ImGui::IsMouseHoveringRect(itemMin, itemMax);
			if (treeItemClicked && !renaming) {
				selectEntity(
					entity->id,
					ImGui::GetIO().KeyCtrl,
					ImGui::GetIO().KeyShift
				);
			}
			if (treeItemDoubleClicked && !renaming) {
				const uint64_t prefabRootId =
					document.FindPrefabInstanceRoot(entity->id);
				const SceneEntity* prefabRoot = prefabRootId != 0
					? document.FindEntity(prefabRootId)
					: nullptr;
				const std::string prefabAssetPath = prefabRoot
					? PrefabAssetRegistry::ResolvePath(
						prefabRoot->prefabAssetId,
						prefabRoot->prefabSourcePath
					)
					: std::string{};
				if (!prefabAssetPath.empty()) {
					RequestOpenPrefab(prefabAssetPath);
				} else if (editable) {
					hierarchyRenameEntityId_ = entity->id;
					strncpy_s(
						hierarchyRenameBuffer_,
						entity->name.c_str(),
						_TRUNCATE
					);
					hierarchyRenameFocusRequested_ = true;
				}
			}
			if (renaming) {
				const float nameStart = itemMin.x + ImGui::GetTreeNodeToLabelSpacing();
				ImGui::SetCursorScreenPos(ImVec2(nameStart, itemMin.y));
				ImGui::SetNextItemWidth((std::max)(itemMax.x - nameStart - 4.0f, 80.0f));
				if (hierarchyRenameFocusRequested_) {
					ImGui::SetKeyboardFocusHere();
					hierarchyRenameFocusRequested_ = false;
				}
				const bool renameCommitted = ImGui::InputText(
					"##RenameEntity",
					hierarchyRenameBuffer_,
					sizeof(hierarchyRenameBuffer_),
					ImGuiInputTextFlags_AutoSelectAll |
						ImGuiInputTextFlags_EnterReturnsTrue
				);
				const bool renameCanceled = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
				if (renameCommitted && hierarchyRenameBuffer_[0] != '\0') {
					if (SceneEntity* mutableEntity = document.FindEntity(entity->id)) {
						mutableEntity->name = hierarchyRenameBuffer_;
						document.MarkDirty();
					}
					hierarchyRenameEntityId_ = 0;
				} else if (renameCanceled || ImGui::IsItemDeactivated()) {
					hierarchyRenameEntityId_ = 0;
				}
				ImGui::SetCursorScreenPos(nextItemCursor);
			}
			if (hierarchyRevealRequested_ && selectedEntityId_ == entity->id) {
				ImGui::SetScrollHereY(0.5f);
				hierarchyRevealRequested_ = false;
			}

			const bool rowHovered =
				ImGui::IsMouseHoveringRect(itemMin, itemMax);
			if (
				editable &&
				!searchActive &&
				rootsCanBeEdited(
					isEntitySelected(entity->id)
						? getSelectedRoots()
						: std::vector<uint64_t>{ entity->id }
				) &&
				rowHovered &&
				ImGui::IsMouseClicked(ImGuiMouseButton_Left)
			) {
				hierarchyDragSourceId_ = entity->id;
				hierarchyDragActive_ = false;
			}
			if (
				hierarchyDragSourceId_ == entity->id &&
				ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)
			) {
				hierarchyDragActive_ = true;
			}
			if (hierarchyDragActive_ && hierarchyDragSourceId_ != 0) {
				const std::vector<uint64_t> draggedRoots = getDraggedRoots();
				const bool targetIsDragged = std::find(
					draggedRoots.begin(),
					draggedRoots.end(),
					entity->id
				) != draggedRoots.end();
				if (
					rootsCanBeEdited(draggedRoots) &&
					!targetIsDragged &&
					!entity->locked &&
					rowHovered
				) {
					const float itemHeight =
						(std::max)(itemMax.y - itemMin.y, 1.0f);
					const float localY = ImGui::GetMousePos().y - itemMin.y;
					const bool canDropIntoFolder = entity->folder && std::all_of(
						draggedRoots.begin(),
						draggedRoots.end(),
						[&](uint64_t draggedId) {
							return !document.IsDescendantOf(entity->id, draggedId);
						}
					);
					const bool canDropAsSibling = std::all_of(
						draggedRoots.begin(),
						draggedRoots.end(),
						[&](uint64_t draggedId) {
							return entity->parentId == 0 ||
								!document.IsDescendantOf(entity->parentId, draggedId);
						}
					);
					if (
						canDropIntoFolder &&
						localY >= itemHeight * 0.25f &&
						localY <= itemHeight * 0.75f
					) {
						if (hierarchyAutoOpenFolderId_ != entity->id) {
							hierarchyAutoOpenFolderId_ = entity->id;
							hierarchyAutoOpenStartTime_ = ImGui::GetTime();
						} else if (ImGui::GetTime() - hierarchyAutoOpenStartTime_ >= 0.55) {
							hierarchyAutoOpenFolderId_ = entity->id;
						}
						hierarchyDropTargetId = entity->id;
						hierarchyDropAfter = false;
						hierarchyDropIntoFolder = true;
					} else if (canDropAsSibling) {
						hierarchyDropTargetId = entity->id;
						hierarchyDropAfter =
							ImGui::GetMousePos().y >= (itemMin.y + itemMax.y) * 0.5f;
						hierarchyDropIntoFolder = false;
					}
				}
			}
			if (
				hierarchyDragActive_ &&
				hierarchyDropTargetId == entity->id
			) {
				if (hierarchyDropIntoFolder) {
					drawDropRect(itemMin, itemMax);
				} else if (hierarchyDropAfter) {
					drawDropLine(ImVec2(itemMin.x, itemMax.y), itemMax);
				} else {
					drawDropLine(itemMin, ImVec2(itemMax.x, itemMin.y));
				}
			}

			if (
				editable &&
				ImGui::BeginPopupContextItem("EntityContext")
			) {
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"子Entityを作成###CreateChildEntity",
					"Create Child###CreateChildEntity"
				))) {
					createRequested = true;
					createParentId = entity->id;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"子フォルダーを作成###CreateChildFolder",
					"Create Child Folder###CreateChildFolder"
				))) {
					createFolderRequested = true;
					createParentId = entity->id;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"複製###DuplicateHierarchyEntity",
					"Duplicate###DuplicateHierarchyEntity"
				))) {
					duplicateId = entity->id;
				}
				ImGui::Separator();
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"上へ移動###MoveHierarchyEntityUp",
					"Move Up###MoveHierarchyEntityUp"
				))) {
					moveId = entity->id;
					moveDirection = -1;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"下へ移動###MoveHierarchyEntityDown",
					"Move Down###MoveHierarchyEntityDown"
				))) {
					moveId = entity->id;
					moveDirection = 1;
				}
				ImGui::Separator();
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"削除###DeleteHierarchyEntity",
					"Delete###DeleteHierarchyEntity"
				))) {
					removeId = entity->id;
				}
				ImGui::EndPopup();
			}

			if (hasChildren && open) {
				for (const SceneEntity& child : document.GetEntities()) {
					if (
						child.parentId == entity->id &&
						entityVisibleInFilter(child.id)
					) {
						drawEntity(child.id);
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		};

		const char* rootName = document.GetSceneName().empty()
			? (sceneName && sceneName[0] != '\0'
				? sceneName
				: SelectEditorText(editorLanguage_, "Scene", "Scene"))
			: document.GetSceneName().c_str();
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		const bool rootOpen = ImGui::TreeNodeEx(
			"##SceneRoot",
			ImGuiTreeNodeFlags_DefaultOpen |
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_SpanAvailWidth,
			"%s",
			rootName
		);
		const ImVec2 rootItemMin = ImGui::GetItemRectMin();
		const ImVec2 rootItemMax = ImGui::GetItemRectMax();
		if (editorSession_->IsEditing() && ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload =
				ImGui::AcceptDragDropPayload("PROJECT_PREFAB_PATH")) {
				const char* droppedPath =
					static_cast<const char*>(payload->Data);
				if (droppedPath && droppedPath[0] != '\0') {
					hierarchyPrefabDropPath = droppedPath;
					hierarchyPrefabDropParentId = 0;
				}
			}
			ImGui::EndDragDropTarget();
		}
		const std::vector<uint64_t> draggedRoots = getDraggedRoots();
		if (
			hierarchyDragActive_ &&
			rootsCanBeEdited(draggedRoots) &&
			std::any_of(
				draggedRoots.begin(),
				draggedRoots.end(),
				[&](uint64_t entityId) {
					const SceneEntity* entity = document.FindEntity(entityId);
					return entity && entity->parentId != 0;
				}
			) &&
			ImGui::IsMouseHoveringRect(rootItemMin, rootItemMax)
		) {
			hierarchyDropToRoot = true;
			drawDropRect(rootItemMin, rootItemMax);
		}
		if (rootOpen) {
			for (const SceneEntity& entity : document.GetEntities()) {
				if (entity.parentId == 0) {
					drawEntity(entity.id);
				}
			}
			ImGui::TreePop();
		}
		if (hierarchyDragActive_) {
			const ImVec2 mouse = ImGui::GetMousePos();
			const ImVec2 windowPos = ImGui::GetWindowPos();
			const ImVec2 windowSize = ImGui::GetWindowSize();
			const float edgeSize = 28.0f;
			const float scrollStep = 420.0f * ImGui::GetIO().DeltaTime;
			if (mouse.y < windowPos.y + edgeSize) {
				ImGui::SetScrollY((std::max)(0.0f, ImGui::GetScrollY() - scrollStep));
			} else if (mouse.y > windowPos.y + windowSize.y - edgeSize) {
				ImGui::SetScrollY((std::min)(
					ImGui::GetScrollMaxY(),
					ImGui::GetScrollY() + scrollStep
				));
			}
		}

		if (
			hierarchyDragSourceId_ != 0 &&
			!ImGui::IsMouseDown(ImGuiMouseButton_Left)
		) {
			if (
				hierarchyDragActive_ &&
				(hierarchyDropTargetId != 0 || hierarchyDropToRoot)
			) {
				hierarchyDroppedIds = getDraggedRoots();
				if (hierarchyDropToRoot) {
					reparentId = hierarchyDragSourceId_;
					reparentTargetId = 0;
				} else if (hierarchyDropIntoFolder) {
					reparentId = hierarchyDragSourceId_;
					reparentTargetId = hierarchyDropTargetId;
				} else {
					reorderId = hierarchyDragSourceId_;
					reorderTargetId = hierarchyDropTargetId;
					reorderAfter = hierarchyDropAfter;
				}
			}
			hierarchyDragSourceId_ = 0;
			hierarchyDragActive_ = false;
			hierarchyAutoOpenFolderId_ = 0;
			hierarchyAutoOpenStartTime_ = 0.0;
		}

		if (reorderId != 0) {
			const SceneEntity* targetEntity = document.FindEntity(reorderTargetId);
			if (
				targetEntity &&
				!targetEntity->locked &&
				rootsCanBeEdited(hierarchyDroppedIds)
			) {
				if (reorderAfter) {
					uint64_t insertAfterId = reorderTargetId;
					for (uint64_t entityId : hierarchyDroppedIds) {
						if (document.MoveEntityToSibling(entityId, insertAfterId, true)) {
							insertAfterId = entityId;
						}
					}
				} else {
					uint64_t insertBeforeId = reorderTargetId;
					for (auto it = hierarchyDroppedIds.rbegin();
						it != hierarchyDroppedIds.rend(); ++it) {
						if (document.MoveEntityToSibling(*it, insertBeforeId, false)) {
							insertBeforeId = *it;
						}
					}
				}
			}
		}
		if (reparentId != 0) {
			const SceneEntity* targetEntity = document.FindEntity(reparentTargetId);
			if (
				(
					reparentTargetId == 0 ||
					(targetEntity && targetEntity->folder && !targetEntity->locked)
				) &&
				rootsCanBeEdited(hierarchyDroppedIds)
			) {
				for (uint64_t entityId : hierarchyDroppedIds) {
					document.MoveEntityToParent(entityId, reparentTargetId);
				}
			}
		}
		if (moveId != 0) {
			if (const SceneEntity* entity = document.FindEntity(moveId)) {
				if (!entity->locked) {
					document.MoveEntity(moveId, moveDirection);
				}
			}
		}
		if (removeId != 0) {
			const std::vector<uint64_t> removeRoots =
				isEntitySelected(removeId)
					? getSelectedRoots()
					: std::vector<uint64_t>{ removeId };
			bool removedAny = false;
			for (uint64_t entityId : removeRoots) {
				if (document.FindEntity(entityId) && !entitySubtreeHasLocked(entityId)) {
					document.RemoveEntity(entityId);
					removedAny = true;
				}
			}
			if (removedAny) {
				selectedEntityIds_.clear();
				selectedEntityId_ = 0;
				hierarchySelectionAnchorId_ = 0;
			}
		}
		if (duplicateId != 0) {
			const std::vector<uint64_t> duplicateRoots =
				isEntitySelected(duplicateId)
					? getSelectedRoots()
					: std::vector<uint64_t>{ duplicateId };
			std::vector<uint64_t> duplicates;
			for (uint64_t entityId : duplicateRoots) {
				if (const SceneEntity* entity = document.FindEntity(entityId)) {
					if (!entity->locked) {
						const uint64_t duplicateId = document.DuplicateEntity(entityId);
						if (duplicateId != 0) {
							duplicates.push_back(duplicateId);
						}
					}
				}
			}
			if (!duplicates.empty()) {
				selectedEntityIds_.clear();
				selectedEntityIds_.insert(duplicates.begin(), duplicates.end());
				selectedEntityId_ = duplicates.back();
				hierarchySelectionAnchorId_ = selectedEntityId_;
				hierarchyRevealRequested_ = true;
			}
		}
		if (createRequested) {
			SceneEntity& entity = document.CreateEntity("Entity", createParentId);
			selectedEntityId_ = entity.id;
			selectedEntityIds_ = { entity.id };
			hierarchySelectionAnchorId_ = entity.id;
			hierarchyRevealRequested_ = true;
			selectedProjectFile_.clear();
		}
		if (createFolderRequested) {
			SceneEntity& folder = document.CreateEntity("Folder", createParentId);
			folder.folder = true;
			selectedEntityId_ = folder.id;
			selectedEntityIds_ = { folder.id };
			hierarchySelectionAnchorId_ = folder.id;
			hierarchyRevealRequested_ = true;
			selectedProjectFile_.clear();
		}
		if (createCameraPathRequested) {
			std::string pathName = "CameraPath";
			const std::string baseName = pathName;
			uint32_t suffix = 2;
			while (document.FindEntityByName(pathName)) {
				pathName = baseName + " " + std::to_string(suffix++);
			}
			SceneEntity& entity = document.CreateEntity(pathName, createParentId);
			const uint64_t pathEntityId = entity.id;
			document.AddComponent(pathEntityId, "CameraPath");
			selectedEntityId_ = pathEntityId;
			selectedEntityIds_ = { pathEntityId };
			hierarchySelectionAnchorId_ = pathEntityId;
			hierarchyRevealRequested_ = true;
			selectedProjectFile_.clear();
		}
		if (!hierarchyPrefabDropPath.empty()) {
			InstantiatePrefabInEditScene(
				hierarchyPrefabDropPath,
				hierarchyPrefabDropParentId
			);
		}
		hierarchyObservedEntityId_ = selectedEntityId_;
		ImGui::End();
		return;
	}

	const char* rootName = sceneName && sceneName[0] != '\0'
		? sceneName
		: SelectEditorText(editorLanguage_, "Scene", "Scene");
	const std::string rootLabel = std::string(rootName) +
		"###DefaultHierarchySceneRoot";

	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	if (ImGui::TreeNode(rootLabel.c_str())) {
		const char* items[] = {
			SelectEditorText(editorLanguage_, "Main Camera", "Main Camera"),
			SelectEditorText(editorLanguage_, "Environment", "Environment"),
			SelectEditorText(editorLanguage_, "Scene Objects", "Scene Objects"),
			SelectEditorText(editorLanguage_, "Lights", "Lights"),
			SelectEditorText(editorLanguage_, "Effects", "Effects")
		};
		for (int index = 0; index < IM_ARRAYSIZE(items); ++index) {
			const std::string itemLabel = std::string(items[index]) +
				"###DefaultHierarchyItem" + std::to_string(index);
			if (ImGui::Selectable(
				itemLabel.c_str(),
				selectedHierarchyItem_ == index
			)) {
				selectedHierarchyItem_ = index;
			}
		}
		ImGui::TreePop();
	}
	ImGui::End();
}

void ImGuiManager::ResetComponentPicker(ComponentPickerState& state) {
	state.document = nullptr;
	state.documentKey.clear();
	state.entityId = 0;
	state.searchBuffer[0] = '\0';
	state.category = -1;
	state.selectedTagMask = 0;
	state.tagMatchMode = ComponentTagMatchMode::All;
	state.favoritesOnly = false;
	state.items.clear();
	state.error.clear();
}

void ImGuiManager::QueueProjectLauncherRequest(const ProjectLauncherRequest& request) {
	if (projectLauncherRequestPending_ || request.operation == ProjectLauncherRequestOperation::None) {
		return;
	}
	projectLauncherRequest_ = request;
	projectLauncherRequestPending_ = true;
}

void ImGuiManager::DrawProjectLauncherWindow() {
	if (!showProjectLauncher_) {
		return;
	}
	if (!ImGui::Begin("Project Manager", &showProjectLauncher_)) {
		ImGui::End();
		return;
	}

	ImGui::TextDisabled("Local registry only. Project files are changed by queued requests on the next frame.");
	ImGui::BeginDisabled(projectLauncherRequestPending_);
	if (ImGui::Button("Refresh###ProjectLauncherRefresh")) {
		QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::Refresh });
	}
	ImGui::EndDisabled();
	if (!projectLauncherView_.statusMessage.empty()) {
		ImGui::TextWrapped("%s", projectLauncherView_.statusMessage.c_str());
	}
	ImGui::SeparatorText("New Project");
	ImGui::InputText("Project ID###ProjectLauncherProjectId", projectLauncherProjectIdBuffer_, sizeof(projectLauncherProjectIdBuffer_));
	ImGui::InputText("Display Name###ProjectLauncherDisplayName", projectLauncherDisplayNameBuffer_, sizeof(projectLauncherDisplayNameBuffer_));
	ImGui::InputText("Destination Root###ProjectLauncherDestinationRoot", projectLauncherDestinationRootBuffer_, sizeof(projectLauncherDestinationRootBuffer_));
	ImGui::InputText("Start Scene ID###ProjectLauncherStartSceneId", projectLauncherStartSceneIdBuffer_, sizeof(projectLauncherStartSceneIdBuffer_));
	ImGui::TextDisabled("Template Source: %s", projectLauncherView_.templateSourceRoot.empty() ? "Not configured" : projectLauncherView_.templateSourceRoot.c_str());
	if (!projectLauncherView_.visualStudioInstances.empty()) {
		const char* selected = projectLauncherVisualStudioIndex_ >= 0
			? projectLauncherView_.visualStudioInstances[projectLauncherVisualStudioIndex_].displayName.c_str()
			: "No selection";
		if (ImGui::BeginCombo("Visual Studio###ProjectLauncherVisualStudio", selected)) {
			for (size_t index = 0; index < projectLauncherView_.visualStudioInstances.size(); ++index) {
				const ProjectLauncherVisualStudioView& instance = projectLauncherView_.visualStudioInstances[index];
				const bool isSelected = projectLauncherVisualStudioIndex_ == static_cast<int>(index);
				if (ImGui::Selectable(instance.displayName.c_str(), isSelected)) {
					projectLauncherVisualStudioIndex_ = static_cast<int>(index);
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
	}
	const bool canCreate =
		!projectLauncherView_.creationInProgress &&
		projectLauncherProjectIdBuffer_[0] != '\0' &&
		projectLauncherDisplayNameBuffer_[0] != '\0' &&
		projectLauncherDestinationRootBuffer_[0] != '\0' &&
		projectLauncherStartSceneIdBuffer_[0] != '\0' &&
		!projectLauncherView_.templateSourceRoot.empty();
	ImGui::Checkbox("Open Solution after creation###ProjectLauncherOpenSolutionAfterCreate", &projectLauncherOpenSolutionAfterCreate_);
	ImGui::BeginDisabled(!canCreate || projectLauncherRequestPending_);
	if (ImGui::Button("Create...###ProjectLauncherCreate")) {
		projectLauncherCreateConfirmationOpen_ = true;
	}
	ImGui::EndDisabled();
	if (projectLauncherCreateConfirmationOpen_) {
		ImGui::OpenPopup("Create Project?###ProjectLauncherCreateConfirmation");
		projectLauncherCreateConfirmationOpen_ = false;
	}
	if (ImGui::BeginPopupModal("Create Project?###ProjectLauncherCreateConfirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Output: %s", projectLauncherDestinationRootBuffer_);
		ImGui::Text("Project ID: %s", projectLauncherProjectIdBuffer_);
		ImGui::Text("Template Source: %s", projectLauncherView_.templateSourceRoot.c_str());
		ImGui::TextDisabled("Git initialization and dependency restore are not part of this action.");
		if (ImGui::Button(projectLauncherOpenSolutionAfterCreate_ ? "Create and Open Solution###ConfirmProjectLauncherCreate" : "Create only###ConfirmProjectLauncherCreate")) {
			ProjectLauncherRequest request{};
			request.operation = ProjectLauncherRequestOperation::Create;
			request.projectId = projectLauncherProjectIdBuffer_;
			request.displayName = projectLauncherDisplayNameBuffer_;
			request.destinationRoot = projectLauncherDestinationRootBuffer_;
			request.startSceneId = projectLauncherStartSceneIdBuffer_;
			if (projectLauncherVisualStudioIndex_ >= 0) {
				request.visualStudioInstanceId = projectLauncherView_.visualStudioInstances[projectLauncherVisualStudioIndex_].instanceId;
			}
			request.openSolutionAfterCreate = projectLauncherOpenSolutionAfterCreate_;
			QueueProjectLauncherRequest(request);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel###CancelProjectLauncherCreate")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::SeparatorText("Import Existing Project");
	ImGui::InputText("Descriptor Path###ProjectLauncherImportDescriptor", projectLauncherImportDescriptorBuffer_, sizeof(projectLauncherImportDescriptorBuffer_));
	ImGui::BeginDisabled(projectLauncherImportDescriptorBuffer_[0] == '\0' || projectLauncherRequestPending_);
	if (ImGui::Button("Register Descriptor###ProjectLauncherImport")) {
		ProjectLauncherRequest request{};
		request.operation = ProjectLauncherRequestOperation::Import;
		request.descriptorPath = projectLauncherImportDescriptorBuffer_;
		QueueProjectLauncherRequest(request);
	}
	ImGui::EndDisabled();
	ImGui::TextDisabled("Legacy Project descriptor creation is intentionally not automatic.");

	ImGui::SeparatorText("Projects");
	if (projectLauncherView_.projects.empty()) {
		ImGui::TextDisabled("No registered Projects.");
	}
	for (const ProjectLauncherProjectView& project : projectLauncherView_.projects) {
		ImGui::PushID(project.descriptorPath.c_str());
		ImGui::Text("%s (%s)", project.displayName.c_str(), project.projectId.c_str());
		ImGui::TextDisabled("%s", project.projectRoot.c_str());
		ImGui::TextWrapped("%s%s%s", project.status.c_str(), project.detail.empty() ? "" : ": ", project.detail.c_str());
		if (!project.generationStatus.empty()) {
			ImGui::TextWrapped("Generation: %s%s%s", project.generationStatus.c_str(), project.generationDetail.empty() ? "" : ": ", project.generationDetail.c_str());
		}
		ImGui::BeginDisabled(!project.canGenerateSolutionPreview || projectLauncherRequestPending_);
		if (ImGui::Button("Generate Preview")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::GenerateSolutionPreview, project.descriptorPath });
		}
		ImGui::EndDisabled();
		if (!project.previewOperationId.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("Preview artifacts: %u", project.previewArtifactCount);
		}
		if (ImGui::Button(project.pinned ? "Unpin" : "Pin")) {
			ProjectLauncherRequest request{};
			request.operation = ProjectLauncherRequestOperation::SetPinned;
			request.descriptorPath = project.descriptorPath;
			request.pinned = !project.pinned;
			QueueProjectLauncherRequest(request);
		}
		ImGui::SameLine();
		if (ImGui::Button("Open Folder")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::OpenFolder, project.descriptorPath });
		}
		ImGui::SameLine();
		const std::string selectedInstanceId = projectLauncherVisualStudioIndex_ >= 0
			? projectLauncherView_.visualStudioInstances[projectLauncherVisualStudioIndex_].instanceId : std::string{};
		ImGui::BeginDisabled(!project.canOpenSolution || projectLauncherRequestPending_);
		if (ImGui::Button("Open Solution")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::OpenSolution, project.descriptorPath, {}, {}, {}, {}, {}, selectedInstanceId });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!project.canOpenSolutionPreview || projectLauncherRequestPending_);
		if (ImGui::Button("Open Preview Solution")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::OpenSolutionPreview, project.descriptorPath, project.previewOperationId, {}, {}, {}, {}, selectedInstanceId });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!project.canAdoptSolutionPreview || projectLauncherRequestPending_);
		if (ImGui::Button("Adopt Verified Preview...")) {
			projectLauncherGenerationConfirmationRequest_ = {};
			projectLauncherGenerationConfirmationRequest_.operation = ProjectLauncherRequestOperation::AdoptSolutionPreview;
			projectLauncherGenerationConfirmationRequest_.descriptorPath = project.descriptorPath;
			projectLauncherGenerationConfirmationRequest_.operationId = project.previewOperationId;
			projectLauncherGenerationConfirmationRequest_.projectId = project.projectId;
			projectLauncherGenerationConfirmationDetail_ = "Artifacts: " + std::to_string(project.previewArtifactCount) + "\nCanonical root: " + project.projectRoot;
			projectLauncherGenerationConfirmationVerified_ = false;
			projectLauncherGenerationConfirmationOpen_ = true;
		}
		ImGui::EndDisabled();
		if (project.layoutMigrationRequired) {
			ImGui::SameLine();
			ImGui::BeginDisabled(!project.canAdoptGroupedSolutionLayout || projectLauncherRequestPending_);
			if (ImGui::Button("Adopt Grouped Layout...")) {
				projectLauncherGenerationConfirmationRequest_ = {};
				projectLauncherGenerationConfirmationRequest_.operation = ProjectLauncherRequestOperation::AdoptGroupedSolutionLayout;
				projectLauncherGenerationConfirmationRequest_.descriptorPath = project.descriptorPath;
				projectLauncherGenerationConfirmationRequest_.operationId = project.previewOperationId;
				projectLauncherGenerationConfirmationRequest_.projectId = project.projectId;
				projectLauncherGenerationConfirmationDetail_ =
					"New artifacts: " + std::to_string(project.previewArtifactCount) +
					"\nRetired artifacts: " + std::to_string(project.retiredArtifactCount) +
					"\nModified owned artifacts: " + std::to_string(project.modifiedOwnedArtifactCount) +
					"\nDescriptor: " + project.descriptorPath +
					"\nOld directory: " + project.legacyArtifactDirectory +
					"\nNew directory: " + project.groupedArtifactDirectory;
				projectLauncherGenerationConfirmationModifiedOwnedArtifactCount_ = project.modifiedOwnedArtifactCount;
				projectLauncherGenerationConfirmationVerified_ = false;
				projectLauncherGenerationConfirmationOpen_ = true;
			}
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!project.canOpenEditor || projectLauncherRequestPending_);
		if (ImGui::Button("Open Editor")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::OpenEditor, project.descriptorPath });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!project.canSwitchEditor || projectLauncherRequestPending_);
		if (ImGui::Button("Switch Editor")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::SwitchEditor, project.descriptorPath });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Remove from List")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::Remove, project.descriptorPath });
		}
		ImGui::Separator();
		ImGui::PopID();
	}
	if (!projectLauncherView_.generationRecoveries.empty()) {
		ImGui::SeparatorText("Solution Generation Recovery");
		for (const ProjectLauncherGenerationRecoveryView& recovery : projectLauncherView_.generationRecoveries) {
			ImGui::PushID(recovery.operationId.c_str());
			ImGui::Text("%s: %s", recovery.projectId.c_str(), recovery.state.c_str());
			ImGui::TextDisabled("Staging: %s", recovery.stagingRoot.c_str());
			ImGui::TextDisabled("Rollback: %s", recovery.rollbackRoot.c_str());
			ImGui::TextDisabled("Artifacts: %u", recovery.fileCount);
			ImGui::TextWrapped("%s", recovery.detail.c_str());
			ImGui::BeginDisabled(!recovery.canCommitStaged || projectLauncherRequestPending_);
			if (ImGui::Button("Commit Staged...")) {
				projectLauncherGenerationConfirmationRequest_ = {};
				projectLauncherGenerationConfirmationRequest_.operation = ProjectLauncherRequestOperation::CommitStagedSolutionGeneration;
				projectLauncherGenerationConfirmationRequest_.operationId = recovery.operationId;
				projectLauncherGenerationConfirmationRequest_.projectId = recovery.projectId;
				projectLauncherGenerationConfirmationDetail_ = "Artifacts: " + std::to_string(recovery.fileCount) + "\nStaging: " + recovery.stagingRoot;
				projectLauncherGenerationConfirmationVerified_ = true;
				projectLauncherGenerationConfirmationOpen_ = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!recovery.canResumeCommit || projectLauncherRequestPending_);
			if (ImGui::Button("Resume Commit...")) {
				projectLauncherGenerationConfirmationRequest_ = {};
				projectLauncherGenerationConfirmationRequest_.operation = ProjectLauncherRequestOperation::ResumeSolutionGenerationCommit;
				projectLauncherGenerationConfirmationRequest_.operationId = recovery.operationId;
				projectLauncherGenerationConfirmationRequest_.projectId = recovery.projectId;
				projectLauncherGenerationConfirmationDetail_ = "Artifacts: " + std::to_string(recovery.fileCount) + "\nStaging: " + recovery.stagingRoot + "\nRollback: " + recovery.rollbackRoot;
				projectLauncherGenerationConfirmationVerified_ = true;
				projectLauncherGenerationConfirmationOpen_ = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!recovery.canRecheck || projectLauncherRequestPending_);
			if (ImGui::Button("Recheck Recovery")) {
				QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::RecheckSolutionGenerationRecovery, {}, recovery.operationId });
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!recovery.canRestorePrevious || projectLauncherRequestPending_);
			if (ImGui::Button("Restore Previous...")) {
				projectLauncherRestoreGenerationOperationId_ = recovery.operationId;
				projectLauncherGenerationConfirmationDetail_ = "Project: " + recovery.projectId + "\nArtifacts: " + std::to_string(recovery.fileCount) + "\nRollback: " + recovery.rollbackRoot;
				projectLauncherRestoreGenerationConfirmationOpen_ = true;
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
	}
	if (projectLauncherGenerationConfirmationOpen_) {
		ImGui::OpenPopup("Confirm Solution Generation Operation###ProjectLauncherGenerationConfirmation");
		projectLauncherGenerationConfirmationOpen_ = false;
	}
	if (ImGui::BeginPopupModal("Confirm Solution Generation Operation###ProjectLauncherGenerationConfirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		const ProjectLauncherRequestOperation operation = projectLauncherGenerationConfirmationRequest_.operation;
		const bool adoption = operation == ProjectLauncherRequestOperation::AdoptSolutionPreview;
		const bool groupedLayoutAdoption = operation == ProjectLauncherRequestOperation::AdoptGroupedSolutionLayout;
		const bool groupedLayoutDrift = groupedLayoutAdoption && projectLauncherGenerationConfirmationModifiedOwnedArtifactCount_ != 0;
		const char* action = adoption ? "Adopt Verified Preview" : groupedLayoutAdoption ? "Adopt Grouped Layout" : operation == ProjectLauncherRequestOperation::CommitStagedSolutionGeneration ? "Commit Staged Generation" : "Resume Generation Commit";
		ImGui::TextWrapped("%s for Project %s.", action, projectLauncherGenerationConfirmationRequest_.projectId.c_str());
		ImGui::TextDisabled("Operation: %s", projectLauncherGenerationConfirmationRequest_.operationId.c_str());
		ImGui::TextWrapped("%s", projectLauncherGenerationConfirmationDetail_.c_str());
		if (groupedLayoutAdoption) {
			ImGui::TextWrapped("This changes tracked generated files and the descriptor path. It does not change legacy CG2, Git, or Build output.");
			if (groupedLayoutDrift) {
				ImGui::TextWrapped("Current modified generated files are journaled as the previous set for transaction rollback. Their edits are not merged into Grouped output.");
				ImGui::Checkbox("I verified this Preview and accept replacing modified generated files", &projectLauncherGenerationConfirmationVerified_);
			} else {
				ImGui::Checkbox("I verified this Preview", &projectLauncherGenerationConfirmationVerified_);
			}
		} else if (adoption) {
			ImGui::TextWrapped("This changes tracked generated files only. It does not run Git.");
			ImGui::Checkbox("I verified this Preview", &projectLauncherGenerationConfirmationVerified_);
		} else if (operation == ProjectLauncherRequestOperation::ResumeSolutionGenerationCommit) {
			ImGui::TextWrapped("Only hash-proven incomplete artifacts continue toward the new generation.");
		} else {
			ImGui::TextWrapped("This continues the explicitly staged generation operation.");
		}
		ImGui::BeginDisabled(!projectLauncherGenerationConfirmationVerified_ || projectLauncherRequestPending_);
		if (ImGui::Button("Confirm###ConfirmProjectLauncherGeneration")) {
			QueueProjectLauncherRequest(projectLauncherGenerationConfirmationRequest_);
			projectLauncherGenerationConfirmationRequest_ = {};
			projectLauncherGenerationConfirmationDetail_.clear();
			projectLauncherGenerationConfirmationModifiedOwnedArtifactCount_ = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel###CancelProjectLauncherGeneration")) {
			projectLauncherGenerationConfirmationRequest_ = {};
			projectLauncherGenerationConfirmationDetail_.clear();
			projectLauncherGenerationConfirmationModifiedOwnedArtifactCount_ = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (projectLauncherRestoreGenerationConfirmationOpen_) {
		ImGui::OpenPopup("Restore Previous Generation?###ProjectLauncherRestoreGenerationConfirmation");
		projectLauncherRestoreGenerationConfirmationOpen_ = false;
	}
	if (ImGui::BeginPopupModal("Restore Previous Generation?###ProjectLauncherRestoreGenerationConfirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("This restores only journal-owned generated files for operation %s.", projectLauncherRestoreGenerationOperationId_.c_str());
		ImGui::TextWrapped("%s", projectLauncherGenerationConfirmationDetail_.c_str());
		if (ImGui::Button("Restore Previous Generation###ConfirmProjectLauncherRestoreGeneration")) {
			QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::RestorePreviousSolutionGeneration, {}, projectLauncherRestoreGenerationOperationId_ });
			projectLauncherRestoreGenerationOperationId_.clear();
			projectLauncherGenerationConfirmationDetail_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel###CancelProjectLauncherRestoreGeneration")) {
			projectLauncherRestoreGenerationOperationId_.clear();
			projectLauncherGenerationConfirmationDetail_.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (!projectLauncherView_.operations.empty()) {
		ImGui::SeparatorText("Creation Recovery");
		for (const ProjectLauncherOperationView& operation : projectLauncherView_.operations) {
			ImGui::PushID(operation.operationId.c_str());
			ImGui::Text("%s: %s", operation.projectId.c_str(), operation.state.c_str());
			ImGui::TextWrapped("%s", operation.detail.c_str());
			ImGui::BeginDisabled(!operation.canRetryFinalize || projectLauncherRequestPending_);
			if (ImGui::Button("Retry Descriptor Finalize")) {
				QueueProjectLauncherRequest({ ProjectLauncherRequestOperation::RetryFinalize, {}, operation.operationId });
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
	}
	if (projectLauncherView_.switchBlockedBySceneDirty || projectLauncherView_.switchBlockedByPrefabDirty || projectLauncherView_.switchBlockedByPlayMode) {
		ImGui::Separator();
		ImGui::TextDisabled("Switch Editor is unavailable:");
		if (projectLauncherView_.switchBlockedBySceneDirty) ImGui::BulletText("Scene has unsaved changes.");
		if (projectLauncherView_.switchBlockedByPrefabDirty) ImGui::BulletText("Prefab has unsaved changes.");
		if (projectLauncherView_.switchBlockedByPlayMode) ImGui::BulletText("Stop Play or Pause mode first.");
	}
	ImGui::End();
}

void ImGuiManager::StopAudioPreview() {
	if (Audio* audio = Audio::GetInstance()) {
		audio->SoundStop(previewAudioPlayback_);
	} else {
		previewAudioPlayback_ = {};
	}
	previewAudioClip_.reset();
	previewAudioError_.clear();
}

void ImGuiManager::OpenComponentPicker(
	ComponentPickerState& state,
	ComponentPickerTarget target,
	SceneDocument& document,
	const std::string& documentKey,
	uint64_t entityId
) {
	ResetComponentPicker(state);
	state.document = &document;
	state.documentKey = documentKey;
	state.entityId = entityId;
	const std::string popupLabel = std::string(SelectEditorText(
		editorLanguage_,
		"Componentを追加",
		"Add Components"
	)) + (target == ComponentPickerTarget::Scene
		? "###SceneComponentPicker"
		: "###PrefabComponentPicker");
	ImGui::OpenPopup(popupLabel.c_str());
}

bool ImGuiManager::IsComponentPickerItemQueued(
	const ComponentPickerState& state,
	const std::string& type
) const {
	return std::any_of(
		state.items.begin(),
		state.items.end(),
		[&type](const ComponentPickerItem& item) {
			return item.type == type;
		}
	);
}

void ImGuiManager::DrawComponentSummary(
	const SceneEntity& entity,
	const std::string& documentKey,
	bool prefabAnimationFocusOnly,
	std::string& selectedType
) {
	std::vector<const SceneComponent*> visibleComponents;
	for (const SceneComponent& component : entity.components) {
		if (
			prefabAnimationFocusOnly &&
			component.type != "AttackSet" &&
			component.type != "PrefabAnimator"
		) {
			continue;
		}
		visibleComponents.push_back(&component);
	}

	const bool selectionExists = std::any_of(
		visibleComponents.begin(),
		visibleComponents.end(),
		[&selectedType](const SceneComponent* component) {
			return component->type == selectedType;
		}
	);
	if (!selectionExists) {
		selectedType = visibleComponents.empty()
			? std::string{}
			: visibleComponents.front()->type;
	}

	ImGui::SeparatorText(SelectEditorText(
		editorLanguage_,
		"Component概要",
		"Component Summary"
	));
	ImGui::PushID("ComponentSummary");
	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"表示:",
		"View:"
	));
	ImGui::SameLine();
	bool settingsChanged = false;
	if (ImGui::RadioButton(
		SelectEditorText(
			editorLanguage_,
			"簡易###ComponentInspectorSimple",
			"Simple###ComponentInspectorSimple"
		),
		componentInspectorMode_ == ComponentInspectorMode::Simple
	)) {
		componentInspectorMode_ = ComponentInspectorMode::Simple;
		settingsChanged = true;
	}
	ImGui::SameLine();
	if (ImGui::RadioButton(
		SelectEditorText(
			editorLanguage_,
			"詳細###ComponentInspectorDetailed",
			"Detailed###ComponentInspectorDetailed"
		),
		componentInspectorMode_ == ComponentInspectorMode::Detailed
	)) {
		componentInspectorMode_ = ComponentInspectorMode::Detailed;
		settingsChanged = true;
	}

	if (visibleComponents.empty()) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"Componentはありません。",
			"No components."
		));
	} else {
		const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
		const float listHeight = std::clamp(
			rowHeight * static_cast<float>(visibleComponents.size()) + 8.0f,
			48.0f,
			180.0f
		);
		ImGui::BeginChild(
			"ComponentSummaryList",
			ImVec2(0.0f, listHeight),
			true
		);
		for (size_t index = 0; index < visibleComponents.size(); ++index) {
			const SceneComponent& component = *visibleComponents[index];
			const EditorComponentDefinition* definition =
				FindEditorComponentDefinition(component.type);
			const char* displayName = definition
				? GetEditorComponentDisplayName(*definition, editorLanguage_)
				: component.type.c_str();
			const char* enabledState = component.enabled
				? SelectEditorText(editorLanguage_, "有効", "Enabled")
				: SelectEditorText(editorLanguage_, "無効", "Disabled");
			const std::string rowLabel = "[" + std::string(enabledState) + "] " +
				displayName + "\n" + component.type +
				"###ComponentSummaryRow";
			ImGui::PushID(static_cast<int>(index));
			if (ImGui::Selectable(
				rowLabel.c_str(),
				selectedType == component.type,
				ImGuiSelectableFlags_None,
				ImVec2(0.0f, rowHeight)
			)) {
				selectedType = component.type;
				const std::string foldoutKey = MakeComponentFoldoutKey(
					documentKey,
					entity.id,
					component.type
				);
				const auto foldout = componentFoldoutStates_.find(foldoutKey);
				if (
					foldout != componentFoldoutStates_.end() &&
					!foldout->second
				) {
					foldout->second = true;
					settingsChanged = true;
				}
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	if (settingsChanged) {
		SaveEditorSettings();
	}
	ImGui::PopID();
}

void ImGuiManager::RemoveComponentPickerItem(
	ComponentPickerState& state,
	const std::string& type
) {
	std::unordered_set<std::string> removeTypes{ type };
	bool changed = true;
	while (changed) {
		changed = false;
		for (const ComponentPickerItem& item : state.items) {
			if (
				!item.requiredByType.empty() &&
				removeTypes.contains(item.requiredByType)
			) {
				changed |= removeTypes.insert(item.type).second;
			}
			if (
				removeTypes.contains(item.type) &&
				!item.requiredByType.empty()
			) {
				changed |= removeTypes.insert(item.requiredByType).second;
			}
			const EditorComponentDefinition* definition =
				FindEditorComponentDefinition(item.type);
			if (
				definition && definition->requiredType[0] != '\0' &&
				removeTypes.contains(definition->requiredType)
			) {
				changed |= removeTypes.insert(item.type).second;
			}
		}
	}
	state.items.erase(
		std::remove_if(
			state.items.begin(),
			state.items.end(),
			[&removeTypes](const ComponentPickerItem& item) {
				return removeTypes.contains(item.type);
			}
		),
		state.items.end()
	);
}

void ImGuiManager::ToggleComponentPickerItem(
	ComponentPickerState& state,
	ComponentPickerTarget target,
	const SceneEntity& entity,
	const std::string& type
) {
	if (IsComponentPickerItemQueued(state, type)) {
		RemoveComponentPickerItem(state, type);
		return;
	}
	const EditorComponentContext context = target == ComponentPickerTarget::Scene
		? EditorComponentContext::Scene
		: EditorComponentContext::Prefab;
	const EditorComponentDefinition* definition =
		FindEditorComponentDefinition(type);
	if (
		!definition ||
		!SupportsEditorComponentContext(*definition, context) ||
		HasComponent(entity, type.c_str())
	) {
		return;
	}
	if (
		definition->requiredType[0] != '\0' &&
		!HasComponent(entity, definition->requiredType) &&
		!IsComponentPickerItemQueued(state, definition->requiredType)
	) {
		const EditorComponentDefinition* requiredDefinition =
			FindEditorComponentDefinition(definition->requiredType);
		if (
			requiredDefinition &&
			SupportsEditorComponentContext(*requiredDefinition, context)
		) {
			state.items.push_back({
				requiredDefinition->type,
				definition->type
			});
		}
	}
	state.items.push_back({ definition->type, "" });
	state.error.clear();
}

void ImGuiManager::DrawComponentPicker(
	ComponentPickerState& state,
	ComponentPickerTarget target
) {
	if (state.entityId == 0) {
		return;
	}
	const bool sceneTarget = target == ComponentPickerTarget::Scene;
	const EditorComponentContext context = sceneTarget
		? EditorComponentContext::Scene
		: EditorComponentContext::Prefab;
	const std::string popupLabel = std::string(SelectEditorText(
		editorLanguage_,
		"Componentを追加",
		"Add Components"
	)) + (sceneTarget
		? "###SceneComponentPicker"
		: "###PrefabComponentPicker");
	ImGui::SetNextWindowSize(ImVec2(820.0f, 650.0f), ImGuiCond_FirstUseEver);
	bool keepOpen = true;
	if (!ImGui::BeginPopupModal(
		popupLabel.c_str(),
		&keepOpen,
		ImGuiWindowFlags_NoSavedSettings
	)) {
		if (!keepOpen || !ImGui::IsPopupOpen(popupLabel.c_str())) {
			ResetComponentPicker(state);
		}
		return;
	}

	bool closePicker = false;
	const uint64_t selectedEntityId = sceneTarget
		? selectedEntityId_
		: prefabSelectedEntityId_;
	if (selectedEntityId != state.entityId) {
		closePicker = true;
	}
	if (
		sceneTarget &&
		(!selectedProjectFile_.empty() || selectedEntityIds_.size() > 1)
	) {
		closePicker = true;
	}
	SceneDocument* document = nullptr;
	SceneEntity* targetEntity = nullptr;
	std::string documentKey;
	const bool contextAvailable = sceneTarget
		? editorSession_ && editorSession_->IsEditing()
		: prefabEditorSession_ && prefabEditorSession_->IsOpen();
	if (contextAvailable && !closePicker) {
		if (sceneTarget) {
			document = &editorSession_->GetEditDocument();
			documentKey = editorSession_->GetEditSceneId();
		} else {
			document = &prefabEditorSession_->GetDocument();
			documentKey = prefabEditorSession_->GetFilePath();
		}
		if (document == state.document && documentKey == state.documentKey) {
			targetEntity = document->FindEntity(state.entityId);
		} else {
			closePicker = true;
		}
	}
	if (!contextAvailable) {
		if (sceneTarget) {
			state.error = SelectEditorText(
				editorLanguage_,
				"Play中はComponentを追加できません。",
				"Components cannot be added during Play mode."
			);
		} else {
			closePicker = true;
		}
	} else if (
		document == state.document && documentKey == state.documentKey &&
		!targetEntity
	) {
		state.error = SelectEditorText(
			editorLanguage_,
			"対象Entityが見つかりません。",
			"The target entity no longer exists."
		);
	} else if (sceneTarget && targetEntity && targetEntity->locked) {
		state.error = SelectEditorText(
			editorLanguage_,
			"対象Entityはロックされています。",
			"The target entity is locked."
		);
	} else if (targetEntity && targetEntity->folder) {
		state.error = SelectEditorText(
			editorLanguage_,
			"FolderへComponentは追加できません。",
			"Components cannot be added to a folder."
		);
	}
	constexpr EditorComponentCategory categories[] = {
		EditorComponentCategory::Rendering,
		EditorComponentCategory::World,
		EditorComponentCategory::Camera,
		EditorComponentCategory::Physics,
		EditorComponentCategory::Gameplay,
		EditorComponentCategory::Animation,
		EditorComponentCategory::EventAndFlow
	};
	const auto matchesFilters = [
		this,
		&state,
		&categories
	](
		const EditorComponentDefinition& definition,
		uint16_t selectedTagMask
	) {
		if (
			state.category >= 0 &&
			definition.category != categories[state.category]
		) {
			return false;
		}
		if (!MatchesEditorComponentSearch(
			definition,
			state.searchBuffer
		)) {
			return false;
		}
		if (
			state.favoritesOnly &&
			!favoriteComponentTypes_.contains(definition.type)
		) {
			return false;
		}
		if (selectedTagMask == 0) {
			return true;
		}
		if (state.tagMatchMode == ComponentTagMatchMode::Any) {
			return (definition.tagMask & selectedTagMask) != 0;
		}
		return (definition.tagMask & selectedTagMask) == selectedTagMask;
	};
	const auto countSelectedTags = [](uint16_t selectedTagMask) {
		int count = 0;
		for (EditorComponentTag tag : GetEditorComponentTags()) {
			if ((selectedTagMask & EditorComponentTagBit(tag)) != 0) {
				++count;
			}
		}
		return count;
	};
	int selectedTagCount = countSelectedTags(state.selectedTagMask);

	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"文字検索、カテゴリ、タグから選び、下の追加予定へまとめます。",
		"Choose by text, category, or tag, then review the pending components below."
	));
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint(
		"##ComponentPickerSearch",
		SelectEditorText(
			editorLanguage_,
			"名前・Type ID・説明を検索...",
			"Search names, type IDs, or descriptions..."
		),
		state.searchBuffer,
		sizeof(state.searchBuffer)
	);
	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"使用中のタグ",
		"Active tags"
	));
	const float tagRowRight = ImGui::GetCursorScreenPos().x +
		ImGui::GetContentRegionAvail().x;
	bool hasTagRowItem = false;
	const auto placeTagRowItem = [
		&hasTagRowItem,
		tagRowRight
	](float itemWidth) {
		if (
			hasTagRowItem &&
			ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
				itemWidth <= tagRowRight
		) {
			ImGui::SameLine();
		}
		hasTagRowItem = true;
	};
	uint16_t removeTagBit = 0;
	for (EditorComponentTag tag : GetEditorComponentTags()) {
		const uint16_t tagBit = EditorComponentTagBit(tag);
		if ((state.selectedTagMask & tagBit) == 0) {
			continue;
		}
		const std::string tagLabel = std::string(
			GetEditorComponentTagDisplayName(tag, editorLanguage_)
		) + "  ×##SelectedTagFilter" +
			GetEditorComponentTagId(tag);
		const float tagWidth = ImGui::CalcTextSize(
			tagLabel.c_str(),
			nullptr,
			true
		).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		placeTagRowItem(tagWidth);
		ImGui::PushStyleColor(
			ImGuiCol_Button,
			ImGui::GetStyleColorVec4(ImGuiCol_Header)
		);
		ImGui::PushStyleColor(
			ImGuiCol_ButtonHovered,
			ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered)
		);
		ImGui::PushStyleColor(
			ImGuiCol_ButtonActive,
			ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive)
		);
		if (ImGui::Button(tagLabel.c_str())) {
			removeTagBit = tagBit;
		}
		ImGui::PopStyleColor(3);
	}
	if (removeTagBit != 0) {
		state.selectedTagMask &= static_cast<uint16_t>(~removeTagBit);
		selectedTagCount = countSelectedTags(state.selectedTagMask);
	}
	if (!hasTagRowItem) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"タグ条件なし",
			"No tag filters"
		));
		hasTagRowItem = true;
	}
	const char* addTagLabel = SelectEditorText(
		editorLanguage_,
		"＋ タグを追加##OpenTagFilterPopup",
		"+ Add tag##OpenTagFilterPopup"
	);
	const float addTagWidth = ImGui::CalcTextSize(
		addTagLabel,
		nullptr,
		true
	).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	placeTagRowItem(addTagWidth);
	if (ImGui::Button(addTagLabel)) {
		ImGui::OpenPopup("ComponentTagFilterPopup");
	}
	if (selectedTagCount >= 2) {
		const char* clearTagsLabel = SelectEditorText(
			editorLanguage_,
			"すべて解除##ClearTagFilters",
			"Clear all##ClearTagFilters"
		);
		const float clearTagsWidth = ImGui::CalcTextSize(
			clearTagsLabel,
			nullptr,
			true
		).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		placeTagRowItem(clearTagsWidth);
		if (ImGui::Button(clearTagsLabel)) {
			state.selectedTagMask = 0;
		}
	}
	if (ImGui::BeginPopup("ComponentTagFilterPopup")) {
		ImGui::TextUnformatted(SelectEditorText(
			editorLanguage_,
			"タグを追加",
			"Add tags"
		));
		ImGui::Separator();
		bool hasAvailableTag = false;
		for (EditorComponentTag tag : GetEditorComponentTags()) {
			const uint16_t tagBit = EditorComponentTagBit(tag);
			if ((state.selectedTagMask & tagBit) != 0) {
				continue;
			}
			bool existsInContext = false;
			int matchingCount = 0;
			const uint16_t hypotheticalTagMask =
				state.selectedTagMask | tagBit;
			for (const EditorComponentDefinition* definition :
				GetEditorComponentDefinitions(context)) {
				if (HasEditorComponentTag(*definition, tag)) {
					existsInContext = true;
				}
				if (matchesFilters(*definition, hypotheticalTagMask)) {
					++matchingCount;
				}
			}
			if (!existsInContext) {
				continue;
			}
			hasAvailableTag = true;
			const std::string candidateLabel = std::string(
				GetEditorComponentTagDisplayName(tag, editorLanguage_)
			) + "  " + std::to_string(matchingCount) +
				(editorLanguage_ == EditorLanguage::Japanese
					? "件"
					: " results") +
				"##AddTagFilter" + GetEditorComponentTagId(tag);
			if (ImGui::Selectable(
				candidateLabel.c_str(),
				false,
				ImGuiSelectableFlags_NoAutoClosePopups
			)) {
				state.selectedTagMask |= tagBit;
			}
			if (
				matchingCount == 0 &&
				ImGui::IsItemHovered()
			) {
				ImGui::SetTooltip("%s", SelectEditorText(
					editorLanguage_,
					"追加できますが、現在の検索条件では結果が0件になります。",
					"You can add this tag, but it produces no results with the current filters."
				));
			}
		}
		if (!hasAvailableTag) {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"追加できるタグはありません。",
				"There are no more tags to add."
			));
		}
		ImGui::EndPopup();
	}
	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"一致条件:",
		"Match:"
	));
	ImGui::SameLine();
	if (ImGui::RadioButton(
		SelectEditorText(
			editorLanguage_,
			"すべて含む (AND)##TagMatchAll",
			"Match all (AND)##TagMatchAll"
		),
		state.tagMatchMode == ComponentTagMatchMode::All
	)) {
		state.tagMatchMode = ComponentTagMatchMode::All;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", SelectEditorText(
			editorLanguage_,
			"選択したタグをすべて持つComponentを表示します。",
			"Show components containing every selected tag."
		));
	}
	ImGui::SameLine();
	if (ImGui::RadioButton(
		SelectEditorText(
			editorLanguage_,
			"いずれかを含む (OR)##TagMatchAny",
			"Match any (OR)##TagMatchAny"
		),
		state.tagMatchMode == ComponentTagMatchMode::Any
	)) {
		state.tagMatchMode = ComponentTagMatchMode::Any;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", SelectEditorText(
			editorLanguage_,
			"選択したタグを1つ以上持つComponentを表示します。",
			"Show components containing at least one selected tag."
		));
	}
	ImGui::Checkbox(
		SelectEditorText(
			editorLanguage_,
			"お気に入りのみ##FavoriteComponentsOnly",
			"Favorites only##FavoriteComponentsOnly"
		),
		&state.favoritesOnly
	);
	ImGui::Separator();

	const float bodyHeight = (std::max)(
		250.0f,
		ImGui::GetContentRegionAvail().y - 190.0f
	);
	ImGui::BeginChild("ComponentPickerCategories", ImVec2(155.0f, bodyHeight), true);
	if (ImGui::Selectable(
		SelectEditorText(editorLanguage_, "すべて##CategoryAll", "All##CategoryAll"),
		state.category < 0
	)) {
		state.category = -1;
	}
	for (int index = 0; index < static_cast<int>(std::size(categories)); ++index) {
		const std::string label = std::string(
			GetEditorComponentCategoryDisplayName(
				categories[index],
				editorLanguage_
			)
		) + "##Category" + std::to_string(index);
		if (ImGui::Selectable(
			label.c_str(),
			state.category == index
		)) {
			state.category = index;
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("ComponentPickerCards", ImVec2(0.0f, bodyHeight), true);
	int visibleCount = 0;
	for (const EditorComponentDefinition* definition :
		GetEditorComponentDefinitions(context)) {
		if (!matchesFilters(*definition, state.selectedTagMask)) {
			continue;
		}
		++visibleCount;
		const bool installed = targetEntity &&
			HasComponent(*targetEntity, definition->type);
		const bool queued = IsComponentPickerItemQueued(
			state,
			definition->type
		);
		ImGui::PushID(definition->type);
		ImGui::BeginDisabled(installed || !targetEntity);
		const bool selected = ImGui::Selectable(
			"##ComponentCard",
			queued,
			ImGuiSelectableFlags_AllowOverlap,
			ImVec2(0.0f, 122.0f)
		);
		ImGui::EndDisabled();
		const ImVec2 cardMin = ImGui::GetItemRectMin();
		const ImVec2 cardMax = ImGui::GetItemRectMax();
		const ImVec2 cardEndCursor = ImGui::GetCursorScreenPos();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 textColor = ImGui::GetColorU32(
			installed ? ImGuiCol_TextDisabled : ImGuiCol_Text
		);
		const ImU32 detailColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		drawList->AddRect(
			cardMin,
			cardMax,
			ImGui::GetColorU32(ImGuiCol_Border),
			4.0f
		);
		drawList->AddText(
			ImVec2(cardMin.x + 10.0f, cardMin.y + 7.0f),
			textColor,
			GetEditorComponentDisplayName(*definition, editorLanguage_)
		);
		const char* status = installed
			? SelectEditorText(editorLanguage_, "追加済み", "Added")
			: queued
				? SelectEditorText(editorLanguage_, "追加予定", "Pending")
				: "";
		if (status[0] != '\0') {
			const ImVec2 statusSize = ImGui::CalcTextSize(status);
			drawList->AddText(
				ImVec2(cardMax.x - statusSize.x - 40.0f, cardMin.y + 7.0f),
				queued ? ImGui::GetColorU32(ImGuiCol_CheckMark) : detailColor,
				status
			);
		}
		drawList->AddText(
			ImVec2(cardMin.x + 10.0f, cardMin.y + 27.0f),
			detailColor,
			definition->type
		);
		drawList->AddText(
			ImGui::GetFont(),
			ImGui::GetFontSize(),
			ImVec2(cardMin.x + 10.0f, cardMin.y + 48.0f),
			textColor,
			GetEditorComponentDescription(*definition, editorLanguage_),
			nullptr,
			(std::max)(cardMax.x - cardMin.x - 20.0f, 1.0f)
		);
		std::string tagText;
		int displayedTagCount = 0;
		for (EditorComponentTag tag : GetEditorComponentTags()) {
			if (!HasEditorComponentTag(*definition, tag) || displayedTagCount >= 3) {
				continue;
			}
			if (!tagText.empty()) {
				tagText += "  ";
			}
			tagText += "[";
			tagText += GetEditorComponentTagDisplayName(tag, editorLanguage_);
			tagText += "]";
			++displayedTagCount;
		}
		drawList->AddText(
			ImVec2(cardMin.x + 10.0f, cardMin.y + 91.0f),
			ImGui::GetColorU32(ImGuiCol_CheckMark),
			tagText.c_str()
		);
		if (definition->requiredType[0] != '\0') {
			const std::string requiredText = std::string(SelectEditorText(
				editorLanguage_,
				"必要: ",
				"Requires: "
			)) + definition->requiredType;
			drawList->AddText(
				ImVec2(cardMin.x + 10.0f, cardMin.y + 106.0f),
				detailColor,
				requiredText.c_str()
			);
		}
		if (!installed && targetEntity && ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload(
				"EDITOR_COMPONENT_TYPE",
				definition->type,
				std::strlen(definition->type) + 1
			);
			ImGui::TextUnformatted(
				GetEditorComponentDisplayName(*definition, editorLanguage_)
			);
			ImGui::EndDragDropSource();
		}
		const bool favorite = favoriteComponentTypes_.contains(definition->type);
		ImGui::SetCursorScreenPos(ImVec2(cardMax.x - 31.0f, cardMin.y + 4.0f));
		if (favorite) {
			ImGui::PushStyleColor(
				ImGuiCol_Text,
				ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)
			);
		}
		const bool favoriteClicked = ImGui::SmallButton(
			favorite
				? "★##ComponentFavorite"
				: "☆##ComponentFavorite"
		);
		if (favorite) {
			ImGui::PopStyleColor();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", SelectEditorText(
				editorLanguage_,
				favorite
					? "お気に入りから解除"
					: "お気に入りに追加",
				favorite
					? "Remove from favorites"
					: "Add to favorites"
			));
		}
		if (favoriteClicked) {
			if (favorite) {
				favoriteComponentTypes_.erase(definition->type);
			} else {
				favoriteComponentTypes_.insert(definition->type);
			}
			SaveEditorSettings();
		}
		ImGui::SetCursorScreenPos(cardEndCursor);
		if (
			selected && !favoriteClicked && targetEntity &&
			!(queued && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		) {
			ToggleComponentPickerItem(
				state,
				target,
				*targetEntity,
				definition->type
			);
		}
		ImGui::PopID();
		ImGui::Spacing();
	}
	if (visibleCount == 0) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"一致するComponentはありません。",
			"No matching components."
		));
	}
	ImGui::EndChild();

	ImGui::SeparatorText(SelectEditorText(
		editorLanguage_,
		"追加予定",
		"Pending Components"
	));
	std::string removeType;
	ImGui::BeginChild("ComponentPickerTray", ImVec2(0.0f, 66.0f), true);
	if (state.items.empty()) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"カードをクリックするか、ここへドラッグしてください。",
			"Click a card or drag it here."
		));
	}
	for (const ComponentPickerItem& item : state.items) {
		const EditorComponentDefinition* definition =
			FindEditorComponentDefinition(item.type);
		if (!definition) {
			continue;
		}
		ImGui::PushID(item.type.c_str());
		const std::string itemLabel = std::string(
			GetEditorComponentDisplayName(*definition, editorLanguage_)
		) + "  x";
		if (ImGui::SmallButton(itemLabel.c_str())) {
			removeType = item.type;
		}
		if (!item.requiredByType.empty() && ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"%s: %s",
				SelectEditorText(editorLanguage_, "必要元", "Required by"),
				item.requiredByType.c_str()
			);
		}
		ImGui::SameLine();
		ImGui::PopID();
	}
	ImGui::NewLine();
	ImGui::EndChild();
	const ImVec2 trayMin = ImGui::GetItemRectMin();
	const ImVec2 trayMax = ImGui::GetItemRectMax();
	if (
		targetEntity &&
		ImGui::BeginDragDropTargetCustom(
			ImRect(trayMin, trayMax),
			ImGui::GetID("ComponentPickerTrayDropTarget")
		)
	) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
			"EDITOR_COMPONENT_TYPE"
		)) {
			const char* type = static_cast<const char*>(payload->Data);
			if (type && type[0] != '\0' &&
				!IsComponentPickerItemQueued(state, type)) {
				ToggleComponentPickerItem(state, target, *targetEntity, type);
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (!removeType.empty()) {
		RemoveComponentPickerItem(state, removeType);
	}

	if (!state.error.empty()) {
		ImGui::TextColored(
			ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
			"%s",
			state.error.c_str()
		);
	}
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"キャンセル##CancelComponentPicker",
		"Cancel##CancelComponentPicker"
	))) {
		closePicker = true;
	}
	ImGui::SameLine();
	const std::string confirmLabel = editorLanguage_ == EditorLanguage::Japanese
		? std::to_string(state.items.size()) +
			"件をまとめて追加##ConfirmComponents"
		: "Add " + std::to_string(state.items.size()) +
			" Components##ConfirmComponents";
	const bool canConfirm = targetEntity &&
		(!sceneTarget || !targetEntity->locked) &&
		!targetEntity->folder && contextAvailable && !state.items.empty();
	ImGui::BeginDisabled(!canConfirm);
	if (ImGui::Button(confirmLabel.c_str())) {
		std::vector<std::string> orderedTypes;
		std::unordered_set<std::string> orderedTypeSet;
		for (const EditorComponentDefinition* definition :
			GetEditorComponentDefinitions(context)) {
			if (!IsComponentPickerItemQueued(state, definition->type)) {
				continue;
			}
			if (
				definition->requiredType[0] != '\0' &&
				!HasComponent(*targetEntity, definition->requiredType) &&
				orderedTypeSet.insert(definition->requiredType).second
			) {
				orderedTypes.push_back(definition->requiredType);
			}
			if (orderedTypeSet.insert(definition->type).second) {
				orderedTypes.push_back(definition->type);
			}
		}
		int addedCount = 0;
		for (const std::string& type : orderedTypes) {
			const EditorComponentDefinition* definition =
				FindEditorComponentDefinition(type);
			if (
				!definition ||
				!SupportsEditorComponentContext(*definition, context) ||
				HasComponent(*targetEntity, type.c_str())
			) {
				continue;
			}
			if (document->AddComponent(targetEntity->id, type)) {
				++addedCount;
			}
		}
		if (addedCount > 0) {
			if (sceneTarget) {
				editorSession_->RequestSceneReload();
			}
			closePicker = true;
		} else {
			state.error = SelectEditorText(
				editorLanguage_,
				"追加できるComponentがありません。",
				"There are no components that can be added."
			);
		}
	}
	ImGui::EndDisabled();

	if (closePicker || !keepOpen) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
	if (closePicker || !keepOpen) {
		ResetComponentPicker(state);
	}
}

void ImGuiManager::DrawSceneComponentPicker() {
	DrawComponentPicker(
		sceneComponentPicker_,
		ComponentPickerTarget::Scene
	);
}

void ImGuiManager::DrawPrefabComponentPicker() {
	DrawComponentPicker(
		prefabComponentPicker_,
		ComponentPickerTarget::Prefab
	);
}

void ImGuiManager::DrawInspectorWindow() {
	if (revealInspectorRequested_) {
		if (ImGuiWindow* inspectorWindow = ImGui::FindWindowByName("Inspector")) {
			if (ImGuiDockNode* dockNode = inspectorWindow->DockNode) {
				dockNode->SelectedTabId = inspectorWindow->TabId;
				if (dockNode->TabBar) {
					dockNode->TabBar->SelectedTabId = inspectorWindow->TabId;
				}
			}
		}
		revealInspectorRequested_ = false;
	}
	// コンテンツ量の境界でスクロールバーが出入りすると、幅依存のPreviewが再配置を繰り返す。
	ImGui::Begin(
		SelectEditorText(
			editorLanguage_,
			"Inspector###Inspector",
			"Inspector###Inspector"
		),
		&showInspector_,
		ImGuiWindowFlags_AlwaysVerticalScrollbar
	);
	DrawSceneComponentPicker();

	if (!selectedProjectFile_.empty()) {
		if (ImGui::Button("Back to Hierarchy Selection")) {
			selectedProjectFile_.clear();
			ImGui::End();
			return;
		}
		ImGui::Separator();

		const std::filesystem::path path = PathFromUtf8(selectedProjectFile_);
		std::string fileName = PathToUtf8(path.filename());
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		ImGui::Text("Asset Name: %s", fileName.c_str());
		ImGui::Text("Path: %s", selectedProjectFile_.c_str());

		std::error_code ec;
		auto fileSize = std::filesystem::file_size(path, ec);
		if (!ec) {
			if (fileSize < 1024) {
				ImGui::Text("Size: %llu Bytes", fileSize);
			} else if (fileSize < 1024 * 1024) {
				ImGui::Text("Size: %.2f KB", fileSize / 1024.0f);
			} else {
				ImGui::Text("Size: %.2f MB", fileSize / (1024.0f * 1024.0f));
			}
		}
		ImGui::Separator();

		// Check for specific file types
		if (TextureFormat::IsSupportedTexturePath(path)) {
			// Texture asset inspector
			const std::string texturePath = GetProjectResourcePath(
				selectedProjectFile_
			);
			bool isLoaded = TextureManager::GetInstance() &&
				TextureManager::GetInstance()->HasTexture(texturePath);
			if (isLoaded) {
				ImGui::Text("Status: Loaded in memory");
				
				const auto& metadata = TextureManager::GetInstance()->GetMetaData(
					texturePath
				);
				ImGui::Text("Width: %zu px", metadata.width);
				ImGui::Text("Height: %zu px", metadata.height);
				ImGui::Text("Mip Levels: %zu", metadata.mipLevels);
				
				// Render thumbnail
				D3D12_GPU_DESCRIPTOR_HANDLE handle =
					TextureManager::GetInstance()->GetSrvHandleGPU(texturePath);
				const ImTextureID textureId = static_cast<ImTextureID>(handle.ptr);
				
				float aspect = static_cast<float>(metadata.width) / static_cast<float>(metadata.height);
				float drawWidth = 150.0f;
				float drawHeight = drawWidth / (aspect > 0.0f ? aspect : 1.0f);
				if (drawHeight > 150.0f) {
					drawHeight = 150.0f;
					drawWidth = drawHeight * aspect;
				}
				ImGui::TextUnformatted("Preview:");
				ImGui::Image(ImTextureRef(textureId), ImVec2(drawWidth, drawHeight));
			} else {
				ImGui::Text("Status: Not loaded");
				if (ImGui::Button("Load Texture")) {
					if (TextureManager::GetInstance()) {
						TextureManager::GetInstance()->LoadTexture(texturePath);
					}
				}
			}
			ImGui::Separator();
			ImGui::BeginDisabled(!editorSession_ || !editorSession_->IsEditing());
			if (ImGui::Button("Add Sprite to Scene")) {
				SceneDocument& document = editorSession_->GetEditDocument();
				std::string entityName = PathToUtf8(path.stem());
				if (entityName.empty()) {
					entityName = "Sprite";
				}
				const std::string baseName = entityName;
				uint32_t suffix = 2;
				while (document.FindEntityByName(entityName)) {
					entityName = baseName + " " + std::to_string(suffix++);
				}
				SceneEntity& entity = document.CreateEntity(entityName);
				entity.spriteTexturePath = GetProjectResourcePath(
					PathToUtf8(path)
				);
				document.AddComponent(entity.id, "SpriteRenderer");
				entity.components.front().texturePath = entity.spriteTexturePath;
				entity.transform.translate = {
					static_cast<float>(dxCommon_->GetClientWidth()) * 0.5f,
					static_cast<float>(dxCommon_->GetClientHeight()) * 0.5f,
					0.0f
				};
				if (TextureManager::GetInstance()) {
					TextureManager::GetInstance()->LoadTexture(entity.spriteTexturePath);
					const auto& metadata = TextureManager::GetInstance()->GetMetaData(
						entity.spriteTexturePath
					);
					entity.spriteSize = {
						static_cast<float>(metadata.width),
						static_cast<float>(metadata.height)
					};
					entity.components.front().spriteSize = entity.spriteSize;
				}
				selectedEntityId_ = entity.id;
				selectedProjectFile_.clear();
			}
			ImGui::EndDisabled();
		} 
		else if (ModelFormat::IsBasicModelPath(path)) {
			// Model asset inspector
			std::string relativePath = GetModelPathRelativeToResources(
				selectedProjectFile_
			);
			if (const ModelFormat::Descriptor* format =
				ModelFormat::FindByPath(path)) {
				ImGui::Text("Format: %s", format->displayName.data());
			}
			Model* loadedModel = ModelManager::GetInstance()
				? ModelManager::GetInstance()->FindModel(relativePath)
				: nullptr;
			bool isLoaded = loadedModel != nullptr;
			
			if (isLoaded) {
				ImGui::Text("Status: Loaded in ModelManager");
				ImGui::Text("Key: %s", relativePath.c_str());
				ImGui::Text("Vertices: %u", loadedModel->GetVertexCount());
			} else {
				ImGui::Text("Status: Not loaded");
				if (ImGui::Button("Load Model")) {
					if (ModelManager::GetInstance()) {
						ModelManager::GetInstance()->LoadModel(relativePath);
					}
				}
			}

			ImGui::SeparatorText("Preview");
			if (
				modelPreviewRenderedPath_ == relativePath &&
				modelPreviewTexture_.ptr != 0
			) {
				const float availableWidth = ImGui::GetContentRegionAvail().x;
				const float previewSize = std::clamp(
					availableWidth,
					160.0f,
					360.0f
				);
				const ImVec2 imageSize(previewSize, previewSize);
				ImGui::Image(
					ImTextureRef(static_cast<ImTextureID>(modelPreviewTexture_.ptr)),
					imageSize
				);
				if (ImGui::IsItemHovered()) {
					const ImGuiIO& io = ImGui::GetIO();
					if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
						modelPreviewYaw_ += io.MouseDelta.x * 0.01f;
						modelPreviewPitch_ = std::clamp(
							modelPreviewPitch_ + io.MouseDelta.y * 0.01f,
							-1.45f,
							1.45f
						);
					}
					if (io.MouseWheel != 0.0f) {
						modelPreviewZoom_ = std::clamp(
							modelPreviewZoom_ * (1.0f - io.MouseWheel * 0.12f),
							0.25f,
							4.0f
						);
					}
					ImGui::SetTooltip("Drag to orbit | Wheel to zoom");
				}
			} else {
				ImGui::TextDisabled("Preparing preview...");
			}
			if (ImGui::SmallButton("Reset View")) {
				modelPreviewYaw_ = 0.65f;
				modelPreviewPitch_ = 0.25f;
				modelPreviewZoom_ = 1.0f;
			}

			ImGui::Separator();
			ImGui::BeginDisabled(
				!editorSession_ || !editorSession_->IsEditing()
			);
			if (ImGui::Button("Add Model to Scene")) {
				SceneDocument& document = editorSession_->GetEditDocument();
				std::string entityName = PathToUtf8(path.stem());
				if (entityName.empty()) {
					entityName = "Model";
				}
				const std::string baseName = entityName;
				uint32_t suffix = 2;
				while (document.FindEntityByName(entityName)) {
					entityName = baseName + " " + std::to_string(suffix++);
				}
				SceneEntity& entity = document.CreateEntity(entityName);
				entity.modelPath = relativePath;
				document.AddComponent(entity.id, "MeshRenderer");
				entity.components.front().modelPath = entity.modelPath;
				selectedEntityId_ = entity.id;
				selectedProjectFile_.clear();
			}
			ImGui::EndDisabled();
			if (!editorSession_) {
				ImGui::TextDisabled("Scene editing is unavailable");
			} else if (!editorSession_->IsEditing()) {
				ImGui::TextDisabled("Stop Play Mode to edit the scene");
			}
		}
		else if (IsAudioAssetExtension(ext)) {
			Audio* audio = Audio::GetInstance();
			if (!previewAudioClip_) {
				if (ImGui::Button("Load & Play Sound")) {
					if (audio) {
						previewAudioClip_ = audio->LoadAudioFile(
							selectedProjectFile_.c_str(),
							&previewAudioError_
						);
						if (previewAudioClip_) {
							previewAudioPlayback_ = audio->PlayAudioClip(
								previewAudioClip_
							);
							if (!previewAudioPlayback_.IsValid()) {
								previewAudioError_ =
									"Failed to create an XAudio2 Source Voice.";
							}
						}
					} else {
						previewAudioError_ = "Audio subsystem is unavailable.";
					}
				}
				if (!previewAudioError_.empty()) {
					ImGui::TextColored(
						ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
						"%s",
						previewAudioError_.c_str()
					);
				}
			} else {
				ImGui::Text("Container: %s", previewAudioClip_->container.c_str());
				ImGui::Text("Decoded format: PCM");
				ImGui::Text("Load mode: Decompress On Load");
				ImGui::Text("Duration: %.2f s", previewAudioClip_->durationSeconds);
				ImGui::Text("Channels: %u", previewAudioClip_->channelCount);
				ImGui::Text("Sample rate: %u Hz", previewAudioClip_->sampleRate);
				ImGui::Text(
					"Bits per sample: %u",
					static_cast<unsigned int>(previewAudioClip_->bitsPerSample)
				);
				const bool playing = audio &&
					audio->IsPlaying(previewAudioPlayback_);
				ImGui::TextColored(
					ImVec4(0.3f, 0.8f, 0.3f, 1.0f),
					playing ? "Playing" : "Loaded"
				);
				if (playing) {
					if (ImGui::Button("Stop Sound") && audio) {
						audio->SoundStop(previewAudioPlayback_);
					}
				} else if (ImGui::Button("Play Sound") && audio) {
					previewAudioPlayback_ = audio->PlayAudioClip(
						previewAudioClip_
					);
					if (!previewAudioPlayback_.IsValid()) {
						previewAudioError_ =
							"Failed to create an XAudio2 Source Voice.";
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Unload Sound")) {
					StopAudioPreview();
				}
				if (!previewAudioError_.empty()) {
					ImGui::TextColored(
						ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
						"%s",
						previewAudioError_.c_str()
					);
				}
			}
		} 
		else if (ext == ".json") {
			if (fileName.ends_with(".prefab.json")) {
				const PrefabAssetReference variantBase =
					PrefabAssetRegistry::ReadVariantBase(selectedProjectFile_);
				ImGui::Text(
					"Type: %s",
					variantBase.assetId.empty()
						? "Entity Prefab"
						: "Prefab Variant"
				);
				if (!variantBase.assetId.empty()) {
					const std::string basePath =
						PrefabAssetRegistry::ResolvePath(variantBase);
					ImGui::TextWrapped(
						"Base: %s",
						basePath.empty()
							? "Missing or ambiguous"
							: basePath.c_str()
					);
				}
				SceneDocument prefabPreview;
				if (prefabPreview.Load(selectedProjectFile_)) {
					ImGui::Text(
						"Entities: %zu",
						prefabPreview.GetEntities().size()
					);
				} else {
					ImGui::TextWrapped(
						"Load error: %s",
						prefabPreview.GetLastLoadError().c_str()
					);
				}
				if (ImGui::Button("Open Prefab Editor")) {
					RequestOpenPrefab(selectedProjectFile_);
				}
				ImGui::BeginDisabled(
					!editorSession_ || !editorSession_->IsEditing()
				);
				static uint64_t prefabParentEntityId = 0;
				SceneDocument* editDocument = editorSession_ && editorSession_->IsEditing()
					? &editorSession_->GetEditDocument()
					: nullptr;
				const SceneEntity* prefabParent = editDocument
					? editDocument->FindEntity(prefabParentEntityId)
					: nullptr;
				if (!prefabParent) {
					prefabParentEntityId = 0;
				}
				if (ImGui::BeginCombo(
					"Instance Parent",
					prefabParent ? prefabParent->name.c_str() : "Scene Root"
				)) {
					if (ImGui::Selectable("Scene Root", prefabParentEntityId == 0)) {
						prefabParentEntityId = 0;
					}
					if (editDocument) {
						for (const SceneEntity& candidate : editDocument->GetEntities()) {
							if (ImGui::Selectable(
								candidate.name.c_str(),
								prefabParentEntityId == candidate.id
							)) {
								prefabParentEntityId = candidate.id;
							}
						}
					}
					ImGui::EndCombo();
				}
				if (ImGui::Button("Instantiate Prefab")) {
					InstantiatePrefabInEditScene(
						selectedProjectFile_,
						prefabParentEntityId
					);
				}
				ImGui::EndDisabled();
			} else {
				// Particle JSON inspector
				ParticleEffectDesc desc;
				if (ParticleEffectResource::Load(selectedProjectFile_, desc)) {
				ImGui::Text("Type: Particle Effect Description");
				ImGui::Text("Effect Name: %s", desc.name.c_str());
				ImGui::Text("Texture: %s", desc.textureFilePath.c_str());
				ImGui::Text("Particle Count: %u", desc.emitter.count);
				ImGui::Text("Spawn Size: (%.2f, %.2f, %.2f)", desc.emitter.spawnSize.x, desc.emitter.spawnSize.y, desc.emitter.spawnSize.z);
				
				ImGui::Separator();
				if (ImGui::Button("Load into Particle Editor")) {
					particleToLoad_ = selectedProjectFile_;
					requestLoadParticle_ = true;
				}
				} else {
					ImGui::Text("Type: JSON File");
					ImGui::Text("Unable to parse as ParticleEffectDesc");
				}
			}
		}
		else {
			ImGui::Text("Type: Unknown Asset / Plain File");
		}
	}
	else if (editorSession_ && selectedEntityIds_.size() > 1) {
		SceneDocument& document = editorSession_->GetActiveDocument();
		std::vector<SceneEntity*> selectedEntities;
		for (uint64_t entityId : selectedEntityIds_) {
			if (SceneEntity* entity = document.FindEntity(entityId)) {
				selectedEntities.push_back(entity);
			}
		}
		if (selectedEntities.size() <= 1) {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"選択が変わりました。",
				"Selection changed"
			));
			ImGui::End();
			return;
		}

		ImGui::Text(
			SelectEditorText(
				editorLanguage_,
				"%zu 個のEntityを選択中",
				"%zu Entities Selected"
			),
			selectedEntities.size()
		);
		ImGui::Separator();
		bool allActive = std::all_of(
			selectedEntities.begin(),
			selectedEntities.end(),
			[](const SceneEntity* entity) { return entity->active; }
		);
		bool allLocked = std::all_of(
			selectedEntities.begin(),
			selectedEntities.end(),
			[](const SceneEntity* entity) { return entity->locked; }
		);
		const bool activeMixed = std::any_of(
			selectedEntities.begin(),
			selectedEntities.end(),
			[allActive](const SceneEntity* entity) { return entity->active != allActive; }
		);
		const bool lockedMixed = std::any_of(
			selectedEntities.begin(),
			selectedEntities.end(),
			[allLocked](const SceneEntity* entity) { return entity->locked != allLocked; }
		);
		if (activeMixed) {
			ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		}
		if (ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"有効###MultiEntityActive",
			"Active###MultiEntityActive"
		), &allActive)) {
			for (SceneEntity* entity : selectedEntities) {
				entity->active = allActive;
			}
			document.MarkDirty();
		}
		if (activeMixed) {
			ImGui::PopItemFlag();
		}
		if (lockedMixed) {
			ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		}
		if (ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"ロック###MultiEntityLocked",
			"Locked###MultiEntityLocked"
		), &allLocked)) {
			for (SceneEntity* entity : selectedEntities) {
				entity->locked = allLocked;
			}
			document.MarkDirty();
		}
		if (lockedMixed) {
			ImGui::PopItemFlag();
		}
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"TransformとComponentはアクティブなEntityで編集します。",
			"Transform and components are edited on the active Entity."
		));
	}
	else if (editorSession_ && selectedEntityId_ != 0) {
		SceneDocument& document = editorSession_->GetActiveDocument();
		SceneEntity* entity = document.FindEntity(selectedEntityId_);
		if (!entity) {
			if (sceneComponentPicker_.entityId != selectedEntityId_) {
				selectedEntityId_ = 0;
			}
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"Entityは存在しません。",
				"Entity no longer exists"
			));
			ImGui::End();
			return;
		}
		const bool entityLocked = entity->locked;

		char nameBuffer[128]{};
		strncpy_s(nameBuffer, entity->name.c_str(), _TRUNCATE);
		ImGui::BeginDisabled(entityLocked);
		if (ImGui::InputText(SelectEditorText(
			editorLanguage_,
			"名前###SceneEntityName",
			"Name###SceneEntityName"
		), nameBuffer, sizeof(nameBuffer))) {
			entity->name = nameBuffer;
			document.MarkDirty();
		}
		ImGui::EndDisabled();
		if (ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"有効###SceneEntityActive",
			"Active###SceneEntityActive"
		), &entity->active)) {
			document.MarkDirty();
		}
		if (ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"ロック###SceneEntityLocked",
			"Locked###SceneEntityLocked"
		), &entity->locked)) {
			document.MarkDirty();
		}
		ImGui::BeginDisabled(entityLocked || !entity->components.empty());
		bool folder = entity->folder;
		if (ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"Folder###SceneEntityFolder",
			"Folder###SceneEntityFolder"
		), &folder)) {
			entity->folder = folder;
			if (entity->folder) {
				entity->modelPath.clear();
				entity->spriteTexturePath.clear();
			}
			document.MarkDirty();
			editorSession_->RequestSceneReload();
		}
		ImGui::EndDisabled();

		ImGui::SeparatorText("Prefab");
		const uint64_t prefabInstanceRootId =
			document.FindPrefabInstanceRoot(entity->id);
		if (prefabInstanceRootId != 0) {
			const SceneEntity* prefabRoot =
				document.FindEntity(prefabInstanceRootId);
			const std::string linkedPrefabAssetPath = prefabRoot
				? PrefabAssetRegistry::ResolvePath(
					prefabRoot->prefabAssetId,
					prefabRoot->prefabSourcePath
				)
				: std::string{};
			bool prefabEditConflict = false;
			if (
				prefabRoot &&
				!linkedPrefabAssetPath.empty() &&
				prefabEditorSession_ &&
				prefabEditorSession_->IsOpen() &&
				prefabEditorSession_->IsDirty()
			) {
				const std::filesystem::path openPrefabPath =
					EditableResourcePath::ResolveResource(
						PathFromUtf8(prefabEditorSession_->GetFilePath())
					);
				const std::filesystem::path linkedPrefabPath =
					EditableResourcePath::ResolveResource(
						PathFromUtf8(linkedPrefabAssetPath)
					);
				std::error_code equivalentError;
				prefabEditConflict = std::filesystem::equivalent(
					openPrefabPath,
					linkedPrefabPath,
					equivalentError
				);
				if (equivalentError) {
					prefabEditConflict =
						openPrefabPath.lexically_normal() ==
						linkedPrefabPath.lexically_normal();
				}
			}
			bool applyPrefabRequested = false;
			bool revertPrefabRequested = false;
			bool unpackPrefabRequested = false;
			ImGui::TextWrapped(
				"Linked Asset: %s",
				!linkedPrefabAssetPath.empty()
					? linkedPrefabAssetPath.c_str()
					: "Missing or ambiguous"
			);
			if (prefabRoot && !prefabRoot->prefabAssetId.empty()) {
				ImGui::TextDisabled(
					"Asset ID: %s",
					prefabRoot->prefabAssetId.c_str()
				);
			}
			if (ImGui::BeginPopupContextItem("PrefabInstanceContext")) {
				const bool hasLinkedAsset = !linkedPrefabAssetPath.empty();
				if (ImGui::MenuItem(
					"Open Prefab",
					nullptr,
					false,
					hasLinkedAsset
				)) {
					RequestOpenPrefab(linkedPrefabAssetPath);
				}
				if (ImGui::MenuItem(
					"Select Asset",
					nullptr,
					false,
					hasLinkedAsset
				)) {
					SelectPrefabAssetInProject(linkedPrefabAssetPath);
				}
				ImGui::Separator();
				const bool canModifyInstance =
					!entityLocked &&
					editorSession_->IsEditing() &&
					!prefabEditConflict;
				if (ImGui::MenuItem(
					"Apply Instance To Prefab",
					nullptr,
					false,
					canModifyInstance
				)) {
					applyPrefabRequested = true;
				}
				if (ImGui::MenuItem(
					"Revert Instance",
					nullptr,
					false,
					canModifyInstance
				)) {
					revertPrefabRequested = true;
				}
				if (ImGui::MenuItem(
					"Unpack",
					nullptr,
					false,
					canModifyInstance
				)) {
					unpackPrefabRequested = true;
				}
				ImGui::EndPopup();
			}
			ImGui::TextDisabled(
				"Instance Root: %llu / Local Entity: %llu",
				static_cast<unsigned long long>(prefabInstanceRootId),
				static_cast<unsigned long long>(entity->prefabLocalId)
			);
			ImGui::BeginDisabled(linkedPrefabAssetPath.empty());
			if (ImGui::Button("Open Prefab")) {
				RequestOpenPrefab(linkedPrefabAssetPath);
			}
			ImGui::SameLine();
			if (ImGui::Button("Select Asset")) {
				SelectPrefabAssetInProject(linkedPrefabAssetPath);
			}
			ImGui::EndDisabled();
			static std::string prefabInstanceStatus;
			std::vector<ScenePrefabPropertyOverride> propertyOverrides;
			int applyPropertyOverrideIndex = -1;
			int revertPropertyOverrideIndex = -1;
			if (ImGui::TreeNode("Overrides")) {
				const std::vector<std::string> entityOverrides =
					document.CollectPrefabInstanceOverrides(prefabInstanceRootId);
				propertyOverrides = document.CollectPrefabPropertyOverrides(
					prefabInstanceRootId
				);
				bool hasLegacyOverrideStatus = false;
				for (const std::string& overrideLabel : entityOverrides) {
					if (
						overrideLabel.starts_with("Modified Entity:") ||
						overrideLabel.starts_with("Added Entity:") ||
						overrideLabel.starts_with("Removed Entity:") ||
						overrideLabel.starts_with("Stale Entity:")
					) {
						continue;
					}
					hasLegacyOverrideStatus = true;
					ImGui::BulletText("%s", overrideLabel.c_str());
				}
				if (!propertyOverrides.empty()) {
					ImGui::SeparatorText("Individual Overrides");
				}
				const bool canModifyProperty =
					!entityLocked &&
					editorSession_->IsEditing() &&
					!prefabEditConflict;
				for (size_t index = 0; index < propertyOverrides.size(); ++index) {
					const ScenePrefabPropertyOverride& overrideValue =
						propertyOverrides[index];
					ImGui::PushID(static_cast<int>(index));
					ImGui::BulletText("%s", overrideValue.label.c_str());
					ImGui::Indent();
					ImGui::BeginDisabled(!canModifyProperty);
					if (ImGui::SmallButton("Apply")) {
						applyPropertyOverrideIndex = static_cast<int>(index);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("Revert")) {
						revertPropertyOverrideIndex = static_cast<int>(index);
					}
					ImGui::EndDisabled();
					ImGui::Unindent();
					ImGui::PopID();
				}
				if (propertyOverrides.empty() && !hasLegacyOverrideStatus) {
					ImGui::TextDisabled("No overrides.");
				}
				ImGui::TreePop();
			}
			if (applyPropertyOverrideIndex >= 0) {
				const ScenePrefabPropertyOverride& overrideValue =
					propertyOverrides[applyPropertyOverrideIndex];
				if (document.ApplyPrefabPropertyOverride(
					prefabInstanceRootId,
					overrideValue
				)) {
					prefabInstanceStatus = "Applied: " + overrideValue.label;
					InvalidateProjectCache();
				} else {
					prefabInstanceStatus = "Failed to apply: " + overrideValue.label;
				}
			}
			if (revertPropertyOverrideIndex >= 0) {
				const ScenePrefabPropertyOverride& overrideValue =
					propertyOverrides[revertPropertyOverrideIndex];
				const uint64_t selectedId = entity->id;
				const bool removesSelectedBranch =
					(
						overrideValue.kind == ScenePrefabOverrideKind::AddedEntity ||
						overrideValue.kind == ScenePrefabOverrideKind::StaleEntity
					) &&
					(
						selectedId == overrideValue.instanceEntityId ||
						document.IsDescendantOf(
							selectedId,
							overrideValue.instanceEntityId
						)
					);
				if (document.RevertPrefabPropertyOverride(
					prefabInstanceRootId,
					overrideValue
				)) {
					prefabInstanceStatus = "Reverted: " + overrideValue.label;
					selectedEntityId_ = removesSelectedBranch
						? prefabInstanceRootId
						: selectedId;
					selectedEntityIds_ = { selectedEntityId_ };
					editorSession_->RequestSceneReload();
					ImGui::End();
					return;
				}
				prefabInstanceStatus = "Failed to revert: " + overrideValue.label;
			}
			if (prefabEditConflict) {
				ImGui::TextColored(
					ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
					"Save or close the open Prefab before changing this instance."
				);
			}
			ImGui::BeginDisabled(
				entityLocked ||
				!editorSession_->IsEditing() ||
				prefabEditConflict
			);
			if (
				ImGui::Button("Apply Instance To Prefab") ||
				applyPrefabRequested
			) {
				if (document.ApplyPrefabInstance(prefabInstanceRootId)) {
					prefabInstanceStatus = "Applied instance values to the Prefab asset.";
					InvalidateProjectCache();
				} else {
					prefabInstanceStatus = "Failed to apply the Prefab instance.";
				}
			}
			ImGui::SameLine();
			if (
				ImGui::Button("Revert Instance") ||
				revertPrefabRequested
			) {
				if (document.RevertPrefabInstance(prefabInstanceRootId)) {
					prefabInstanceStatus = "Reverted the instance from its Prefab asset.";
					selectedEntityId_ = prefabInstanceRootId;
					selectedEntityIds_ = { prefabInstanceRootId };
					editorSession_->RequestSceneReload();
					ImGui::EndDisabled();
					ImGui::End();
					return;
				}
				prefabInstanceStatus = "Failed to revert the Prefab instance.";
			}
			ImGui::SameLine();
			if (ImGui::Button("Unpack") || unpackPrefabRequested) {
				if (document.UnpackPrefabInstance(prefabInstanceRootId)) {
					prefabInstanceStatus = "Unpacked the Prefab instance.";
				} else {
					prefabInstanceStatus = "Failed to unpack the Prefab instance.";
				}
			}
			ImGui::EndDisabled();
			if (!prefabInstanceStatus.empty()) {
				ImGui::TextWrapped("%s", prefabInstanceStatus.c_str());
			}
			ImGui::SeparatorText("Create Prefab Asset");
		}
		ImGui::BeginDisabled(entityLocked || !editorSession_->IsEditing());
		static std::string prefabOperationStatus;
		static uint64_t prefabFileNameEntityId = 0;
		static char prefabFileName[192]{};
		if (prefabFileNameEntityId != entity->id) {
			prefabFileNameEntityId = entity->id;
			std::string defaultName = entity->name.empty() ? "Prefab" : entity->name;
			CopyTextBuffer(prefabFileName, sizeof(prefabFileName), defaultName);
		}
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText(
			"Prefab File Name",
			prefabFileName,
			sizeof(prefabFileName)
		);
		if (ImGui::Button("Save / Overwrite Prefab")) {
			std::string prefabName = prefabFileName;
			for (char& character : prefabName) {
				if (
					static_cast<unsigned char>(character) < 32 ||
					std::strchr("<>:\"/\\|?*", character)
				) {
					character = '_';
				}
			}
			if (prefabName.empty()) {
				prefabName = "Prefab";
			}
			if (!prefabName.ends_with(".prefab.json")) {
				prefabName += ".prefab.json";
			}
			const std::filesystem::path prefabDirectory =
				GetProjectResourceRoot() / "prefabs";
			const std::filesystem::path prefabFilePath =
				prefabDirectory / PathFromUtf8(prefabName);
			const std::string prefabPath = PathToUtf8(prefabFilePath);
			std::error_code prefabDirectoryError;
			std::filesystem::create_directories(
				prefabDirectory,
				prefabDirectoryError
			);
			if (!prefabDirectoryError) {
				const bool prefabSaved = document.SaveEntityBranchAsPrefab(
					entity->id,
					prefabPath
				);
				if (prefabSaved) {
					prefabOperationStatus = "Saved: resources/prefabs/" +
						prefabName;
					selectedProjectFolder_ = PathToUtf8(prefabDirectory);
					selectedProjectFile_ = prefabPath;
					InvalidateProjectCache();
				} else {
					prefabOperationStatus = "Failed to save: " + prefabPath;
				}
			} else {
				prefabOperationStatus = "Failed to create: " +
					PathToUtf8(prefabDirectory);
			}
		}
		static char prefabInstantiatePath[256] =
			"resources/prefabs/Prefab.prefab.json";
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText(
			"Prefab Path",
			prefabInstantiatePath,
			sizeof(prefabInstantiatePath)
		);
		if (ImGui::Button("Instantiate As Child")) {
			const uint64_t instanceId = document.InstantiatePrefab(
				prefabInstantiatePath,
				entity->id
			);
			if (instanceId != 0) {
				prefabOperationStatus = "Instantiated: " +
					std::string(prefabInstantiatePath);
				selectedEntityId_ = instanceId;
				editorSession_->RequestSceneReload();
				ImGui::EndDisabled();
				ImGui::End();
				return;
			}
			prefabOperationStatus = "Failed to instantiate: " +
				std::string(prefabInstantiatePath);
		}
		if (!prefabOperationStatus.empty()) {
			ImGui::TextWrapped("%s", prefabOperationStatus.c_str());
		}
		ImGui::EndDisabled();
		if (!entity->components.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("Folder requires no components");
		}

		const SceneEntity* parent = document.FindEntity(entity->parentId);
		const char* parentName = parent ? parent->name.c_str() : "None (Root)";
		ImGui::BeginDisabled(entityLocked);
		if (ImGui::BeginCombo("Parent", parentName)) {
			if (ImGui::Selectable("None (Root)", entity->parentId == 0)) {
				document.SetParent(entity->id, 0);
			}
			for (const SceneEntity& candidate : document.GetEntities()) {
				if (
					candidate.id == entity->id ||
					document.IsDescendantOf(candidate.id, entity->id)
				) {
					continue;
				}
				if (ImGui::Selectable(
					candidate.name.c_str(),
					entity->parentId == candidate.id
				)) {
					document.SetParent(entity->id, candidate.id);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();

		auto resolveEffectiveTeamName = [&]() {
			if (!entity->teamName.empty()) {
				return entity->teamName;
			}
			return document.ResolveInheritedFolderTeamName(entity->id);
		};
		const std::string inheritedFolderTeamName =
			entity->teamName.empty()
				? document.ResolveInheritedFolderTeamName(entity->id)
				: std::string{};
		const bool teamInheritedFromFolder =
			!inheritedFolderTeamName.empty();

		const std::string sceneInspectorKey = "scene/" +
			editorSession_->GetActiveSceneId() + "/" +
			std::to_string(entity->id) + "/";
		if (DrawPersistentInspectorHeader(
			sceneInspectorKey + "Team",
			"Team###SceneTeamSection"
		)) {
		const std::string currentTeamLabel = entity->teamName.empty()
			? (
				teamInheritedFromFolder
					? "Inherit: " + inheritedFolderTeamName
					: std::string("None")
			)
			: entity->teamName;
		ImGui::BeginDisabled(entityLocked);
		if (ImGui::BeginCombo("Team", currentTeamLabel.c_str())) {
			if (ImGui::Selectable("None", entity->teamName.empty())) {
				entity->teamName.clear();
				entity->folderTeamEnabled = false;
				document.MarkDirty();
				editorSession_->RequestSceneReload();
			}
			for (const SceneTeamSettings& team : document.GetTeams()) {
				if (ImGui::Selectable(
					team.name.c_str(),
					entity->teamName == team.name
				)) {
					entity->teamName = team.name;
					document.MarkDirty();
					editorSession_->RequestSceneReload();
				}
			}
			ImGui::EndCombo();
		}
		static char newTeamNameBuffer[64] = "Team";
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputText(
			"New Team",
			newTeamNameBuffer,
			sizeof(newTeamNameBuffer)
		);
		ImGui::SameLine();
		if (ImGui::SmallButton("Create")) {
			SceneTeamSettings& team = document.CreateTeam(newTeamNameBuffer);
			entity->teamName = team.name;
			if (entity->folder) {
				entity->folderTeamEnabled = true;
			}
			document.MarkDirty();
			editorSession_->RequestSceneReload();
		}
		ImGui::EndDisabled();

		if (teamInheritedFromFolder) {
			ImGui::Text(
				"Inherited from folder: %s",
				inheritedFolderTeamName.c_str()
			);
		}

		SceneTeamSettings* selectedTeam =
			document.FindTeam(resolveEffectiveTeamName());
		if (selectedTeam) {
			int memberCount = 0;
			for (const SceneEntity& candidate : document.GetEntities()) {
				if (candidate.folder) {
					continue;
				}
				const SceneTeamSettings* candidateTeam =
					document.ResolveEntityTeam(candidate);
				if (candidateTeam && candidateTeam->name == selectedTeam->name) {
					++memberCount;
				}
			}
			ImGui::Text("Members: %d", memberCount);
			if (ImGui::TreeNodeEx(
				"Team Settings",
				ImGuiTreeNodeFlags_DefaultOpen
			)) {
				ImGui::BeginDisabled(entityLocked);
				std::string previousTeamName = selectedTeam->name;
				char teamNameBuffer[64]{};
				strncpy_s(
					teamNameBuffer,
					selectedTeam->name.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					"Team Name",
					teamNameBuffer,
					sizeof(teamNameBuffer)
				)) {
					document.RenameTeam(previousTeamName, teamNameBuffer);
					selectedTeam = document.FindTeam(resolveEffectiveTeamName());
					editorSession_->RequestSceneReload();
				}
				if (selectedTeam) {
					bool teamChanged = false;
					ImGui::SeparatorText("Agent Common");
					teamChanged |= ImGui::Checkbox(
						"Team Agent Settings",
						&selectedTeam->agentBehaviorOverride
					);
					ImGui::BeginDisabled(
						!selectedTeam->agentBehaviorOverride
					);
					char teamGroupBuffer[64]{};
					strncpy_s(
						teamGroupBuffer,
						selectedTeam->agentGroupName.c_str(),
						_TRUNCATE
					);
					if (ImGui::InputText(
						"Team Agent Group",
						teamGroupBuffer,
						sizeof(teamGroupBuffer)
					)) {
						selectedTeam->agentGroupName = teamGroupBuffer;
						teamChanged = true;
					}
					teamChanged |= ImGui::DragFloat(
						"Team Min Speed",
						&selectedTeam->agentMinSpeed,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Max Speed",
						&selectedTeam->agentMaxSpeed,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Turn Speed",
						&selectedTeam->agentTurnSpeed,
						0.05f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Wander Strength",
						&selectedTeam->agentWanderStrength,
						0.05f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Wander Change Interval",
						&selectedTeam->agentWanderChangeInterval,
						0.05f,
						0.0f,
						60.0f
					);
					teamChanged |= ImGui::SliderFloat(
						"Team Wander Direction Range",
						&selectedTeam->agentWanderDirectionRange,
						0.0f,
						3.141592f
					);
					teamChanged |= ImGui::SliderFloat(
						"Team Wander Vertical Range",
						&selectedTeam->agentWanderVerticalRange,
						0.0f,
						1.0f
					);
					teamChanged |= ImGui::Checkbox(
						"Randomize Seed On Play",
						&selectedTeam->agentRandomizeSeedOnPlay
					);
					ImGui::BeginDisabled(selectedTeam->agentRandomizeSeedOnPlay);
					teamChanged |= ImGui::InputInt(
						"Random Seed",
						&selectedTeam->agentRandomSeed
					);
					ImGui::EndDisabled();
					teamChanged |= ImGui::Checkbox(
						"Use Leader Start Position",
						&selectedTeam->agentUseLeaderStartPosition
					);
					ImGui::BeginDisabled(!selectedTeam->agentUseLeaderStartPosition);
					teamChanged |= ImGui::DragFloat3(
						"Leader Start Position",
						&selectedTeam->agentLeaderStartPosition.x,
						0.05f
					);
					ImGui::EndDisabled();
					teamChanged |= ImGui::DragFloat(
						"Team Decision Interval",
						&selectedTeam->agentFlockDecisionInterval,
						0.01f,
						0.0f,
						5.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Acceleration",
						&selectedTeam->agentFlockAcceleration,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Max Turn Rate",
						&selectedTeam->agentFlockTurnRate,
						0.01f,
						0.0f,
						6.283185f
					);
					ImGui::SeparatorText("Member Follow");
					teamChanged |= ImGui::DragFloat(
						"Member Return Strength",
						&selectedTeam->agentMemberCenterFollow,
						0.05f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Jitter Strength",
						&selectedTeam->agentMemberJitterStrength,
						0.01f,
						0.0f,
						10.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Jitter Frequency",
						&selectedTeam->agentMemberJitterFrequency,
						0.01f,
						0.0f,
						10.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Jitter Update Interval",
						&selectedTeam->agentMemberJitterUpdateInterval,
						0.01f,
						0.0f,
						10.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Jitter Follow Speed",
						&selectedTeam->agentMemberJitterFollowSpeed,
						0.01f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Max Distance",
						&selectedTeam->agentMemberLeashDistance,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Leash Strength",
						&selectedTeam->agentMemberLeashStrength,
						0.05f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Catchup Speed",
						&selectedTeam->agentMemberCatchupSpeed,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Member Minimum Distance",
						&selectedTeam->agentMemberMinimumDistance,
						0.05f,
						0.0f,
						100.0f
					);
					ImGui::SeparatorText("Formation Capsule");
					teamChanged |= ImGui::Checkbox(
						"Enable Formation Capsule",
						&selectedTeam->agentFormationCapsuleEnabled
					);
					teamChanged |= ImGui::Checkbox(
						"Scale Capsule With Active Members",
						&selectedTeam->agentFormationCapsuleScaleWithActiveMembers
					);
					teamChanged |= ImGui::DragFloat(
						"Formation Capsule Radius",
						&selectedTeam->agentFormationCapsuleRadius,
						0.05f,
						0.0f,
						1000.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Formation Capsule Half Segment Length",
						&selectedTeam->agentFormationCapsuleHalfSegmentLength,
						0.05f,
						0.0f,
						1000.0f
					);
					if (ImGui::Button("Fit Capsule From Team Members")) {
						int memberCount = 0;
						for (const SceneEntity& candidate : document.GetEntities()) {
							if (!IsEntityActiveInHierarchy(document, candidate) ||
								!FindEnabledComponent(candidate, "AgentBehavior")) {
								continue;
							}
							const SceneTeamSettings* candidateTeam =
								document.ResolveEntityTeam(candidate);
							if (candidateTeam && candidateTeam->name == selectedTeam->name) {
								++memberCount;
							}
						}
						const float memberCountValue = static_cast<float>((std::max)(memberCount, 1));
						const float spacing = (std::max)(
							selectedTeam->agentMemberMinimumDistance,
							0.001f
						);
						constexpr float kPi = 3.14159265359f;
						constexpr float kPackingEfficiency = 0.72f;
						constexpr float kSafetyFactor = 1.10f;
						constexpr float kLengthToWidthRatio = 2.25f;
						const float requiredArea =
							memberCountValue * kPi * (spacing * 0.5f) * (spacing * 0.5f) *
							kSafetyFactor / kPackingEfficiency;
						const float areaCoefficient =
							4.0f * (kLengthToWidthRatio - 1.0f) + kPi;
						const float radius = std::sqrt(requiredArea / areaCoefficient);
						selectedTeam->agentFormationCapsuleRadius =
							std::ceil(radius * 2.0f) * 0.5f;
						selectedTeam->agentFormationCapsuleHalfSegmentLength =
							std::ceil(
								(kLengthToWidthRatio - 1.0f) * radius * 2.0f
							) * 0.5f;
						teamChanged = true;
					}
					ImGui::Text(
						"Members: %d | Width: %.1f | Length: %.1f",
						static_cast<int>(std::count_if(
							document.GetEntities().begin(),
							document.GetEntities().end(),
							[&document, selectedTeam](const SceneEntity& candidate) {
								return IsEntityActiveInHierarchy(document, candidate) &&
									FindEnabledComponent(candidate, "AgentBehavior") &&
									document.ResolveEntityTeam(candidate) == selectedTeam;
							}
						)),
						selectedTeam->agentFormationCapsuleRadius * 2.0f,
						2.0f * (
							selectedTeam->agentFormationCapsuleHalfSegmentLength +
							selectedTeam->agentFormationCapsuleRadius
						)
					);

					ImGui::SeparatorText("Team Heading");
					teamChanged |= ImGui::Checkbox(
						"Team Use Heading",
						&selectedTeam->agentUseTeamHeading
					);
					teamChanged |= ImGui::DragFloat3(
						"Team Heading Direction",
						&selectedTeam->agentTeamHeadingDirection.x,
						0.01f,
						-1.0f,
						1.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Heading Weight",
						&selectedTeam->agentTeamHeadingWeight,
						0.05f,
						0.0f,
						20.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Heading Follow Speed",
						&selectedTeam->agentTeamHeadingFollowSpeed,
						0.05f,
						0.0f,
						20.0f
					);

					ImGui::SeparatorText("Agent Rotation");
					teamChanged |= ImGui::Checkbox(
						"Team Align Forward To Velocity",
						&selectedTeam->agentAlignForwardToVelocity
					);
					const char* forwardAxes[] = {
						"+Z",
						"-Z",
						"+X",
						"-X",
						"+Y",
						"-Y"
					};
					const char* currentForwardAxis =
						selectedTeam->agentForwardAxis.c_str();
					if (ImGui::BeginCombo(
						"Team Forward Axis",
						currentForwardAxis
					)) {
						for (const char* axis : forwardAxes) {
							if (ImGui::Selectable(
								axis,
								selectedTeam->agentForwardAxis == axis
							)) {
								selectedTeam->agentForwardAxis = axis;
								teamChanged = true;
							}
						}
						ImGui::EndCombo();
					}
					teamChanged |= ImGui::Checkbox(
						"Team Rotate X",
						&selectedTeam->agentRotateAxisX
					);
					ImGui::SameLine();
					teamChanged |= ImGui::Checkbox(
						"Team Rotate Y",
						&selectedTeam->agentRotateAxisY
					);
					ImGui::SameLine();
					teamChanged |= ImGui::Checkbox(
						"Team Rotate Z",
						&selectedTeam->agentRotateAxisZ
					);
					teamChanged |= ImGui::DragFloat(
						"Team Rotation Follow Speed",
						&selectedTeam->agentRotationFollowSpeed,
						0.05f,
						0.0f,
						60.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Pitch From Vertical Velocity",
						&selectedTeam->agentPitchFromVerticalVelocity,
						0.05f,
						0.0f,
						4.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Banking Strength",
						&selectedTeam->agentBankingStrength,
						0.05f,
						0.0f,
						4.0f
					);

					teamChanged |= ImGui::Checkbox(
						"Team Schooling",
						&selectedTeam->agentSchooling
					);
					teamChanged |= ImGui::DragFloat(
						"Team Schooling Update Interval",
						&selectedTeam->agentSchoolingUpdateInterval,
						0.01f,
						0.0f,
						5.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Schooling Update Jitter",
						&selectedTeam->agentSchoolingUpdateJitter,
						0.01f,
						0.0f,
						1.0f
					);
					teamChanged |= ImGui::InputInt(
						"Team Neighbor Limit",
						&selectedTeam->agentNeighborLimit
					);
					teamChanged |= ImGui::SliderFloat(
						"Team Schooling Blend",
						&selectedTeam->agentSchoolingBlend,
						0.0f,
						1.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Separation Radius",
						&selectedTeam->agentSeparationRadius,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Alignment Radius",
						&selectedTeam->agentAlignmentRadius,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Cohesion Radius",
						&selectedTeam->agentCohesionRadius,
						0.05f,
						0.0f,
						100.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Separation Weight",
						&selectedTeam->agentSeparationWeight,
						0.05f,
						0.0f,
						50.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Alignment Weight",
						&selectedTeam->agentAlignmentWeight,
						0.05f,
						0.0f,
						50.0f
					);
					teamChanged |= ImGui::DragFloat(
						"Team Cohesion Weight",
						&selectedTeam->agentCohesionWeight,
						0.05f,
						0.0f,
						50.0f
					);
					teamChanged |= ImGui::ColorEdit4(
						"Team Agent Color",
						&selectedTeam->agentVisualColor.x,
						ImGuiColorEditFlags_Float
					);
					teamChanged |= ImGui::Checkbox(
						"Team Agent Lighting",
						&selectedTeam->agentEnableLighting
					);
					ImGui::EndDisabled();

					const float previousMinSpeed = selectedTeam->agentMinSpeed;
					const float previousMaxSpeed = selectedTeam->agentMaxSpeed;
					const float previousTurnSpeed = selectedTeam->agentTurnSpeed;
					const float previousWanderStrength =
						selectedTeam->agentWanderStrength;
					const Vector3 previousTeamHeadingDirection =
						selectedTeam->agentTeamHeadingDirection;
					const float previousTeamHeadingWeight =
						selectedTeam->agentTeamHeadingWeight;
					const float previousTeamHeadingFollowSpeed =
						selectedTeam->agentTeamHeadingFollowSpeed;
					const float previousTeamRotationWeight =
						selectedTeam->agentTeamRotationWeight;
					const float previousTeamRotationFollowSpeed =
						selectedTeam->agentTeamRotationFollowSpeed;
					const float previousRotationFollowSpeed =
						selectedTeam->agentRotationFollowSpeed;
					const float previousPitchFromVerticalVelocity =
						selectedTeam->agentPitchFromVerticalVelocity;
					const float previousBankingStrength =
						selectedTeam->agentBankingStrength;
					const float previousSchoolingUpdateInterval =
						selectedTeam->agentSchoolingUpdateInterval;
					const float previousSchoolingUpdateJitter =
						selectedTeam->agentSchoolingUpdateJitter;
					const int previousNeighborLimit =
						selectedTeam->agentNeighborLimit;
					const float previousSchoolingBlend =
						selectedTeam->agentSchoolingBlend;
					const float previousSeparationRadius =
						selectedTeam->agentSeparationRadius;
					const float previousAlignmentRadius =
						selectedTeam->agentAlignmentRadius;
					const float previousCohesionRadius =
						selectedTeam->agentCohesionRadius;
					const float previousSeparationWeight =
						selectedTeam->agentSeparationWeight;
					const float previousAlignmentWeight =
						selectedTeam->agentAlignmentWeight;
					const float previousCohesionWeight =
						selectedTeam->agentCohesionWeight;
					selectedTeam->agentMinSpeed =
						(std::max)(selectedTeam->agentMinSpeed, 0.0f);
					selectedTeam->agentMaxSpeed =
						(std::max)(
							selectedTeam->agentMaxSpeed,
							selectedTeam->agentMinSpeed
						);
					selectedTeam->agentTurnSpeed =
						(std::max)(selectedTeam->agentTurnSpeed, 0.0f);
					selectedTeam->agentWanderStrength =
						(std::max)(selectedTeam->agentWanderStrength, 0.0f);
					if (
						Math::Length(selectedTeam->agentTeamHeadingDirection) <=
						0.000001f
					) {
						selectedTeam->agentTeamHeadingDirection = {
							0.0f,
							0.0f,
							1.0f
						};
					} else {
						selectedTeam->agentTeamHeadingDirection =
							Math::Normalize(
								selectedTeam->agentTeamHeadingDirection
							);
					}
					selectedTeam->agentTeamHeadingWeight =
						(std::max)(selectedTeam->agentTeamHeadingWeight, 0.0f);
					selectedTeam->agentTeamHeadingFollowSpeed =
						(std::max)(
							selectedTeam->agentTeamHeadingFollowSpeed,
							0.0f
						);
					selectedTeam->agentTeamRotationWeight = std::clamp(
						selectedTeam->agentTeamRotationWeight,
						0.0f,
						1.0f
					);
					selectedTeam->agentTeamRotationFollowSpeed =
						(std::max)(
							selectedTeam->agentTeamRotationFollowSpeed,
							0.0f
						);
					selectedTeam->agentRotationFollowSpeed =
						(std::max)(
							selectedTeam->agentRotationFollowSpeed,
							0.0f
						);
					selectedTeam->agentPitchFromVerticalVelocity =
						(std::max)(
							selectedTeam->agentPitchFromVerticalVelocity,
							0.0f
						);
					selectedTeam->agentBankingStrength =
						(std::max)(selectedTeam->agentBankingStrength, 0.0f);
					selectedTeam->agentSchoolingUpdateInterval =
						(std::max)(
							selectedTeam->agentSchoolingUpdateInterval,
							0.0f
						);
					selectedTeam->agentSchoolingUpdateJitter =
						(std::max)(
							selectedTeam->agentSchoolingUpdateJitter,
							0.0f
						);
					selectedTeam->agentNeighborLimit =
						(std::max)(selectedTeam->agentNeighborLimit, 0);
					selectedTeam->agentSchoolingBlend = std::clamp(
						selectedTeam->agentSchoolingBlend,
						0.0f,
						1.0f
					);
					selectedTeam->agentSeparationRadius =
						(std::max)(selectedTeam->agentSeparationRadius, 0.0f);
					selectedTeam->agentAlignmentRadius =
						(std::max)(selectedTeam->agentAlignmentRadius, 0.0f);
					selectedTeam->agentCohesionRadius =
						(std::max)(selectedTeam->agentCohesionRadius, 0.0f);
					selectedTeam->agentSeparationWeight =
						(std::max)(selectedTeam->agentSeparationWeight, 0.0f);
					selectedTeam->agentAlignmentWeight =
						(std::max)(selectedTeam->agentAlignmentWeight, 0.0f);
					selectedTeam->agentCohesionWeight =
						(std::max)(selectedTeam->agentCohesionWeight, 0.0f);
					if (!std::isfinite(selectedTeam->agentMemberMinimumDistance) ||
						selectedTeam->agentMemberMinimumDistance < 0.0f) {
						selectedTeam->agentMemberMinimumDistance = 0.0f;
					}
					teamChanged |=
						previousMinSpeed != selectedTeam->agentMinSpeed ||
						previousMaxSpeed != selectedTeam->agentMaxSpeed ||
						previousTurnSpeed != selectedTeam->agentTurnSpeed ||
						previousWanderStrength !=
							selectedTeam->agentWanderStrength ||
						previousTeamHeadingDirection.x !=
							selectedTeam->agentTeamHeadingDirection.x ||
						previousTeamHeadingDirection.y !=
							selectedTeam->agentTeamHeadingDirection.y ||
						previousTeamHeadingDirection.z !=
							selectedTeam->agentTeamHeadingDirection.z ||
						previousTeamHeadingWeight !=
							selectedTeam->agentTeamHeadingWeight ||
						previousTeamHeadingFollowSpeed !=
							selectedTeam->agentTeamHeadingFollowSpeed ||
						previousTeamRotationWeight !=
							selectedTeam->agentTeamRotationWeight ||
						previousTeamRotationFollowSpeed !=
							selectedTeam->agentTeamRotationFollowSpeed ||
						previousRotationFollowSpeed !=
							selectedTeam->agentRotationFollowSpeed ||
						previousPitchFromVerticalVelocity !=
							selectedTeam->agentPitchFromVerticalVelocity ||
						previousBankingStrength !=
							selectedTeam->agentBankingStrength ||
						previousSchoolingUpdateInterval !=
							selectedTeam->agentSchoolingUpdateInterval ||
						previousSchoolingUpdateJitter !=
							selectedTeam->agentSchoolingUpdateJitter ||
						previousNeighborLimit !=
							selectedTeam->agentNeighborLimit ||
						previousSchoolingBlend !=
							selectedTeam->agentSchoolingBlend ||
						previousSeparationRadius !=
							selectedTeam->agentSeparationRadius ||
						previousAlignmentRadius !=
							selectedTeam->agentAlignmentRadius ||
						previousCohesionRadius !=
							selectedTeam->agentCohesionRadius ||
						previousSeparationWeight !=
							selectedTeam->agentSeparationWeight ||
						previousAlignmentWeight !=
							selectedTeam->agentAlignmentWeight ||
						previousCohesionWeight !=
							selectedTeam->agentCohesionWeight;
					if (teamChanged) {
						document.MarkDirty();
					}
					if (ImGui::SmallButton("Remove Team")) {
						const std::string removeTeamName = selectedTeam->name;
						document.RemoveTeam(removeTeamName);
						selectedTeam = nullptr;
						editorSession_->RequestSceneReload();
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}
		}

		if (entity->folder) {
			if (DrawPersistentInspectorHeader(
				sceneInspectorKey + "Folder",
				"Folder###SceneFolderSection"
			)) {
			ImGui::TextDisabled("Folders organize children in the hierarchy.");
			ImGui::BeginDisabled(entityLocked || entity->teamName.empty());
			bool folderTeamEnabled = entity->folderTeamEnabled;
			if (ImGui::Checkbox("Use Folder As Team", &folderTeamEnabled)) {
				entity->folderTeamEnabled = folderTeamEnabled;
				document.MarkDirty();
				editorSession_->RequestSceneReload();
			}
			ImGui::EndDisabled();
			if (entity->teamName.empty()) {
				ImGui::TextDisabled("Assign a Team above to enable folder team.");
			}
			if (editorSession_->IsPlaying() || editorSession_->IsPaused()) {
				ImGui::TextDisabled("Play mode changes are temporary");
			}
			}
			ImGui::End();
			return;
		}

		if (DrawPersistentInspectorHeader(
			sceneInspectorKey + "Transform",
			"Transform###SceneTransformSection"
		)) {
		bool transformChanged = false;
		ImGui::BeginDisabled(entityLocked);
		if (HasComponent(*entity, "SpriteRenderer")) {
			transformChanged |= ImGui::DragFloat2(
				SelectEditorText(editorLanguage_, "位置###SceneTransformPosition", "Position###SceneTransformPosition"),
				&entity->transform.translate.x,
				0.5f
			);
			Vector3 spriteEuler =
				MakeEulerFromQuaternion(entity->transform.rotate);
			if (ImGui::DragFloat(
				SelectEditorText(editorLanguage_, "回転###SceneTransformRotation", "Rotation###SceneTransformRotation"),
				&spriteEuler.z,
				0.01f
			)) {
				entity->transform.rotate = MakeQuaternionFromEuler({
					0.0f, 0.0f, spriteEuler.z
				});
				transformChanged = true;
			}
			transformChanged |= ImGui::DragFloat2(
				SelectEditorText(editorLanguage_, "スケール###SceneTransformScale", "Scale###SceneTransformScale"),
				&entity->transform.scale.x,
				0.01f,
				0.001f,
				1000.0f
			);
		} else if (HasComponent(*entity, "TextRenderer")) {
			ImGui::TextDisabled("TextRenderer placement is edited in its active Render Space profile.");
		} else {
			transformChanged |= ImGui::DragFloat3(
				SelectEditorText(editorLanguage_, "位置###SceneTransformPosition", "Position###SceneTransformPosition"),
				&entity->transform.translate.x,
				0.05f
			);
			if (
				inspectorRotationEntityId_ != entity->id ||
				!IsSameRotation(
					inspectorRotationSource_,
					entity->transform.rotate
				)
			) {
				inspectorRotationEntityId_ = entity->id;
				inspectorRotationEuler_ =
					MakeEulerFromQuaternion(entity->transform.rotate);
				inspectorRotationSource_ = entity->transform.rotate;
			}
			if (ImGui::DragFloat3(
				SelectEditorText(editorLanguage_, "回転###SceneTransformRotation", "Rotation###SceneTransformRotation"),
				&inspectorRotationEuler_.x,
				0.01f
			)) {
				entity->transform.rotate =
					MakeQuaternionFromEuler(inspectorRotationEuler_);
				inspectorRotationSource_ = entity->transform.rotate;
				transformChanged = true;
			}
			transformChanged |= ImGui::DragFloat3(
				SelectEditorText(editorLanguage_, "スケール###SceneTransformScale", "Scale###SceneTransformScale"),
				&entity->transform.scale.x,
				0.01f,
				0.001f,
				1000.0f
			);
		}
		ImGui::EndDisabled();
		if (transformChanged) {
			document.MarkDirty();
		}
		}

		if (DrawPersistentInspectorHeader(
			sceneInspectorKey + "ComponentOverview",
			SelectEditorText(
				editorLanguage_,
				"Component概要###SceneComponentOverviewSection",
				"Component Overview###SceneComponentOverviewSection"
			)
		)) {
			DrawComponentSummary(
				*entity,
				editorSession_->GetActiveSceneId(),
				false,
				sceneSummarySelectedComponentType_
			);
		}
		const bool simpleComponentInspector =
			componentInspectorMode_ == ComponentInspectorMode::Simple;
		std::string removeComponentType;
		for (SceneComponent& component : entity->components) {
			if (
				simpleComponentInspector &&
				component.type != sceneSummarySelectedComponentType_
			) {
				continue;
			}
			ImGui::PushID(component.type.c_str());
			const char* componentLabel = component.type == "OBBCollider"
				? "Collider"
				: component.type.c_str();
			const std::string componentHeaderLabel =
				std::string(componentLabel) + "##ComponentHeader";
			const std::string foldoutKey = MakeComponentFoldoutKey(
				editorSession_->GetActiveSceneId(),
				entity->id,
				component.type
			);
			const auto savedFoldout = componentFoldoutStates_.find(foldoutKey);
			const bool wasComponentOpen = savedFoldout == componentFoldoutStates_.end()
				? true
				: savedFoldout->second;
			ImGui::SetNextItemOpen(wasComponentOpen, ImGuiCond_Always);
			const bool componentOpen = ImGui::CollapsingHeader(
				componentHeaderLabel.c_str(),
				ImGuiTreeNodeFlags_SpanAvailWidth
			);
			if (componentOpen != wasComponentOpen) {
				componentFoldoutStates_[foldoutKey] = componentOpen;
				SaveEditorSettings();
			}
			if (!componentOpen) {
				ImGui::PopID();
				continue;
			}
			ImGui::BeginDisabled(entityLocked || !editorSession_->IsEditing());
			if (ImGui::Checkbox(SelectEditorText(
				editorLanguage_,
				"有効###SceneComponentEnabled",
				"Enabled###SceneComponentEnabled"
			), &component.enabled)) {
				document.MarkDirty();
				editorSession_->RequestSceneReload();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(SelectEditorText(
				editorLanguage_,
				"削除###SceneComponentRemove",
				"Remove###SceneComponentRemove"
			))) {
				removeComponentType = component.type;
			}
			ImGui::EndDisabled();

			if (component.type == "MeshRenderer") {
				const bool modelEditingDisabled =
					!editorSession_->IsEditing() || entityLocked;
				auto assignModel = [&](const std::string& modelPath) {
					if (component.modelPath == modelPath) {
						return;
					}
					component.modelPath = modelPath;
					entity->modelPath = component.modelPath;
					document.MarkDirty();
					editorSession_->RequestSceneReload();
				};

				if (!component.modelPath.empty()) {
					const bool previewReady =
						modelPreviewRenderedPath_ == component.modelPath &&
						modelPreviewTexture_.ptr != 0;
					const float previewSize = std::clamp(
						ImGui::GetContentRegionAvail().x,
						180.0f,
						320.0f
					);
					if (previewReady) {
						ImGui::Image(
							ImTextureRef(static_cast<ImTextureID>(modelPreviewTexture_.ptr)),
							ImVec2(previewSize, previewSize)
						);
						if (ImGui::IsItemHovered()) {
							const ImGuiIO& io = ImGui::GetIO();
							if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
								modelPreviewYaw_ += io.MouseDelta.x * 0.01f;
								modelPreviewPitch_ = std::clamp(
									modelPreviewPitch_ + io.MouseDelta.y * 0.01f,
									-1.45f,
									1.45f
								);
							}
							if (io.MouseWheel != 0.0f) {
								modelPreviewZoom_ = std::clamp(
									modelPreviewZoom_ * (1.0f - io.MouseWheel * 0.12f),
									0.25f,
									4.0f
								);
							}
							ImGui::SetTooltip(
								"Drag to orbit | Wheel to zoom | Drop model to replace"
							);
						}
					} else {
						ImGui::Button(
							"Preparing model preview...",
							ImVec2(previewSize, previewSize)
						);
					}
					if (!modelEditingDisabled && ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
							"PROJECT_MODEL_PATH"
						)) {
							const char* droppedPath =
								static_cast<const char*>(payload->Data);
							if (droppedPath && droppedPath[0] != '\0') {
								assignModel(GetModelPathRelativeToResources(droppedPath));
							}
						}
						ImGui::EndDragDropTarget();
					}
					if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Reset Preview"))) {
						modelPreviewYaw_ = 0.65f;
						modelPreviewPitch_ = 0.25f;
						modelPreviewZoom_ = 1.0f;
					}
					ImGui::SameLine();
					ImGui::BeginDisabled(modelEditingDisabled);
					if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Clear Model"))) {
						assignModel({});
					}
					ImGui::EndDisabled();
				} else {
					ImGui::BeginDisabled(modelEditingDisabled);
					ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Drop Model Here"), ImVec2(-1.0f, 48.0f));
					ImGui::EndDisabled();
					if (!modelEditingDisabled && ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
							"PROJECT_MODEL_PATH"
						)) {
							const char* droppedPath =
								static_cast<const char*>(payload->Data);
							if (droppedPath && droppedPath[0] != '\0') {
								assignModel(GetModelPathRelativeToResources(droppedPath));
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::TextDisabled("No model assigned. Select or drop a model.");
				}

				const char* currentModel = component.modelPath.empty()
					? "None"
					: component.modelPath.c_str();
				ImGui::BeginDisabled(modelEditingDisabled);
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Model"), currentModel)) {
					if (ImGui::Selectable("None", component.modelPath.empty())) {
						assignModel({});
					}
					for (const std::string& modelPath : GetCachedModelAssetPaths()) {
						if (ImGui::Selectable(
							modelPath.c_str(),
							component.modelPath == modelPath
						)) {
							assignModel(modelPath);
						}
					}
					ImGui::EndCombo();
				}
				if (!modelEditingDisabled && ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
						"PROJECT_MODEL_PATH"
					)) {
						const char* droppedPath = static_cast<const char*>(payload->Data);
						if (droppedPath && droppedPath[0] != '\0') {
							assignModel(GetModelPathRelativeToResources(droppedPath));
						}
					}
					ImGui::EndDragDropTarget();
				}
				if (!component.modelPath.empty()) {
					ModelManager::GetInstance()->LoadModel(component.modelPath);
					Model* model = ModelManager::GetInstance()->FindModel(
						component.modelPath
					);
					if (model) {
						const std::vector<Model::MaterialSlot>& materialSlots =
							model->GetMaterialSlots();
						ImGui::SeparatorText(SelectEditorText(editorLanguage_, "マテリアル", "Materials"));
						ImGui::TextDisabled(
							"%zu meshes / %zu materials",
							model->GetSubMeshes().size(),
							materialSlots.size()
						);
						for (size_t materialIndex = 0;
							materialIndex < materialSlots.size();
							++materialIndex) {
							const Model::MaterialSlot& materialSlot =
								materialSlots[materialIndex];
							const std::string materialLabel = materialSlot.name.empty()
								? "Material " + std::to_string(materialIndex + 1)
								: materialSlot.name;
							ImGui::PushID(static_cast<int>(materialIndex));
							if (ImGui::TreeNode(materialLabel.c_str())) {
								auto overrideIt = std::find_if(
									component.meshMaterialOverrides.begin(),
									component.meshMaterialOverrides.end(),
									[&](const SceneMeshMaterialOverride& override) {
										return override.materialName == materialSlot.name;
									}
								);
								bool overrideEnabled = overrideIt !=
									component.meshMaterialOverrides.end() && overrideIt->enabled;
								if (ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Override"), &overrideEnabled)) {
									if (overrideIt == component.meshMaterialOverrides.end()) {
										component.meshMaterialOverrides.push_back({
											materialSlot.name,
											true
										});
										overrideIt = std::prev(
											component.meshMaterialOverrides.end()
										);
									} else {
										overrideIt->enabled = overrideEnabled;
									}
									document.MarkDirty();
								}

								SceneMeshMaterialOverride* override = overrideIt ==
									component.meshMaterialOverrides.end()
									? nullptr
									: &*overrideIt;
								ImGui::BeginDisabled(!overrideEnabled);
								bool materialChanged = false;
								if (override) {
									materialChanged |= ImGui::Checkbox(
										LocalizedComponentWidgetLabel(editorLanguage_, "Override Color"),
										&override->colorOverrideEnabled
									);
									ImGui::BeginDisabled(!override->colorOverrideEnabled);
									materialChanged |= ImGui::ColorEdit4(
										LocalizedComponentWidgetLabel(editorLanguage_, "Color"), &override->color.x
									);
									ImGui::EndDisabled();
									const char* texturePath = override->texturePath.empty()
										? "Using model texture"
										: override->texturePath.c_str();
									ImGui::TextWrapped("Texture: %s", texturePath);
									if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Clear Texture"))) {
										override->texturePath.clear();
										materialChanged = true;
									}
									ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Drop Texture Here"), ImVec2(-1.0f, 28.0f));
									if (ImGui::BeginDragDropTarget()) {
										if (const ImGuiPayload* payload =
											ImGui::AcceptDragDropPayload("PROJECT_TEXTURE_PATH")) {
											const char* droppedPath =
												static_cast<const char*>(payload->Data);
											if (droppedPath && droppedPath[0] != '\0') {
											override->texturePath =
													GetProjectResourcePath(droppedPath);
												materialChanged = true;
											}
										}
										ImGui::EndDragDropTarget();
									}
								}
								ImGui::EndDisabled();
								if (materialChanged) {
									document.MarkDirty();
								}
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
					}
				}
				const bool visualRotationChanged = ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Visual Rotation (radians)"),
					&component.meshVisualRotation.x,
					0.01f
				);
				if (visualRotationChanged) {
					document.MarkDirty();
					editorSession_->RequestSceneReload();
				}
				const char* currentCullMode = component.meshCullMode.empty()
					? "Back"
					: component.meshCullMode.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Cull Mode"), currentCullMode)) {
					const char* cullModes[] = { "Back", "Front", "None" };
					for (const char* cullMode : cullModes) {
						if (ImGui::Selectable(
							cullMode,
							component.meshCullMode == cullMode ||
								(component.meshCullMode.empty() &&
									std::strcmp(cullMode, "Back") == 0)
						)) {
							component.meshCullMode = cullMode;
							document.MarkDirty();
						}
					}
					ImGui::EndCombo();
				}
				bool reflectionChanged = false;
				if (!component.meshEnvironmentReflectionOverride) {
					ImGui::TextDisabled("Using Environment Reflection");
				}
				reflectionChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Override Environment Reflection"),
					&component.meshEnvironmentReflectionOverride
				);
				ImGui::BeginDisabled(
					!component.meshEnvironmentReflectionOverride
				);
				reflectionChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Reflection Intensity"),
					&component.meshEnvironmentReflectionIntensity,
					0.01f,
					0.0f,
					1.0f
				);
				ImGui::EndDisabled();
				if (component.meshEnvironmentReflectionIntensity < 0.0f) {
					component.meshEnvironmentReflectionIntensity = 0.0f;
					reflectionChanged = true;
				}
				if (component.meshEnvironmentReflectionIntensity > 1.0f) {
					component.meshEnvironmentReflectionIntensity = 1.0f;
					reflectionChanged = true;
				}
				if (reflectionChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "Environment") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool environmentChanged = false;
				environmentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Skybox Enabled"),
					&component.environmentSkyboxEnabled
				);

				auto assignSkybox = [&](const std::string& texturePath) {
					if (component.environmentSkyboxPath == texturePath) {
						return;
					}
					component.environmentSkyboxPath = texturePath;
					TextureManager::GetInstance()->LoadTexture(texturePath);
					environmentChanged = true;
					editorSession_->RequestSceneReload();
				};

				const char* currentSkybox =
					component.environmentSkyboxPath.empty()
					? "None"
					: component.environmentSkyboxPath.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Skybox DDS"), currentSkybox)) {
					if (ImGui::Selectable(
						"None",
						component.environmentSkyboxPath.empty()
					)) {
						assignSkybox({});
					}
					for (const std::string& texturePath : GetCachedTextureAssetPaths()) {
						const std::filesystem::path path =
							PathFromUtf8(texturePath);
						std::string extension = path.extension().string();
						std::transform(
							extension.begin(),
							extension.end(),
							extension.begin(),
							::tolower
						);
						if (extension != ".dds") {
							continue;
						}
						if (ImGui::Selectable(
							texturePath.c_str(),
							component.environmentSkyboxPath == texturePath
						)) {
							assignSkybox(texturePath);
						}
					}
					ImGui::EndCombo();
				}
				ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Drop DDS Skybox Here"), ImVec2(-1.0f, 38.0f));
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
						"PROJECT_TEXTURE_PATH"
					)) {
						const char* droppedPath =
							static_cast<const char*>(payload->Data);
						if (droppedPath && droppedPath[0] != '\0') {
							const std::filesystem::path path =
								PathFromUtf8(droppedPath);
							std::string extension = path.extension().string();
							std::transform(
								extension.begin(),
								extension.end(),
								extension.begin(),
								::tolower
							);
							if (extension == ".dds") {
								assignSkybox(GetProjectResourcePath(droppedPath));
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
				environmentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Skybox Intensity"),
					&component.environmentSkyboxIntensity,
					0.01f,
					0.0f,
					10.0f
				);
				environmentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Reflection Intensity"),
					&component.environmentReflectionIntensity,
					0.01f,
					0.0f,
					1.0f
				);
				if (component.environmentSkyboxIntensity < 0.0f) {
					component.environmentSkyboxIntensity = 0.0f;
					environmentChanged = true;
				}
				if (component.environmentReflectionIntensity < 0.0f) {
					component.environmentReflectionIntensity = 0.0f;
					environmentChanged = true;
				}
				if (component.environmentReflectionIntensity > 1.0f) {
					component.environmentReflectionIntensity = 1.0f;
					environmentChanged = true;
				}
				if (environmentChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "SpriteRenderer") {
				const char* currentTexture = component.texturePath.empty()
					? "None"
					: component.texturePath.c_str();
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Texture"), currentTexture)) {
					for (const std::string& texturePath : GetCachedTextureAssetPaths()) {
						if (ImGui::Selectable(
							texturePath.c_str(),
							component.texturePath == texturePath
						)) {
							component.texturePath = texturePath;
							entity->spriteTexturePath = component.texturePath;
							TextureManager::GetInstance()->LoadTexture(texturePath);
							document.MarkDirty();
						}
					}
					ImGui::EndCombo();
				}
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
						"PROJECT_TEXTURE_PATH"
					)) {
						const char* droppedPath = static_cast<const char*>(payload->Data);
						if (droppedPath && droppedPath[0] != '\0') {
							component.texturePath = GetProjectResourcePath(droppedPath);
							entity->spriteTexturePath = component.texturePath;
							TextureManager::GetInstance()->LoadTexture(
								component.texturePath
							);
							document.MarkDirty();
						}
					}
					ImGui::EndDragDropTarget();
				}
				bool spriteChanged = false;
				spriteChanged |= ImGui::DragFloat2(
					LocalizedComponentWidgetLabel(editorLanguage_, "Size"),
					&component.spriteSize.x,
					1.0f,
					1.0f,
					8192.0f
				);
				spriteChanged |= ImGui::DragFloat2(
					LocalizedComponentWidgetLabel(editorLanguage_, "Anchor"),
					&component.spriteAnchor.x,
					0.01f,
					0.0f,
					1.0f
				);
				const char* renderSpaces[] = { "Scene2D", "ScreenOverlay" };
				int renderSpaceIndex = component.spriteRenderSpace == "ScreenOverlay" ? 1 : 0;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Render Space"),
					&renderSpaceIndex,
					renderSpaces,
					IM_ARRAYSIZE(renderSpaces)
				)) {
					component.spriteRenderSpace = renderSpaces[renderSpaceIndex];
					spriteChanged = true;
				}
				spriteChanged |= ImGui::DragFloat2(
					LocalizedComponentWidgetLabel(editorLanguage_, "Viewport Anchor"),
					&component.spriteViewportAnchor.x,
					0.01f,
					0.0f,
					1.0f
				);
				spriteChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Color"),
					&component.spriteColor.x
				);
				spriteChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Flip X"), &component.spriteFlipX);
				ImGui::SameLine();
				spriteChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Flip Y"), &component.spriteFlipY);
				if (spriteChanged) {
					entity->spriteSize = component.spriteSize;
					entity->spriteAnchor = component.spriteAnchor;
					entity->spriteColor = component.spriteColor;
					entity->spriteFlipX = component.spriteFlipX;
					entity->spriteFlipY = component.spriteFlipY;
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "TextRenderer") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool textChanged = false;
				textChanged |= InputTextMultilineString(LocalizedComponentWidgetLabel(editorLanguage_, "Text"), component.textValue);
				textChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Font Family"), component.textFontFamily);
				textChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Font Size"), &component.textFontSize, 1.0f, 1.0f, 512.0f
				);
				const char* renderSpaces[] = { "ScreenOverlay", "Scene2D" };
				int renderSpaceIndex = component.textRenderSpace == "Scene2D" ? 1 : 0;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Render Space"), &renderSpaceIndex, renderSpaces, IM_ARRAYSIZE(renderSpaces)
				)) {
					component.textRenderSpace = renderSpaces[renderSpaceIndex];
					textChanged = true;
				}
				const char* weights[] = { "Regular", "Bold" };
				int weightIndex = component.textFontWeight == "Bold" ? 1 : 0;
				if (ImGui::Combo(LocalizedComponentWidgetLabel(editorLanguage_, "Weight"), &weightIndex, weights, IM_ARRAYSIZE(weights))) {
					component.textFontWeight = weights[weightIndex];
					textChanged = true;
				}
				const char* styles[] = { "Normal", "Italic" };
				int styleIndex = component.textFontStyle == "Italic" ? 1 : 0;
				if (ImGui::Combo(LocalizedComponentWidgetLabel(editorLanguage_, "Style"), &styleIndex, styles, IM_ARRAYSIZE(styles))) {
					component.textFontStyle = styles[styleIndex];
					textChanged = true;
				}
				textChanged |= ImGui::ColorEdit4(LocalizedComponentWidgetLabel(editorLanguage_, "Color"), &component.textColor.x);
				textChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Opacity"), &component.textOpacity, 0.01f, 0.0f, 1.0f
				);
				const char* horizontalAlignments[] = { "Left", "Center", "Right" };
				int horizontalIndex = component.textHorizontalAlignment == "Center"
					? 1 : component.textHorizontalAlignment == "Right" ? 2 : 0;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Horizontal Align"), &horizontalIndex, horizontalAlignments,
					IM_ARRAYSIZE(horizontalAlignments)
				)) {
					component.textHorizontalAlignment = horizontalAlignments[horizontalIndex];
					textChanged = true;
				}
				const char* verticalAlignments[] = { "Top", "Center", "Bottom" };
				int verticalIndex = component.textVerticalAlignment == "Center"
					? 1 : component.textVerticalAlignment == "Bottom" ? 2 : 0;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Vertical Align"), &verticalIndex, verticalAlignments,
					IM_ARRAYSIZE(verticalAlignments)
				)) {
					component.textVerticalAlignment = verticalAlignments[verticalIndex];
					textChanged = true;
				}
				const char* wrapModes[] = { "NoWrap", "Word" };
				int wrapIndex = component.textWrapMode == "Word" ? 1 : 0;
				if (ImGui::Combo(LocalizedComponentWidgetLabel(editorLanguage_, "Wrap"), &wrapIndex, wrapModes, IM_ARRAYSIZE(wrapModes))) {
					component.textWrapMode = wrapModes[wrapIndex];
					textChanged = true;
				}
				const char* overflowModes[] = { "Overflow", "Clip", "Ellipsis" };
				int overflowIndex = component.textOverflowMode == "Clip"
					? 1 : component.textOverflowMode == "Ellipsis" ? 2 : 0;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Overflow"), &overflowIndex, overflowModes, IM_ARRAYSIZE(overflowModes)
				)) {
					component.textOverflowMode = overflowModes[overflowIndex];
					textChanged = true;
				}
				textChanged |= ImGui::DragFloat2(
					LocalizedComponentWidgetLabel(editorLanguage_, "Layout Size"), &component.textLayoutSize.x, 1.0f, 0.0f, 4096.0f
				);
				textChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Character Spacing"), &component.textCharacterSpacing, 0.1f, -32.0f, 128.0f
				);
				textChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Line Spacing"), &component.textLineSpacing, 0.01f, 0.1f, 8.0f
				);
				textChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Outline"), &component.textOutlineEnabled);
				if (component.textOutlineEnabled) {
					textChanged |= ImGui::ColorEdit4(LocalizedComponentWidgetLabel(editorLanguage_, "Outline Color"), &component.textOutlineColor.x);
					textChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Outline Width"), &component.textOutlineWidth, 0.1f, 0.0f, 32.0f
					);
				}
				textChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Shadow"), &component.textShadowEnabled);
				if (component.textShadowEnabled) {
					textChanged |= ImGui::ColorEdit4(LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Color"), &component.textShadowColor.x);
					textChanged |= ImGui::DragFloat2(
						LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Offset"), &component.textShadowOffset.x, 0.1f, -128.0f, 128.0f
					);
				}
				if (!component.textHasPlacementProfiles) {
					const Vector3 legacyEuler = MakeEulerFromQuaternion(entity->transform.rotate);
					Text2DPlacement legacyPlacement{};
					legacyPlacement.position = { entity->transform.translate.x, entity->transform.translate.y };
					legacyPlacement.rotation = legacyEuler.z;
					legacyPlacement.scale = { entity->transform.scale.x, entity->transform.scale.y };
					legacyPlacement.pivot = component.textPivot;
					legacyPlacement.viewportAnchor = component.textViewportAnchor;
					legacyPlacement.sortingOrder = component.textSortingOrder;
					legacyPlacement.clipEnabled = component.textClipEnabled;
					component.textOverlayPlacement = legacyPlacement;
					component.textScene2DPlacement = legacyPlacement;
					component.textHasPlacementProfiles = true;
					textChanged = true;
				}
				Text2DPlacement& placement = component.textRenderSpace == "Scene2D"
					? component.textScene2DPlacement : component.textOverlayPlacement;
				ImGui::SeparatorText(component.textRenderSpace == "Scene2D"
					? SelectEditorText(editorLanguage_, "配置: Scene 2D", "Placement: Scene 2D")
					: SelectEditorText(editorLanguage_, "配置: Screen Overlay", "Placement: Screen Overlay"));
				if (component.textRenderSpace == "ScreenOverlay") {
					textChanged |= ImGui::DragFloat2(
						SelectEditorText(editorLanguage_, "Viewport Anchor###TextViewportAnchor", "Viewport Anchor###TextViewportAnchor"), &placement.viewportAnchor.x, 0.01f, 0.0f, 1.0f
					);
				}
				textChanged |= ImGui::DragFloat2(SelectEditorText(editorLanguage_, "位置###TextPlacementPosition", "Position###TextPlacementPosition"), &placement.position.x, 0.5f);
				textChanged |= ImGui::DragFloat(SelectEditorText(editorLanguage_, "回転###TextPlacementRotation", "Rotation###TextPlacementRotation"), &placement.rotation, 0.01f);
				textChanged |= ImGui::DragFloat2(
					SelectEditorText(editorLanguage_, "スケール###TextPlacementScale", "Scale###TextPlacementScale"), &placement.scale.x, 0.01f, 0.001f, 1000.0f
				);
				textChanged |= ImGui::DragFloat2(SelectEditorText(editorLanguage_, "Pivot###TextPlacementPivot", "Pivot###TextPlacementPivot"), &placement.pivot.x, 0.01f, 0.0f, 1.0f);
				textChanged |= ImGui::DragInt(SelectEditorText(editorLanguage_, "描画順###TextSortingOrder", "Sorting Order###TextSortingOrder"), &placement.sortingOrder);
				textChanged |= ImGui::Checkbox(SelectEditorText(editorLanguage_, "クリップ###TextPlacementClip", "Clip###TextPlacementClip"), &placement.clipEnabled);
				if (textChanged) {
					component.textFontSize = std::clamp(component.textFontSize, 1.0f, 512.0f);
					component.textOpacity = std::clamp(component.textOpacity, 0.0f, 1.0f);
					component.textLayoutSize.x = std::clamp(component.textLayoutSize.x, 0.0f, 4096.0f);
					component.textLayoutSize.y = std::clamp(component.textLayoutSize.y, 0.0f, 4096.0f);
					component.textLineSpacing = std::clamp(component.textLineSpacing, 0.1f, 8.0f);
					component.textOutlineWidth = std::clamp(component.textOutlineWidth, 0.0f, 32.0f);
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "Light") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool lightChanged = false;
				const char* lightTypes[] = {
					"Directional",
					"Point",
					"Spot"
				};
				int lightTypeIndex = component.lightType == "Directional"
					? 0
					: component.lightType == "Spot" ? 2 : 1;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Type"),
					&lightTypeIndex,
					lightTypes,
					IM_ARRAYSIZE(lightTypes)
				)) {
					component.lightType = lightTypes[lightTypeIndex];
					if (component.lightType == "Point") {
						component.lightCastsShadow = false;
					}
					lightChanged = true;
				}
				lightChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Color"),
					&component.lightColor.x,
					ImGuiColorEditFlags_Float
				);
				lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Intensity"),
					&component.lightIntensity,
					0.05f,
					0.0f,
					30.0f
				);

				if (component.lightType == "Point" || component.lightType == "Spot") {
					lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Range"),
						&component.lightRange,
						0.1f,
						0.1f,
						1000.0f
					);
					lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Decay"),
						&component.lightDecay,
						0.05f,
						0.0f,
						10.0f
					);
				}
				if (component.lightType == "Spot") {
					lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Inner Angle"),
						&component.lightSpotInnerAngle,
						0.25f,
						0.0f,
						component.lightSpotOuterAngle,
						"%.1f deg"
					);
					lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Outer Angle"),
						&component.lightSpotOuterAngle,
						0.25f,
						1.0f,
						89.0f,
						"%.1f deg"
					);
					component.lightSpotOuterAngle = std::clamp(
						component.lightSpotOuterAngle,
						1.0f,
						89.0f
					);
					component.lightSpotInnerAngle = std::clamp(
						component.lightSpotInnerAngle,
						0.0f,
						component.lightSpotOuterAngle
					);
				}

				if (component.lightType == "Directional") {
					const Matrix4x4 localRotation =
						MakeRotateMatrix(entity->transform.rotate);
					Vector3 localDirection{
						localRotation.m[2][0],
						localRotation.m[2][1],
						localRotation.m[2][2]
					};
					if (ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Direction"),
						&localDirection.x,
						0.01f,
						-1.0f,
						1.0f,
						"%.3f"
					)) {
						if (Math::Length(localDirection) <= 0.000001f) {
							localDirection = { 0.0f, 0.0f, 1.0f };
						} else {
							localDirection = Math::Normalize(localDirection);
						}
						Vector3 localUp{
							localRotation.m[1][0],
							localRotation.m[1][1],
							localRotation.m[1][2]
						};
						entity->transform.rotate =
							MakeLookRotationQuaternion(localDirection, localUp);
						lightChanged = true;
					}
					ImGui::TextDisabled(
						"Local direction; parent rotation is applied. Position is the shadow focus."
					);
				} else if (component.lightType == "Spot") {
					ImGui::TextDisabled("Transform +Z is the light direction.");
				}

				if (component.lightType != "Point") {
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "影", "Shadow"));
					lightChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Cast Shadow"),
						&component.lightCastsShadow
					);
					if (component.lightCastsShadow) {
						lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Bias"),
							&component.lightShadowBias,
							0.0001f,
							0.0f,
							0.05f,
							"%.5f"
						);
						lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Normal Bias"),
							&component.lightShadowNormalBias,
							0.001f,
							0.0f,
							0.2f,
							"%.4f"
						);
						lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Strength"),
							&component.lightShadowStrength,
							0.01f,
							0.0f,
							1.0f
						);
						if (component.lightType == "Directional") {
							lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Distance"),
								&component.lightShadowDistance,
								0.5f,
								1.0f,
								1000.0f
							);
							lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Orthographic Size"),
								&component.lightShadowOrthographicSize,
								0.5f,
								1.0f,
								1000.0f
							);
							lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Near Clip"),
								&component.lightShadowNearClip,
								0.01f,
								0.001f,
								1000.0f
							);
							lightChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Far Clip"),
								&component.lightShadowFarClip,
								0.5f,
								1.0f,
								5000.0f
							);
							lightChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Texel Snap"),
								&component.lightShadowTexelSnap
							);
						}
					}
				}

				ImGui::SeparatorText("Scene Lighting");
				const char* shadowMapLabels[] = { "1024", "2048", "4096" };
				SceneLightingSettings lightingSettings =
					document.GetLightingSettings();
				int shadowMapIndex = lightingSettings.shadowMapSize <= 1024
					? 0
					: lightingSettings.shadowMapSize <= 2048 ? 1 : 2;
				if (ImGui::Combo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Shadow Map Size"),
					&shadowMapIndex,
					shadowMapLabels,
					IM_ARRAYSIZE(shadowMapLabels)
				)) {
					const uint32_t sizes[] = { 1024, 2048, 4096 };
					lightingSettings.shadowMapSize = sizes[shadowMapIndex];
					document.SetLightingSettings(lightingSettings);
				}
				if (lightChanged) {
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
						component.lightIntensity,
						0.0f
					);
					component.lightRange = (std::max)(component.lightRange, 0.1f);
					component.lightDecay = (std::max)(component.lightDecay, 0.0f);
					component.lightShadowNearClip = (std::max)(
						component.lightShadowNearClip,
						0.001f
					);
					component.lightShadowFarClip = (std::max)(
						component.lightShadowFarClip,
						component.lightShadowNearClip + 0.001f
					);
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "Camera") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool cameraChanged = false;
				bool isMainCamera = component.cameraIsMain;
				if (ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Main Camera"), &isMainCamera)) {
					if (isMainCamera) {
						for (SceneEntity& candidate : document.GetEntities()) {
							if (SceneComponent* cameraComponent =
								FindComponent(candidate, "Camera")) {
								cameraComponent->cameraIsMain = false;
							}
						}
					}
					component.cameraIsMain = isMainCamera;
					cameraChanged = true;
				}

				constexpr float radiansToDegrees = 57.2957795f;
				constexpr float degreesToRadians = 0.0174532925f;
				float fovDegrees = component.cameraFovY * radiansToDegrees;
				if (ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "FOV Y"), &fovDegrees, 0.5f, 1.0f, 179.0f)) {
					component.cameraFovY = fovDegrees * degreesToRadians;
					cameraChanged = true;
				}
				cameraChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Near Clip"),
					&component.cameraNearClip,
					0.01f,
					0.001f,
					100.0f
				);
				cameraChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Far Clip"),
					&component.cameraFarClip,
					1.0f,
					1.0f,
					10000.0f
				);
				if (component.cameraFarClip <= component.cameraNearClip) {
					component.cameraFarClip = component.cameraNearClip + 0.001f;
					cameraChanged = true;
				}
				if (cameraChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "MonitorRenderer") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool monitorChanged = false;
				struct MonitorResolutionPreset {
					const char* label;
					uint32_t width;
					uint32_t height;
				};
				constexpr MonitorResolutionPreset monitorPresets[] = {
					{ "Square 512", 512, 512 },
					{ "HD 16:9", 1280, 720 },
					{ "Full HD 16:9", 1920, 1080 },
					{ "Portrait 9:16", 720, 1280 },
					{ "Wide 21:9", 1792, 768 },
					{ "Low 16:9", 640, 360 },
					{ "Custom", 0, 0 },
				};
				const SceneEntity* currentCameraEntity = nullptr;
				if (component.monitorCameraEntityId != 0) {
					const SceneEntity* idCameraEntity =
						document.FindEntity(component.monitorCameraEntityId);
					if (
						component.monitorCameraName.empty() ||
						(
							idCameraEntity &&
							idCameraEntity->name == component.monitorCameraName
						)
					) {
						currentCameraEntity = idCameraEntity;
					}
				}
				if (
					!currentCameraEntity &&
					!component.monitorCameraName.empty()
				) {
					currentCameraEntity =
						document.FindEntityByName(component.monitorCameraName);
				}
				char cameraNameBuffer[128]{};
				strncpy_s(
					cameraNameBuffer,
					component.monitorCameraName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Camera Name"),
					cameraNameBuffer,
					sizeof(cameraNameBuffer)
				)) {
					component.monitorCameraName = cameraNameBuffer;
					component.monitorCameraEntityId = 0;
					monitorChanged = true;
				}

				const std::string currentCameraLabel = currentCameraEntity
					? currentCameraEntity->name
					: (
						component.monitorCameraName.empty()
						? std::string("Select Camera...")
						: component.monitorCameraName
					);
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Camera Entity"), currentCameraLabel.c_str())) {
					for (const SceneEntity& candidate : document.GetEntities()) {
						const SceneComponent* cameraComponent =
							FindEnabledComponent(candidate, "Camera");
						if (!cameraComponent) {
							continue;
						}
						std::string cameraLabel = candidate.name;
						if (cameraComponent->cameraIsMain) {
							cameraLabel += " (Main)";
						}
						if (HasComponent(candidate, "PlayerBehavior")) {
							cameraLabel += " (Gameplay)";
						}
						const bool selected =
							component.monitorCameraEntityId == candidate.id ||
							(
								component.monitorCameraEntityId == 0 &&
								component.monitorCameraName == candidate.name
							);
						if (ImGui::Selectable(
							cameraLabel.c_str(),
							selected
						)) {
							component.monitorCameraEntityId = candidate.id;
							component.monitorCameraName = candidate.name;
							monitorChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (currentCameraEntity) {
					ImGui::TextDisabled(
						"Bound Camera ID: %llu",
						static_cast<unsigned long long>(currentCameraEntity->id)
					);
				}
				if (
					component.monitorCameraEntityId != 0 ||
					!component.monitorCameraName.empty()
				) {
					const SceneEntity* selectedCamera = nullptr;
					const SceneEntity* idCamera = nullptr;
					if (component.monitorCameraEntityId != 0) {
						idCamera =
							document.FindEntity(component.monitorCameraEntityId);
						selectedCamera = idCamera;
					}
					const SceneEntity* namedCamera = nullptr;
					if (!component.monitorCameraName.empty()) {
						namedCamera =
							document.FindEntityByName(component.monitorCameraName);
					}
					if (
						idCamera &&
						namedCamera &&
						idCamera->id != namedCamera->id
					) {
						selectedCamera = namedCamera;
						ImGui::TextColored(
							ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
							"Stored camera ID and name differ; name is used"
						);
						if (ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Repair Camera ID"))) {
							component.monitorCameraEntityId = namedCamera->id;
							monitorChanged = true;
						}
					} else if (
						namedCamera &&
						component.monitorCameraEntityId != namedCamera->id
					) {
						selectedCamera = namedCamera;
						ImGui::TextDisabled(
							"Camera name resolves, but ID is not bound"
						);
						if (ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Repair Camera ID"))) {
							component.monitorCameraEntityId = namedCamera->id;
							monitorChanged = true;
						}
					} else if (!selectedCamera) {
						selectedCamera = namedCamera;
					}
					if (
						!selectedCamera ||
						!FindEnabledComponent(*selectedCamera, "Camera")
					) {
						ImGui::TextColored(
							ImVec4(0.95f, 0.55f, 0.25f, 1.0f),
							"Target camera is missing"
						);
					} else if (!selectedCamera->active) {
						ImGui::TextColored(
							ImVec4(0.95f, 0.55f, 0.25f, 1.0f),
							"Target camera is inactive"
						);
					} else if (HasComponent(*selectedCamera, "PlayerBehavior")) {
						ImGui::TextDisabled(
							"Target is the gameplay/player camera"
						);
					}
				}

				const char* currentPreset = component.monitorResolutionPreset.empty()
					? "Custom"
					: component.monitorResolutionPreset.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Resolution Preset"), currentPreset)) {
					for (const MonitorResolutionPreset& preset : monitorPresets) {
						const bool selected =
							component.monitorResolutionPreset == preset.label ||
							(
								component.monitorResolutionPreset.empty() &&
								std::string(preset.label) == "Custom"
							);
						if (ImGui::Selectable(preset.label, selected)) {
							component.monitorResolutionPreset = preset.label;
							if (preset.width != 0 && preset.height != 0) {
								component.monitorWidth = preset.width;
								component.monitorHeight = preset.height;
							}
							monitorChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				int monitorWidth = static_cast<int>(component.monitorWidth);
				int monitorHeight = static_cast<int>(component.monitorHeight);
				if (ImGui::DragInt(LocalizedComponentWidgetLabel(editorLanguage_, "Width"), &monitorWidth, 16.0f, 64, 2048)) {
					component.monitorWidth =
						static_cast<uint32_t>(std::clamp(monitorWidth, 64, 2048));
					component.monitorResolutionPreset = "Custom";
					monitorChanged = true;
				}
				if (ImGui::DragInt(LocalizedComponentWidgetLabel(editorLanguage_, "Height"), &monitorHeight, 16.0f, 64, 2048)) {
					component.monitorHeight =
						static_cast<uint32_t>(std::clamp(monitorHeight, 64, 2048));
					component.monitorResolutionPreset = "Custom";
					monitorChanged = true;
				}
				monitorChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Hide Self In View"),
					&component.monitorHideSelf
				);
				if (monitorChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "CameraSwitcher") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool switcherChanged = false;
				ImGui::TextDisabled(
					"Cycles registered cameras in Play Mode"
				);
				const char* currentKey = component.cameraSwitchTriggerKey.empty()
					? "F5"
					: component.cameraSwitchTriggerKey.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Switch Key"), currentKey)) {
					for (const char* key : {
						"F1", "F2", "F3", "F4", "F5", "F6",
						"F7", "F8", "F9", "F10", "F11", "F12"
					}) {
						if (ImGui::Selectable(
							key,
							component.cameraSwitchTriggerKey == key ||
								(component.cameraSwitchTriggerKey.empty() &&
									std::strcmp(key, "F5") == 0)
						)) {
							component.cameraSwitchTriggerKey = key;
							switcherChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				switcherChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Wrap To First"),
					&component.cameraSwitchWrap
				);
				int removeCameraIndex = -1;
				int moveCameraIndex = -1;
				int moveCameraDirection = 0;
				for (size_t cameraIndex = 0;
					cameraIndex < component.cameraSwitchEntries.size();
					++cameraIndex) {
					SceneCameraSwitchEntry& entry =
						component.cameraSwitchEntries[cameraIndex];
					ImGui::PushID(static_cast<int>(cameraIndex));
					const SceneEntity* selectedCamera =
						entry.cameraEntityId != 0
						? document.FindEntity(entry.cameraEntityId)
						: nullptr;
					if (
						!entry.cameraEntityName.empty() &&
						(!selectedCamera ||
							selectedCamera->name != entry.cameraEntityName)
					) {
						selectedCamera = document.FindEntityByName(
							entry.cameraEntityName
						);
					}
					const std::string cameraLabel = selectedCamera
						? selectedCamera->name
						: "Select Camera...";
					if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Camera"), cameraLabel.c_str())) {
						for (const SceneEntity& candidate : document.GetEntities()) {
							if (!FindEnabledComponent(candidate, "Camera")) {
								continue;
							}
							std::string label = candidate.name;
							if (const SceneComponent* cameraComponent =
								FindEnabledComponent(candidate, "Camera");
								cameraComponent && cameraComponent->cameraIsMain) {
								label += " (Main)";
							}
							if (ImGui::Selectable(
								label.c_str(),
								selectedCamera && selectedCamera->id == candidate.id
							)) {
								entry.cameraEntityId = candidate.id;
								entry.cameraEntityName = candidate.name;
								switcherChanged = true;
							}
						}
						ImGui::EndCombo();
					}
					ImGui::BeginDisabled(cameraIndex == 0);
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "上へ###CameraSwitcherUp", "Up###CameraSwitcherUp"))) {
						moveCameraIndex = static_cast<int>(cameraIndex);
						moveCameraDirection = -1;
					}
					ImGui::EndDisabled();
					ImGui::SameLine();
					ImGui::BeginDisabled(
						cameraIndex + 1 >= component.cameraSwitchEntries.size()
					);
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "下へ###CameraSwitcherDown", "Down###CameraSwitcherDown"))) {
						moveCameraIndex = static_cast<int>(cameraIndex);
						moveCameraDirection = 1;
					}
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "削除###CameraSwitcherRemove", "Remove###CameraSwitcherRemove"))) {
						removeCameraIndex = static_cast<int>(cameraIndex);
					}
					ImGui::PopID();
				}
				if (moveCameraIndex >= 0) {
					std::swap(
						component.cameraSwitchEntries[moveCameraIndex],
						component.cameraSwitchEntries[
							moveCameraIndex + moveCameraDirection
						]
					);
					switcherChanged = true;
				}
				if (removeCameraIndex >= 0) {
					component.cameraSwitchEntries.erase(
						component.cameraSwitchEntries.begin() + removeCameraIndex
					);
					switcherChanged = true;
				}
				SceneCameraSwitchEntry nextCameraEntry{};
				for (const SceneEntity& candidate : document.GetEntities()) {
					if (!FindEnabledComponent(candidate, "Camera")) {
						continue;
					}
					const bool alreadyRegistered = std::any_of(
						component.cameraSwitchEntries.begin(),
						component.cameraSwitchEntries.end(),
						[&candidate](const SceneCameraSwitchEntry& existing) {
							return existing.cameraEntityId == candidate.id ||
								(!existing.cameraEntityName.empty() &&
									existing.cameraEntityName == candidate.name);
						}
					);
					if (!alreadyRegistered) {
						nextCameraEntry.cameraEntityId = candidate.id;
						nextCameraEntry.cameraEntityName = candidate.name;
						break;
					}
				}
				ImGui::BeginDisabled(nextCameraEntry.cameraEntityId == 0);
				if (ImGui::Button(LocalizedComponentWidgetLabel(editorLanguage_, "Add Camera"))) {
					component.cameraSwitchEntries.push_back(
						std::move(nextCameraEntry)
					);
					switcherChanged = true;
				}
				ImGui::EndDisabled();
				if (switcherChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "ThirdPersonCamera") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool thirdPersonChanged = false;
				const SceneEntity* targetEntity =
					component.thirdPersonTargetEntityId != 0
					? document.FindEntity(component.thirdPersonTargetEntityId)
					: nullptr;
				if (
					!component.thirdPersonTargetEntityName.empty() &&
					(!targetEntity ||
						targetEntity->name != component.thirdPersonTargetEntityName)
				) {
					targetEntity = document.FindEntityByName(
						component.thirdPersonTargetEntityName
					);
				}
				const std::string targetLabel = targetEntity
					? targetEntity->name
					: "Auto / Legacy Target";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity"), targetLabel.c_str())) {
					if (ImGui::Selectable(
						"Auto / Legacy Target",
						component.thirdPersonTargetEntityId == 0 &&
							component.thirdPersonTargetEntityName.empty()
					)) {
						component.thirdPersonTargetEntityId = 0;
						component.thirdPersonTargetEntityName.clear();
						thirdPersonChanged = true;
					}
					for (const SceneEntity& candidate : document.GetEntities()) {
						if (ImGui::Selectable(
							candidate.name.c_str(),
							targetEntity && targetEntity->id == candidate.id
						)) {
							component.thirdPersonTargetEntityId = candidate.id;
							component.thirdPersonTargetEntityName = candidate.name;
							thirdPersonChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (!targetEntity) {
					ImGui::TextDisabled(
						"Auto uses this Entity when it has a model/behavior, otherwise Player"
					);
				}
				thirdPersonChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Allow Mouse Input"),
					&component.thirdPersonAllowMouseInput
				);
				const char* yawReference =
					component.thirdPersonYawReference == "Target"
					? "Target"
					: "World";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Yaw Reference"), yawReference)) {
					for (const char* reference : { "World", "Target" }) {
						if (ImGui::Selectable(
							reference,
							component.thirdPersonYawReference == reference
						)) {
							component.thirdPersonYawReference = reference;
							thirdPersonChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::TextDisabled(
					"World: fixed orbit direction / Target: inherit target yaw"
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Distance"),
					&component.thirdPersonDistance,
					0.05f,
					0.01f,
					30.0f
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Aim Distance"),
					&component.thirdPersonAimDistance,
					0.05f,
					0.01f,
					30.0f
				);
				thirdPersonChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Aim Mode Enabled"),
					&component.thirdPersonAimModeEnabled
				);
				thirdPersonChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Offset"),
					&component.thirdPersonTargetOffset.x,
					0.01f
				);
				thirdPersonChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Aim Target Offset"),
					&component.thirdPersonAimTargetOffset.x,
					0.01f
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Mouse Sensitivity"),
					&component.thirdPersonMouseSensitivity,
					0.0001f,
					0.0f,
					0.1f,
					"%.4f"
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Min Pitch"),
					&component.thirdPersonMinPitch,
					0.01f,
					-1.56f,
					1.56f
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Pitch"),
					&component.thirdPersonMaxPitch,
					0.01f,
					-1.56f,
					1.56f
				);
				thirdPersonChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Occlusion Margin"),
					&component.thirdPersonOcclusionMargin,
					0.01f,
					0.0f,
					5.0f
				);
				thirdPersonChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Occlusion Enabled"),
					&component.thirdPersonOcclusionEnabled
				);
				thirdPersonChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Occlusion Layer Mask"),
					ImGuiDataType_U32,
					&component.thirdPersonOcclusionMask
				);
				ImGui::TextDisabled(
					"Only non-trigger colliders on matching layers block the camera."
				);
				thirdPersonChanged |= ImGui::DragFloat(
					SelectEditorText(editorLanguage_, "遮蔽時の追従時間###OcclusionPullIn", "Occlusion Pull-In Smooth Time###OcclusionPullIn"),
					&component.thirdPersonOcclusionPullInSmoothTime,
					0.01f,
					0.0f,
					5.0f,
					"%.2f s"
				);
				thirdPersonChanged |= ImGui::DragFloat(
					SelectEditorText(editorLanguage_, "遮蔽解除時の復帰時間###OcclusionRecovery", "Occlusion Recovery Smooth Time###OcclusionRecovery"),
					&component.thirdPersonOcclusionRecoverySmoothTime,
					0.01f,
					0.0f,
					5.0f,
					"%.2f s"
				);
				thirdPersonChanged |= ImGui::DragFloat(
					SelectEditorText(editorLanguage_, "位置の追従時間###ThirdPersonPositionSmooth", "Position Smooth Time###ThirdPersonPositionSmooth"),
					&component.thirdPersonPositionSmoothTime,
					0.01f,
					0.0f,
					5.0f,
					"%.2f s"
				);
				thirdPersonChanged |= ImGui::DragFloat(
					SelectEditorText(editorLanguage_, "回転の追従時間###ThirdPersonRotationSmooth", "Rotation Smooth Time###ThirdPersonRotationSmooth"),
					&component.thirdPersonRotationSmoothTime,
					0.01f,
					0.0f,
					5.0f,
					"%.2f s"
				);
				thirdPersonChanged |= ImGui::Checkbox(
					SelectEditorText(editorLanguage_, "横方向を反転###ThirdPersonInvertHorizontal", "Invert Horizontal###ThirdPersonInvertHorizontal"),
					&component.thirdPersonInvertYaw
				);
				thirdPersonChanged |= ImGui::Checkbox(
					SelectEditorText(editorLanguage_, "縦方向を反転###ThirdPersonInvertVertical", "Invert Vertical###ThirdPersonInvertVertical"),
					&component.thirdPersonInvertPitch
				);
				if (component.thirdPersonDistance < 0.01f) {
					component.thirdPersonDistance = 0.01f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonAimDistance < 0.01f) {
					component.thirdPersonAimDistance = 0.01f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonMouseSensitivity < 0.0f) {
					component.thirdPersonMouseSensitivity = 0.0f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonMaxPitch < component.thirdPersonMinPitch) {
					std::swap(
						component.thirdPersonMinPitch,
						component.thirdPersonMaxPitch
					);
					thirdPersonChanged = true;
				}
				if (component.thirdPersonOcclusionMargin < 0.0f) {
					component.thirdPersonOcclusionMargin = 0.0f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonOcclusionPullInSmoothTime < 0.0f) {
					component.thirdPersonOcclusionPullInSmoothTime = 0.0f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonOcclusionRecoverySmoothTime < 0.0f) {
					component.thirdPersonOcclusionRecoverySmoothTime = 0.0f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonPositionSmoothTime < 0.0f) {
					component.thirdPersonPositionSmoothTime = 0.0f;
					thirdPersonChanged = true;
				}
				if (component.thirdPersonRotationSmoothTime < 0.0f) {
					component.thirdPersonRotationSmoothTime = 0.0f;
					thirdPersonChanged = true;
				}
				if (thirdPersonChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "CameraPath") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool pathChanged = false;
				const char* currentTargetCamera =
					component.cameraPathTargetCameraName.empty()
					? "Main Camera / Current"
					: component.cameraPathTargetCameraName.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Target Camera Entity"), currentTargetCamera)) {
					if (ImGui::Selectable(
						"Main Camera / Current",
						component.cameraPathTargetCameraName.empty()
					)) {
						component.cameraPathTargetCameraName.clear();
						pathChanged = true;
					}
					for (const SceneEntity& candidate : document.GetEntities()) {
						if (!FindEnabledComponent(candidate, "Camera")) {
							continue;
						}
						if (ImGui::Selectable(
							candidate.name.c_str(),
							component.cameraPathTargetCameraName == candidate.name
						)) {
							component.cameraPathTargetCameraName = candidate.name;
							pathChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				const char* currentTrigger = component.cameraPathTriggerType.empty()
					? "Key"
					: component.cameraPathTriggerType.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Trigger Type"), currentTrigger)) {
					const char* triggerTypes[] = { "Manual", "Key" };
					for (const char* triggerType : triggerTypes) {
						if (ImGui::Selectable(
							triggerType,
							component.cameraPathTriggerType == triggerType
						)) {
							component.cameraPathTriggerType = triggerType;
							pathChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				char triggerKeyBuffer[32]{};
				strncpy_s(
					triggerKeyBuffer,
					component.cameraPathTriggerKey.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Trigger Key"),
					triggerKeyBuffer,
					sizeof(triggerKeyBuffer)
				)) {
					component.cameraPathTriggerKey = triggerKeyBuffer;
					pathChanged = true;
				}
				pathChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Enter Duration"),
					&component.cameraPathEnterDuration,
					0.01f,
					0.0f,
					60.0f
				);
				pathChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Exit Duration"),
					&component.cameraPathExitDuration,
					0.01f,
					0.0f,
					60.0f
				);
				const char* currentInterpolation =
					component.cameraPathInterpolation.empty()
					? "Linear"
					: component.cameraPathInterpolation.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Interpolation"), currentInterpolation)) {
					const char* interpolations[] = { "Linear", "CatmullRom" };
					for (const char* interpolation : interpolations) {
						if (ImGui::Selectable(
							interpolation,
							component.cameraPathInterpolation == interpolation
						)) {
							component.cameraPathInterpolation = interpolation;
							pathChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				const char* currentEasing =
					component.cameraPathDefaultEasing.empty()
					? "SmoothStep"
					: component.cameraPathDefaultEasing.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Default Easing"), currentEasing)) {
					const char* easings[] = {
						"Linear",
						"EaseIn",
						"EaseOut",
						"EaseInOut",
						"SmoothStep"
					};
					for (const char* easing : easings) {
						if (ImGui::Selectable(
							easing,
							component.cameraPathDefaultEasing == easing
						)) {
							component.cameraPathDefaultEasing = easing;
							pathChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				pathChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Return To Previous Camera"),
					&component.cameraPathReturnToPreviousCamera
				);
				pathChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Start From Current Camera"),
					&component.cameraPathStartFromCurrentCamera
				);
				pathChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Auto Collect Child Points"),
					&component.cameraPathAutoCollectChildPoints
				);
				if (component.cameraPathEnterDuration < 0.0f) {
					component.cameraPathEnterDuration = 0.0f;
					pathChanged = true;
				}
				if (component.cameraPathExitDuration < 0.0f) {
					component.cameraPathExitDuration = 0.0f;
					pathChanged = true;
				}
				if (pathChanged) {
					document.MarkDirty();
				}

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "子Point", "Child Points"));
				uint32_t pointCount = 0;
				for (const SceneEntity& candidate : document.GetEntities()) {
					if (candidate.parentId != entity->id) {
						continue;
					}
					if (!HasComponent(candidate, "CameraPathPoint")) {
						continue;
					}
					ImGui::PushID(static_cast<int>(candidate.id));
					ImGui::Text("%02u: %s", pointCount, candidate.name.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Select"))) {
						selectedEntityId_ = candidate.id;
					}
					ImGui::PopID();
					++pointCount;
				}
				if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Add Point"))) {
					char pointName[32]{};
					sprintf_s(pointName, "Point_%02u", pointCount);
					SceneEntity& point = document.CreateEntity(pointName, entity->id);
					point.transform.translate = {
						0.0f,
						0.0f,
						static_cast<float>(pointCount) * 5.0f
					};
					document.AddComponent(point.id, "CameraPathPoint");
					point.components.back().cameraPathPointDurationToNext = 1.0f;
					point.components.back().cameraPathPointEasingToNext =
						"SmoothStep";
					selectedEntityId_ = point.id;
					editorSession_->RequestSceneReload();
				}
				ImGui::EndDisabled();
			} else if (component.type == "CameraPathPoint") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool pointChanged = false;
				pointChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Duration To Next"),
					&component.cameraPathPointDurationToNext,
					0.01f,
					0.0f,
					60.0f
				);
				const char* currentEasing =
					component.cameraPathPointEasingToNext.empty()
					? "SmoothStep"
					: component.cameraPathPointEasingToNext.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Easing To Next"), currentEasing)) {
					const char* easings[] = {
						"Linear",
						"EaseIn",
						"EaseOut",
						"EaseInOut",
						"SmoothStep"
					};
					for (const char* easing : easings) {
						if (ImGui::Selectable(
							easing,
							component.cameraPathPointEasingToNext == easing
						)) {
							component.cameraPathPointEasingToNext = easing;
							pointChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (component.cameraPathPointDurationToNext < 0.0f) {
					component.cameraPathPointDurationToNext = 0.0f;
					pointChanged = true;
				}
				if (pointChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "EntityReference") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool referenceChanged = false;
				char referenceNameBuffer[64]{};
				CopyTextBuffer(
					referenceNameBuffer,
					sizeof(referenceNameBuffer),
					component.entityReferenceName
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Reference Name"),
					referenceNameBuffer,
					sizeof(referenceNameBuffer)
				)) {
					component.entityReferenceName = referenceNameBuffer;
					referenceChanged = true;
				}
				char targetSceneIdBuffer[128]{};
				CopyTextBuffer(
					targetSceneIdBuffer,
					sizeof(targetSceneIdBuffer),
					component.entityReferenceTarget.sceneId
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Scene Id"),
					targetSceneIdBuffer,
					sizeof(targetSceneIdBuffer)
				)) {
					component.entityReferenceTarget.sceneId = targetSceneIdBuffer;
					referenceChanged = true;
				}
				if (component.entityReferenceTarget.sceneId.empty()) {
					ImGui::TextDisabled(SelectEditorText(editorLanguage_, "Scene IDが空の場合は、このScene Instanceを対象にします。", "Empty Scene Id targets this Scene Instance."));
				}
				char instanceKeyBuffer[128]{};
				CopyTextBuffer(
					instanceKeyBuffer,
					sizeof(instanceKeyBuffer),
					component.entityReferenceTarget.instanceKey
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Instance Key"),
					instanceKeyBuffer,
					sizeof(instanceKeyBuffer)
				)) {
					component.entityReferenceTarget.instanceKey = instanceKeyBuffer;
					referenceChanged = true;
				}
				referenceChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Id"),
					ImGuiDataType_U64,
					&component.entityReferenceTarget.entityId
				);
				if (referenceChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "SceneTransition") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool transitionChanged = false;
				const SceneDescriptor* targetScene = sceneCatalog_
					? sceneCatalog_->Find(
						component.sceneTransitionTargetSceneId
					)
					: nullptr;
				const char* targetLabel = targetScene
					? targetScene->displayName.c_str()
					: "Select...";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Target Scene"), targetLabel)) {
					if (sceneCatalog_) {
						for (const SceneDescriptor& scene :
							sceneCatalog_->GetScenes()) {
							const std::string sceneLabel =
								scene.displayName + "##" + scene.id;
							if (ImGui::Selectable(
								sceneLabel.c_str(),
								component.sceneTransitionTargetSceneId ==
									scene.id
							)) {
								component.sceneTransitionTargetSceneId = scene.id;
								transitionChanged = true;
							}
						}
					}
					ImGui::EndCombo();
				}

				const char* triggerKeys[] = {
					"ENTER", "SPACE", "ESCAPE", "TAB",
					"A", "B", "C", "D", "E", "F", "G", "H",
					"I", "J", "K", "L", "M", "N", "O", "P",
					"Q", "R", "S", "T", "U", "V", "W", "X",
					"Y", "Z"
				};
				const char* triggerKey =
					component.sceneTransitionTriggerKey.empty()
						? "ENTER"
						: component.sceneTransitionTriggerKey.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Trigger Key"), triggerKey)) {
					for (const char* key : triggerKeys) {
						if (ImGui::Selectable(
							key,
							component.sceneTransitionTriggerKey == key
						)) {
							component.sceneTransitionTriggerType = "Key";
							component.sceneTransitionTriggerKey = key;
							transitionChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (transitionChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "Animator") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool animatorChanged = false;
				animatorChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"),
					&component.animatorPlayOnStart
				);
				animatorChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Loop"),
					&component.animatorLoop
				);
				animatorChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Speed"),
					&component.animatorSpeed,
					0.01f,
					-8.0f,
					8.0f
				);
				animatorChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Default Clip Index"),
					&component.animatorDefaultClip,
					1.0f,
					0,
					1024
				);
				animatorChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Transition Duration"),
					&component.animatorTransitionDuration,
					0.01f,
					0.0f,
					10.0f
				);
				const char* blendCurve =
					component.animatorBlendCurve == "Linear"
					? "Linear"
					: "SmoothStep";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Blend Curve"), blendCurve)) {
					for (const char* candidate : { "Linear", "SmoothStep" }) {
						if (ImGui::Selectable(
							candidate,
							component.animatorBlendCurve == candidate
						)) {
							component.animatorBlendCurve = candidate;
							animatorChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				component.animatorDefaultClip = (std::max)(
					component.animatorDefaultClip,
					0
				);
				component.animatorTransitionDuration = (std::max)(
					component.animatorTransitionDuration,
					0.0f
				);
				if (animatorChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "AudioSource") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool audioChanged = DrawAudioClipAssetField(LocalizedComponentWidgetLabel(editorLanguage_, "Clip Path"), component.audioClipPath);
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Spatial Mode"), GetAudioSpatialModeDisplayName(component.audioSpatialMode))) {
					for (const auto& [value, label] : { std::pair{ "TwoD", "TwoD Stereo" }, std::pair{ "ThreeD", "ThreeD Point" }, std::pair{ "ThreeDPointDownmix", "ThreeD Point Downmix" }, std::pair{ "ThreeDStereoArea", "ThreeD Stereo Area" } }) {
						if (ImGui::Selectable(label, component.audioSpatialMode == value)) { component.audioSpatialMode = value; audioChanged = true; }
					}
					ImGui::EndCombo();
				}
				if (IsThreeDAudioSpatialMode(component.audioSpatialMode)) {
					audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Minimum Distance"), &component.audioMinimumDistance, 0.05f, 0.0f, 10000.0f);
					audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Maximum Distance"), &component.audioMaximumDistance, 0.1f, 0.01f, 10000.0f);
					if (component.audioSpatialMode == "ThreeD") {
						ImGui::TextDisabled("ThreeD Point requires a mono clip.");
					} else if (component.audioSpatialMode == "ThreeDPointDownmix") {
						ImGui::TextDisabled("Stereo clips are downmixed to mono. Phase-opposed channels can cancel.");
						ImGui::TextDisabled("Decompress On Load avoids first-play decode and conversion work.");
					} else if (component.audioSpatialMode == "ThreeDStereoArea") {
						audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Area Width"), &component.audioStereoAreaWidth, 0.01f, 0.01f, 10000.0f);
						ImGui::TextDisabled("Stereo clip required. Width is the L/R spacing in Scene units.");
					}
					DrawAudioSpatialClipCompatibilityWarning(editorLanguage_, component);
				}
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Bus"), component.audioBus.c_str())) {
					for (const char* bus : { "BGM", "SFX", "UI", "Ambience" }) if (ImGui::Selectable(bus, component.audioBus == bus)) { component.audioBus = bus; audioChanged = true; }
					ImGui::EndCombo();
				}
				audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Volume"), &component.audioVolume, 0.01f, 0.0f, 4.0f);
				audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Pitch"), &component.audioPitch, 0.01f, 0.01f, 8.0f);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop"), &component.audioLoop);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"), &component.audioPlayOnStart);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Stop On Disable"), &component.audioStopOnDisable);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Stream From Disk"), &component.audioStreamFromDisk);
				ImGui::BeginDisabled(component.audioStreamFromDisk);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Decompress On Load"), &component.audioDecompressOnLoad);
				ImGui::EndDisabled();
				const bool persistentBgmCompatible = component.audioStreamFromDisk && component.audioBus == "BGM" && component.audioSpatialMode == "TwoD";
				ImGui::BeginDisabled(!persistentBgmCompatible);
				audioChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Persist Across Scenes"), &component.audioPersistAcrossScenes);
				ImGui::EndDisabled();
				if (component.audioBus == "BGM" && component.audioSpatialMode == "TwoD") {
					audioChanged |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "BGM Fade Seconds"), &component.audioBgmFadeSeconds, 0.05f, 0.0f, 30.0f);
				}
				if (component.audioStreamFromDisk && (component.audioBus != "BGM" || component.audioSpatialMode != "TwoD")) {
					ImGui::TextDisabled("Stream From Disk requires TwoD and BGM Bus.");
				}
				if (audioChanged) document.MarkDirty();
				ImGui::EndDisabled();
			} else if (component.type == "AudioListener") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool listenerChanged = false;
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Mode"), component.audioListenerMode.c_str())) {
					for (const char* mode : { "ActiveCamera", "Entity", "Hybrid" }) if (ImGui::Selectable(mode, component.audioListenerMode == mode)) { component.audioListenerMode = mode; listenerChanged = true; }
					ImGui::EndCombo();
				}
				ImGui::TextDisabled("Only one enabled listener is allowed per Scene.");
				if (listenerChanged) document.MarkDirty();
				ImGui::EndDisabled();
			} else if (component.type == "OBBCollider") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool colliderChanged = false;
				colliderChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Collider Active"),
					&component.colliderActive
				);
				colliderChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Is Trigger"),
					&component.colliderIsTrigger
				);
				colliderChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Collision Layer"),
					ImGuiDataType_U32,
					&component.colliderLayer
				);
				colliderChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Collision Mask"),
					ImGuiDataType_U32,
					&component.colliderMask
				);
				const char* shape = component.colliderShape == "Sphere"
					? "Sphere"
					: "Box";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Shape"), shape)) {
					for (const char* candidate : { "Box", "Sphere" }) {
						if (ImGui::Selectable(
							candidate,
							component.colliderShape == candidate
						)) {
							component.colliderShape = candidate;
							colliderChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				colliderChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Offset"),
					&component.colliderOffset.x,
					0.01f
				);
				if (component.colliderShape == "Sphere") {
					colliderChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Radius"),
						&component.colliderSphereRadius,
						0.01f,
						0.001f,
						100.0f
					);
				} else {
					colliderChanged |= ImGui::DragFloat3(
						LocalizedComponentWidgetLabel(editorLanguage_, "Size Multiplier"),
						&component.colliderSizeMultiplier.x,
						0.01f,
						0.001f,
						100.0f
					);
				}
				colliderChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Debug Visible"),
					&component.colliderDebugVisible
				);
				if (ImGui::BeginCombo(
					"Draw Mode",
					component.colliderDebugDrawMode.c_str()
				)) {
					for (const char* mode : {
						"Wireframe", "Solid", "WireframeAndSolid"
					}) {
						if (ImGui::Selectable(
							mode,
							component.colliderDebugDrawMode == mode
						)) {
							component.colliderDebugDrawMode = mode;
							colliderChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (component.colliderShape == "Sphere") {
					colliderChanged |= ImGui::SliderInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Debug Segments"),
						&component.colliderDebugSegments,
						4,
						64
					);
				}
				colliderChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Debug Color"),
					&component.colliderDebugColor.x,
					ImGuiColorEditFlags_Float
				);
				component.colliderSizeMultiplier.x = (std::max)(
					component.colliderSizeMultiplier.x,
					0.001f
				);
				component.colliderSizeMultiplier.y = (std::max)(
					component.colliderSizeMultiplier.y,
					0.001f
				);
				component.colliderSizeMultiplier.z = (std::max)(
					component.colliderSizeMultiplier.z,
					0.001f
				);
				component.colliderSphereRadius = (std::max)(
					component.colliderSphereRadius,
					0.001f
				);
				component.colliderDebugSegments = std::clamp(
					component.colliderDebugSegments,
					4,
					64
				);
				if (colliderChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "StatSet") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool statsChanged = false;
				int removeStatIndex = -1;
				for (size_t statIndex = 0; statIndex < component.stats.size(); ++statIndex) {
					SceneStatDefinition& stat = component.stats[statIndex];
					ImGui::PushID(static_cast<int>(statIndex));
					const std::string statLabel = stat.displayName.empty()
						? stat.id
						: stat.displayName;
					if (ImGui::TreeNodeEx(
						"Stat",
						ImGuiTreeNodeFlags_DefaultOpen,
						"%s",
						statLabel.empty() ? "Stat" : statLabel.c_str()
					)) {
						statsChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Id"), stat.id);
						statsChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Display Name"), stat.displayName);
						statsChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Min"), &stat.minValue, 0.1f
						);
						statsChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Max"), &stat.maxValue, 0.1f
						);
						statsChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Initial"), &stat.initialValue, 0.1f
						);
						if (stat.maxValue < stat.minValue) {
							stat.maxValue = stat.minValue;
							statsChanged = true;
						}
						const float clampedInitial = std::clamp(
							stat.initialValue,
							stat.minValue,
							stat.maxValue
						);
						if (clampedInitial != stat.initialValue) {
							stat.initialValue = clampedInitial;
							statsChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Statを削除###RemoveStat", "Remove Stat###RemoveStat"))) {
							removeStatIndex = static_cast<int>(statIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeStatIndex >= 0) {
					component.stats.erase(
						component.stats.begin() + removeStatIndex
					);
					statsChanged = true;
				}
				if (ImGui::Button(SelectEditorText(editorLanguage_, "Statを追加###AddStat", "Add Stat###AddStat"))) {
					SceneStatDefinition stat{};
					stat.id = "stat" + std::to_string(component.stats.size() + 1);
					stat.displayName = stat.id;
					component.stats.push_back(std::move(stat));
					statsChanged = true;
				}
				if (statsChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "StateMachine") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool stateMachineChanged = false;
				const char* initialPreview = component.stateMachineInitialState.empty()
					? "Select State..."
					: component.stateMachineInitialState.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Initial State"), initialPreview)) {
					for (const SceneStateDefinition& state :
						component.stateMachineStates) {
						if (ImGui::Selectable(
							state.name.c_str(),
							component.stateMachineInitialState == state.name
						)) {
							component.stateMachineInitialState = state.name;
							stateMachineChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				stateMachineChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Reset On Disable"), &component.stateMachineResetOnDisable
				);
				ImGui::TextDisabled(
					"Actions are C++ classes registered by Action Id."
				);
				int removeStateIndex = -1;
				for (size_t stateIndex = 0;
					stateIndex < component.stateMachineStates.size();
					++stateIndex) {
					SceneStateDefinition& state =
						component.stateMachineStates[stateIndex];
					ImGui::PushID(static_cast<int>(stateIndex));
					if (ImGui::TreeNodeEx(
						"State",
						ImGuiTreeNodeFlags_DefaultOpen,
						"%s",
						state.name.empty() ? "State" : state.name.c_str()
					)) {
						stateMachineChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Name"), state.name);
						stateMachineChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Action Id"), state.actionId
						);
						if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Built-in Action"), state.actionId.c_str())) {
							for (const char* actionId : {
								"Builtin.Idle", "Builtin.Move", "Builtin.MeleeAttack",
								"Builtin.MeleeComboAttack"
							}) {
								if (ImGui::Selectable(
									actionId, state.actionId == actionId
								)) {
									state.actionId = actionId;
									stateMachineChanged = true;
								}
							}
							ImGui::EndCombo();
						}

						int removeParameterIndex = -1;
						for (size_t parameterIndex = 0;
							parameterIndex < state.parameters.size();
							++parameterIndex) {
							SceneStateParameter& parameter =
								state.parameters[parameterIndex];
							ImGui::PushID(static_cast<int>(parameterIndex));
							ImGui::SeparatorText(
								parameter.name.empty() ? "Parameter" : parameter.name.c_str()
							);
							stateMachineChanged |= InputTextString(
								LocalizedComponentWidgetLabel(editorLanguage_, "Parameter Name"), parameter.name
							);
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Type"), parameter.type.c_str())) {
								for (const char* type : {
									"Float", "Int", "Bool", "String", "Input", "Entity"
								}) {
									if (ImGui::Selectable(
										type, parameter.type == type
									)) {
										parameter.type = type;
										stateMachineChanged = true;
									}
								}
								ImGui::EndCombo();
							}
							if (parameter.type == "Float") {
								stateMachineChanged |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Value"), &parameter.floatValue, 0.01f
								);
							} else if (parameter.type == "Int") {
								stateMachineChanged |= ImGui::DragInt(
									LocalizedComponentWidgetLabel(editorLanguage_, "Value"), &parameter.intValue
								);
							} else if (parameter.type == "Bool") {
								stateMachineChanged |= ImGui::Checkbox(
									LocalizedComponentWidgetLabel(editorLanguage_, "Value"), &parameter.boolValue
								);
							} else if (parameter.type == "String") {
								stateMachineChanged |= InputTextString(
									LocalizedComponentWidgetLabel(editorLanguage_, "Value"), parameter.stringValue
								);
							} else if (parameter.type == "Input") {
								const char* inputPreview = parameter.stringValue.empty()
									? "Select Input..."
									: parameter.stringValue.c_str();
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Value"), inputPreview)) {
									for (const char* inputName : {
										"Mouse Left", "Mouse Right", "Mouse Middle",
										"Space", "Enter", "Escape", "Tab",
										"A", "B", "C", "D", "E", "F", "G",
										"H", "I", "J", "K", "L", "M", "N",
										"O", "P", "Q", "R", "S", "T", "U",
										"V", "W", "X", "Y", "Z"
									}) {
										if (ImGui::Selectable(
											inputName,
											parameter.stringValue == inputName
										)) {
											parameter.stringValue = inputName;
											stateMachineChanged = true;
										}
									}
									ImGui::EndCombo();
								}
							} else if (parameter.type == "Entity") {
								const SceneEntity* selectedParameterEntity =
									parameter.entityId != 0
									? document.FindEntity(parameter.entityId)
									: nullptr;
								if (
									!selectedParameterEntity &&
									!parameter.entityName.empty()
								) {
									selectedParameterEntity = document.FindEntityByName(
										parameter.entityName
									);
								}
								const char* entityPreview = selectedParameterEntity
									? selectedParameterEntity->name.c_str()
									: "Select Entity...";
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Value"), entityPreview)) {
									for (const SceneEntity& candidate : document.GetEntities()) {
										if (ImGui::Selectable(
											candidate.name.c_str(),
											selectedParameterEntity &&
											selectedParameterEntity->id == candidate.id
										)) {
											parameter.entityId = candidate.id;
											parameter.entityName = candidate.name;
											stateMachineChanged = true;
										}
									}
									ImGui::EndCombo();
								}
							}
							if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Parameterを削除###RemoveStateParameter", "Remove Parameter###RemoveStateParameter"))) {
								removeParameterIndex = static_cast<int>(parameterIndex);
							}
							ImGui::PopID();
						}
						if (removeParameterIndex >= 0) {
							state.parameters.erase(
								state.parameters.begin() + removeParameterIndex
							);
							stateMachineChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Parameterを追加###AddStateParameter", "Add Parameter###AddStateParameter"))) {
							state.parameters.push_back(SceneStateParameter{});
							stateMachineChanged = true;
						}
						ImGui::SameLine();
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Stateを削除###RemoveState", "Remove State###RemoveState"))) {
							removeStateIndex = static_cast<int>(stateIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeStateIndex >= 0) {
					component.stateMachineStates.erase(
						component.stateMachineStates.begin() + removeStateIndex
					);
					stateMachineChanged = true;
				}
				if (ImGui::Button(SelectEditorText(editorLanguage_, "Stateを追加###AddState", "Add State###AddState"))) {
					SceneStateDefinition state{};
					state.name = "State" + std::to_string(
						component.stateMachineStates.size() + 1
					);
					component.stateMachineStates.push_back(std::move(state));
					stateMachineChanged = true;
				}
				if (stateMachineChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "TextMotion") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool textMotionChanged = false;
				int removeClipIndex = -1;
				for (size_t clipIndex = 0;
					clipIndex < component.textMotionClips.size();
					++clipIndex) {
					SceneTextMotionClip& clip = component.textMotionClips[clipIndex];
					ImGui::PushID(static_cast<int>(clipIndex));
					if (ImGui::TreeNodeEx(
						"TextMotionClip",
						ImGuiTreeNodeFlags_DefaultOpen,
						"Clip %zu: %s",
						clipIndex + 1,
						clip.id.empty() ? "<empty>" : clip.id.c_str()
					)) {
						textMotionChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Clip Id"),
							clip.id
						);
						textMotionChanged |= ImGui::Checkbox(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hold Final Pose"),
							&clip.holdFinalPose
						);
						int removeKeyframeIndex = -1;
						for (size_t keyframeIndex = 0;
							keyframeIndex < clip.keyframes.size();
							++keyframeIndex) {
							SceneTextMotionKeyframe& keyframe = clip.keyframes[keyframeIndex];
							ImGui::PushID(static_cast<int>(keyframeIndex));
							if (ImGui::TreeNodeEx(
								"TextMotionKeyframe",
								ImGuiTreeNodeFlags_DefaultOpen,
								"Keyframe %zu", keyframeIndex + 1
							)) {
								textMotionChanged |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Time Seconds"),
									&keyframe.timeSeconds,
									0.01f
								);
								textMotionChanged |= ImGui::DragFloat2(
									LocalizedComponentWidgetLabel(editorLanguage_, "Position Offset"),
									&keyframe.positionOffset.x,
									0.5f
								);
								textMotionChanged |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Rotation Offset"),
									&keyframe.rotationOffset,
									0.5f
								);
								textMotionChanged |= ImGui::DragFloat2(
									LocalizedComponentWidgetLabel(editorLanguage_, "Scale Multiplier"),
									&keyframe.scaleMultiplier.x,
									0.01f
								);
								textMotionChanged |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Opacity Multiplier"),
									&keyframe.opacityMultiplier,
									0.01f
								);
								if (ImGui::BeginCombo(
									LocalizedComponentWidgetLabel(editorLanguage_, "Easing To Next"),
									keyframe.easingToNext.c_str()
								)) {
									for (const char* easing : {
										"Linear", "EaseIn", "EaseOut", "EaseInOut", "SmoothStep"
									}) {
										if (ImGui::Selectable(
											easing,
											keyframe.easingToNext == easing
										)) {
											keyframe.easingToNext = easing;
											textMotionChanged = true;
										}
									}
									ImGui::EndCombo();
								}
								if (ImGui::SmallButton(SelectEditorText(
									editorLanguage_, "Keyframeを削除###RemoveTextMotionKeyframe",
									"Remove Keyframe###RemoveTextMotionKeyframe"
								))) {
									removeKeyframeIndex = static_cast<int>(keyframeIndex);
								}
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
						if (removeKeyframeIndex >= 0) {
							clip.keyframes.erase(
								clip.keyframes.begin() + removeKeyframeIndex
							);
							textMotionChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(
							editorLanguage_, "Keyframeを追加###AddTextMotionKeyframe",
							"Add Keyframe###AddTextMotionKeyframe"
						))) {
							SceneTextMotionKeyframe keyframe{};
							keyframe.timeSeconds = clip.keyframes.empty()
								? 0.0f : clip.keyframes.back().timeSeconds + 0.25f;
							clip.keyframes.push_back(std::move(keyframe));
							textMotionChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(
							editorLanguage_, "Clipを削除###RemoveTextMotionClip",
							"Remove Clip###RemoveTextMotionClip"
						))) {
							removeClipIndex = static_cast<int>(clipIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeClipIndex >= 0) {
					component.textMotionClips.erase(
						component.textMotionClips.begin() + removeClipIndex
					);
					textMotionChanged = true;
				}
				if (ImGui::Button(SelectEditorText(
					editorLanguage_, "Clipを追加###AddTextMotionClip",
					"Add Clip###AddTextMotionClip"
				))) {
					SceneTextMotionClip clip{};
					clip.id = "Clip" + std::to_string(
						component.textMotionClips.size() + 1
					);
					clip.keyframes = {
						SceneTextMotionKeyframe{},
						SceneTextMotionKeyframe{ 0.25f }
					};
					component.textMotionClips.push_back(std::move(clip));
					textMotionChanged = true;
				}
				if (textMotionChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "EventTrigger") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool eventsChanged = false;
				auto drawComponentTargetCombo = [
					&document,
					&eventsChanged
				](
					const char* label,
					uint64_t& targetId,
					std::string& targetName,
					const char* componentName,
					const char* missingLabel
				) {
					const SceneEntity* selected = targetId != 0
						? document.FindEntity(targetId)
						: nullptr;
					if (!selected && !targetName.empty()) {
						selected = document.FindEntityByName(targetName);
					}
					const SceneComponent* selectedComponent = selected
						? FindComponent(*selected, componentName)
						: nullptr;
					const bool selectedValid = selectedComponent && selectedComponent->enabled;
					const std::string preview = selectedValid
						? BuildEntityHierarchyLabel(document, *selected)
						: missingLabel;
					if (ImGui::BeginCombo(label, preview.c_str())) {
						for (const SceneEntity& candidate : document.GetEntities()) {
							const SceneComponent* candidateComponent =
								FindComponent(candidate, componentName);
							if (!candidateComponent || !candidateComponent->enabled) {
								continue;
							}
							const std::string candidateLabel =
								BuildEntityHierarchyLabel(document, candidate);
							if (ImGui::Selectable(
								candidateLabel.c_str(), targetId == candidate.id
							)) {
								targetId = candidate.id;
								targetName = candidate.name;
								eventsChanged = true;
							}
						}
						ImGui::EndCombo();
					}
					if (!selectedValid) {
						ImGui::TextDisabled("%s", missingLabel);
					}
				};
				auto drawTextMotionTargetAndClip = [
					this,
					&document,
					&drawComponentTargetCombo,
					&eventsChanged
				](
					const char* targetLabel,
					const char* clipLabel,
					uint64_t& targetId,
					std::string& targetName,
					std::string& clipId,
					bool allowAnyClip
				) {
					drawComponentTargetCombo(
						targetLabel,
						targetId,
						targetName,
						"TextMotion",
						SelectEditorText(editorLanguage_, "TextMotionがありません", "Missing TextMotion")
					);
					const SceneEntity* selected = targetId != 0
						? document.FindEntity(targetId) : nullptr;
					if (!selected && !targetName.empty()) {
						selected = document.FindEntityByName(targetName);
					}
					const SceneComponent* motion = selected
						? FindComponent(*selected, "TextMotion") : nullptr;
					if (!motion || !motion->enabled) {
						return;
					}
					const char* preview = clipId.empty() && allowAnyClip
						? SelectEditorText(editorLanguage_, "すべてのClip", "Any Clip")
						: (clipId.empty()
							? SelectEditorText(editorLanguage_, "Clipを選択", "Select Clip")
							: clipId.c_str());
					if (ImGui::BeginCombo(clipLabel, preview)) {
						if (allowAnyClip && ImGui::Selectable(
							SelectEditorText(editorLanguage_, "すべてのClip", "Any Clip"),
							clipId.empty()
						)) {
							clipId.clear();
							eventsChanged = true;
						}
						for (const SceneTextMotionClip& clip : motion->textMotionClips) {
							if (ImGui::Selectable(clip.id.c_str(), clip.id == clipId)) {
								clipId = clip.id;
								eventsChanged = true;
							}
						}
						ImGui::EndCombo();
					}
				};
				int removeBindingIndex = -1;
				for (size_t bindingIndex = 0;
					bindingIndex < component.eventBindings.size();
					++bindingIndex) {
					SceneEventBinding& binding = component.eventBindings[bindingIndex];
					ImGui::PushID(static_cast<int>(bindingIndex));
					if (ImGui::TreeNodeEx(
						"Event Binding",
						ImGuiTreeNodeFlags_DefaultOpen,
						"Event %zu: %s",
						bindingIndex + 1,
						binding.triggerType.c_str()
					)) {
						if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Trigger"), binding.triggerType.c_str())) {
							for (const char* trigger : {
								"OnStart", "OnInterval", "OnStatReachedMin", "OnStatCompare",
								"OnPositionReached", "OnKeyPressed",
								"OnFishingScoreAttackResultInput",
								"OnCameraPathCompleted", "OnAudioFinished",
								"OnTextMotionCompleted"
							}) {
								if (ImGui::Selectable(
									trigger,
									binding.triggerType == trigger
								)) {
									binding.triggerType = trigger;
									if (binding.triggerType == "OnKeyPressed") {
										binding.triggerOnce = false;
									}
									if (binding.triggerType == "OnFishingScoreAttackResultInput") {
										binding.triggerOnce = true;
										if (binding.triggerKey.empty()) {
											binding.triggerKey = "ENTER";
										}
									}
									if (binding.triggerType == "OnInterval") {
										binding.triggerOnce = false;
										if (binding.cooldown <= 0.0f) {
											binding.cooldown = 1.0f;
										}
									}
									eventsChanged = true;
								}
							}
							ImGui::EndCombo();
						}
						const bool triggerNeedsTarget =
							binding.triggerType == "OnStatReachedMin" ||
							binding.triggerType == "OnStatCompare" ||
							binding.triggerType == "OnPositionReached";
						if (binding.triggerType == "OnCameraPathCompleted") {
							drawComponentTargetCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Camera Path"),
								binding.targetEntityId,
								binding.targetEntityName,
								"CameraPath",
								SelectEditorText(editorLanguage_, "CameraPathがありません", "Missing CameraPath")
							);
						} else if (binding.triggerType == "OnAudioFinished") {
							drawComponentTargetCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Audio Source"),
								binding.targetEntityId,
								binding.targetEntityName,
								"AudioSource",
								SelectEditorText(editorLanguage_, "AudioSourceがありません", "Missing AudioSource")
							);
						} else if (binding.triggerType == "OnTextMotionCompleted") {
							drawTextMotionTargetAndClip(
								LocalizedComponentWidgetLabel(editorLanguage_, "Text Motion"),
								LocalizedComponentWidgetLabel(editorLanguage_, "Clip"),
								binding.targetEntityId,
								binding.targetEntityName,
								binding.textMotionClipId,
								true
							);
						} else if (binding.triggerType == "OnFishingScoreAttackResultInput") {
							drawComponentTargetCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Fishing Score Attack Director"),
								binding.targetEntityId,
								binding.targetEntityName,
								"FishingScoreAttackDirector",
								SelectEditorText(
									editorLanguage_,
									"FishingScoreAttackDirectorがありません",
									"Missing FishingScoreAttackDirector"
								)
							);
						} else if (triggerNeedsTarget) {
							eventsChanged |= ImGui::InputScalar(
								LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Id"),
								ImGuiDataType_U64,
								&binding.targetEntityId
							);
							eventsChanged |= InputTextString(
								LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Name"), binding.targetEntityName
							);
						}
						if (
							binding.triggerType == "OnStatReachedMin" ||
							binding.triggerType == "OnStatCompare"
						) {
							eventsChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Stat Id"), binding.statId);
						}
						if (binding.triggerType == "OnStatCompare") {
							if (ImGui::BeginCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Comparison"), binding.statComparison.c_str()
							)) {
								for (const char* comparison : {
									"LessOrEqual", "Less", "Equal",
									"Greater", "GreaterOrEqual"
								}) {
									if (ImGui::Selectable(
										comparison,
										binding.statComparison == comparison
									)) {
										binding.statComparison = comparison;
										eventsChanged = true;
									}
								}
								ImGui::EndCombo();
							}
							eventsChanged |= ImGui::DragFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Compare Value"), &binding.statValue, 0.1f
							);
						}
						if (binding.triggerType == "OnPositionReached") {
							eventsChanged |= ImGui::DragFloat3(
								LocalizedComponentWidgetLabel(editorLanguage_, "Target Position"), &binding.targetPosition.x, 0.05f
							);
							eventsChanged |= ImGui::DragFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Radius"), &binding.radius, 0.05f, 0.0f, 10000.0f
							);
						}
						if (binding.triggerType == "OnKeyPressed") {
							eventsChanged |= DrawSceneInputExpressionEditor(
								LocalizedComponentWidgetLabel(editorLanguage_, "Input"),
								binding.inputExpression,
								binding.triggerKey,
								editorLanguage_
							);
						}
						if (binding.triggerType == "OnFishingScoreAttackResultInput") {
							eventsChanged |= DrawSceneInputExpressionEditor(
								LocalizedComponentWidgetLabel(editorLanguage_, "Input"),
								binding.inputExpression,
								binding.triggerKey,
								editorLanguage_
							);
						}
						eventsChanged |= ImGui::Checkbox(
							LocalizedComponentWidgetLabel(editorLanguage_, "Trigger Once"), &binding.triggerOnce
						);
						eventsChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Cooldown"), &binding.cooldown, 0.01f, 0.0f, 10000.0f
						);
						binding.radius = (std::max)(binding.radius, 0.0f);
						binding.cooldown = (std::max)(binding.cooldown, 0.0f);

						ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Action", "Actions"));
						int removeActionIndex = -1;
						for (size_t actionIndex = 0;
							actionIndex < binding.actions.size();
							++actionIndex) {
							SceneEventAction& action = binding.actions[actionIndex];
							ImGui::PushID(static_cast<int>(actionIndex));
							if (ImGui::TreeNodeEx(
								"Action",
								ImGuiTreeNodeFlags_DefaultOpen,
								"Action %zu: %s",
								actionIndex + 1,
								action.type.c_str()
							)) {
								if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Type"), action.type.c_str())) {
									for (const char* actionType : {
										"ModifyStat", "SetEntityActive",
										"InstantiatePrefab", "ChangeState",
										"SceneTransition", "SetPostProcessProfile",
										"NextPostProcessProfile",
										"ResetPostProcessProfile", "PlayCameraPath",
										"StopCameraPath", "SelectCamera", "PlayAudio",
									"StopAudio", "PauseAudio", "ResumeAudio",
									"PlayTextMotion", "StopTextMotion", "ResetTextMotion",
									"AdjustFishingFishCount"
									}) {
										if (ImGui::Selectable(
											actionType,
											action.type == actionType
										)) {
											action.type = actionType;
											eventsChanged = true;
										}
									}
									ImGui::EndCombo();
								}
								if (
								action.type != "SceneTransition" &&
								action.type != "SetPostProcessProfile" &&
								action.type != "NextPostProcessProfile" &&
								action.type != "ResetPostProcessProfile" &&
								action.type != "PlayCameraPath" &&
								action.type != "StopCameraPath" &&
								action.type != "SelectCamera" &&
								action.type != "PlayTextMotion" &&
								action.type != "StopTextMotion" &&
												action.type != "ResetTextMotion" &&
												action.type != "AdjustFishingFishCount"
								) {
									eventsChanged |= ImGui::InputScalar(
										LocalizedComponentWidgetLabel(editorLanguage_, "Action Target Entity Id"),
										ImGuiDataType_U64,
										&action.targetEntityId
									);
									eventsChanged |= InputTextString(
										LocalizedComponentWidgetLabel(editorLanguage_, "Action Target Entity Name"),
										action.targetEntityName
									);
								}
								if (action.type == "ModifyStat") {
									eventsChanged |= InputTextString(
										LocalizedComponentWidgetLabel(editorLanguage_, "Action Stat Id"), action.statId
									);
									if (ImGui::BeginCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Operation"), action.statOperation.c_str()
									)) {
										for (const char* operation : {
											"Add", "Subtract", "Set", "Multiply",
											"SetMin", "SetMax", "RestoreToMax"
										}) {
											if (ImGui::Selectable(
												operation,
												action.statOperation == operation
											)) {
												action.statOperation = operation;
												eventsChanged = true;
											}
										}
										ImGui::EndCombo();
									}
									eventsChanged |= ImGui::DragFloat(
										LocalizedComponentWidgetLabel(editorLanguage_, "Value"), &action.value, 0.1f
									);
								} else if (action.type == "AdjustFishingFishCount") {
									if (action.value != 1.0f && action.value != -1.0f) {
										action.value = 1.0f;
										eventsChanged = true;
									}
									if (ImGui::BeginCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Fish Count Delta"),
										action.value > 0.0f ? "+1" : "-1"
									)) {
										for (const float delta : { 1.0f, -1.0f }) {
											const char* deltaLabel = delta > 0.0f ? "+1" : "-1";
											if (ImGui::Selectable(deltaLabel, action.value == delta)) {
												action.value = delta;
												eventsChanged = true;
											}
										}
										ImGui::EndCombo();
									}
								} else if (action.type == "SetEntityActive") {
									eventsChanged |= ImGui::Checkbox(
										SelectEditorText(editorLanguage_, "有効###EventActionActive", "Active###EventActionActive"), &action.active
									);
								} else if (action.type == "InstantiatePrefab") {
									eventsChanged |= InputTextString(
										LocalizedComponentWidgetLabel(editorLanguage_, "Prefab Path"), action.prefabPath
									);
									eventsChanged |= ImGui::Checkbox(
										LocalizedComponentWidgetLabel(editorLanguage_, "Parent To Target"), &action.prefabParentToTarget
									);
									eventsChanged |= ImGui::Checkbox(
										LocalizedComponentWidgetLabel(editorLanguage_, "Spawn At Target Transform"),
										&action.prefabUseTargetTransform
									);
								} else if (action.type == "ChangeState") {
									eventsChanged |= InputTextString(
										LocalizedComponentWidgetLabel(editorLanguage_, "State Name"), action.stateName
									);
								} else if (
									action.type == "PlayAudio" || action.type == "StopAudio" ||
									action.type == "PauseAudio" || action.type == "ResumeAudio"
								) {
									drawComponentTargetCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Audio Source"),
										action.targetEntityId, action.targetEntityName, "AudioSource",
										SelectEditorText(editorLanguage_, "AudioSourceがありません", "Missing AudioSource")
									);
								} else if (
									action.type == "PlayTextMotion" ||
									action.type == "StopTextMotion" ||
									action.type == "ResetTextMotion"
								) {
									drawTextMotionTargetAndClip(
										LocalizedComponentWidgetLabel(editorLanguage_, "Text Motion"),
										LocalizedComponentWidgetLabel(editorLanguage_, "Clip"),
										action.targetEntityId,
										action.targetEntityName,
										action.textMotionClipId,
										action.type != "PlayTextMotion"
									);
								} else if (action.type == "SceneTransition") {
									eventsChanged |= InputTextString(
										LocalizedComponentWidgetLabel(editorLanguage_, "Scene Id"), action.sceneId
									);
								} else if (
									action.type == "PlayCameraPath" ||
									action.type == "StopCameraPath"
								) {
									drawComponentTargetCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Camera Path"),
										action.targetEntityId,
										action.targetEntityName,
										"CameraPath",
										SelectEditorText(editorLanguage_, "CameraPathがありません", "Missing CameraPath")
									);
								} else if (action.type == "SelectCamera") {
									drawComponentTargetCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Camera"),
										action.targetEntityId,
										action.targetEntityName,
										"Camera",
										SelectEditorText(editorLanguage_, "Cameraがありません", "Missing Camera")
									);
								} else if (
									action.type == "SetPostProcessProfile" ||
									action.type == "NextPostProcessProfile"
								) {
									const SceneEntity* selectedManager =
										action.postProcessManagerEntityId != 0
										? document.FindEntity(action.postProcessManagerEntityId)
										: nullptr;
									if (
										!selectedManager &&
										!action.postProcessManagerEntityName.empty()
									) {
										selectedManager = document.FindEntityByName(
											action.postProcessManagerEntityName
										);
									}
									const SceneComponent* managerComponent =
										selectedManager
										? FindComponent(
											*selectedManager,
											"PostProcessProfileManager"
										)
										: nullptr;
									const std::string managerPreview = managerComponent
										? BuildEntityHierarchyLabel(document, *selectedManager)
										: SelectEditorText(editorLanguage_, "Managerがありません", "Missing Manager");
									if (ImGui::BeginCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Manager"), managerPreview.c_str()
									)) {
										for (const SceneEntity& candidate : document.GetEntities()) {
											if (!FindComponent(
												candidate, "PostProcessProfileManager"
											)) {
												continue;
											}
											const std::string label =
												BuildEntityHierarchyLabel(document, candidate);
											if (ImGui::Selectable(
												label.c_str(),
												action.postProcessManagerEntityId == candidate.id
											)) {
												action.postProcessManagerEntityId = candidate.id;
												action.postProcessManagerEntityName = candidate.name;
												action.postProcessProfileId.clear();
												eventsChanged = true;
											}
										}
										ImGui::EndCombo();
									}
									if (!managerComponent) {
										ImGui::TextDisabled(SelectEditorText(editorLanguage_, "PostProcessProfileManagerを選択してください。", "Select a PostProcessProfileManager."));
									} else if (action.type == "SetPostProcessProfile") {
										const ScenePostProcessProfile* selectedProfile = nullptr;
										for (const ScenePostProcessProfile& profile :
											managerComponent->postProcessProfiles) {
											if (profile.id == action.postProcessProfileId) {
												selectedProfile = &profile;
												break;
											}
										}
										const char* profilePreview = selectedProfile
											? (selectedProfile->label.empty()
												? selectedProfile->id.c_str()
												: selectedProfile->label.c_str())
											: SelectEditorText(editorLanguage_, "Profileがありません", "Missing Profile");
									if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Profile"), profilePreview)) {
											for (const ScenePostProcessProfile& profile :
												managerComponent->postProcessProfiles) {
												const char* label = profile.label.empty()
													? profile.id.c_str()
													: profile.label.c_str();
												if (ImGui::Selectable(
													label,
													profile.id == action.postProcessProfileId
												)) {
													action.postProcessProfileId = profile.id;
													eventsChanged = true;
												}
											}
											ImGui::EndCombo();
										}
									}
								}
								if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Actionを削除###RemoveEventAction", "Remove Action###RemoveEventAction"))) {
									removeActionIndex = static_cast<int>(actionIndex);
								}
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
						if (removeActionIndex >= 0) {
							binding.actions.erase(
								binding.actions.begin() + removeActionIndex
							);
							eventsChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Actionを追加###AddEventAction", "Add Action###AddEventAction"))) {
							binding.actions.push_back(SceneEventAction{});
							eventsChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Eventを削除###RemoveEvent", "Remove Event###RemoveEvent"))) {
							removeBindingIndex = static_cast<int>(bindingIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeBindingIndex >= 0) {
					component.eventBindings.erase(
						component.eventBindings.begin() + removeBindingIndex
					);
					eventsChanged = true;
				}
				if (ImGui::Button(SelectEditorText(editorLanguage_, "Eventを追加###AddEvent", "Add Event###AddEvent"))) {
					component.eventBindings.push_back(SceneEventBinding{});
					eventsChanged = true;
				}
				if (eventsChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "PostProcessProfileManager") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool profilesChanged = false;
				int removeProfileIndex = -1;
				int moveProfileIndex = -1;
				int moveProfileDirection = 0;
				for (size_t profileIndex = 0;
					profileIndex < component.postProcessProfiles.size();
					++profileIndex
				) {
					ScenePostProcessProfile& profile =
						component.postProcessProfiles[profileIndex];
					ImGui::PushID(static_cast<int>(profileIndex));
					if (ImGui::TreeNodeEx(
						"Profile", ImGuiTreeNodeFlags_DefaultOpen,
						"Profile %zu: %s", profileIndex + 1, profile.label.c_str()
					)) {
						ImGui::TextDisabled(SelectEditorText(editorLanguage_, "ID: %s", "Id: %s"), profile.id.c_str());
						profilesChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Label"), profile.label);
						const bool duplicateProfileId = !profile.id.empty() &&
							std::any_of(
								component.postProcessProfiles.begin(),
								component.postProcessProfiles.end(),
								[&profile](const ScenePostProcessProfile& candidate) {
									return &candidate != &profile &&
										candidate.id == profile.id;
								}
							);
						if (duplicateProfileId) {
							ImGui::TextDisabled(
								"Profile Id must be unique within this Manager."
							);
						}
						if (ImGui::SmallButton(LocalizedComponentWidgetLabel(editorLanguage_, "Copy Scene Baseline"))) {
							profile.settings = document.GetPostProcessSettings();
							profilesChanged = true;
						}
						profilesChanged |= DrawPostProcessSettingsEditor(profile.settings);
						bool dissolveAutomationEnabled =
							!profile.automations.empty();
						if (ImGui::Checkbox(
							LocalizedComponentWidgetLabel(editorLanguage_, "Animate Dissolve Threshold"),
							&dissolveAutomationEnabled
						)) {
							if (dissolveAutomationEnabled) {
								profile.automations = {
									ScenePostProcessAutomation{}
								};
							} else {
								profile.automations.clear();
							}
							profilesChanged = true;
						}
						if (!profile.automations.empty()) {
							ScenePostProcessAutomation& automation =
								profile.automations.front();
							profilesChanged |= ImGui::SliderFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Automation Start"),
								&automation.startValue,
								0.0f,
								1.0f
							);
							profilesChanged |= ImGui::SliderFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Automation End"),
								&automation.endValue,
								0.0f,
								1.0f
							);
							profilesChanged |= ImGui::DragFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Automation Duration"),
								&automation.duration,
								0.05f,
								0.05f,
								60.0f,
								"%.2f s"
							);
							automation.duration = (std::max)(
								automation.duration,
								0.05f
							);
							ImGui::TextDisabled(
								"Playback: OneShot / Easing: Linear"
							);
						}
						if (profile.id.empty()) {
							ImGui::TextDisabled(SelectEditorText(editorLanguage_, "Event ActionにはProfile IDが必要です。", "Profile Id is required for Event actions."));
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Profileを削除###RemovePostProcessProfile", "Remove Profile###RemovePostProcessProfile"))) {
							removeProfileIndex = static_cast<int>(profileIndex);
						}
						ImGui::SameLine();
						ImGui::BeginDisabled(profileIndex == 0);
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "上へ###MovePostProcessProfileUp", "Move Up###MovePostProcessProfileUp"))) {
							moveProfileIndex = static_cast<int>(profileIndex);
							moveProfileDirection = -1;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						ImGui::BeginDisabled(
							profileIndex + 1 == component.postProcessProfiles.size()
						);
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "下へ###MovePostProcessProfileDown", "Move Down###MovePostProcessProfileDown"))) {
							moveProfileIndex = static_cast<int>(profileIndex);
							moveProfileDirection = 1;
						}
						ImGui::EndDisabled();
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeProfileIndex >= 0) {
					component.postProcessProfiles.erase(
						component.postProcessProfiles.begin() + removeProfileIndex
					);
					profilesChanged = true;
				}
				if (moveProfileIndex >= 0) {
					const int destination =
						moveProfileIndex + moveProfileDirection;
					std::swap(
						component.postProcessProfiles[moveProfileIndex],
						component.postProcessProfiles[destination]
					);
					profilesChanged = true;
				}
				if (ImGui::Button(SelectEditorText(editorLanguage_, "Profileを追加###AddPostProcessProfile", "Add Profile###AddPostProcessProfile"))) {
					ScenePostProcessProfile profile{};
					for (size_t candidateIndex = 1;; ++candidateIndex) {
						profile.id = "Profile" + std::to_string(candidateIndex);
						const bool alreadyExists = std::any_of(
							component.postProcessProfiles.begin(),
							component.postProcessProfiles.end(),
							[&profile](const ScenePostProcessProfile& candidate) {
								return candidate.id == profile.id;
							}
						);
						if (!alreadyExists) {
							break;
						}
					}
					profile.label = profile.id;
					component.postProcessProfiles.push_back(std::move(profile));
					profilesChanged = true;
				}
				if (profilesChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "PrefabAnimator") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool animationChanged = false;
				int removeClipIndex = -1;
				for (size_t clipIndex = 0;
					clipIndex < component.prefabAnimationClips.size();
					++clipIndex) {
					ScenePrefabAnimationClip& clip =
						component.prefabAnimationClips[clipIndex];
					ImGui::PushID(static_cast<int>(clipIndex));
					if (ImGui::TreeNodeEx(
						"Prefab Clip",
						ImGuiTreeNodeFlags_DefaultOpen,
						"Clip %zu: %s",
						clipIndex + 1,
						clip.name.c_str()
					)) {
						animationChanged |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Clip Name"), clip.name);
						animationChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Duration"), &clip.duration, 0.01f, 0.001f, 10000.0f
						);
						animationChanged |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop"), &clip.loop);
						animationChanged |= ImGui::Checkbox(
							LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"), &clip.playOnStart
						);
						clip.duration = (std::max)(clip.duration, 0.001f);
						int removeTrackIndex = -1;
						for (size_t trackIndex = 0;
							trackIndex < clip.tracks.size();
							++trackIndex) {
							SceneAnimationTrack& track = clip.tracks[trackIndex];
							ImGui::PushID(static_cast<int>(trackIndex));
							if (ImGui::TreeNodeEx(
								"Track",
								ImGuiTreeNodeFlags_DefaultOpen,
								"Track %zu: %s",
								trackIndex + 1,
								track.property.c_str()
							)) {
								animationChanged |= ImGui::InputScalar(
									LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Id"),
									ImGuiDataType_U64,
									&track.targetEntityId
								);
								animationChanged |= InputTextString(
									LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Name"), track.targetEntityName
								);
								if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Property"), track.property.c_str())) {
									for (const char* property : {
										"LocalPosition", "LocalRotation", "LocalScale", "Active"
									}) {
										if (ImGui::Selectable(
											property,
											track.property == property
										)) {
											track.property = property;
											animationChanged = true;
										}
									}
									ImGui::EndCombo();
								}
								if (track.property != "Active") {
									if (ImGui::BeginCombo(
										LocalizedComponentWidgetLabel(editorLanguage_, "Easing"),
										track.easing.empty()
											? "SmoothStep"
											: track.easing.c_str()
									)) {
										for (const char* easing : {
											"Linear", "EaseIn", "EaseOut", "EaseInOut",
											"SmoothStep"
										}) {
											if (ImGui::Selectable(
												easing,
												track.easing == easing
											)) {
												track.easing = easing;
												animationChanged = true;
											}
										}
										ImGui::EndCombo();
									}
								}
								int removeKeyframeIndex = -1;
								bool keyframeTimeChanged = false;
								for (size_t keyframeIndex = 0;
									keyframeIndex < track.keyframes.size();
									++keyframeIndex) {
									SceneAnimationKeyframe& keyframe =
										track.keyframes[keyframeIndex];
									ImGui::PushID(static_cast<int>(keyframeIndex));
									ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Keyframe", "Keyframe"));
									if (ImGui::DragFloat(
										LocalizedComponentWidgetLabel(editorLanguage_, "Time"), &keyframe.time, 0.01f, 0.0f, clip.duration
									)) {
										keyframe.time = std::clamp(
											keyframe.time,
											0.0f,
											clip.duration
										);
										keyframeTimeChanged = true;
										animationChanged = true;
									}
									if (track.property == "Active") {
										bool activeValue = keyframe.value.x >= 0.5f;
										if (ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Active Value"), &activeValue)) {
											keyframe.value.x = activeValue ? 1.0f : 0.0f;
											animationChanged = true;
										}
									} else {
										animationChanged |= ImGui::DragFloat3(
											track.property == "LocalRotation"
												? LocalizedComponentWidgetLabel(editorLanguage_, "Euler Value (Radians)")
												: LocalizedComponentWidgetLabel(editorLanguage_, "Value"),
											&keyframe.value.x,
											0.01f
										);
									}
									if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Keyframeを削除###RemoveKeyframe", "Remove Keyframe###RemoveKeyframe"))) {
										removeKeyframeIndex = static_cast<int>(keyframeIndex);
									}
									ImGui::PopID();
								}
								if (removeKeyframeIndex >= 0) {
									track.keyframes.erase(
										track.keyframes.begin() + removeKeyframeIndex
									);
									animationChanged = true;
								}
								if (keyframeTimeChanged) {
									std::stable_sort(
										track.keyframes.begin(),
										track.keyframes.end(),
										[](const SceneAnimationKeyframe& left,
											const SceneAnimationKeyframe& right) {
											return left.time < right.time;
										}
									);
								}
								if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Keyframeを追加###AddKeyframe", "Add Keyframe###AddKeyframe"))) {
									SceneAnimationKeyframe keyframe{};
									keyframe.time = track.keyframes.empty()
										? 0.0f
										: (std::min)(
											track.keyframes.back().time + 0.1f,
											clip.duration
										);
									if (!track.keyframes.empty()) {
										keyframe.value = track.keyframes.back().value;
									}
									track.keyframes.push_back(keyframe);
									animationChanged = true;
								}
								if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Trackを削除###RemoveTrack", "Remove Track###RemoveTrack"))) {
									removeTrackIndex = static_cast<int>(trackIndex);
								}
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
						if (removeTrackIndex >= 0) {
							clip.tracks.erase(clip.tracks.begin() + removeTrackIndex);
							animationChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Trackを追加###AddTrack", "Add Track###AddTrack"))) {
							SceneAnimationTrack track{};
							track.keyframes = {
								{ 0.0f, {} },
								{ clip.duration, {} }
							};
							clip.tracks.push_back(std::move(track));
							animationChanged = true;
						}
						if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Clipを削除###RemoveClip", "Remove Clip###RemoveClip"))) {
							removeClipIndex = static_cast<int>(clipIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeClipIndex >= 0) {
					component.prefabAnimationClips.erase(
						component.prefabAnimationClips.begin() + removeClipIndex
					);
					animationChanged = true;
				}
				if (ImGui::Button(SelectEditorText(editorLanguage_, "Clipを追加###AddClip", "Add Clip###AddClip"))) {
					component.prefabAnimationClips.push_back(
						ScenePrefabAnimationClip{}
					);
					animationChanged = true;
				}
				if (animationChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "Faction") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				if (InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Faction"), component.factionName)) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "HitBox") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool hitBoxChanged = false;
				hitBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Damage"), &component.hitBoxDamage, 0.1f, 0.0f, 100000.0f
				);
				hitBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Poise Damage"),
					&component.hitBoxPoiseDamage,
					0.1f,
					0.0f,
					100000.0f
				);
				hitBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Knockback"),
					&component.hitBoxKnockback,
					0.1f, 0.0f, 100000.0f
				);
				hitBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Vertical Knockback"),
					&component.hitBoxVerticalKnockback,
					0.1f, 0.0f, 100000.0f
				);
				hitBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Hit Stop Duration"),
					&component.hitBoxHitStopDuration,
					0.001f, 0.0f, 1.0f
				);
				hitBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Reaction Tag"), component.hitBoxReactionTag
				);
				hitBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Damage Stat"), component.hitBoxDamageStatId
				);
				hitBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Poise Stat"), component.hitBoxPoiseStatId
				);
				hitBoxChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Owner Entity Id"),
					ImGuiDataType_U64,
					&component.hitBoxOwnerEntityId
				);
				hitBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Owner Entity Name"), component.hitBoxOwnerEntityName
				);
				hitBoxChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Ignore Same Faction"), &component.hitBoxIgnoreSameFaction
				);
				component.hitBoxDamage = (std::max)(component.hitBoxDamage, 0.0f);
				component.hitBoxPoiseDamage = (std::max)(
					component.hitBoxPoiseDamage,
					0.0f
				);
				if (hitBoxChanged) {
					document.MarkDirty();
				}
				ImGui::TextDisabled(SelectEditorText(editorLanguage_, "このEntityにはTrigger Colliderが必要です。", "Requires a Trigger Collider on this Entity."));
				ImGui::EndDisabled();
			} else if (component.type == "HurtBox") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool hurtBoxChanged = false;
				hurtBoxChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Damage Multiplier"),
					&component.hurtBoxDamageMultiplier,
					0.01f,
					0.0f,
					100.0f
				);
				hurtBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Health Stat"), component.hurtBoxHealthStatId
				);
				hurtBoxChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Stats Entity Id"),
					ImGuiDataType_U64,
					&component.hurtBoxStatsEntityId
				);
				hurtBoxChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Stats Entity Name"), component.hurtBoxStatsEntityName
				);
				component.hurtBoxDamageMultiplier = (std::max)(
					component.hurtBoxDamageMultiplier,
					0.0f
				);
				if (hurtBoxChanged) {
					document.MarkDirty();
				}
				ImGui::TextDisabled(SelectEditorText(editorLanguage_, "このEntityにはTrigger Colliderが必要です。", "Requires a Trigger Collider on this Entity."));
				ImGui::EndDisabled();
			} else if (component.type == "HitReaction") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool reactionChanged = false;
				reactionChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Knockback Multiplier"),
					&component.hitReactionKnockbackMultiplier,
					0.01f, 0.0f, 100.0f
				);
				const char* reactionModePreview =
					component.hitReactionTriggerMode == "PoiseBreak"
					? "Poise Break" : "Minimum Damage";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Reaction Trigger"), reactionModePreview)) {
					if (ImGui::Selectable(
						"Minimum Damage",
						component.hitReactionTriggerMode == "MinimumDamage"
					)) {
						component.hitReactionTriggerMode = "MinimumDamage";
						reactionChanged = true;
					}
					if (ImGui::Selectable(
						"Poise Break",
						component.hitReactionTriggerMode == "PoiseBreak"
					)) {
						component.hitReactionTriggerMode = "PoiseBreak";
						reactionChanged = true;
					}
					ImGui::EndCombo();
				}
				if (component.hitReactionTriggerMode == "PoiseBreak") {
					reactionChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Poise Stat"), component.hitReactionPoiseStatId
					);
					reactionChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Poise Recovery Delay"),
						&component.hitReactionPoiseRecoveryDelay,
						0.05f, 0.0f, 60.0f
					);
				} else {
					reactionChanged |= ImGui::DragFloat(
						"Minimum Poise Damage",
						&component.hitReactionMinimumPoiseDamage,
						0.1f, 0.0f, 100000.0f
					);
				}
				reactionChanged |= InputTextString(
					"Hit State", component.hitReactionStateName
				);
				if (reactionChanged) { document.MarkDirty(); }
				ImGui::EndDisabled();
			} else if (component.type == "DeathPresentation") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool deathChanged = false;
				deathChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Death State"), component.deathPresentationStateName
				);
				deathChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Deactivate Delay"),
					&component.deathPresentationDeactivateDelay,
					0.05f, 0.0f, 60.0f
				);
				deathChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Death Effect Path"), component.deathPresentationEffectPath
				);
				if (deathChanged) { document.MarkDirty(); }
				ImGui::EndDisabled();
			} else if (component.type == "BoneAttachment") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool attachmentChanged = false;
				const SceneEntity* targetEntity = nullptr;
				if (component.boneAttachmentTargetEntityId != 0) {
					targetEntity = document.FindEntity(
						component.boneAttachmentTargetEntityId
					);
				}
				if (
					!targetEntity &&
					!component.boneAttachmentTargetEntityName.empty()
				) {
					targetEntity = document.FindEntityByName(
						component.boneAttachmentTargetEntityName
					);
				}
				const SceneEntity* parentEntity = document.FindEntity(entity->parentId);
				const SceneEntity* effectiveTargetEntity = targetEntity
					? targetEntity
					: parentEntity;
				const std::string targetLabel = targetEntity
					? targetEntity->name
					: "Parent / Auto";
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity"), targetLabel.c_str())) {
					const bool autoSelected =
						component.boneAttachmentTargetEntityId == 0 &&
						component.boneAttachmentTargetEntityName.empty();
					if (ImGui::Selectable("Parent / Auto", autoSelected)) {
						component.boneAttachmentTargetEntityId = 0;
						component.boneAttachmentTargetEntityName.clear();
						component.boneAttachmentJointName.clear();
						attachmentChanged = true;
					}
					for (const SceneEntity& candidate : document.GetEntities()) {
						const SceneComponent* meshRenderer =
							FindEnabledComponent(candidate, "MeshRenderer");
						if (
							candidate.id == entity->id ||
							!meshRenderer ||
							meshRenderer->modelPath.empty()
						) {
							continue;
						}
						if (ImGui::Selectable(
							candidate.name.c_str(),
							targetEntity && targetEntity->id == candidate.id
						)) {
							component.boneAttachmentTargetEntityId = candidate.id;
							component.boneAttachmentTargetEntityName = candidate.name;
							component.boneAttachmentJointName.clear();
							attachmentChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				if (targetEntity) {
					ImGui::TextDisabled(
						SelectEditorText(editorLanguage_, "接続先Entity ID: %llu", "Bound Entity ID: %llu"),
						static_cast<unsigned long long>(targetEntity->id)
					);
				} else if (!parentEntity) {
					ImGui::TextDisabled(SelectEditorText(editorLanguage_, "対象Entityを選択するか、親を設定してください。", "Select a target Entity or set a parent."));
				}

				const std::vector<std::string> targetJointNames =
					effectiveTargetEntity
					? CollectEntityJointNames(*effectiveTargetEntity)
					: std::vector<std::string>{};
				ImGui::BeginDisabled(targetJointNames.empty());
				attachmentChanged |= DrawJointNameCombo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Bone"),
					targetJointNames,
					component.boneAttachmentJointName
				);
				ImGui::EndDisabled();
				if (targetJointNames.empty()) {
					ImGui::TextDisabled(
						SelectEditorText(editorLanguage_, "対象EntityにはBoneを持つMeshRendererモデルが必要です。", "The target Entity needs a MeshRenderer model with bones.")
					);
				}

				const bool matchesSourceBone =
					component.boneAttachmentAlignmentMode == "MatchSourceBone";
				if (ImGui::BeginCombo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Alignment Mode"),
					matchesSourceBone ? "Match Weapon Bone" : "Manual Offset"
				)) {
					if (ImGui::Selectable(
						"Manual Offset", !matchesSourceBone
					)) {
						component.boneAttachmentAlignmentMode = "ManualOffset";
						attachmentChanged = true;
					}
					if (ImGui::Selectable(
						"Match Weapon Bone", matchesSourceBone
					)) {
						component.boneAttachmentAlignmentMode = "MatchSourceBone";
						attachmentChanged = true;
					}
					ImGui::EndCombo();
				}
				if (matchesSourceBone) {
					const std::vector<std::string> sourceJointNames =
						CollectEntityJointNames(*entity);
					ImGui::BeginDisabled(sourceJointNames.empty());
					attachmentChanged |= DrawJointNameCombo(
						LocalizedComponentWidgetLabel(editorLanguage_, "Weapon Bone"),
						sourceJointNames,
						component.boneAttachmentSourceJointName
					);
					ImGui::EndDisabled();
					if (sourceJointNames.empty()) {
						ImGui::TextDisabled(
							SelectEditorText(editorLanguage_, "このEntityにはBoneを持つMeshRendererモデルが必要です。", "This Entity needs a MeshRenderer model with bones.")
						);
					} else {
						ImGui::TextDisabled(
							SelectEditorText(editorLanguage_, "両方のBoneを正確に一致させるため、このEntityのTransformは使用しません。", "This Entity Transform is ignored so both bones match exactly.")
						);
					}
				} else {
					ImGui::TextDisabled(
						SelectEditorText(editorLanguage_, "このEntityの上部TransformをAttachmentのオフセットとして使用します。", "Use this Entity's Transform section above as the attachment offset.")
					);
				}
				attachmentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Inherit Bone Scale"), &component.boneAttachmentInheritScale
				);
				if (attachmentChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "EnemyBehavior") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool enemyChanged = false;
				enemyChanged |= ImGui::InputScalar(
					"Target Entity Id",
					ImGuiDataType_U64,
					&component.enemyTargetEntityId
				);
				enemyChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity Name"), component.enemyTargetEntityName
				);
				enemyChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Health Stat"), component.enemyHealthStatId
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Detection Range"), &component.enemyDetectionRange, 0.1f, 0.0f, 10000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Lose Range"), &component.enemyLoseRange, 0.1f, 0.0f, 10000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Range"), &component.enemyAttackRange, 0.1f, 0.0f, 10000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Move Speed"), &component.enemyMoveSpeed, 0.05f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Turn Speed"), &component.enemyTurnSpeed, 0.05f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Cooldown"), &component.enemyAttackCooldown, 0.01f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Windup"), &component.enemyAttackWindup, 0.01f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Active Time"), &component.enemyAttackActiveTime, 0.01f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Recovery"), &component.enemyAttackRecovery, 0.01f, 0.0f, 1000.0f
				);
				enemyChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Animation Clip"), &component.enemyAttackAnimationClip, 1.0f, 0, 1024
				);
				enemyChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack Prefab Animation Clip"),
					component.enemyAttackPrefabAnimationClip
				);
				enemyChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack HitBox Entity Id"),
					ImGuiDataType_U64,
					&component.enemyAttackHitBoxEntityId
				);
				enemyChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attack HitBox Entity Name"),
					component.enemyAttackHitBoxEntityName
				);
				component.enemyLoseRange = (std::max)(
					component.enemyLoseRange,
					component.enemyDetectionRange
				);
				if (enemyChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "EnemySpawner") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool spawnerChanged = false;
				spawnerChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Enemy Prefab"), component.enemySpawnerPrefabPath
				);
				spawnerChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Initial Count"), &component.enemySpawnerInitialCount,
					1.0f, 0, 10000
				);
				spawnerChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Alive"), &component.enemySpawnerMaxAlive,
					1.0f, 0, 10000
				);
				spawnerChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Respawn Interval"), &component.enemySpawnerInterval,
					0.05f, 0.0f, 3600.0f
				);
				spawnerChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Spawn Radius"), &component.enemySpawnerRadius,
					0.1f, 0.0f, 10000.0f
				);
				spawnerChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Auto Start"), &component.enemySpawnerAutoStart
				);
				component.enemySpawnerInitialCount = (std::max)(
					component.enemySpawnerInitialCount, 0
				);
				component.enemySpawnerMaxAlive = (std::max)(
					component.enemySpawnerMaxAlive,
					component.enemySpawnerInitialCount
				);
				component.enemySpawnerInterval = (std::max)(
					component.enemySpawnerInterval, 0.0f
				);
				component.enemySpawnerRadius = (std::max)(
					component.enemySpawnerRadius, 0.0f
				);
				if (spawnerChanged) {
					document.MarkDirty();
				}
				ImGui::TextDisabled(
					"Runtime-only instances are reset to their prefab baseline before reuse."
				);
				ImGui::EndDisabled();
			} else if (component.type == "Projectile") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool projectileChanged = false;
				projectileChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Local Direction"), &component.projectileDirection.x, 0.01f
				);
				projectileChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Speed"), &component.projectileSpeed, 0.1f, 0.0f, 10000.0f
				);
				projectileChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Gravity"), &component.projectileGravity, 0.1f, -1000.0f, 1000.0f
				);
				projectileChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Lifetime"), &component.projectileLifetime, 0.05f, 0.0f, 10000.0f
				);
				projectileChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Destroy On Hit"), &component.projectileDestroyOnHit
				);
				projectileChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Homing Target Entity Id"),
					ImGuiDataType_U64,
					&component.projectileHomingTargetEntityId
				);
				projectileChanged |= InputTextString(
					LocalizedComponentWidgetLabel(editorLanguage_, "Homing Target Entity Name"),
					component.projectileHomingTargetEntityName
				);
				projectileChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Homing Strength"),
					&component.projectileHomingStrength,
					0.1f,
					0.0f,
					1000.0f
				);
				if (projectileChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "PhysicsBody") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool physicsChanged = false;
				const char* currentBodyType = component.physicsBodyType.empty()
					? "Static"
					: component.physicsBodyType.c_str();
				if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Body Type"), currentBodyType)) {
					const char* bodyTypes[] = { "Static", "Dynamic", "Kinematic" };
					for (const char* bodyType : bodyTypes) {
						if (ImGui::Selectable(
							bodyType,
							component.physicsBodyType == bodyType ||
								(component.physicsBodyType.empty() &&
									std::strcmp(bodyType, "Static") == 0)
						)) {
							component.physicsBodyType = bodyType;
							physicsChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				physicsChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Mass"),
					&component.physicsMass,
					0.05f,
					0.001f,
					10000.0f
				);
				physicsChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Use Gravity"),
					&component.physicsUseGravity
				);
				physicsChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Gravity Scale"),
					&component.physicsGravityScale,
					0.05f,
					-10.0f,
					10.0f
				);
				physicsChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Drag"),
					&component.physicsDrag,
					0.02f,
					0.0f,
					100.0f
				);
				physicsChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Restitution"),
					&component.physicsRestitution,
					0.0f,
					1.0f
				);
				physicsChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Friction"),
					&component.physicsFriction,
					0.0f,
					1.0f
				);
				physicsChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Fall Speed"),
					&component.physicsMaxFallSpeed,
					0.1f,
					0.0f,
					1000.0f
				);
				physicsChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Velocity"),
					&component.physicsVelocity.x,
					0.05f
				);
				ImGui::TextDisabled(LocalizedComponentWidgetLabel(editorLanguage_, "Freeze Position"));
				physicsChanged |= ImGui::Checkbox(
					"X##FreezePosition",
					&component.physicsFreezePositionX
				);
				ImGui::SameLine();
				physicsChanged |= ImGui::Checkbox(
					"Y##FreezePosition",
					&component.physicsFreezePositionY
				);
				ImGui::SameLine();
				physicsChanged |= ImGui::Checkbox(
					"Z##FreezePosition",
					&component.physicsFreezePositionZ
				);
				if (component.physicsMass <= 0.0f) {
					component.physicsMass = 0.001f;
					physicsChanged = true;
				}
				component.physicsRestitution = std::clamp(
					component.physicsRestitution,
					0.0f,
					1.0f
				);
				component.physicsFriction = std::clamp(
					component.physicsFriction,
					0.0f,
					1.0f
				);
				if (component.physicsMaxFallSpeed < 0.0f) {
					component.physicsMaxFallSpeed = 0.0f;
					physicsChanged = true;
				}
				if (physicsChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "PlayerBehavior") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool playerChanged = false;
				playerChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Move Speed"),
					&component.playerMoveSpeed,
					0.1f,
					0.0f,
					100.0f
				);
				playerChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Jump Velocity"),
					&component.playerJumpVelocity,
					0.1f,
					0.0f,
					200.0f
				);
				playerChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Turn Responsiveness"),
					&component.playerTurnResponsiveness,
					0.0f,
					1.0f
				);
				playerChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Dash Multiplier"),
					&component.playerDashMultiplier,
					0.05f,
					1.0f,
					5.0f
				);
				playerChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Camera Relative Move"),
					&component.playerCameraRelativeMove
				);
				playerChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Allow Jump"),
					&component.playerAllowJump
				);
				playerChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Auto Forward"),
					&component.playerAutoForward
				);
				if (ImGui::BeginCombo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Input Mode"),
					component.playerInputMode.c_str()
				)) {
					for (const char* mode : { "KeyboardMouse", "Gamepad", "Both" }) {
						if (ImGui::Selectable(mode, component.playerInputMode == mode)) {
							component.playerInputMode = mode;
							playerChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				const bool gamepadMode = component.playerInputMode == "Gamepad" ||
					component.playerInputMode == "Both";
				ImGui::BeginDisabled(!gamepadMode);
				playerChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Gamepad Deadzone"),
					&component.playerGamepadDeadzone,
					0.0f,
					0.95f
				);
				ImGui::EndDisabled();
				component.playerGamepadDeadzone = std::clamp(
					component.playerGamepadDeadzone,
					0.0f,
					0.95f
				);
				if (component.playerMoveSpeed < 0.0f) {
					component.playerMoveSpeed = 0.0f;
					playerChanged = true;
				}
				if (component.playerJumpVelocity < 0.0f) {
					component.playerJumpVelocity = 0.0f;
					playerChanged = true;
				}
				component.playerTurnResponsiveness = std::clamp(
					component.playerTurnResponsiveness,
					0.0f,
					1.0f
				);
				if (component.playerDashMultiplier < 1.0f) {
					component.playerDashMultiplier = 1.0f;
					playerChanged = true;
				}
				if (playerChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "FishingScoreAttackDirector") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool fishingChanged = false;
				auto drawFishingEntityReference = [
					&document,
					&fishingChanged
				](const char* label, uint64_t& entityId, const char* requiredType) {
					const SceneEntity* selected = entityId != 0
						? document.FindEntity(entityId)
						: nullptr;
					const std::string preview = selected
						? BuildEntityHierarchyLabel(document, *selected)
						: "Select Entity...";
					if (ImGui::BeginCombo(label, preview.c_str())) {
						for (const SceneEntity& candidate : document.GetEntities()) {
							if (!FindEnabledComponent(candidate, requiredType)) {
								continue;
							}
							const std::string candidateLabel =
								BuildEntityHierarchyLabel(document, candidate);
							if (ImGui::Selectable(
								candidateLabel.c_str(), entityId == candidate.id
							)) {
								entityId = candidate.id;
								fishingChanged = true;
							}
						}
						ImGui::EndCombo();
					}
				};
				drawFishingEntityReference(
					LocalizedComponentWidgetLabel(editorLanguage_, "Player"),
					component.fishingPlayerEntityId,
					"PlayerBehavior"
				);
				drawFishingEntityReference(
					LocalizedComponentWidgetLabel(editorLanguage_, "Water Volume"),
					component.fishingWaterVolumeEntityId,
					"WaterVolume"
				);
				drawFishingEntityReference(
					LocalizedComponentWidgetLabel(editorLanguage_, "Hook Spawn Area"),
					component.fishingHookSpawnAreaEntityId,
					"FishingHookSpawnArea"
				);
				drawFishingEntityReference(
					LocalizedComponentWidgetLabel(editorLanguage_, "Hook Pool"),
					component.fishingHookPoolEntityId,
					"FishingHookPool"
				);
				fishingChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Duration Seconds"),
					&component.fishingDurationSeconds, 0.1f, 0.1f, 3600.0f
				);
				fishingChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Use Hook Band Settings"),
					&component.fishingUseHookBandSettings
				);
				const int fishCountUpperBound = (std::max)(
					1,
					static_cast<int>((std::min)(
						component.fishingFishEntityIds.size(),
						static_cast<size_t>((std::numeric_limits<int>::max)())
					))
				);
				if (component.fishingMaxSelectableFishCount > fishCountUpperBound) {
					component.fishingMaxSelectableFishCount = fishCountUpperBound;
					fishingChanged = true;
				}
				fishingChanged |= ImGui::SliderInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Fish Count"),
					&component.fishingMaxSelectableFishCount, 1, fishCountUpperBound
				);
				fishingChanged |= DrawSceneInputExpressionEditor(
					LocalizedComponentWidgetLabel(editorLanguage_, "Fish Count Confirm Input"),
					component.fishingConfirmInputExpression,
					component.fishingConfirmInput,
					editorLanguage_
				);
				if (!component.fishingUseHookBandSettings) {
					ImGui::SeparatorText(
						LocalizedComponentWidgetLabel(editorLanguage_, "Legacy Distance Settings")
					);
					fishingChanged |= ImGui::DragInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Distance Band Count"),
						&component.fishingDistanceBandCount, 1.0f, 1, 32
					);
					fishingChanged |= ImGui::SliderInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Hooks Per Distance Band"),
						&component.fishingHooksPerDistanceBand, 1, 4
					);
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Multiplier Base"),
						&component.fishingDistanceMultiplierBase, 0.05f, 0.0f, 100.0f
					);
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Multiplier Step"),
						&component.fishingDistanceMultiplierStep, 0.05f, 0.0f, 100.0f
					);
				}
				if (component.fishingUseHookBandSettings) {
					ImGui::SeparatorText(
						LocalizedComponentWidgetLabel(editorLanguage_, "Hook Band Settings")
					);
					if (ImGui::Button(SelectEditorText(
						editorLanguage_,
						"推奨5区間設定を適用###ApplyFishingHookBandTemplate",
						"Apply Recommended 5-Band Template###ApplyFishingHookBandTemplate"
					))) {
						component.fishingHookBands = {
							{ 0.0f, 0, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
							{ 1.0f, 4, { 40.0f, 35.0f, 25.0f, 2.0f, 2.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f } },
							{ 1.2f, 3, { 8.0f, 8.0f, 8.0f, 24.0f, 24.0f, 24.0f, 2.0f, 2.0f, 1.0f, 1.0f } },
							{ 1.4f, 2, { 6.0f, 6.0f, 6.0f, 20.0f, 20.0f, 20.0f, 10.0f, 10.0f, 1.0f, 1.0f } },
							{ 1.6f, 2, { 3.0f, 3.0f, 3.0f, 10.0f, 10.0f, 10.0f, 24.0f, 24.0f, 4.0f, 9.0f } }
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
						EnsureFishingHookRanks(component);
						for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
							component.fishingHookRanks[tierIndex].color =
								component.fishingHookMultiplierColors[tierIndex];
						}
						fishingChanged = true;
					}
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Hook Score Unit"),
						&component.fishingHookScoreUnit, 10.0f, 0.001f, 1000000000.0f
					);
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Fish Multiplier Base"),
						&component.fishingFishMultiplierBase, 0.05f, 0.0f, 100000.0f
					);
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Fish Multiplier Per Additional Fish"),
						&component.fishingFishMultiplierPerAdditionalFish,
						0.05f, 0.0f, 100000.0f
					);
					const size_t rankCountBefore = component.fishingHookRanks.size();
					EnsureFishingHookRanks(component);
					fishingChanged |= rankCountBefore != component.fishingHookRanks.size();
					ImGui::SeparatorText(
						LocalizedComponentWidgetLabel(editorLanguage_, "Hook Rank Definitions")
					);
					for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
						SceneFishingHookRankDefinition& rank =
							component.fishingHookRanks[tierIndex];
						ImGui::PushID(static_cast<int>(tierIndex));
						const std::string rankLabel = SelectEditorText(
							editorLanguage_, "ランク", "Rank "
						) + std::to_string(tierIndex + 1);
						ImGui::TextUnformatted(rankLabel.c_str());
						fishingChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Stable ID"),
							rank.id
						);
						fishingChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Display Name"),
							rank.displayName
						);
						const char* currentModel = rank.modelPath.empty()
							? "None" : rank.modelPath.c_str();
						if (ImGui::BeginCombo(
							LocalizedComponentWidgetLabel(editorLanguage_, "Model"),
							currentModel
						)) {
							if (ImGui::Selectable("None", rank.modelPath.empty())) {
								rank.modelPath.clear();
								fishingChanged = true;
							}
							for (const std::string& modelPath : GetCachedModelAssetPaths()) {
								if (ImGui::Selectable(
									modelPath.c_str(), rank.modelPath == modelPath
								)) {
									rank.modelPath = modelPath;
									fishingChanged = true;
								}
							}
							ImGui::EndCombo();
						}
						if (ImGui::BeginDragDropTarget()) {
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
								"PROJECT_MODEL_PATH"
							)) {
								const char* droppedPath =
									static_cast<const char*>(payload->Data);
								if (droppedPath && droppedPath[0] != '\0') {
									rank.modelPath = GetModelPathRelativeToResources(droppedPath);
									fishingChanged = true;
								}
							}
							ImGui::EndDragDropTarget();
						}
						const char* currentIconTexture = rank.iconTexturePath.empty()
							? "None" : rank.iconTexturePath.c_str();
						if (ImGui::BeginCombo(
							LocalizedComponentWidgetLabel(editorLanguage_, "Icon Texture"),
							currentIconTexture
						)) {
							if (ImGui::Selectable("None", rank.iconTexturePath.empty())) {
								rank.iconTexturePath.clear();
								fishingChanged = true;
							}
							for (const std::string& texturePath : GetCachedTextureAssetPaths()) {
								if (ImGui::Selectable(
									texturePath.c_str(), rank.iconTexturePath == texturePath
								)) {
									rank.iconTexturePath = texturePath;
									fishingChanged = true;
								}
							}
							ImGui::EndCombo();
						}
						if (ImGui::BeginDragDropTarget()) {
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
								"PROJECT_TEXTURE_PATH"
							)) {
								const char* droppedPath = static_cast<const char*>(payload->Data);
								if (droppedPath && droppedPath[0] != '\0') {
									rank.iconTexturePath = GetProjectResourcePath(droppedPath);
									fishingChanged = true;
								}
							}
							ImGui::EndDragDropTarget();
						}
						fishingChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Score Multiplier"),
							&rank.scoreMultiplier,
							0.05f, 0.0f, 100000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp
						);
						fishingChanged |= ImGui::ColorEdit4(
							LocalizedComponentWidgetLabel(editorLanguage_, "Color"),
							&rank.color.x
						);
						ImGui::Separator();
						ImGui::PopID();
					}
					fishingChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Hook Color Emissive Intensity"),
						&component.fishingHookColorEmissiveIntensity,
						0.05f, 0.0f, 100.0f
					);
					for (size_t bandIndex = 0; bandIndex < component.fishingHookBands.size(); ++bandIndex) {
						SceneFishingHookBandSettings& band = component.fishingHookBands[bandIndex];
						ImGui::PushID(static_cast<int>(bandIndex));
						if (ImGui::TreeNodeEx(
							"FishingHookBandSettings", ImGuiTreeNodeFlags_DefaultOpen,
							"Band %zu", bandIndex
						)) {
							fishingChanged |= ImGui::DragFloat(
								"Distance Multiplier", &band.distanceMultiplier, 0.05f, 0.0f, 100.0f
							);
							fishingChanged |= ImGui::SliderInt(
								"Hook Count", &band.hookCount, 0, 30
							);
							if (band.hookMultiplierWeights.size() != 10) {
								band.hookMultiplierWeights.resize(10, 0.0f);
								fishingChanged = true;
							}
							float totalWeight = 0.0f;
							for (float weight : band.hookMultiplierWeights) {
								totalWeight += weight;
							}
							for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
								const std::string label = "x" + std::to_string(tierIndex + 1);
								fishingChanged |= ImGui::DragFloat(
									label.c_str(), &band.hookMultiplierWeights[tierIndex],
									0.1f, 0.0f, 100000.0f
								);
								const float percentage = totalWeight > 0.0f
									? band.hookMultiplierWeights[tierIndex] / totalWeight * 100.0f
									: 0.0f;
								ImGui::SameLine();
								ImGui::Text("(%.1f%%)", percentage);
							}
							ImGui::TreePop();
						}
						ImGui::PopID();
					}
				}
				fishingChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Randomize Seed On Play"),
					&component.fishingRandomizeSeedOnPlay
				);
				fishingChanged |= ImGui::InputInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Random Seed"),
					&component.fishingRandomSeed
				);
				ImGui::SeparatorText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Formation Capsule")
				);
				fishingChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Use Formation Capsule Collision"
					),
					&component.fishingUseFormationCapsuleCollision
				);
				fishingChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Formation Outline Visible"
					),
					&component.fishingFormationOutlineVisible
				);
				fishingChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Formation Outline Color"
					),
					&component.fishingFormationOutlineColor.x
				);
				fishingChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Formation Outline Bloom Intensity"
					),
					&component.fishingFormationOutlineBloomIntensity,
					0.1f,
					0.0f,
					32.0f
				);
				fishingChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Formation Outline Y Offset"
					),
					&component.fishingFormationOutlineYOffset,
					0.01f,
					-100.0f,
					100.0f
				);
				fishingChanged |= ImGui::SliderInt(
					LocalizedComponentWidgetLabel(
						editorLanguage_, "Formation Outline Segments"
					),
					&component.fishingFormationOutlineSegments,
					12,
					128
				);
				if (ImGui::TreeNodeEx(
					LocalizedComponentWidgetLabel(editorLanguage_, "Fish Entities"),
					ImGuiTreeNodeFlags_DefaultOpen
				)) {
					int removeFishIndex = -1;
					for (size_t fishIndex = 0;
						fishIndex < component.fishingFishEntityIds.size();
						++fishIndex) {
						ImGui::PushID(static_cast<int>(fishIndex));
						drawFishingEntityReference(
							"Fish", component.fishingFishEntityIds[fishIndex],
							"AgentBehavior"
						);
						if (ImGui::SmallButton(SelectEditorText(
							editorLanguage_, "削除###RemoveFishingFish", "Remove###RemoveFishingFish"
						))) {
							removeFishIndex = static_cast<int>(fishIndex);
						}
						ImGui::PopID();
					}
					if (removeFishIndex >= 0) {
						component.fishingFishEntityIds.erase(
							component.fishingFishEntityIds.begin() + removeFishIndex
						);
						if (component.fishingMaxSelectableFishCount >
							static_cast<int>(component.fishingFishEntityIds.size())) {
							component.fishingMaxSelectableFishCount = (std::max)(
								1, static_cast<int>(component.fishingFishEntityIds.size())
							);
						}
						fishingChanged = true;
					}
					if (ImGui::SmallButton(
						SelectEditorText(editorLanguage_, "魚を追加###AddFishingFish", "Add Fish###AddFishingFish")
					)) {
						component.fishingFishEntityIds.push_back(0);
						fishingChanged = true;
					}
					ImGui::TreePop();
				}
				if (ImGui::TreeNodeEx(
					LocalizedComponentWidgetLabel(editorLanguage_, "HUD Text References"),
					ImGuiTreeNodeFlags_DefaultOpen
				)) {
					drawFishingEntityReference(
						LocalizedComponentWidgetLabel(editorLanguage_, "Fish Count Text"),
						component.fishingFishCountTextEntityId, "TextRenderer"
					);
					drawFishingEntityReference(
						LocalizedComponentWidgetLabel(editorLanguage_, "Timer Text"),
						component.fishingTimerTextEntityId, "TextRenderer"
					);
					drawFishingEntityReference(
						LocalizedComponentWidgetLabel(editorLanguage_, "Score Text"),
						component.fishingScoreTextEntityId, "TextRenderer"
					);
					drawFishingEntityReference(
						LocalizedComponentWidgetLabel(editorLanguage_, "Multiplier Text"),
						component.fishingMultiplierTextEntityId, "TextRenderer"
					);
					drawFishingEntityReference(
						LocalizedComponentWidgetLabel(editorLanguage_, "Result Text"),
						component.fishingResultTextEntityId, "TextRenderer"
					);
					if (component.fishingUseHookBandSettings) {
						fishingChanged |= ImGui::Checkbox(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Legend Visible"),
							&component.fishingHookLegendVisible
						);
						drawFishingEntityReference(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Legend Title Text"),
							component.fishingHookLegendTitleTextEntityId, "TextRenderer"
						);
						for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
							if (component.fishingHookLegendTextEntityIds.size() <= tierIndex) {
								component.fishingHookLegendTextEntityIds.resize(tierIndex + 1, 0);
							}
							ImGui::PushID(static_cast<int>(tierIndex));
							drawFishingEntityReference(
								("Hook Legend x" + std::to_string(tierIndex + 1)).c_str(),
								component.fishingHookLegendTextEntityIds[tierIndex], "TextRenderer"
							);
							ImGui::PopID();
						}
						if (component.fishingHookLegendIconEntityIds.size() != 10) {
							component.fishingHookLegendIconEntityIds.resize(10, 0);
						}
						for (size_t tierIndex = 0; tierIndex < 10; ++tierIndex) {
							ImGui::PushID(static_cast<int>(tierIndex));
							drawFishingEntityReference(
								("Hook Legend Icon " + std::to_string(tierIndex + 1)).c_str(),
								component.fishingHookLegendIconEntityIds[tierIndex],
								"SpriteRenderer"
							);
							ImGui::PopID();
						}
						fishingChanged |= ImGui::DragFloat2(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Legend Icon Size"),
							&component.fishingHookLegendIconSize.x,
							1.0f,
							1.0f,
							8192.0f
						);
						fishingChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Legend Title"),
							component.fishingHookLegendTitle
						);
						fishingChanged |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Legend Prefix"),
							component.fishingHookLegendPrefix
						);
					}
					fishingChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Fish Count Prefix"),
						component.fishingFishCountPrefix
					);
					fishingChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Timer Prefix"),
						component.fishingTimerPrefix
					);
					fishingChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Score Prefix"),
						component.fishingScorePrefix
					);
					fishingChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Multiplier Prefix"),
						component.fishingMultiplierPrefix
					);
					fishingChanged |= InputTextString(
						LocalizedComponentWidgetLabel(editorLanguage_, "Result Prefix"),
						component.fishingResultPrefix
					);
					ImGui::TreePop();
				}
				if (fishingChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "FishingHookSpawnArea") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool fishingAreaChanged = false;
				fishingAreaChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Half Size X"),
					&component.fishingSpawnHalfSizeX, 0.1f, 0.001f, 10000.0f
				);
				fishingAreaChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Half Size Z"),
					&component.fishingSpawnHalfSizeZ, 0.1f, 0.001f, 10000.0f
				);
				fishingAreaChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Minimum Distance"),
					&component.fishingSpawnMinimumDistance, 0.1f, 0.0f, 10000.0f
				);
				fishingAreaChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Spawn Attempts"),
					&component.fishingSpawnMaxAttempts, 1.0f, 1, 256
				);
				if (fishingAreaChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "FishingHookPool") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool fishingPoolChanged = false;
				int removeEntryIndex = -1;
				for (size_t entryIndex = 0;
					entryIndex < component.fishingHookPoolEntries.size();
					++entryIndex) {
					SceneFishingHookPoolEntry& entry =
						component.fishingHookPoolEntries[entryIndex];
					ImGui::PushID(static_cast<int>(entryIndex));
					if (ImGui::TreeNodeEx(
						"FishingHookPoolEntry",
						ImGuiTreeNodeFlags_DefaultOpen,
						"Entry %zu", entryIndex + 1
					)) {
						const SceneEntity* selected = entry.hookEntityId != 0
							? document.FindEntity(entry.hookEntityId) : nullptr;
						const std::string preview = selected
							? BuildEntityHierarchyLabel(document, *selected)
							: "Select Hook Entity...";
						if (ImGui::BeginCombo(
							LocalizedComponentWidgetLabel(editorLanguage_, "Hook Entity"),
							preview.c_str()
						)) {
							for (const SceneEntity& candidate : document.GetEntities()) {
								if (!FindEnabledComponent(candidate, "FishingHook")) {
									continue;
									}
								const std::string candidateLabel =
									BuildEntityHierarchyLabel(document, candidate);
								if (ImGui::Selectable(
									candidateLabel.c_str(), entry.hookEntityId == candidate.id
								)) {
									entry.hookEntityId = candidate.id;
									fishingPoolChanged = true;
								}
							}
							ImGui::EndCombo();
						}
						for (size_t bandIndex = 0;
							bandIndex < entry.weightsByDistanceBand.size();
							++bandIndex) {
							fishingPoolChanged |= ImGui::DragFloat(
								("Band " + std::to_string(bandIndex)).c_str(),
								&entry.weightsByDistanceBand[bandIndex], 0.1f, 0.0f, 100000.0f
							);
						}
						if (ImGui::SmallButton(SelectEditorText(
							editorLanguage_, "Bandを追加###AddFishingWeightBand", "Add Band###AddFishingWeightBand"
						))) {
							entry.weightsByDistanceBand.push_back(1.0f);
							fishingPoolChanged = true;
						}
						if (!entry.weightsByDistanceBand.empty()) {
							ImGui::SameLine();
							if (ImGui::SmallButton(SelectEditorText(
								editorLanguage_, "Bandを削除###RemoveFishingWeightBand", "Remove Band###RemoveFishingWeightBand"
							))) {
								entry.weightsByDistanceBand.pop_back();
								fishingPoolChanged = true;
							}
						}
						if (ImGui::SmallButton(SelectEditorText(
							editorLanguage_, "Entryを削除###RemoveFishingHookEntry", "Remove Entry###RemoveFishingHookEntry"
						))) {
							removeEntryIndex = static_cast<int>(entryIndex);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (removeEntryIndex >= 0) {
					component.fishingHookPoolEntries.erase(
						component.fishingHookPoolEntries.begin() + removeEntryIndex
					);
					fishingPoolChanged = true;
				}
				if (ImGui::Button(SelectEditorText(
					editorLanguage_, "Entryを追加###AddFishingHookEntry", "Add Entry###AddFishingHookEntry"
				))) {
					SceneFishingHookPoolEntry entry{};
					entry.weightsByDistanceBand = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
					component.fishingHookPoolEntries.push_back(std::move(entry));
					fishingPoolChanged = true;
				}
				if (fishingPoolChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "FishingHook") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool fishingHookChanged = false;
				fishingHookChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Base Score"),
					&component.fishingHookBaseScore, 10.0f, 0, 1000000000
				);
				if (fishingHookChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "FishingShark") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool sharkChanged = false;
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Patrol Radius X"),
					&component.fishingSharkRadiusX, 0.1f, 0.001f, 10000.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Patrol Radius Z"),
					&component.fishingSharkRadiusZ, 0.1f, 0.001f, 10000.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Angular Speed"),
					&component.fishingSharkAngularSpeed, 0.01f, -100.0f, 100.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Initial Phase"),
					&component.fishingSharkInitialPhase, 0.01f, -1000.0f, 1000.0f
				);
				sharkChanged |= ImGui::DragInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Penalty Score"),
					&component.fishingSharkPenaltyScore, 10.0f, 0, 1000000000
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Hit Cooldown Seconds"),
					&component.fishingSharkHitCooldownSeconds, 0.05f, 0.0f, 3600.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Path Randomness"),
					&component.fishingSharkPathRandomness, 0.01f, 0.0f, 1.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Wander Move Speed"),
					&component.fishingSharkWanderMoveSpeed, 0.1f, 0.0f, 10000.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Wander Maximum Turn Rate"),
					&component.fishingSharkWanderMaximumTurnRate, 0.05f, 0.0f, 1000.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Obstacle Avoidance Distance"),
					&component.fishingSharkObstacleAvoidanceDistance, 0.1f, 0.0f, 10000.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Obstacle Avoidance Strength"),
					&component.fishingSharkObstacleAvoidanceStrength, 0.01f, 0.0f, 1.0f
				);
				sharkChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Obstacle Avoidance Response"),
					&component.fishingSharkObstacleAvoidanceResponse, 0.1f, 0.0f, 1000.0f
				);
				component.fishingSharkRadiusX = (std::max)(
					component.fishingSharkRadiusX, 0.001f
				);
				component.fishingSharkRadiusZ = (std::max)(
					component.fishingSharkRadiusZ, 0.001f
				);
				component.fishingSharkHitCooldownSeconds = (std::max)(
					component.fishingSharkHitCooldownSeconds, 0.0f
				);
				component.fishingSharkPathRandomness = (std::clamp)(
					component.fishingSharkPathRandomness, 0.0f, 1.0f
				);
				component.fishingSharkWanderMoveSpeed = (std::max)(
					component.fishingSharkWanderMoveSpeed, 0.0f
				);
				component.fishingSharkWanderMaximumTurnRate = (std::max)(
					component.fishingSharkWanderMaximumTurnRate, 0.0f
				);
				component.fishingSharkObstacleAvoidanceDistance = (std::max)(
					component.fishingSharkObstacleAvoidanceDistance, 0.0f
				);
				component.fishingSharkObstacleAvoidanceStrength = (std::clamp)(
					component.fishingSharkObstacleAvoidanceStrength, 0.0f, 1.0f
				);
				component.fishingSharkObstacleAvoidanceResponse = (std::max)(
					component.fishingSharkObstacleAvoidanceResponse, 0.0f
				);
				if (sharkChanged) {
					document.MarkDirty();
				}
				ImGui::TextDisabled(
					SelectEditorText(
						editorLanguage_,
						"Wander Move Speedが0なら従来の楕円周回、正なら自由遊泳です。初期位置はFishingScoreAttackDirectorが決めます。OBBColliderをTriggerにしてください。",
						"A Wander Move Speed of 0 uses the legacy ellipse patrol; a positive value enables free wander. FishingScoreAttackDirector chooses the initial position. Set the OBBCollider as a trigger."
					)
				);
				ImGui::EndDisabled();
			} else if (component.type == "FishingObstacle") {
				ImGui::TextDisabled(
					SelectEditorText(
						editorLanguage_,
						"MeshRendererと非Trigger Colliderを使用するStatic障害物です。",
						"Uses MeshRenderer and a non-trigger collider as a static obstacle."
					)
				);
			} else if (component.type == "AgentTeamLeaderController") {
				ImGui::TextDisabled(
					SelectEditorText(
						editorLanguage_,
						"所属Teamの仮想リーダーを、このEntityのTransformで制御します。Event設定はありません。",
						"Controls the owning Team's virtual leader from this Entity's Transform. No Event settings are available."
					)
				);
			} else if (component.type == "AgentBehavior") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool agentChanged = false;
				char behaviorBuffer[64]{};
				strncpy_s(
					behaviorBuffer,
					component.agentBehaviorName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Behavior"),
					behaviorBuffer,
					sizeof(behaviorBuffer)
				)) {
					component.agentBehaviorName = behaviorBuffer;
					agentChanged = true;
				}
				char profileBuffer[64]{};
				strncpy_s(
					profileBuffer,
					component.agentProfileName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Profile"),
					profileBuffer,
					sizeof(profileBuffer)
				)) {
					component.agentProfileName = profileBuffer;
					agentChanged = true;
				}
				const SceneTeamSettings* agentTeam =
					document.ResolveEntityTeam(*entity);
				const bool belongsToAgentTeam =
					agentTeam && !agentTeam->name.empty();
				const bool hasTeamAgentSettings =
					belongsToAgentTeam && agentTeam->agentBehaviorOverride;
				if (hasTeamAgentSettings) {
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Override Team Agent Settings"),
						&component.agentTeamSettingsOverride
					);
				}
				const bool useTeamAgentSettings =
					hasTeamAgentSettings &&
					!component.agentTeamSettingsOverride;
				if (useTeamAgentSettings) {
					ImGui::Text(
						SelectEditorText(
							editorLanguage_,
							"Team Agent設定を使用中: %s",
							"Using Team Agent Settings: %s"
						),
						agentTeam->name.c_str()
					);
				}
				const bool isGroundAgent =
					component.agentMovementMode == "GroundXZ";
				if (ImGui::BeginCombo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Movement Mode"),
					isGroundAgent ? "Ground XZ" : "Free 3D"
				)) {
					if (ImGui::Selectable("Free 3D", !isGroundAgent)) {
						component.agentMovementMode = "Free3D";
						agentChanged = true;
					}
					if (ImGui::Selectable("Ground XZ", isGroundAgent)) {
						component.agentMovementMode = "GroundXZ";
						agentChanged = true;
					}
					ImGui::EndCombo();
				}
				if (isGroundAgent) {
					ImGui::TextDisabled("%s", SelectEditorText(
						editorLanguage_,
						"EnemyBehavior後のPhysicsBody速度へXZ方向の分離を加えます。",
						"Adds XZ separation to PhysicsBody velocity after EnemyBehavior."
					));
					ImGui::TextDisabled("%s", SelectEditorText(
						editorLanguage_,
						"Transform、回転、垂直速度は他Systemが管理します。",
						"Transform, rotation, and vertical velocity remain owned by other systems."
					));
					ImGui::BeginDisabled(useTeamAgentSettings);
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "地上分離", "Ground Separation"));
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Radius"),
						&component.agentSeparationRadius,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Weight"),
						&component.agentSeparationWeight,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::InputInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Neighbor Limit"),
						&component.agentNeighborLimit
					);
					ImGui::EndDisabled();
				}
				ImGui::BeginDisabled(useTeamAgentSettings);
				if (
					belongsToAgentTeam ||
					component.agentSchooling ||
					isGroundAgent
				) {
					char groupBuffer[64]{};
					strncpy_s(
						groupBuffer,
						component.agentGroupName.c_str(),
						_TRUNCATE
					);
					if (ImGui::InputText(
						LocalizedComponentWidgetLabel(editorLanguage_, "Group"),
						groupBuffer,
						sizeof(groupBuffer)
					)) {
						component.agentGroupName = groupBuffer;
						agentChanged = true;
					}
				}

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "移動", "Motion"));
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Min Speed"),
					&component.agentMinSpeed,
					0.05f,
					0.0f,
					100.0f
				);
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Speed"),
					&component.agentMaxSpeed,
					0.05f,
					0.0f,
					100.0f
				);
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Turn Speed"),
					&component.agentTurnSpeed,
					0.05f,
					0.0f,
					20.0f
				);
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Wander Strength"),
					&component.agentWanderStrength,
					0.05f,
					0.0f,
					20.0f
				);
				if (component.agentWanderStrength > 0.0f) {
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Wander Change Interval"),
						&component.agentWanderChangeInterval,
						0.05f,
						0.0f,
						60.0f
					);
					agentChanged |= ImGui::SliderFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Wander Direction Range"),
						&component.agentWanderDirectionRange,
						0.0f,
						3.141592f
					);
					agentChanged |= ImGui::SliderFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Wander Vertical Range"),
						&component.agentWanderVerticalRange,
						0.0f,
						1.0f
					);
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Randomize Seed On Play"),
						&component.agentRandomizeSeedOnPlay
					);
					if (!component.agentRandomizeSeedOnPlay) {
						agentChanged |= ImGui::InputInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Random Seed"),
							&component.agentRandomSeed
						);
					}
				}
				if (belongsToAgentTeam) {
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Flock Decision Interval"),
						&component.agentFlockDecisionInterval,
						0.01f,
						0.0f,
						5.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Flock Acceleration"),
						&component.agentFlockAcceleration,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Flock Max Turn Rate"),
						&component.agentFlockTurnRate,
						0.01f,
						0.0f,
						6.283185f
					);
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "メンバー追従", "Member Follow"));
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Return Strength"),
						&component.agentMemberCenterFollow,
						0.05f,
						0.0f,
						20.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Jitter Strength"),
						&component.agentMemberJitterStrength,
						0.01f,
						0.0f,
						10.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Jitter Frequency"),
						&component.agentMemberJitterFrequency,
						0.01f,
						0.0f,
						10.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Jitter Update Interval"),
						&component.agentMemberJitterUpdateInterval,
						0.01f,
						0.0f,
						10.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Jitter Follow Speed"),
						&component.agentMemberJitterFollowSpeed,
						0.01f,
						0.0f,
						20.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Max Distance"),
						&component.agentMemberLeashDistance,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Leash Strength"),
						&component.agentMemberLeashStrength,
						0.05f,
						0.0f,
						20.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Catchup Speed"),
						&component.agentMemberCatchupSpeed,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Update Interval"),
						&component.agentMemberSeparationUpdateInterval,
						0.01f,
						0.0f,
						5.0f
					);
					agentChanged |= ImGui::SliderFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Blend"),
						&component.agentMemberSeparationBlend,
						0.0f,
						1.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Member Minimum Distance"),
						&component.agentMemberMinimumDistance,
						0.05f,
						0.0f,
						100.0f
					);

					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Team Heading", "Team Heading"));
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Use Team Heading"),
						&component.agentUseTeamHeading
					);
					if (component.agentUseTeamHeading) {
						agentChanged |= ImGui::DragFloat3(
							LocalizedComponentWidgetLabel(editorLanguage_, "Team Heading Direction"),
							&component.agentTeamHeadingDirection.x,
							0.01f,
							-1.0f,
							1.0f
						);
						agentChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Team Heading Weight"),
							&component.agentTeamHeadingWeight,
							0.05f,
							0.0f,
							20.0f
						);
						agentChanged |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Team Heading Follow Speed"),
							&component.agentTeamHeadingFollowSpeed,
							0.05f,
							0.0f,
							20.0f
						);
					}
				}

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "回転", "Rotation"));
				agentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Align Forward To Velocity"),
					&component.agentAlignForwardToVelocity
				);
				if (component.agentAlignForwardToVelocity) {
					const char* agentForwardAxes[] = {
						"+Z",
						"-Z",
						"+X",
						"-X",
						"+Y",
						"-Y"
					};
					if (ImGui::BeginCombo(
						LocalizedComponentWidgetLabel(editorLanguage_, "Forward Axis"),
						component.agentForwardAxis.c_str()
					)) {
						for (const char* axis : agentForwardAxes) {
							if (ImGui::Selectable(
								axis,
								component.agentForwardAxis == axis
							)) {
								component.agentForwardAxis = axis;
								agentChanged = true;
							}
						}
						ImGui::EndCombo();
					}
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Rotate X"),
						&component.agentRotateAxisX
					);
					ImGui::SameLine();
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Rotate Y"),
						&component.agentRotateAxisY
					);
					ImGui::SameLine();
					agentChanged |= ImGui::Checkbox(
						LocalizedComponentWidgetLabel(editorLanguage_, "Rotate Z"),
						&component.agentRotateAxisZ
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Rotation Follow Speed"),
						&component.agentRotationFollowSpeed,
						0.05f,
						0.0f,
						60.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Pitch From Vertical Velocity"),
						&component.agentPitchFromVerticalVelocity,
						0.05f,
						0.0f,
						4.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Banking Strength"),
						&component.agentBankingStrength,
						0.05f,
						0.0f,
						4.0f
					);
				}
				ImGui::EndDisabled();

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Bounds", "Bounds"));
				agentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Use Water Bounds"),
					&component.agentUseWaterBounds
				);
				agentChanged |= ImGui::InputScalar(
					LocalizedComponentWidgetLabel(editorLanguage_, "Bounds Entity Id"),
					ImGuiDataType_U64,
					&component.agentBoundsEntityId
				);
				char boundsNameBuffer[128]{};
				strncpy_s(
					boundsNameBuffer,
					component.agentBoundsName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Bounds Name"),
					boundsNameBuffer,
					sizeof(boundsNameBuffer)
				)) {
					component.agentBoundsName = boundsNameBuffer;
					agentChanged = true;
				}
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Bounds Weight"),
					&component.agentBoundsWeight,
					0.05f,
					0.0f,
					50.0f
				);

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Attractor", "Attractor"));
				agentChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Attractor Weight"),
					&component.agentAttractorWeight,
					0.05f,
					0.0f,
					50.0f
				);
				if (component.agentAttractorWeight > 0.0f) {
					agentChanged |= ImGui::InputScalar(
						LocalizedComponentWidgetLabel(editorLanguage_, "Attractor Entity Id"),
						ImGuiDataType_U64,
						&component.agentAttractorEntityId
					);
					char attractorTagBuffer[64]{};
					strncpy_s(
						attractorTagBuffer,
						component.agentAttractorTag.c_str(),
						_TRUNCATE
					);
					if (ImGui::InputText(
						LocalizedComponentWidgetLabel(editorLanguage_, "Attractor Tag"),
						attractorTagBuffer,
						sizeof(attractorTagBuffer)
					)) {
						component.agentAttractorTag = attractorTagBuffer;
						agentChanged = true;
					}
				}

				ImGui::BeginDisabled(useTeamAgentSettings);
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Schooling", "Schooling"));
				agentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Schooling"),
					&component.agentSchooling
				);
				if (component.agentSchooling) {
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Schooling Update Interval"),
						&component.agentSchoolingUpdateInterval,
						0.01f,
						0.0f,
						5.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Schooling Update Jitter"),
						&component.agentSchoolingUpdateJitter,
						0.01f,
						0.0f,
						1.0f
					);
					agentChanged |= ImGui::InputInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Neighbor Limit"),
						&component.agentNeighborLimit
					);
					agentChanged |= ImGui::SliderFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Schooling Blend"),
						&component.agentSchoolingBlend,
						0.0f,
						1.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Radius"),
						&component.agentSeparationRadius,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Alignment Radius"),
						&component.agentAlignmentRadius,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Cohesion Radius"),
						&component.agentCohesionRadius,
						0.05f,
						0.0f,
						100.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Separation Weight"),
						&component.agentSeparationWeight,
						0.05f,
						0.0f,
						50.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Alignment Weight"),
						&component.agentAlignmentWeight,
						0.05f,
						0.0f,
						50.0f
					);
					agentChanged |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Cohesion Weight"),
						&component.agentCohesionWeight,
						0.05f,
						0.0f,
						50.0f
					);
				}

				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "表示", "Visual"));
				agentChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Visual Color"),
					&component.agentVisualColor.x,
					ImGuiColorEditFlags_Float
				);
				agentChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Enable Lighting"),
					&component.agentEnableLighting
				);
				ImGui::EndDisabled();

				if (component.agentBehaviorName.empty()) {
					component.agentBehaviorName = "Agent";
					agentChanged = true;
				}
				if (component.agentProfileName.empty()) {
					component.agentProfileName = "Default";
					agentChanged = true;
				}
				component.agentMinSpeed =
					(std::max)(component.agentMinSpeed, 0.0f);
				component.agentMaxSpeed =
					(std::max)(component.agentMaxSpeed, component.agentMinSpeed);
				component.agentTurnSpeed =
					(std::max)(component.agentTurnSpeed, 0.0f);
				component.agentWanderStrength =
					(std::max)(component.agentWanderStrength, 0.0f);
				component.agentBoundsWeight =
					(std::max)(component.agentBoundsWeight, 0.0f);
				const Vector3 normalizedTeamHeading =
					Math::Length(component.agentTeamHeadingDirection) <= 0.000001f
						? Vector3{ 0.0f, 0.0f, 1.0f }
						: Math::Normalize(component.agentTeamHeadingDirection);
				if (
					component.agentTeamHeadingDirection.x !=
						normalizedTeamHeading.x ||
					component.agentTeamHeadingDirection.y !=
						normalizedTeamHeading.y ||
					component.agentTeamHeadingDirection.z !=
						normalizedTeamHeading.z
				) {
					component.agentTeamHeadingDirection =
						normalizedTeamHeading;
					agentChanged = true;
				}
				if (component.agentTeamHeadingWeight < 0.0f) {
					component.agentTeamHeadingWeight = 0.0f;
					agentChanged = true;
				}
				if (component.agentTeamHeadingFollowSpeed < 0.0f) {
					component.agentTeamHeadingFollowSpeed = 0.0f;
					agentChanged = true;
				}
				const float teamRotationWeight = std::clamp(
					component.agentTeamRotationWeight,
					0.0f,
					1.0f
				);
				if (component.agentTeamRotationWeight != teamRotationWeight) {
					component.agentTeamRotationWeight = teamRotationWeight;
					agentChanged = true;
				}
				if (component.agentTeamRotationFollowSpeed < 0.0f) {
					component.agentTeamRotationFollowSpeed = 0.0f;
					agentChanged = true;
				}
				if (
					component.agentForwardAxis != "+Z" &&
					component.agentForwardAxis != "-Z" &&
					component.agentForwardAxis != "+X" &&
					component.agentForwardAxis != "-X" &&
					component.agentForwardAxis != "+Y" &&
					component.agentForwardAxis != "-Y"
				) {
					component.agentForwardAxis = "+Z";
					agentChanged = true;
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
				component.agentSchoolingBlend = std::clamp(
					component.agentSchoolingBlend,
					0.0f,
					1.0f
				);
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
				if (agentChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "AgentAttractor") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool attractorChanged = false;
				char tagBuffer[64]{};
				strncpy_s(
					tagBuffer,
					component.attractorTag.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(LocalizedComponentWidgetLabel(editorLanguage_, "Tag"), tagBuffer, sizeof(tagBuffer))) {
					component.attractorTag = tagBuffer;
					attractorChanged = true;
				}
				char targetBehaviorBuffer[64]{};
				strncpy_s(
					targetBehaviorBuffer,
					component.attractorTargetBehaviorName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Behavior"),
					targetBehaviorBuffer,
					sizeof(targetBehaviorBuffer)
				)) {
					component.attractorTargetBehaviorName =
						targetBehaviorBuffer;
					attractorChanged = true;
				}
				char targetProfileBuffer[64]{};
				strncpy_s(
					targetProfileBuffer,
					component.attractorTargetProfileName.c_str(),
					_TRUNCATE
				);
				if (ImGui::InputText(
					LocalizedComponentWidgetLabel(editorLanguage_, "Target Profile"),
					targetProfileBuffer,
					sizeof(targetProfileBuffer)
				)) {
					component.attractorTargetProfileName =
						targetProfileBuffer;
					attractorChanged = true;
				}
				attractorChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Radius"),
					&component.attractorRadius,
					0.1f,
					0.0f,
					500.0f
				);
				attractorChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Strength"),
					&component.attractorStrength,
					0.05f,
					0.0f,
					50.0f
				);
				attractorChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Visual Color"),
					&component.attractorVisualColor.x,
					ImGuiColorEditFlags_Float
				);
				if (component.attractorTag.empty()) {
					component.attractorTag = "Default";
					attractorChanged = true;
				}
				component.attractorRadius =
					(std::max)(component.attractorRadius, 0.0f);
				component.attractorStrength =
					(std::max)(component.attractorStrength, 0.0f);
				if (attractorChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			} else if (component.type == "WaterVolume") {
				ImGui::BeginDisabled(!editorSession_->IsEditing() || entityLocked);
				bool waterChanged = false;
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Volume", "Volume"));
				waterChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Half Size"),
					&component.waterHalfSize.x,
					0.1f,
					0.1f,
					500.0f
				);
				waterChanged |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Offset"),
					&component.waterOffset.x,
					0.1f
				);
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Surface", "Surface"));
				waterChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Surface Enabled"),
					&component.waterSurfaceEnabled
				);
				waterChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Base Color"),
					&component.waterSurfaceBaseColor.x,
					ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR
				);
				waterChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Highlight Color"),
					&component.waterSurfaceHighlightColor.x,
					ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Surface Alpha"),
					&component.waterSurfaceAlpha,
					0.0f,
					1.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Wave Scale"),
					&component.waterSurfaceWaveScale,
					0.0f,
					3.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Normal Strength"),
					&component.waterSurfaceNormalStrength,
					0.0f,
					2.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Fresnel Power"),
					&component.waterSurfaceFresnelPower,
					0.2f,
					8.0f
				);
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Water Light", "Water Light"));
				waterChanged |= ImGui::Checkbox(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Shafts"),
					&component.waterLightShaftEnabled
				);
				waterChanged |= ImGui::ColorEdit4(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Color"),
					&component.waterLightColor.x,
					ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR
				);
				if (ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Direction"),
					&component.waterLightDirection.x,
					0.01f,
					-1.0f,
					1.0f
				)) {
					waterChanged = true;
				}
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Intensity"),
					&component.waterLightIntensity,
					0.0f,
					3.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Light Density"),
					&component.waterLightDensity,
					0.0f,
					0.25f,
					"%.4f"
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Caustics Intensity"),
					&component.waterLightCausticsIntensity,
					0.0f,
					2.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Caustics Scale"),
					&component.waterLightCausticsScale,
					0.02f,
					8.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Caustics Speed"),
					&component.waterLightCausticsSpeed,
					0.0f,
					5.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Breakup Strength"),
					&component.waterLightBreakupStrength,
					0.0f,
					3.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Warp Strength"),
					&component.waterLightWarpStrength,
					0.0f,
					3.0f
				);
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Noise Scale"),
					&component.waterLightNoiseScale,
					0.1f,
					4.0f
				);
				waterChanged |= ImGui::SliderInt(
					LocalizedComponentWidgetLabel(editorLanguage_, "Raymarch Samples"),
					&component.waterLightSampleCount,
					4,
					32
				);
				ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Player Behavior", "Player Behavior"));
				waterChanged |= ImGui::SliderFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Move Speed Multiplier"),
					&component.waterMoveSpeedMultiplier,
					0.0f,
					1.0f
				);
				waterChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Gravity Scale"),
					&component.waterGravityScale,
					0.02f,
					-5.0f,
					5.0f
				);
				waterChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Drag"),
					&component.waterDrag,
					0.05f,
					0.0f,
					100.0f
				);
				waterChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Max Fall Speed"),
					&component.waterMaxFallSpeed,
					0.1f,
					0.0f,
					100.0f
				);
				waterChanged |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Swim Up Speed"),
					&component.waterSwimUpSpeed,
					0.1f,
					0.0f,
					100.0f
				);
				component.waterHalfSize.x =
					(std::max)(component.waterHalfSize.x, 0.1f);
				component.waterHalfSize.y =
					(std::max)(component.waterHalfSize.y, 0.1f);
				component.waterHalfSize.z =
					(std::max)(component.waterHalfSize.z, 0.1f);
				component.waterMoveSpeedMultiplier = std::clamp(
					component.waterMoveSpeedMultiplier,
					0.0f,
					1.0f
				);
				component.waterSurfaceAlpha = std::clamp(
					component.waterSurfaceAlpha,
					0.0f,
					1.0f
				);
				component.waterSurfaceWaveScale = std::clamp(
					component.waterSurfaceWaveScale,
					0.0f,
					3.0f
				);
				component.waterSurfaceNormalStrength = std::clamp(
					component.waterSurfaceNormalStrength,
					0.0f,
					2.0f
				);
				component.waterSurfaceFresnelPower = std::clamp(
					component.waterSurfaceFresnelPower,
					0.2f,
					8.0f
				);
				component.waterLightIntensity =
					(std::max)(component.waterLightIntensity, 0.0f);
				component.waterLightDensity =
					(std::max)(component.waterLightDensity, 0.0f);
				component.waterLightCausticsIntensity =
					(std::max)(component.waterLightCausticsIntensity, 0.0f);
				component.waterLightCausticsScale =
					(std::max)(component.waterLightCausticsScale, 0.001f);
				component.waterLightBreakupStrength = std::clamp(
					component.waterLightBreakupStrength,
					0.0f,
					3.0f
				);
				component.waterLightWarpStrength = std::clamp(
					component.waterLightWarpStrength,
					0.0f,
					3.0f
				);
				component.waterLightNoiseScale =
					(std::max)(component.waterLightNoiseScale, 0.001f);
				component.waterLightSampleCount = std::clamp(
					component.waterLightSampleCount,
					4,
					32
				);
				component.waterDrag = (std::max)(component.waterDrag, 0.0f);
				component.waterMaxFallSpeed =
					(std::max)(component.waterMaxFallSpeed, 0.0f);
				component.waterSwimUpSpeed =
					(std::max)(component.waterSwimUpSpeed, 0.0f);
				if (waterChanged) {
					document.MarkDirty();
				}
				ImGui::EndDisabled();
			}
			ImGui::PopID();
		}
		if (!removeComponentType.empty()) {
			document.RemoveComponent(entity->id, removeComponentType);
			editorSession_->RequestSceneReload();
		}

		ImGui::SeparatorText(SelectEditorText(
			editorLanguage_,
			"Componentを追加",
			"Add Component"
		));
		const bool canOpenComponentPicker = !entityLocked && !entity->folder &&
			editorSession_->IsEditing();
		ImGui::BeginDisabled(!canOpenComponentPicker);
		if (ImGui::Button(
			SelectEditorText(
				editorLanguage_,
				"Componentを追加...##OpenSceneComponentPicker",
				"Add Components...##OpenSceneComponentPicker"
			),
			ImVec2(-1.0f, 0.0f)
		)) {
			OpenComponentPicker(
				sceneComponentPicker_,
				ComponentPickerTarget::Scene,
				document,
				editorSession_->GetEditSceneId(),
				entity->id
			);
		}
		ImGui::EndDisabled();
		if (!canOpenComponentPicker) {
			ImGui::TextDisabled("%s", entityLocked
				? SelectEditorText(
					editorLanguage_,
					"ロックを解除すると追加できます。",
					"Unlock the entity to add components."
				)
				: entity->folder
					? SelectEditorText(
						editorLanguage_,
						"FolderへComponentは追加できません。",
						"Components cannot be added to a folder."
					)
					: SelectEditorText(
						editorLanguage_,
						"Playを停止すると追加できます。",
						"Stop Play mode to add components."
					));
		}

		if (editorSession_->IsPlaying() || editorSession_->IsPaused()) {
			ImGui::TextDisabled("Play mode changes are temporary");
		}
	}
	else {
		// Default scene hierarchy inspector
		static const char* itemNames[] = {
			"Main Camera",
			"Environment",
			"Scene Objects",
			"Lights",
			"Effects"
		};
		ImGui::TextUnformatted(itemNames[selectedHierarchyItem_]);
		ImGui::Separator();
	}

	ImGui::End();
}

void ImGuiManager::RequestPrefabQuickOpen() {
	showProject_ = true;
	prefabQuickOpenPopupRequested_ = true;
}

void ImGuiManager::DrawProjectPrefabAccessPanel() {
	ImGui::SeparatorText(SelectEditorText(
		editorLanguage_,
		"Prefabs###ProjectPrefabs",
		"Prefabs###ProjectPrefabs"
	));
	if (ImGui::Button(
		SelectEditorText(
			editorLanguage_,
			"Quick Open...###ProjectQuickOpenPrefab",
			"Quick Open...###ProjectQuickOpenPrefab"
		),
		ImVec2(-1.0f, 0.0f)
	)) {
		RequestPrefabQuickOpen();
	}

	std::string openRequestedPath;
	PrefabAssetReference toggleFavoriteReference{};
	bool toggleFavoriteRequested = false;
	PrefabAssetReference removeRecentReference{};
	bool removeRecentRequested = false;
	auto drawPrefabList = [&](
		const char* label,
		const std::vector<PrefabAssetReference>& references,
		bool recentList
	) {
		if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		if (references.empty()) {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"なし",
				"None"
			));
		}
		for (const PrefabAssetReference& reference : references) {
			const std::string resolvedProjectPath =
				PrefabAssetRegistry::ResolvePath(reference);
			const std::string displayProjectPath = resolvedProjectPath.empty()
				? reference.fallbackPath
				: resolvedProjectPath;
			const std::filesystem::path path =
				EditableResourcePath::ResolveResource(
					PathFromUtf8(displayProjectPath)
				).lexically_normal();
			const std::string prefabPath = PathToUtf8(path);
			const std::string fileName = PathToUtf8(path.filename());
			std::error_code existsError;
			const bool exists =
				!resolvedProjectPath.empty() &&
				std::filesystem::exists(path, existsError);
			const std::string itemLabel = exists
				? fileName + "###ProjectPrefabAccessItem"
				: fileName + SelectEditorText(
					editorLanguage_,
					" [見つかりません]###ProjectPrefabAccessItem",
					" [Missing]###ProjectPrefabAccessItem"
				);
			const std::string itemId = reference.assetId.empty()
				? reference.fallbackPath
				: reference.assetId + "|" + reference.fallbackPath;
			ImGui::PushID(itemId.c_str());
			const bool selected = selectedProjectFile_ == prefabPath;
			if (ImGui::Selectable(itemLabel.c_str(), selected) && exists) {
				SelectPrefabAssetInProject(prefabPath);
			}
			if (
				exists &&
				ImGui::IsItemHovered() &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
			) {
				openRequestedPath = prefabPath;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", displayProjectPath.c_str());
			}
			if (ImGui::BeginPopupContextItem("PrefabAccessContext")) {
				if (ImGui::MenuItem(
					SelectEditorText(
						editorLanguage_,
						"Prefabを開く###OpenProjectPrefab",
						"Open Prefab###OpenProjectPrefab"
					),
					nullptr,
					false,
					exists
				)) {
					openRequestedPath = prefabPath;
				}
				const bool favorite = ContainsPrefabAssetReference(
					favoritePrefabReferences_,
					reference
				);
				if (ImGui::MenuItem(
					favorite
						? SelectEditorText(
							editorLanguage_,
							"お気に入りから削除###ToggleProjectPrefabFavorite",
							"Remove from Favorites###ToggleProjectPrefabFavorite"
						)
						: SelectEditorText(
							editorLanguage_,
							"お気に入りに追加###ToggleProjectPrefabFavorite",
							"Add to Favorites###ToggleProjectPrefabFavorite"
						)
				)) {
					toggleFavoriteReference = reference;
					toggleFavoriteRequested = true;
				}
				if (
					recentList &&
					ImGui::MenuItem(SelectEditorText(
						editorLanguage_,
						"最近使用した項目から削除###RemoveRecentPrefab",
						"Remove from Recent###RemoveRecentPrefab"
					))
				) {
					removeRecentReference = reference;
					removeRecentRequested = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	};

	std::vector<PrefabAssetReference> favorites = favoritePrefabReferences_;
	std::sort(
		favorites.begin(),
		favorites.end(),
		[](const PrefabAssetReference& left, const PrefabAssetReference& right) {
			return PathFromUtf8(PrefabAssetRegistry::ResolvePath(left)).filename() <
				PathFromUtf8(PrefabAssetRegistry::ResolvePath(right)).filename();
		}
	);
	drawPrefabList(
		SelectEditorText(
			editorLanguage_,
			"お気に入り###ProjectPrefabFavorites",
			"Favorites###ProjectPrefabFavorites"
		),
		favorites,
		false
	);
	drawPrefabList(
		SelectEditorText(
			editorLanguage_,
			"最近使用した項目###ProjectPrefabRecent",
			"Recent###ProjectPrefabRecent"
		),
		recentPrefabReferences_,
		true
	);

	if (toggleFavoriteRequested) {
		ToggleFavoritePrefab(toggleFavoriteReference);
	}
	if (removeRecentRequested) {
		recentPrefabReferences_.erase(
			std::remove_if(
				recentPrefabReferences_.begin(),
				recentPrefabReferences_.end(),
				[&removeRecentReference](const PrefabAssetReference& recent) {
					return PrefabAssetRegistry::IsSameAsset(
						recent,
						removeRecentReference
					);
				}
			),
			recentPrefabReferences_.end()
		);
		SaveEditorSettings();
	}
	if (!openRequestedPath.empty()) {
		RequestOpenPrefab(openRequestedPath);
	}
}

void ImGuiManager::DrawPrefabQuickOpenPopup() {
	if (!ImGui::BeginPopup(SelectEditorText(
		editorLanguage_,
		"Prefab Quick Open###PrefabQuickOpenPopup",
		"Quick Open Prefab###PrefabQuickOpenPopup"
	))) {
		return;
	}
	ImGui::SetNextItemWidth(520.0f);
	if (prefabQuickOpenFocusRequested_) {
		ImGui::SetKeyboardFocusHere();
		prefabQuickOpenFocusRequested_ = false;
	}
	const bool searchSubmitted = ImGui::InputTextWithHint(
		"##PrefabQuickSearch",
		SelectEditorText(
			editorLanguage_,
			"Prefabを検索...",
			"Search Prefabs..."
		),
		prefabQuickOpenSearchBuffer_,
		sizeof(prefabQuickOpenSearchBuffer_),
		ImGuiInputTextFlags_EnterReturnsTrue
	);
	ImGui::Separator();

	std::string openRequestedPath;
	std::string selectRequestedPath;
	std::string firstVisiblePath;
	int visibleResultCount = 0;
	if (ImGui::BeginChild(
		"PrefabQuickOpenResults",
		ImVec2(520.0f, 300.0f),
		ImGuiChildFlags_Borders
	)) {
		for (const std::string& prefabPath : GetCachedPrefabAssetPaths()) {
			const std::string relativePath = PathToUtf8(
				EditableResourcePath::ToProjectRelative(
					PathFromUtf8(prefabPath)
				)
			);
			if (!ContainsCaseInsensitive(
				relativePath,
				prefabQuickOpenSearchBuffer_
			)) {
				continue;
			}
			if (firstVisiblePath.empty()) {
				firstVisiblePath = prefabPath;
			}
			++visibleResultCount;
			const std::string fileName = PathToUtf8(
				PathFromUtf8(prefabPath).filename()
			);
			const std::string label =
				(IsFavoritePrefab(prefabPath)
					? SelectEditorText(
						editorLanguage_,
						"[お気に入り] ",
						"[Favorite] "
					)
					: "") + fileName + "###" + prefabPath;
			if (ImGui::Selectable(label.c_str())) {
				openRequestedPath = prefabPath;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", relativePath.c_str());
			}
			if (ImGui::BeginPopupContextItem("QuickPrefabContext")) {
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"Prefabを開く###QuickOpenPrefab",
					"Open Prefab###QuickOpenPrefab"
				))) {
					openRequestedPath = prefabPath;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"Assetを選択###SelectQuickOpenPrefabAsset",
					"Select Asset###SelectQuickOpenPrefabAsset"
				))) {
					selectRequestedPath = prefabPath;
				}
				const bool favorite = IsFavoritePrefab(prefabPath);
				if (ImGui::MenuItem(
					favorite
						? SelectEditorText(
							editorLanguage_,
							"お気に入りから削除###ToggleQuickOpenPrefabFavorite",
							"Remove from Favorites###ToggleQuickOpenPrefabFavorite"
						)
						: SelectEditorText(
							editorLanguage_,
							"お気に入りに追加###ToggleQuickOpenPrefabFavorite",
							"Add to Favorites###ToggleQuickOpenPrefabFavorite"
						)
				)) {
					ToggleFavoritePrefab(prefabPath);
				}
				ImGui::EndPopup();
			}
		}
		if (visibleResultCount == 0) {
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"一致するPrefabがありません。",
				"No matching Prefabs."
			));
		}
	}
	ImGui::EndChild();
	if (searchSubmitted && !firstVisiblePath.empty()) {
		openRequestedPath = firstVisiblePath;
	}
	if (!selectRequestedPath.empty()) {
		SelectPrefabAssetInProject(selectRequestedPath);
		ImGui::CloseCurrentPopup();
	}
	if (!openRequestedPath.empty()) {
		RequestOpenPrefab(openRequestedPath);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void ImGuiManager::DrawProjectWindow() {
	if (projectFocusRequested_) {
		ImGui::SetNextWindowFocus();
		projectFocusRequested_ = false;
	}
	if (!ImGui::Begin(
		SelectEditorText(
			editorLanguage_,
			"Project###Project",
			"Project###Project"
		),
		&showProject_
	)) {
		ImGui::End();
		return;
	}
	const std::filesystem::path projectResourceRoot =
		GetProjectResourceRoot();
	const std::string projectResourceRootPath =
		PathToUtf8(projectResourceRoot);
	std::error_code projectRootError;
	if (
		selectedProjectFolder_.empty() ||
		!std::filesystem::exists(
			PathFromUtf8(selectedProjectFolder_),
			projectRootError
		)
	) {
		selectedProjectFolder_ = projectResourceRootPath;
		selectedProjectFile_.clear();
		projectDirectoryCacheDirty_ = true;
	}

	ImGui::Columns(2, "ProjectColumns", true);

	// Left column: directory tree
	static bool setColWidth = false;
	if (!setColWidth) {
		ImGui::SetColumnWidth(0, 180.0f);
		setColWidth = true;
	}

	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"フォルダー",
		"Folders"
	));
	ImGui::Separator();
	
	// Draw recursive tree starting from "resources"
	if (projectTreeCacheDirty_) {
		RefreshProjectTreeCache();
	}
	DrawDirectoryTreeNode(cachedProjectTreeRoot_);
	DrawProjectPrefabAccessPanel();
	
	ImGui::NextColumn();

	// Right column: files inside selected folder
	const std::string displayFolder = PathToUtf8(
		EditableResourcePath::ToProjectRelative(
			PathFromUtf8(selectedProjectFolder_)
		)
	);
	ImGui::Text(
		SelectEditorText(editorLanguage_, "表示中: %s", "Contents of: %s"),
		displayFolder.c_str()
	);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", SelectEditorText(editorLanguage_, "表示:", "View:"));
	ImGui::SameLine();
	if (projectGridView_) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	}
	if (ImGui::SmallButton(SelectEditorText(
		editorLanguage_,
		"グリッド###ProjectGridView",
		"Grid###ProjectGridView"
	))) {
		projectGridView_ = true;
	}
	if (projectGridView_) {
		ImGui::PopStyleColor();
	}
	ImGui::SameLine();
	if (!projectGridView_) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	}
	if (ImGui::SmallButton(SelectEditorText(
		editorLanguage_,
		"リスト###ProjectListView",
		"List###ProjectListView"
	))) {
		projectGridView_ = false;
	}
	if (!projectGridView_) {
		ImGui::PopStyleColor();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(SelectEditorText(
		editorLanguage_,
		"更新###RefreshProjectAssets",
		"Refresh###RefreshProjectAssets"
	))) {
		InvalidateProjectCache();
		projectPreviewLoadAttempted_.clear();
		TextureManager::GetInstance()->ClearFailedTextureCache();
		ModelManager::GetInstance()->ClearFailedModelCache();
	}
	ImGui::SameLine();
	ImGui::Checkbox(
		SelectEditorText(
			editorLanguage_,
			"Prefabのみ###ProjectPrefabsOnly",
			"Prefabs Only###ProjectPrefabsOnly"
		),
		&projectPrefabFilterEnabled_
	);
	if (projectGridView_) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(100.0f);
		ImGui::SliderFloat(
			"##ProjectThumbnailSize",
			&projectThumbnailSize_,
			56.0f,
			128.0f,
			"%.0f px"
		);
	}
	ImGui::Separator();

	std::error_code ec;
	const std::filesystem::path currentProjectFolder =
		PathFromUtf8(selectedProjectFolder_);
	if (std::filesystem::exists(currentProjectFolder, ec)) {
		if (
			currentProjectFolder.has_parent_path() &&
			currentProjectFolder != projectResourceRoot
		) {
			if (ImGui::SmallButton("..")) {
				const std::filesystem::path parent = currentProjectFolder.parent_path();
				selectedProjectFolder_ = parent.empty()
					? projectResourceRootPath
					: PathToUtf8(parent);
				selectedProjectFile_.clear();
				projectDirectoryCacheDirty_ = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s", SelectEditorText(
				editorLanguage_,
				"親フォルダーへ戻る",
				"Back to parent folder"
			));
			ImGui::Separator();
		}

		const std::vector<ProjectDirectoryEntry>& entries =
			GetCachedProjectDirectoryEntries();

		const float cellWidth = projectThumbnailSize_ + 18.0f;
		const float panelWidth = ImGui::GetContentRegionAvail().x;
		const int columnCount = projectGridView_
			? (std::max)(1, static_cast<int>(panelWidth / cellWidth))
			: 1;
		bool tableOpen = false;
		if (projectGridView_) {
			tableOpen = ImGui::BeginTable(
				"ProjectAssetGrid",
				columnCount,
				ImGuiTableFlags_SizingFixedFit
			);
		}

		if (!projectGridView_ || tableOpen) {
		for (const auto& entry : entries) {
			const std::string& fileName = entry.fileName;
			const std::string& filePath = entry.filePath;
			const bool isDirectory = entry.isDirectory;
			const std::string& extension = entry.extension;
			const bool isTexture = entry.isTexture;
			const bool isModel = entry.isModel;
			const bool isAudio = !isDirectory && IsAudioAssetExtension(extension);
			const bool isScene = entry.isScene;
			const bool isPrefab =
				!isDirectory && IsPrefabAssetPath(PathFromUtf8(filePath));
			if (
				projectPrefabFilterEnabled_ &&
				!isDirectory &&
				!isPrefab
			) {
				continue;
			}
			const std::string resourcePath = isTexture
				? GetProjectResourcePath(filePath)
				: std::string{};
			const bool isSelected = selectedProjectFile_ == filePath;

			if (projectGridView_) {
				ImGui::TableNextColumn();
			}
			ImGui::PushID(filePath.c_str());
			if (isSelected || selectedProjectFolder_ == filePath) {
				ImGui::PushStyleColor(
					ImGuiCol_Button,
					ImGui::GetStyleColorVec4(ImGuiCol_Header)
				);
			}

			bool texturePreviewAvailable = false;
			float texturePreviewAspect = 1.0f;
			D3D12_GPU_DESCRIPTOR_HANDLE textureHandle{};
			if (isTexture && TextureManager::GetInstance()) {
				if (TextureManager::GetInstance()->HasTexture(resourcePath)) {
					const auto& metadata = TextureManager::GetInstance()->GetMetaData(
						resourcePath
					);
					texturePreviewAvailable = !metadata.IsCubemap();
					if (texturePreviewAvailable) {
						texturePreviewAspect = metadata.height > 0
							? static_cast<float>(metadata.width) /
								static_cast<float>(metadata.height)
							: 1.0f;
						textureHandle = TextureManager::GetInstance()->GetSrvHandleGPU(
							resourcePath
						);
					}
				}
			}

			bool clicked = false;
			bool openSceneRequested = false;
			bool openPrefabRequested = false;
			bool openPrefabContextRequested = false;
			auto loadHoveredTexturePreview = [&]() {
				if (
					isTexture &&
					TextureManager::GetInstance() &&
					!TextureManager::GetInstance()->HasTexture(resourcePath) &&
					projectPreviewLoadAttempted_.size() < 96 &&
					projectPreviewLoadAttempted_.insert(resourcePath).second
				) {
					TextureManager::GetInstance()->LoadTexture(resourcePath);
				}
			};
			auto drawDragSource = [&]() {
				if (
					!(isModel || isTexture || isAudio || isPrefab) ||
					!ImGui::BeginDragDropSource()
				) {
					return;
				}
				const std::string dragPath = isModel
					? GetModelPathRelativeToResources(filePath)
					: isTexture
						? resourcePath
						: GetProjectResourcePath(filePath);
				const char* payloadType = isModel
					? "PROJECT_MODEL_PATH"
					: isTexture
						? "PROJECT_TEXTURE_PATH"
						: isAudio
							? "PROJECT_AUDIO_PATH"
							: "PROJECT_PREFAB_PATH";
				ImGui::SetDragDropPayload(
					payloadType,
					dragPath.c_str(),
					dragPath.size() + 1
				);
				if (texturePreviewAvailable) {
					ImGui::Image(
						ImTextureRef(static_cast<ImTextureID>(textureHandle.ptr)),
						ImVec2(48.0f, 48.0f)
					);
				}
				ImGui::TextUnformatted(fileName.c_str());
				ImGui::EndDragDropSource();
			};
			auto drawPrefabContextMenu = [&]() {
				if (!isPrefab || !ImGui::BeginPopupContextItem(
					"PrefabAssetContext"
				)) {
					return;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"Prefabを開く###OpenPrefabAsset",
					"Open Prefab###OpenPrefabAsset"
				))) {
					openPrefabContextRequested = true;
				}
				if (ImGui::MenuItem(SelectEditorText(
					editorLanguage_,
					"Assetを選択###SelectPrefabAsset",
					"Select Asset###SelectPrefabAsset"
				))) {
					SelectPrefabAssetInProject(filePath);
				}
				const bool favorite = IsFavoritePrefab(filePath);
				if (ImGui::MenuItem(
					favorite
						? SelectEditorText(
							editorLanguage_,
							"お気に入りから削除###TogglePrefabAssetFavorite",
							"Remove from Favorites###TogglePrefabAssetFavorite"
						)
						: SelectEditorText(
							editorLanguage_,
							"お気に入りに追加###TogglePrefabAssetFavorite",
							"Add to Favorites###TogglePrefabAssetFavorite"
						)
				)) {
					ToggleFavoritePrefab(filePath);
				}
				ImGui::EndPopup();
			};
			if (projectGridView_) {
				if (texturePreviewAvailable) {
					const ImVec2 previewMin = ImGui::GetCursorScreenPos();
					clicked = ImGui::InvisibleButton(
						"##AssetPreview",
						ImVec2(projectThumbnailSize_, projectThumbnailSize_)
					);
					const ImVec2 previewMax = {
						previewMin.x + projectThumbnailSize_,
						previewMin.y + projectThumbnailSize_
					};
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					drawList->AddRectFilled(
						previewMin,
						previewMax,
						ImGui::GetColorU32(
							isSelected ? ImGuiCol_Header : ImGuiCol_FrameBg
						),
						2.0f
					);
					const float innerSize = projectThumbnailSize_ - 8.0f;
					const float imageWidth = texturePreviewAspect >= 1.0f
						? innerSize
						: innerSize * texturePreviewAspect;
					const float imageHeight = texturePreviewAspect >= 1.0f
						? innerSize / texturePreviewAspect
						: innerSize;
					const ImVec2 imageMin = {
						previewMin.x + (projectThumbnailSize_ - imageWidth) * 0.5f,
						previewMin.y + (projectThumbnailSize_ - imageHeight) * 0.5f
					};
					drawList->AddImage(
						ImTextureRef(static_cast<ImTextureID>(textureHandle.ptr)),
						imageMin,
						ImVec2(imageMin.x + imageWidth, imageMin.y + imageHeight)
					);
					if (ImGui::IsItemHovered()) {
						drawList->AddRect(
							previewMin,
							previewMax,
							ImGui::GetColorU32(ImGuiCol_HeaderHovered),
							2.0f,
							0,
							2.0f
						);
					}
					drawDragSource();
				} else {
					const char* typeLabel = isDirectory
						? SelectEditorText(editorLanguage_, "フォルダー###AssetType", "DIR###AssetType") :
						isScene ? SelectEditorText(editorLanguage_, "Scene###AssetType", "SCENE###AssetType") :
						isModel ? "3D" :
						isTexture ? "DDS" :
						IsAudioAssetExtension(extension) ? SelectEditorText(editorLanguage_, "音声###AssetType", "AUDIO###AssetType") :
						extension == ".json" ? "JSON" :
						(extension == ".hlsl" || extension == ".hlsli") ? "SHADER" :
						SelectEditorText(editorLanguage_, "ファイル###AssetType", "FILE###AssetType");
					clicked = ImGui::Button(
						typeLabel,
						ImVec2(projectThumbnailSize_, projectThumbnailSize_)
					);
					openSceneRequested = isScene && ImGui::IsItemHovered() &&
						ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
					openPrefabRequested = fileName.ends_with(".prefab.json") &&
						ImGui::IsItemHovered() &&
						ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
					if (ImGui::IsItemHovered()) {
						loadHoveredTexturePreview();
					}
					drawPrefabContextMenu();
					drawDragSource();
				}
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + projectThumbnailSize_);
				ImGui::TextUnformatted(fileName.c_str());
				ImGui::PopTextWrapPos();
			} else {
				const char* prefix = isDirectory ? SelectEditorText(editorLanguage_, "[フォルダー]", "[Folder]") :
					isScene ? SelectEditorText(editorLanguage_, "[Scene]", "[Scene]") :
					isTexture ? "[Tex]" :
					isModel ? SelectEditorText(editorLanguage_, "[モデル]", "[Model]") :
					IsAudioAssetExtension(extension) ? SelectEditorText(editorLanguage_, "[音声]", "[Audio]") :
					extension == ".json" ? "[JSON]" :
					(extension == ".hlsl" || extension == ".hlsli") ? "[Shader]" :
					SelectEditorText(editorLanguage_, "[ファイル]", "[File]");
				const std::string label = std::string(prefix) + "  " + fileName +
					"###ProjectAssetRow";
				clicked = ImGui::Selectable(
					label.c_str(),
					isDirectory ? selectedProjectFolder_ == filePath : isSelected
				);
				openSceneRequested = isScene && ImGui::IsItemHovered() &&
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				openPrefabRequested = fileName.ends_with(".prefab.json") &&
					ImGui::IsItemHovered() &&
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				if (ImGui::IsItemHovered()) {
					loadHoveredTexturePreview();
				}
				drawPrefabContextMenu();
				drawDragSource();
			}
			openPrefabRequested =
				openPrefabRequested || openPrefabContextRequested;

			if (isSelected || selectedProjectFolder_ == filePath) {
				ImGui::PopStyleColor();
			}
			if (clicked || openSceneRequested || openPrefabRequested) {
				if (isDirectory) {
					selectedProjectFolder_ = filePath;
					selectedProjectFile_.clear();
					selectedEntityId_ = 0;
					projectDirectoryCacheDirty_ = true;
					StopAudioPreview();
				} else {
					selectedProjectFile_ = filePath;
					selectedEntityId_ = 0;
					StopAudioPreview();
					if (openSceneRequested && sceneCatalog_) {
						const SceneDescriptor* scene =
							sceneCatalog_->FindByFilePath(filePath);
						if (scene) {
							RequestOpenScene(scene->id);
						}
					}
					if (openPrefabRequested) {
						RequestOpenPrefab(filePath);
					}
				}
			}
			ImGui::PopID();
		}
		}
		if (tableOpen) {
			ImGui::EndTable();
		}
	}
	ImGui::Columns(1);
	ImGui::End();
}

void ImGuiManager::DrawDirectoryTreeNode(const ProjectDirectoryNode& node) {
	ImGui::PushID(node.folderPath.c_str());
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (selectedProjectFolder_ == node.folderPath) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}
	if (node.children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf;
	}

	bool open = ImGui::TreeNodeEx(node.folderName.c_str(), flags);
	if (ImGui::IsItemClicked()) {
		selectedProjectFolder_ = node.folderPath;
		projectDirectoryCacheDirty_ = true;
	}

	if (open) {
		for (const ProjectDirectoryNode& child : node.children) {
			DrawDirectoryTreeNode(child);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void ImGuiManager::RequestOpenPrefab(
	const std::string& filePath,
	int historyIndex
) {
	if (!prefabEditorSession_ || filePath.empty()) {
		return;
	}

	const std::string resolvedPath = PathToUtf8(
		EditableResourcePath::ResolveResource(
			PathFromUtf8(filePath)
		).lexically_normal()
	);
	showPrefab_ = true;
	prefabFocusFramesRemaining_ = 2;

	if (prefabEditorSession_->IsOpen()) {
		const std::filesystem::path currentPath =
			EditableResourcePath::ResolveResource(
				PathFromUtf8(prefabEditorSession_->GetFilePath())
			).lexically_normal();
		const std::filesystem::path targetPath = PathFromUtf8(resolvedPath);
		std::error_code equivalentError;
		const bool samePrefab = std::filesystem::equivalent(
			currentPath,
			targetPath,
			equivalentError
		);
		if (
			samePrefab ||
			(equivalentError && currentPath == targetPath)
		) {
			if (
				historyIndex >= 0 &&
				historyIndex <
					static_cast<int>(prefabNavigationHistory_.size())
			) {
				prefabNavigationHistory_[historyIndex] =
					PrefabAssetRegistry::CreateReference(resolvedPath);
				prefabNavigationIndex_ = historyIndex;
			}
			return;
		}
		if (prefabEditorSession_->IsDirty()) {
			pendingPrefabOpenPath_ = resolvedPath;
			pendingPrefabHistoryIndex_ = historyIndex;
			prefabOpenPopupRequested_ = true;
			return;
		}
	}

	OpenPrefab(resolvedPath, historyIndex);
}

bool ImGuiManager::OpenPrefab(
	const std::string& filePath,
	int historyIndex
) {
	if (!prefabEditorSession_ || filePath.empty()) {
		return false;
	}

	const std::string resolvedPath = PathToUtf8(
		EditableResourcePath::ResolveResource(
			PathFromUtf8(filePath)
		).lexically_normal()
	);
	if (!prefabEditorSession_->Open(resolvedPath)) {
		prefabNavigationStatus_ = prefabEditorSession_->GetLastError();
		return false;
	}

	ResetComponentPicker(prefabComponentPicker_);
	showPrefab_ = true;
	prefabFocusFramesRemaining_ = 2;
	const SceneDocument& prefab = prefabEditorSession_->GetDocument();
	prefabSelectedEntityId_ = prefab.GetEntities().empty()
		? 0
		: prefab.GetEntities().front().id;
	++prefabPreviewFramingSerial_;
	prefabNavigationStatus_.clear();
	RecordRecentPrefab(resolvedPath);
	const PrefabAssetReference openedReference =
		PrefabAssetRegistry::CreateReference(resolvedPath);

	if (
		historyIndex >= 0 &&
		historyIndex < static_cast<int>(prefabNavigationHistory_.size())
	) {
		prefabNavigationHistory_[historyIndex] = openedReference;
		prefabNavigationIndex_ = historyIndex;
		return true;
	}

	if (
		prefabNavigationIndex_ + 1 <
			static_cast<int>(prefabNavigationHistory_.size())
	) {
		prefabNavigationHistory_.erase(
			prefabNavigationHistory_.begin() + prefabNavigationIndex_ + 1,
			prefabNavigationHistory_.end()
		);
	}
	if (
		prefabNavigationHistory_.empty() ||
		!PrefabAssetRegistry::IsSameAsset(
			prefabNavigationHistory_.back(),
			openedReference
		)
	) {
		prefabNavigationHistory_.push_back(openedReference);
	} else {
		prefabNavigationHistory_.back() = openedReference;
	}
	prefabNavigationIndex_ =
		static_cast<int>(prefabNavigationHistory_.size()) - 1;
	return true;
}

void ImGuiManager::DrawPrefabOpenConfirmation() {
	if (!prefabEditorSession_) {
		return;
	}
	const char* popupLabel = SelectEditorText(
		editorLanguage_,
		"別のPrefabを開きますか？###OpenAnotherPrefabPopup",
		"Open Another Prefab?###OpenAnotherPrefabPopup"
	);
	if (prefabOpenPopupRequested_) {
		ImGui::OpenPopup(popupLabel);
		prefabOpenPopupRequested_ = false;
	}
	if (!ImGui::BeginPopupModal(
		popupLabel,
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		return;
	}

	ImGui::TextUnformatted(SelectEditorText(
		editorLanguage_,
		"現在のPrefabには未保存の変更があります。",
		"The current Prefab has unsaved changes."
	));
	ImGui::TextWrapped(
		SelectEditorText(editorLanguage_, "開く: %s", "Open: %s"),
		pendingPrefabOpenPath_.c_str()
	);
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"保存して開く###SaveAndOpenPrefab",
		"Save and Open###SaveAndOpenPrefab"
	))) {
		if (prefabEditorSession_->Save()) {
			OpenPrefab(
				pendingPrefabOpenPath_,
				pendingPrefabHistoryIndex_
			);
			pendingPrefabOpenPath_.clear();
			pendingPrefabHistoryIndex_ = -1;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"変更を破棄して開く###DiscardAndOpenPrefab",
		"Discard and Open###DiscardAndOpenPrefab"
	))) {
		prefabEditorSession_->Close(true);
		OpenPrefab(
			pendingPrefabOpenPath_,
			pendingPrefabHistoryIndex_
		);
		pendingPrefabOpenPath_.clear();
		pendingPrefabHistoryIndex_ = -1;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(SelectEditorText(
		editorLanguage_,
		"キャンセル###CancelOpenPrefab",
		"Cancel###CancelOpenPrefab"
	))) {
		pendingPrefabOpenPath_.clear();
		pendingPrefabHistoryIndex_ = -1;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void ImGuiManager::SelectPrefabAssetInProject(const std::string& filePath) {
	if (filePath.empty()) {
		return;
	}
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(
			PathFromUtf8(filePath)
		).lexically_normal();
	selectedProjectFolder_ = PathToUtf8(resolvedPath.parent_path());
	selectedProjectFile_ = PathToUtf8(resolvedPath);
	selectedEntityId_ = 0;
	selectedEntityIds_.clear();
	projectDirectoryCacheDirty_ = true;
	showProject_ = true;
	projectFocusRequested_ = true;
}

uint64_t ImGuiManager::InstantiatePrefabInEditScene(
	const std::string& filePath,
	uint64_t parentId,
	const Vector3* rootTranslate
) {
	if (
		!editorSession_ ||
		!editorSession_->IsEditing() ||
		filePath.empty()
	) {
		return 0;
	}
	SceneDocument& document = editorSession_->GetEditDocument();
	const uint64_t instanceId = document.InstantiatePrefab(
		filePath,
		parentId
	);
	if (instanceId == 0) {
		return 0;
	}
	if (rootTranslate) {
		if (SceneEntity* root = document.FindEntity(instanceId)) {
			root->transform.translate = *rootTranslate;
			document.MarkDirty();
		}
	}
	selectedEntityId_ = instanceId;
	selectedEntityIds_ = { instanceId };
	hierarchySelectionAnchorId_ = instanceId;
	hierarchyRevealRequested_ = true;
	selectedProjectFile_.clear();
	editorSession_->RequestSceneReload();
	return instanceId;
}

void ImGuiManager::ValidateAllPrefabAssets() {
	prefabAssetValidationCompleted_ = false;
	prefabAssetValidationScannedCount_ = 0;
	prefabAssetValidationResults_.clear();
	prefabAssetPathCacheDirty_ = true;
	RefreshPrefabAssetPathCache();
	PrefabAssetRegistry::Invalidate();

	std::unordered_map<std::string, std::string> firstPathByAssetId;
	std::unordered_set<std::string> reportedDuplicatePaths;
	std::unordered_map<std::string, std::unordered_set<std::string>>
		dependenciesByPath;
	std::unordered_map<std::string, std::string> resultPathByResolvedPath;
	auto resolveAbsolutePath = [](const std::string& path) {
		return PathToUtf8(
			EditableResourcePath::ResolveResource(
				PathFromUtf8(path)
			).lexically_normal()
		);
	};
	auto addResult = [this](
		const std::string& filePath,
		bool error,
		std::string message
	) {
		prefabAssetValidationResults_.push_back({
			filePath,
			std::move(message),
			error
		});
	};

	for (const std::string& prefabPath : cachedPrefabAssetPaths_) {
		++prefabAssetValidationScannedCount_;
		const std::string resolvedPrefabPath = resolveAbsolutePath(prefabPath);
		dependenciesByPath.try_emplace(resolvedPrefabPath);
		resultPathByResolvedPath[resolvedPrefabPath] = prefabPath;
		const std::string rawAssetId =
			PrefabAssetRegistry::ReadAssetId(prefabPath);
		if (!rawAssetId.empty()) {
			const auto [found, inserted] = firstPathByAssetId.emplace(
				rawAssetId,
				prefabPath
			);
			if (!inserted && found->second != prefabPath) {
				const std::string message =
					"Duplicate Prefab Asset ID: " + rawAssetId;
				if (reportedDuplicatePaths.insert(found->second).second) {
					addResult(found->second, true, message);
				}
				if (reportedDuplicatePaths.insert(prefabPath).second) {
					addResult(prefabPath, true, message);
				}
			}
		}
		SceneDocument document;
		if (!document.Load(prefabPath)) {
			addResult(prefabPath, true, document.GetLastLoadError());
			continue;
		}
		if (!document.GetLastLoadError().empty()) {
			addResult(prefabPath, false, document.GetLastLoadError());
		}
		if (document.IsDirty()) {
			addResult(
				prefabPath,
				false,
				"Migrated or recovered data must be saved to persist the current format."
			);
		}

		const std::string& assetId = document.GetAssetId();
		if (assetId.empty()) {
			addResult(prefabPath, true, "Prefab Asset ID is missing.");
		}

		const PrefabAssetReference currentAsset{ assetId, prefabPath };
		if (document.IsPrefabVariant()) {
			const std::string basePath = PrefabAssetRegistry::ResolvePath(
				document.GetVariantBaseAssetId(),
				document.GetVariantBasePath()
			);
			if (!basePath.empty()) {
				dependenciesByPath[resolvedPrefabPath].insert(
					resolveAbsolutePath(basePath)
				);
			}
		}
		std::unordered_set<std::string> dependencyKeys;
		for (const SceneEntity& entity : document.GetEntities()) {
			for (size_t linkIndex = 0;
				linkIndex < entity.prefabLinks.size();
				++linkIndex) {
				const ScenePrefabLink& link = entity.prefabLinks[linkIndex];
				const PrefabAssetReference source{
					link.assetId,
					link.sourcePath
				};
				const std::string key = !source.assetId.empty()
					? "id:" + source.assetId
					: !source.fallbackPath.empty()
						? "path:" + source.fallbackPath
						: "invalid:" + std::to_string(entity.id) + ":" +
							std::to_string(linkIndex);
				if (!dependencyKeys.insert(key).second) {
					continue;
				}
				if (PrefabAssetRegistry::IsSameAsset(currentAsset, source)) {
					dependenciesByPath[resolvedPrefabPath].insert(
						resolvedPrefabPath
					);
					continue;
				}
				const std::string resolvedPath =
					PrefabAssetRegistry::ResolvePath(source);
				if (resolvedPath.empty()) {
					std::string message =
						"Nested Prefab reference cannot be resolved";
					if (!entity.name.empty()) {
						message += " (Entity: " + entity.name + ")";
					}
					if (!source.fallbackPath.empty()) {
						message += ": " + source.fallbackPath;
					}
					addResult(prefabPath, true, std::move(message));
				} else {
					dependenciesByPath[resolvedPrefabPath].insert(
						resolveAbsolutePath(resolvedPath)
					);
					if (source.assetId.empty()) {
						addResult(
							prefabPath,
							false,
							"Nested Prefab uses a legacy Path-only reference: " +
								resolvedPath
						);
					}
				}
			}
		}
	}

	std::unordered_map<std::string, uint8_t> visitStates;
	std::vector<std::string> dependencyStack;
	std::unordered_set<std::string> reportedCyclePaths;
	std::function<void(const std::string&)> visitDependency;
	visitDependency = [&](const std::string& path) {
		visitStates[path] = 1;
		dependencyStack.push_back(path);
		const auto dependencies = dependenciesByPath.find(path);
		if (dependencies != dependenciesByPath.end()) {
			for (const std::string& dependency : dependencies->second) {
				const uint8_t dependencyState = visitStates[dependency];
				if (dependencyState == 0) {
					visitDependency(dependency);
					continue;
				}
				if (dependencyState != 1) {
					continue;
				}
				const auto cycleStart = std::find(
					dependencyStack.begin(),
					dependencyStack.end(),
					dependency
				);
				std::string cycleLabel;
				for (auto cyclePath = cycleStart;
					cyclePath != dependencyStack.end();
					++cyclePath) {
					if (!cycleLabel.empty()) {
						cycleLabel += " -> ";
					}
					cycleLabel += PathToUtf8(
						PathFromUtf8(*cyclePath).filename()
					);
				}
				cycleLabel += " -> " + PathToUtf8(
					PathFromUtf8(dependency).filename()
				);
				for (auto cyclePath = cycleStart;
					cyclePath != dependencyStack.end();
					++cyclePath) {
					if (!reportedCyclePaths.insert(*cyclePath).second) {
						continue;
					}
					const auto resultPath =
						resultPathByResolvedPath.find(*cyclePath);
					addResult(
						resultPath == resultPathByResolvedPath.end()
							? *cyclePath
							: resultPath->second,
						true,
						"Prefab dependency cycle detected: " + cycleLabel
					);
				}
			}
		}
		dependencyStack.pop_back();
		visitStates[path] = 2;
	};
	for (const auto& [path, dependencies] : dependenciesByPath) {
		(void)dependencies;
		if (visitStates[path] == 0) {
			visitDependency(path);
		}
	}
	prefabAssetValidationCompleted_ = true;
}

void ImGuiManager::DrawPrefabDiagnostics() {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}

	const SceneDocument& document = prefabEditorSession_->GetDocument();
	const std::string& filePath = prefabEditorSession_->GetFilePath();
	std::vector<SceneValidationIssue> issues;
	SceneValidator::ValidateDocument(document, nullptr, {}, filePath, issues);

	struct DependencyStatus {
		PrefabAssetReference reference;
		std::string resolvedPath;
		uint64_t entityId = 0;
	};
	std::vector<DependencyStatus> dependencies;
	std::unordered_set<std::string> dependencyKeys;
	const PrefabAssetReference currentAsset{
		document.GetAssetId(),
		filePath
	};
	for (const SceneEntity& entity : document.GetEntities()) {
		for (size_t linkIndex = 0;
			linkIndex < entity.prefabLinks.size();
			++linkIndex) {
			const ScenePrefabLink& link = entity.prefabLinks[linkIndex];
			const PrefabAssetReference source{ link.assetId, link.sourcePath };
			if (PrefabAssetRegistry::IsSameAsset(currentAsset, source)) {
				continue;
			}
			const std::string key = !source.assetId.empty()
				? "id:" + source.assetId
				: !source.fallbackPath.empty()
					? "path:" + source.fallbackPath
					: "invalid:" + std::to_string(entity.id) + ":" +
						std::to_string(linkIndex);
			if (!dependencyKeys.insert(key).second) {
				continue;
			}
			dependencies.push_back({
				source,
				PrefabAssetRegistry::ResolvePath(source),
				entity.id
			});
		}
	}

	const bool assetIdMissing = document.GetAssetId().empty();
	const bool isVariant = document.IsPrefabVariant();
	const std::string variantBasePath = isVariant
		? PrefabAssetRegistry::ResolvePath(
			document.GetVariantBaseAssetId(),
			document.GetVariantBasePath()
		)
		: std::string{};
	const bool variantBaseMissing = isVariant && variantBasePath.empty();
	size_t errorCount = 0;
	if (assetIdMissing) {
		++errorCount;
	}
	if (variantBaseMissing) {
		++errorCount;
	}
	size_t warningCount = 0;
	for (const DependencyStatus& dependency : dependencies) {
		if (dependency.resolvedPath.empty()) {
			++errorCount;
		} else if (dependency.reference.assetId.empty()) {
			++warningCount;
		}
	}
	for (const SceneValidationIssue& issue : issues) {
		if (issue.severity == SceneValidationSeverity::Error) {
			++errorCount;
		} else {
			++warningCount;
		}
	}
	if (prefabAssetValidationCompleted_) {
		for (const PrefabAssetValidationResult& result :
			prefabAssetValidationResults_) {
			if (result.error) {
				++errorCount;
			} else {
				++warningCount;
			}
		}
	}

	const std::string headerLabel = errorCount == 0 && warningCount == 0
		? "Diagnostics: OK###PrefabDiagnostics"
		: "Diagnostics: " + std::to_string(errorCount) + " error(s), " +
			std::to_string(warningCount) + " warning(s)###PrefabDiagnostics";
	const bool diagnosticsDefaultOpen =
		errorCount > 0 || !prefabAssetValidationCompleted_;
	if (!DrawPersistentInspectorHeader(
		"prefab/" + filePath + "/Diagnostics",
		headerLabel.c_str(),
		diagnosticsDefaultOpen
	)) {
		return;
	}

	size_t componentCount = 0;
	for (const SceneEntity& entity : document.GetEntities()) {
		componentCount += entity.components.size();
	}
	ImGui::Text("Type: %s", isVariant ? "Prefab Variant" : "Prefab");
	ImGui::SameLine();
	ImGui::TextDisabled(
		"Entities: %zu  Components: %zu",
		document.GetEntities().size(),
		componentCount
	);
	if (assetIdMissing) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
			"Asset ID: Missing"
		);
	} else {
		ImGui::TextWrapped("Asset ID: %s", document.GetAssetId().c_str());
	}
	if (isVariant) {
		if (variantBaseMissing) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"Variant Base: Missing or ambiguous"
			);
		} else {
			ImGui::TextWrapped("Variant Base: %s", variantBasePath.c_str());
		}
	}

	ImGui::SeparatorText("Project Prefab Validation");
	if (ImGui::Button("Validate All Prefabs")) {
		ValidateAllPrefabAssets();
	}
	ImGui::SameLine();
	ImGui::TextDisabled(
		"Read-only validation of saved resources/**/*.prefab.json"
	);
	if (!prefabAssetValidationCompleted_) {
		ImGui::TextDisabled("Not validated in this Editor session.");
	} else {
		size_t projectErrorCount = 0;
		size_t projectWarningCount = 0;
		for (const PrefabAssetValidationResult& result :
			prefabAssetValidationResults_) {
			if (result.error) {
				++projectErrorCount;
			} else {
				++projectWarningCount;
			}
		}
		ImGui::Text(
			"Scanned: %zu  Errors: %zu  Warnings: %zu",
			prefabAssetValidationScannedCount_,
			projectErrorCount,
			projectWarningCount
		);
		if (prefabAssetValidationResults_.empty()) {
			ImGui::TextColored(
				ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
				"All Prefab assets passed load and reference validation."
			);
		} else {
			if (ImGui::BeginChild(
				"PrefabAssetValidationResults",
				ImVec2(0.0f, 200.0f),
				ImGuiChildFlags_Borders
			)) {
				for (size_t resultIndex = 0;
					resultIndex < prefabAssetValidationResults_.size();
					++resultIndex) {
					const PrefabAssetValidationResult& result =
						prefabAssetValidationResults_[resultIndex];
					const std::string fileName = PathToUtf8(
						PathFromUtf8(result.filePath).filename()
					);
					ImGui::PushID(static_cast<int>(resultIndex));
					if (ImGui::SmallButton("Select Asset")) {
						SelectPrefabAssetInProject(result.filePath);
					}
					ImGui::SameLine();
					ImGui::TextColored(
						result.error
							? ImVec4(0.95f, 0.35f, 0.3f, 1.0f)
							: ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
						"[%s] %s",
						result.error ? "Error" : "Warning",
						fileName.c_str()
					);
					ImGui::TextWrapped("%s", result.message.c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		}
	}

	ImGui::SeparatorText("Nested Sources");
	if (dependencies.empty()) {
		ImGui::TextDisabled("No Nested Prefab dependencies.");
	}
	ImGui::PushID("PrefabDiagnosticDependencies");
	for (size_t dependencyIndex = 0;
		dependencyIndex < dependencies.size();
		++dependencyIndex) {
		const DependencyStatus& dependency = dependencies[dependencyIndex];
		const std::string displayPath = dependency.resolvedPath.empty()
			? dependency.reference.fallbackPath
			: dependency.resolvedPath;
		const std::string displayName = displayPath.empty()
			? "Missing Prefab"
			: PathToUtf8(PathFromUtf8(displayPath).filename());
		ImGui::PushID(static_cast<int>(dependencyIndex));
		if (document.FindEntity(dependency.entityId)) {
			if (ImGui::SmallButton("Select")) {
				prefabSelectedEntityId_ = dependency.entityId;
			}
			ImGui::SameLine();
		}
		if (dependency.resolvedPath.empty()) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"[Error] %s: missing or ambiguous",
				displayName.c_str()
			);
		} else {
			ImGui::TextUnformatted(displayName.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", dependency.resolvedPath.c_str());
			}
			if (dependency.reference.assetId.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(
					ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
					"[Warning] Path-only reference"
				);
			}
		}
		ImGui::PopID();
	}
	ImGui::PopID();

	ImGui::SeparatorText("Document Validation");
	if (issues.empty() && !assetIdMissing && !variantBaseMissing) {
		ImGui::TextColored(
			ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
			"No document validation issues."
		);
		return;
	}
	if (assetIdMissing) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
			"[Error] Prefab Asset ID is missing."
		);
	}
	if (variantBaseMissing) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
			"[Error] Variant Base cannot be resolved."
		);
	}
	ImGui::PushID("PrefabDiagnosticIssues");
	for (size_t issueIndex = 0; issueIndex < issues.size(); ++issueIndex) {
		const SceneValidationIssue& issue = issues[issueIndex];
		ImGui::PushID(static_cast<int>(issueIndex));
		if (issue.entityId != 0 && document.FindEntity(issue.entityId)) {
			if (ImGui::SmallButton("Select")) {
				prefabSelectedEntityId_ = issue.entityId;
			}
			ImGui::SameLine();
		}
		const bool isError = issue.severity == SceneValidationSeverity::Error;
		ImGui::TextColored(
			isError
				? ImVec4(0.95f, 0.35f, 0.3f, 1.0f)
				: ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
			"[%s] %s",
			isError ? "Error" : "Warning",
			issue.message.c_str()
		);
		ImGui::PopID();
	}
	ImGui::PopID();
}

void ImGuiManager::DrawPrefabWindow() {
	if (!prefabEditorSession_) {
		showPrefab_ = false;
		return;
	}

	const bool requestPrefabFocus = prefabFocusFramesRemaining_ > 0;
	if (requestPrefabFocus) {
		ImGui::SetNextWindowFocus();
	}
	bool windowOpen = true;
	const bool prefabWindowContentsVisible = ImGui::Begin(
		"Prefab",
		&windowOpen
	);
	if (requestPrefabFocus) {
		ImGui::SetWindowFocus("Prefab");
		--prefabFocusFramesRemaining_;
	}
	prefabKeyboardFocusThisFrame_ |= ImGui::IsWindowFocused(
		ImGuiFocusedFlags_RootAndChildWindows
	);
	if (!prefabWindowContentsVisible) {
		ImGui::End();
		if (windowOpen && prefabEditorSession_->IsOpen()) {
			prefabEditorSession_->BeginEditFrame();
			DrawPrefabInspectorWindow();
			const bool editingInteractionActive =
				ImGui::IsAnyItemActive() ||
				ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
				ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
				ImGui::IsMouseDown(ImGuiMouseButton_Middle);
			prefabEditorSession_->EndEditFrame(!editingInteractionActive);
		}
		if (!windowOpen) {
			if (prefabEditorSession_->IsDirty()) {
				prefabClosePopupRequested_ = true;
			} else {
				prefabEditorSession_->Close(true);
				showPrefab_ = false;
				prefabSelectedEntityId_ = 0;
			}
		}
		if (!prefabEditorSession_->IsOpen()) {
			DrawPrefabInspectorWindow();
		}
		DrawPrefabOpenConfirmation();
		return;
	}

	const bool canNavigateBack = prefabNavigationIndex_ > 0;
	const bool canNavigateForward =
		prefabNavigationIndex_ >= 0 &&
		prefabNavigationIndex_ + 1 <
			static_cast<int>(prefabNavigationHistory_.size());
	ImGui::BeginDisabled(!canNavigateBack);
	if (ImGui::Button("<##PrefabBack")) {
		const int targetIndex = prefabNavigationIndex_ - 1;
		const std::string targetPath = PrefabAssetRegistry::ResolvePath(
			prefabNavigationHistory_[targetIndex]
		);
		if (targetPath.empty()) {
			prefabNavigationStatus_ =
				"Unable to resolve the previous Prefab asset.";
		} else {
			RequestOpenPrefab(targetPath, targetIndex);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!canNavigateForward);
	if (ImGui::Button(">##PrefabForward")) {
		const int targetIndex = prefabNavigationIndex_ + 1;
		const std::string targetPath = PrefabAssetRegistry::ResolvePath(
			prefabNavigationHistory_[targetIndex]
		);
		if (targetPath.empty()) {
			prefabNavigationStatus_ =
				"Unable to resolve the next Prefab asset.";
		} else {
			RequestOpenPrefab(targetPath, targetIndex);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Scene")) {
		ImGui::SetWindowFocus("Scene");
	}
	std::string nestedBreadcrumbOpenPath;
	if (prefabEditorSession_->IsOpen()) {
		ImGui::SameLine();
		ImGui::TextDisabled(">");
		ImGui::SameLine();
		const std::string prefabFileName = PathToUtf8(
			PathFromUtf8(
				prefabEditorSession_->GetFilePath()
			).filename()
		);
		if (ImGui::Button(prefabFileName.c_str())) {
			SelectPrefabAssetInProject(
				prefabEditorSession_->GetFilePath()
			);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Select this Prefab in Project");
		}

		const SceneDocument& prefabDocument =
			prefabEditorSession_->GetDocument();
		const SceneEntity* selectedPrefabEntity =
			prefabDocument.FindEntity(prefabSelectedEntityId_);
		if (selectedPrefabEntity) {
			const PrefabAssetReference currentPrefab{
				prefabDocument.GetAssetId(),
				prefabEditorSession_->GetFilePath()
			};
			std::unordered_set<std::string> displayedSources;
			for (size_t linkIndex = 0;
				linkIndex < selectedPrefabEntity->prefabLinks.size();
				++linkIndex) {
				const ScenePrefabLink& link =
					selectedPrefabEntity->prefabLinks[linkIndex];
				const PrefabAssetReference source{
					link.assetId,
					link.sourcePath
				};
				if (PrefabAssetRegistry::IsSameAsset(currentPrefab, source)) {
					continue;
				}
				const std::string sourceKey = !link.assetId.empty()
					? "id:" + link.assetId
					: "path:" + link.sourcePath;
				if (!displayedSources.insert(sourceKey).second) {
					continue;
				}
				const std::string sourcePath =
					PrefabAssetRegistry::ResolvePath(source);
				const std::string displayPath = sourcePath.empty()
					? link.sourcePath
					: sourcePath;
				const std::string displayName = displayPath.empty()
					? "Missing Prefab"
					: PathToUtf8(PathFromUtf8(displayPath).filename());
				ImGui::SameLine();
				ImGui::TextDisabled(">");
				ImGui::SameLine();
				ImGui::PushID(static_cast<int>(linkIndex));
				ImGui::BeginDisabled(sourcePath.empty());
				if (ImGui::Button(displayName.c_str())) {
					nestedBreadcrumbOpenPath = sourcePath;
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip(
						"%s",
						sourcePath.empty()
							? "Prefab asset is missing or ambiguous."
							: sourcePath.c_str()
					);
				}
				ImGui::PopID();
			}
		}
	}
	if (!nestedBreadcrumbOpenPath.empty()) {
		RequestOpenPrefab(nestedBreadcrumbOpenPath);
		ImGui::End();
		DrawPrefabOpenConfirmation();
		return;
	}
	ImGui::Separator();

	if (!prefabEditorSession_->IsOpen()) {
		ImGui::TextDisabled(
			"Select a .prefab.json asset in Project and choose Open Prefab Editor."
		);
		if (!prefabNavigationStatus_.empty()) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"%s",
				prefabNavigationStatus_.c_str()
			);
		}
		ImGui::End();
		DrawPrefabInspectorWindow();
		DrawPrefabOpenConfirmation();
		return;
	}

	std::string variantOpenRequest;
	static char variantFileName[192]{};
	static std::string variantOperationStatus;
	prefabEditorSession_->BeginEditFrame();
	ImGui::TextUnformatted(prefabEditorSession_->GetFilePath().c_str());
	if (prefabEditorSession_->IsDirty()) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "Unsaved");
	}
	if (ImGui::Button("Save")) {
		if (prefabEditorSession_->Save()) {
			InvalidateProjectCache();
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!prefabEditorSession_->CanUndo());
	if (ImGui::Button("Undo")) {
		prefabEditorSession_->Undo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!prefabEditorSession_->CanRedo());
	if (ImGui::Button("Redo")) {
		prefabEditorSession_->Redo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(prefabEditorSession_->IsDirty());
	if (ImGui::Button("Reload")) {
		prefabEditorSession_->Reload();
		const SceneDocument& document = prefabEditorSession_->GetDocument();
		prefabSelectedEntityId_ = document.GetEntities().empty()
			? 0
			: document.GetEntities().front().id;
		++prefabPreviewFramingSerial_;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Inspector")) {
		showPrefabInspector_ = true;
		prefabInspectorFocusRequested_ = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Close")) {
		windowOpen = false;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(prefabEditorSession_->IsDirty());
	if (ImGui::Button("Create Variant")) {
		std::string sourceName = PathToUtf8(
			PathFromUtf8(prefabEditorSession_->GetFilePath()).filename()
		);
		if (sourceName.ends_with(".prefab.json")) {
			sourceName.resize(
				sourceName.size() - std::string(".prefab.json").size()
			);
		}
		const std::string defaultName = sourceName + " Variant.prefab.json";
		strncpy_s(
			variantFileName,
			sizeof(variantFileName),
			defaultName.c_str(),
			_TRUNCATE
		);
		variantOperationStatus.clear();
		ImGui::OpenPopup("Create Prefab Variant");
	}
	ImGui::EndDisabled();
	if (ImGui::BeginPopupModal(
		"Create Prefab Variant",
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::TextUnformatted(
			"Create a Variant that inherits from the open Prefab."
		);
		ImGui::SetNextItemWidth(360.0f);
		ImGui::InputText(
			"File Name",
			variantFileName,
			sizeof(variantFileName)
		);
		if (ImGui::Button("Create")) {
			std::string fileName = variantFileName;
			for (char& character : fileName) {
				if (
					static_cast<unsigned char>(character) < 32 ||
					std::strchr("<>:\"/\\|?*", character)
				) {
					character = '_';
				}
			}
			if (!fileName.ends_with(".prefab.json")) {
				fileName += ".prefab.json";
			}
			const std::filesystem::path sourcePath =
				EditableResourcePath::ResolveResource(
					PathFromUtf8(prefabEditorSession_->GetFilePath())
				).lexically_normal();
			const std::filesystem::path targetPath =
				sourcePath.parent_path() / PathFromUtf8(fileName);
			std::error_code existsError;
			if (std::filesystem::exists(targetPath, existsError)) {
				variantOperationStatus =
					"A Prefab with that file name already exists.";
			} else if (prefabEditorSession_->GetDocument().SaveAsPrefabVariant(
				PathToUtf8(targetPath),
				prefabEditorSession_->GetFilePath()
			)) {
				InvalidateProjectCache();
				variantOpenRequest = PathToUtf8(targetPath);
				variantOperationStatus.clear();
				ImGui::CloseCurrentPopup();
			} else {
				variantOperationStatus = "Failed to create Prefab Variant.";
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			variantOperationStatus.clear();
			ImGui::CloseCurrentPopup();
		}
		if (!variantOperationStatus.empty()) {
			ImGui::TextColored(
				ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
				"%s",
				variantOperationStatus.c_str()
			);
		}
		ImGui::EndPopup();
	}

	SceneDocument& openPrefabDocument =
		prefabEditorSession_->GetDocument();
	if (openPrefabDocument.IsPrefabVariant()) {
		const std::string basePath = PrefabAssetRegistry::ResolvePath(
			openPrefabDocument.GetVariantBaseAssetId(),
			openPrefabDocument.GetVariantBasePath()
		);
		ImGui::TextDisabled(
			"Variant Base: %s",
			basePath.empty() ? "Missing or ambiguous" : basePath.c_str()
		);
		ImGui::SameLine();
		ImGui::BeginDisabled(basePath.empty());
		if (ImGui::SmallButton("Open Base")) {
			variantOpenRequest = basePath;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Revert to Base")) {
			if (openPrefabDocument.RevertPrefabVariantToBase()) {
				prefabSelectedEntityId_ =
					openPrefabDocument.GetEntities().empty()
						? 0
						: openPrefabDocument.GetEntities().front().id;
				++prefabPreviewFramingSerial_;
				variantOperationStatus = "Reverted all Variant overrides.";
			} else {
				variantOperationStatus = "Failed to reload the Variant Base.";
			}
		}
		ImGui::EndDisabled();
	}
	if (!variantOperationStatus.empty()) {
		ImGui::TextWrapped("%s", variantOperationStatus.c_str());
	}
	if (!prefabEditorSession_->GetLastError().empty()) {
		ImGui::TextColored(
			ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
			"%s",
			prefabEditorSession_->GetLastError().c_str()
		);
	}

	DrawPrefabDiagnostics();
	ImGui::Separator();
	DrawPrefabPreview();
	ImGui::Separator();
	const float prefabHierarchyHeight = std::clamp(
		ImGui::GetWindowHeight() * 0.5f,
		260.0f,
		640.0f
	);
	if (ImGui::BeginChild(
		"PrefabHierarchyPane",
		ImVec2(0.0f, prefabHierarchyHeight),
		ImGuiChildFlags_AlwaysUseWindowPadding |
			ImGuiChildFlags_Borders
	)) {
		ImGui::SeparatorText("Hierarchy");
		DrawPrefabHierarchy();
	}
	ImGui::EndChild();

	ImGui::End();
	DrawPrefabInspectorWindow();
	const bool editingInteractionActive =
		ImGui::IsAnyItemActive() ||
		ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
		ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
		ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
		ImGuizmo::IsUsing();
	prefabEditorSession_->EndEditFrame(!editingInteractionActive);
	if (!variantOpenRequest.empty()) {
		RequestOpenPrefab(variantOpenRequest);
	}

	if (!windowOpen) {
		if (prefabEditorSession_->IsDirty()) {
			prefabClosePopupRequested_ = true;
		} else {
			prefabEditorSession_->Close(true);
			showPrefab_ = false;
			prefabSelectedEntityId_ = 0;
		}
	}
	if (prefabClosePopupRequested_) {
		ImGui::OpenPopup("Close Prefab?");
		prefabClosePopupRequested_ = false;
	}
	if (ImGui::BeginPopupModal(
		"Close Prefab?",
		nullptr,
		ImGuiWindowFlags_AlwaysAutoResize
	)) {
		ImGui::TextUnformatted("The Prefab has unsaved changes.");
		if (ImGui::Button("Save and Close")) {
			if (prefabEditorSession_->Save()) {
				prefabEditorSession_->Close(true);
				showPrefab_ = false;
				prefabSelectedEntityId_ = 0;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard")) {
			prefabEditorSession_->Close(true);
			showPrefab_ = false;
			prefabSelectedEntityId_ = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	DrawPrefabOpenConfirmation();
}

void ImGuiManager::DrawPrefabInspectorWindow() {
	if (!showPrefabInspector_) {
		return;
	}

	if (prefabInspectorFocusRequested_) {
		ImGui::SetNextWindowFocus();
		prefabInspectorFocusRequested_ = false;
	}
	bool windowOpen = true;
	const bool inspectorContentsVisible = ImGui::Begin(
		SelectEditorText(
			editorLanguage_,
			"Prefab Inspector###PrefabInspector",
			"Prefab Inspector###PrefabInspector"
		),
		&windowOpen
	);
	DrawPrefabComponentPicker();
	prefabKeyboardFocusThisFrame_ |= ImGui::IsWindowFocused(
		ImGuiFocusedFlags_RootAndChildWindows
	);
	if (!inspectorContentsVisible) {
		ImGui::End();
		showPrefabInspector_ = windowOpen;
		return;
	}

	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"Entityを確認するPrefabを開いてください。",
			"Open a Prefab to inspect its Entities."
		));
	} else {
		const std::string prefabFileName = PathToUtf8(
			PathFromUtf8(prefabEditorSession_->GetFilePath()).filename()
		);
		ImGui::TextDisabled("%s", prefabFileName.c_str());
		ImGui::Separator();
		DrawPrefabInspector();
	}

	ImGui::End();
	showPrefabInspector_ = windowOpen;
}

void ImGuiManager::DrawPrefabPreview() {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}

	const SceneDocument& document = prefabEditorSession_->GetDocument();
	ImGui::SeparatorText("Stage");
	if (
		ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		!ImGui::GetIO().WantTextInput &&
		!ImGuizmo::IsUsing()
	) {
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
			gizmoOperation_ = 0;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
			gizmoOperation_ = 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
			gizmoOperation_ = 2;
		}
	}

	if (ImGui::Button("Frame All")) {
		prefabPreviewYaw_ = 0.65f;
		prefabPreviewPitch_ = 0.25f;
		prefabPreviewZoom_ = 1.0f;
		++prefabPreviewFramingSerial_;
	}
	ImGui::SameLine();
	ImGui::PushID("PrefabStageTools");
	const char* operationLabels[] = { "W", "E", "R" };
	for (int operation = 0; operation < 3; ++operation) {
		if (operation > 0) {
			ImGui::SameLine();
		}
		if (gizmoOperation_ == operation) {
			ImGui::PushStyleColor(
				ImGuiCol_Button,
				ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
			);
		}
		if (ImGui::Button(operationLabels[operation], ImVec2(28.0f, 0.0f))) {
			gizmoOperation_ = operation;
		}
		if (gizmoOperation_ == operation) {
			ImGui::PopStyleColor();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(gizmoLocalMode_ ? "Local" : "World")) {
		gizmoLocalMode_ = !gizmoLocalMode_;
	}
	ImGui::SameLine();
	ImGui::Checkbox("Snap", &gizmoSnapEnabled_);
	if (gizmoSnapEnabled_) {
		float* snapValue = gizmoOperation_ == 0
			? &gizmoTranslationSnap_
			: gizmoOperation_ == 1
				? &gizmoRotationSnapDegrees_
				: &gizmoScaleSnap_;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(64.0f);
		ImGui::DragFloat(
			"##SnapValue",
			snapValue,
			gizmoOperation_ == 1 ? 1.0f : 0.05f,
			0.01f,
			0.0f,
			"%.2f"
		);
	}
	ImGui::PopID();
	ImGui::SameLine();
	ImGui::TextDisabled("LMB: Select | RMB: Orbit | Wheel: Zoom");
	ImGui::PushID("PrefabStageOverlays");
	ImGui::Checkbox("Skeleton", &prefabPreviewShowSkeleton_);
	ImGui::SameLine();
	ImGui::BeginDisabled(!prefabPreviewShowSkeleton_);
	ImGui::Checkbox("Joint Axes", &prefabPreviewShowJointAxes_);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Checkbox("Colliders", &prefabPreviewShowColliders_);
	ImGui::SameLine();
	ImGui::Checkbox("Hit/Hurt", &prefabPreviewShowCombatVolumes_);
	ImGui::SameLine();
	if (ImGui::Checkbox("HitBox Setup", &prefabHitBoxSetupMode_)) {
		if (prefabHitBoxSetupMode_) {
			// Colliderの基準位置はAuthoring Poseで編集する。Preview Poseを
			// 維持したままでは親の回転によりLocal Offsetを判断しづらい。
			prefabAnimationPreviewPlaying_ = false;
			prefabAnimationPreviewActive_ = false;
			prefabAttackPreviewMode_ = false;
			playerCombatPreviewEnabled_ = false;
			playerCombatPreviewStatus_.clear();
		}
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Attack Preview", &prefabAttackPreviewMode_)) {
		if (prefabAttackPreviewMode_) {
			// Attack PreviewはAnimator Poseを正とするため、Base Pose編集とは排他にする。
			prefabHitBoxSetupMode_ = false;
			prefabAnimationPreviewPlaying_ = false;
			prefabAnimationPreviewActive_ = true;
		}
	}
	if (prefabHitBoxSetupMode_) {
		ImGui::SameLine();
		ImGui::TextDisabled("Base Pose");
		const SceneDocument& document = prefabEditorSession_->GetDocument();
		const SceneEntity* owner = document.FindEntity(
			prefabAnimationPreviewOwnerEntityId_
		);
		const SceneComponent* animator = owner
			? FindEnabledComponent(*owner, "PrefabAnimator")
			: nullptr;
		if (!animator) {
			for (const SceneEntity& candidate : document.GetEntities()) {
				if (const SceneComponent* candidateAnimator =
					FindEnabledComponent(candidate, "PrefabAnimator")) {
					owner = &candidate;
					animator = candidateAnimator;
					prefabAnimationPreviewOwnerEntityId_ = candidate.id;
					prefabAnimationPreviewClipIndex_ = 0;
					break;
				}
			}
		}
		if (animator && !animator->prefabAnimationClips.empty()) {
			prefabAnimationPreviewClipIndex_ = std::clamp(
				prefabAnimationPreviewClipIndex_,
				0,
				static_cast<int>(animator->prefabAnimationClips.size() - 1)
			);
			ImGui::SameLine();
			ImGui::Checkbox("Ghost", &prefabHitBoxGhostVisible_);
			ImGui::SameLine();
			const ScenePrefabAnimationClip& ghostClip =
				animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_];
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::BeginCombo("##HitBoxGhostClip", ghostClip.name.c_str())) {
				for (size_t index = 0;
					index < animator->prefabAnimationClips.size();
					++index) {
					const bool selected = static_cast<int>(index) ==
						prefabAnimationPreviewClipIndex_;
					if (ImGui::Selectable(
						animator->prefabAnimationClips[index].name.c_str(),
						selected
					)) {
						prefabAnimationPreviewClipIndex_ = static_cast<int>(index);
						prefabHitBoxGhostTime_ = 0.0f;
					}
				}
				ImGui::EndCombo();
			}
			const ScenePrefabAnimationClip& selectedGhostClip =
				animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_];
			ImGui::SameLine();
			ImGui::SetNextItemWidth(100.0f);
			ImGui::BeginDisabled(!prefabHitBoxGhostVisible_);
			ImGui::SliderFloat(
				"##HitBoxGhostTime",
				&prefabHitBoxGhostTime_,
				0.0f,
				(std::max)(selectedGhostClip.duration, 0.001f),
				"Ghost %.2f s"
			);
			ImGui::EndDisabled();
		} else {
			ImGui::SameLine();
			ImGui::TextDisabled("No PrefabAnimator for Ghost");
		}
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Grid", &prefabGridVisible_)) {
		SaveEditorSettings();
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Axis", &prefabAxisVisible_)) {
		SaveEditorSettings();
	}
	ImGui::PopID();

	const float availableWidth = (std::max)(
		ImGui::GetContentRegionAvail().x,
		240.0f
	);
	const float previewHeight = std::clamp(
		availableWidth * 9.0f / 16.0f,
		220.0f,
		440.0f
	);
	// Wheel scrolling is resolved before widgets are submitted. Keep this child
	// scrollable as the wheel target, then reset it so only Stage zoom changes.
	ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
	if (!ImGui::BeginChild(
		"PrefabStagePreview",
		ImVec2(0.0f, previewHeight),
		ImGuiChildFlags_Borders,
		ImGuiWindowFlags_NoScrollbar
	)) {
		ImGui::EndChild();
		return;
	}

	const ImVec2 imageSize = ImGui::GetContentRegionAvail();
	const float imageStartCursorY = ImGui::GetCursorPosY();
	const bool stageHovered = ImGui::IsWindowHovered(
		ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
	);
	if (stageHovered) {
		// Image has no Item ID, so use a stable Stage ID to own the wheel.
		ImGui::SetKeyOwner(
			ImGuiKey_MouseWheelY,
			ImGui::GetID("PrefabStageWheelOwner"),
			ImGuiInputFlags_LockThisFrame
		);
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.0f) {
			prefabPreviewZoom_ = std::clamp(
				prefabPreviewZoom_ * (1.0f - wheel * 0.1f),
				0.02f,
				1.75f
			);
		}
	}
	const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
	prefabPreviewRequestedWidth_ = static_cast<uint32_t>(std::clamp(
		imageSize.x * framebufferScale.x,
		320.0f,
		1600.0f
	));
	prefabPreviewRequestedHeight_ = static_cast<uint32_t>(std::clamp(
		imageSize.y * framebufferScale.y,
		180.0f,
		900.0f
	));

	const bool previewReady =
		prefabPreviewTexture_.ptr != 0 &&
		prefabPreviewCameraValid_ &&
		prefabPreviewRenderedPath_ == prefabPreviewRequestedPath_ &&
		prefabPreviewRenderedRevision_ == prefabPreviewRequestedRevision_;
	if (previewReady) {
		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		ImGui::Image(
			ImTextureRef(static_cast<ImTextureID>(prefabPreviewTexture_.ptr)),
			imageSize
		);
		const bool imageHovered = ImGui::IsItemHovered();
		if (imageHovered) {
			const ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
				prefabPreviewYaw_ += io.MouseDelta.x * 0.01f;
				prefabPreviewPitch_ = std::clamp(
					prefabPreviewPitch_ + io.MouseDelta.y * 0.01f,
					-1.45f,
					1.45f
				);
			}
		}
		if (!prefabPreviewRequestUsesCombatRig_) {
			DrawPrefabGizmo(imageMin.x, imageMin.y, imageSize.x, imageSize.y);
		}
		if (prefabAxisVisible_) {
			DrawWorldAxisIndicator(
				imageMin,
				ImVec2(
					imageMin.x + imageSize.x,
					imageMin.y + imageSize.y
				),
				prefabPreviewViewMatrix_
			);
		}
		if (prefabPreviewRequestUsesCombatRig_) {
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(imageMin.x + 8.0f, imageMin.y + 8.0f),
				IM_COL32(130, 220, 150, 255),
				"Combat Rig Preview: read-only composition."
			);
		} else if (prefabAnimationPreviewActive_) {
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(imageMin.x + 8.0f, imageMin.y + 8.0f),
				IM_COL32(130, 220, 150, 255),
				"Animation Preview: Gizmo writes a key at the current time."
			);
		}
		if (
			!prefabPreviewRequestUsesCombatRig_ &&
			imageHovered &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGuizmo::IsOver() &&
			!ImGuizmo::IsUsing()
		) {
			PickPrefabEntity(imageMin.x, imageMin.y, imageSize.x, imageSize.y);
		}
	} else {
		const char* message = document.GetEntities().empty()
			? "Prefab has no Entities."
			: "Preparing Prefab Preview...";
		const ImVec2 textSize = ImGui::CalcTextSize(message);
		ImGui::SetCursorPos({
			(std::max)((imageSize.x - textSize.x) * 0.5f, 0.0f),
			(std::max)((imageSize.y - textSize.y) * 0.5f, 0.0f)
		});
		ImGui::TextDisabled("%s", message);
	}
	// A tiny hidden overflow makes this child the ImGui wheel target. Its scroll
	// is reset before BeginChild, so the preview image and Gizmo never shift.
	ImGui::SetCursorPosY(imageStartCursorY + imageSize.y + 1.0f);
	ImGui::Dummy(ImVec2(0.0f, 1.0f));
	ImGui::EndChild();
	DrawPrefabAnimationTimeline();
}

void ImGuiManager::DrawWorldAxisIndicator(
	const ImVec2& imageMin,
	const ImVec2& imageMax,
	const Matrix4x4& viewMatrix
) const {
	const float width = imageMax.x - imageMin.x;
	const float height = imageMax.y - imageMin.y;
	if (width < 80.0f || height < 80.0f) {
		return;
	}

	// View MatrixでWorld軸をCamera空間へ変換するため、Cameraを回しても
	// 色とラベルが常にWorld X/Y/Zの向きを示す。表示専用で入力は受け取らない。
	const ImVec2 center(imageMax.x - 34.0f, imageMax.y - 34.0f);
	constexpr float kAxisLength = 22.0f;
	constexpr float kBackgroundRadius = 27.0f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddCircleFilled(center, kBackgroundRadius, IM_COL32(12, 15, 19, 190));
	drawList->AddCircle(center, kBackgroundRadius, IM_COL32(210, 220, 230, 170));

	struct AxisStyle {
		const char* label;
		ImU32 color;
		Vector3 worldDirection;
	};
	const AxisStyle axes[] = {
		{ "X", IM_COL32(235, 78, 78, 255), { 1.0f, 0.0f, 0.0f } },
		{ "Y", IM_COL32(98, 210, 105, 255), { 0.0f, 1.0f, 0.0f } },
		{ "Z", IM_COL32(83, 145, 245, 255), { 0.0f, 0.0f, 1.0f } }
	};
	for (const AxisStyle& axis : axes) {
		const float viewX =
			axis.worldDirection.x * viewMatrix.m[0][0] +
			axis.worldDirection.y * viewMatrix.m[1][0] +
			axis.worldDirection.z * viewMatrix.m[2][0];
		const float viewY =
			axis.worldDirection.x * viewMatrix.m[0][1] +
			axis.worldDirection.y * viewMatrix.m[1][1] +
			axis.worldDirection.z * viewMatrix.m[2][1];
		const ImVec2 direction(viewX, -viewY);
		const float directionLength = std::sqrt(
			direction.x * direction.x + direction.y * direction.y
		);
		const ImVec2 endpoint(
			center.x + direction.x * kAxisLength,
			center.y + direction.y * kAxisLength
		);
		drawList->AddLine(center, endpoint, axis.color, 2.5f);
		drawList->AddCircleFilled(endpoint, 3.5f, axis.color);

		const ImVec2 labelSize = ImGui::CalcTextSize(axis.label);
		const ImVec2 labelOffset = directionLength > 0.001f
			? ImVec2(
				direction.x / directionLength * 5.0f,
				direction.y / directionLength * 5.0f
			)
			: ImVec2(4.0f, 4.0f);
		drawList->AddText(
			ImVec2(
				endpoint.x + labelOffset.x - labelSize.x * 0.5f,
				endpoint.y + labelOffset.y - labelSize.y * 0.5f
			),
			axis.color,
			axis.label
		);
	}
}

void ImGuiManager::HandleEditShortcuts() {
	const ImGuiIO& io = ImGui::GetIO();
	if (
		!io.KeyCtrl ||
		io.WantTextInput ||
		ImGui::IsAnyItemActive()
	) {
		return;
	}

	const bool saveRequested = ImGui::IsKeyPressed(ImGuiKey_S, false);
	const bool undoRequested =
		!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false);
	const bool redoRequested =
		ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
		(io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false));
	if (!saveRequested && !undoRequested && !redoRequested) {
		return;
	}

	if (
		prefabKeyboardFocusThisFrame_ &&
		prefabEditorSession_ &&
		prefabEditorSession_->IsOpen()
	) {
		if (saveRequested && prefabEditorSession_->Save()) {
			InvalidateProjectCache();
		}
		if (undoRequested) {
			prefabEditorSession_->Undo();
		}
		if (redoRequested) {
			prefabEditorSession_->Redo();
		}
		return;
	}

	if (!editorSession_ || !editorSession_->IsEditing()) {
		return;
	}
	if (saveRequested) {
		editorSession_->Save();
	}
	if (undoRequested) {
		editorSession_->Undo();
	}
	if (redoRequested) {
		editorSession_->Redo();
	}
}

void ImGuiManager::DrawSceneDebugLabels(
	const ImVec2& imageMin,
	const ImVec2& imageMax,
	const Matrix4x4& viewProjectionMatrix
) const {
	const float width = imageMax.x - imageMin.x;
	const float height = imageMax.y - imageMin.y;
	if (width <= 1.0f || height <= 1.0f) {
		return;
	}

	const std::vector<DebugRenderer::WorldLabel>& labels =
		DebugRenderer::GetInstance()->GetWorldLabels();
	if (labels.empty()) {
		return;
	}

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->PushClipRect(imageMin, imageMax, true);
	for (const DebugRenderer::WorldLabel& label : labels) {
		const Vector3& position = label.position;
		const float clipX =
			position.x * viewProjectionMatrix.m[0][0] +
			position.y * viewProjectionMatrix.m[1][0] +
			position.z * viewProjectionMatrix.m[2][0] +
			viewProjectionMatrix.m[3][0];
		const float clipY =
			position.x * viewProjectionMatrix.m[0][1] +
			position.y * viewProjectionMatrix.m[1][1] +
			position.z * viewProjectionMatrix.m[2][1] +
			viewProjectionMatrix.m[3][1];
		const float clipZ =
			position.x * viewProjectionMatrix.m[0][2] +
			position.y * viewProjectionMatrix.m[1][2] +
			position.z * viewProjectionMatrix.m[2][2] +
			viewProjectionMatrix.m[3][2];
		const float clipW =
			position.x * viewProjectionMatrix.m[0][3] +
			position.y * viewProjectionMatrix.m[1][3] +
			position.z * viewProjectionMatrix.m[2][3] +
			viewProjectionMatrix.m[3][3];
		if (clipW <= 0.0001f || clipZ < 0.0f || clipZ > clipW) {
			continue;
		}

		const float ndcX = clipX / clipW;
		const float ndcY = clipY / clipW;
		if (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f) {
			continue;
		}
		const ImVec2 screenPosition{
			imageMin.x + (ndcX + 1.0f) * 0.5f * width,
			imageMin.y + (1.0f - ndcY) * 0.5f * height
		};
		drawList->AddText(
			ImVec2(screenPosition.x + 7.0f, screenPosition.y - 7.0f),
			ImGui::ColorConvertFloat4ToU32(ImVec4(
				label.color.x,
				label.color.y,
				label.color.z,
				label.color.w
			)),
			label.text.c_str()
		);
	}
	drawList->PopClipRect();
}

void ImGuiManager::DrawPrefabAnimationTimeline() {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}
	if (prefabHitBoxSetupMode_) {
		ImGui::SeparatorText("Prefab Animator Timeline");
		ImGui::TextDisabled(
			"HitBox Setup uses the Authoring Pose. Disable HitBox Setup to preview animation."
		);
		return;
	}

	const std::string& assetPath = prefabEditorSession_->GetFilePath();
	if (prefabAnimationPreviewAssetPath_ != assetPath) {
		prefabAnimationPreviewAssetPath_ = assetPath;
		prefabAnimationPreviewSourceRevision_ = 0;
		prefabAnimationPreviewOwnerEntityId_ = 0;
		prefabAnimationPreviewClipIndex_ = 0;
		prefabAnimationPreviewTime_ = 0.0f;
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = false;
		prefabAttackPreviewIndex_ = 0;
		prefabTransformPoseAddTime_ = 0.0f;
		prefabTransformPoseStatus_.clear();
	}

	const SceneDocument& document = prefabEditorSession_->GetDocument();
	struct AnimatorEntry {
		const SceneEntity* entity = nullptr;
		const SceneComponent* component = nullptr;
	};
	std::vector<AnimatorEntry> animators;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (const SceneComponent* component =
			FindEnabledComponent(entity, "PrefabAnimator")) {
			animators.push_back({ &entity, component });
		}
	}

	ImGui::SeparatorText("Prefab Animator Timeline");
	if (animators.empty()) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = false;
		ImGui::TextDisabled("Add an enabled PrefabAnimator to preview clips.");
		return;
	}

	int animatorIndex = -1;
	for (size_t index = 0; index < animators.size(); ++index) {
		if (animators[index].entity->id == prefabAnimationPreviewOwnerEntityId_) {
			animatorIndex = static_cast<int>(index);
			break;
		}
	}
	if (animatorIndex < 0) {
		animatorIndex = 0;
		prefabAnimationPreviewOwnerEntityId_ = animators.front().entity->id;
		prefabAnimationPreviewClipIndex_ = 0;
		prefabAnimationPreviewTime_ = 0.0f;
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = false;
		prefabAttackPreviewIndex_ = 0;
		prefabTransformPoseAddTime_ = 0.0f;
		prefabTransformPoseStatus_.clear();
	}

	if (ImGui::BeginCombo(
		"Animator",
		animators[animatorIndex].entity->name.c_str()
	)) {
		for (size_t index = 0; index < animators.size(); ++index) {
			const bool selected = static_cast<int>(index) == animatorIndex;
			ImGui::PushID(static_cast<int>(animators[index].entity->id));
			if (ImGui::Selectable(
				animators[index].entity->name.c_str(),
				selected
			)) {
				animatorIndex = static_cast<int>(index);
				prefabAnimationPreviewOwnerEntityId_ =
					animators[index].entity->id;
				prefabAnimationPreviewClipIndex_ = 0;
				prefabAnimationPreviewTime_ = 0.0f;
				prefabAnimationPreviewPlaying_ = false;
				prefabAnimationPreviewActive_ =
					!animators[index].component->prefabAnimationClips.empty();
				prefabAttackPreviewIndex_ = 0;
				prefabTransformPoseAddTime_ = 0.0f;
				prefabTransformPoseStatus_.clear();
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}

	const AnimatorEntry& animatorEntry = animators[animatorIndex];
	const std::vector<ScenePrefabAnimationClip>& clips =
		animatorEntry.component->prefabAnimationClips;
	if (clips.empty()) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = false;
		ImGui::TextDisabled("The selected PrefabAnimator has no clips.");
		return;
	}

	const SceneComponent* attackSet = FindEnabledComponent(
		*animatorEntry.entity,
		"AttackSet"
	);
	if (prefabAttackPreviewMode_) {
		if (!attackSet || attackSet->attackDefinitions.empty()) {
			ImGui::TextDisabled(
				"Attack Preview requires an AttackSet on the selected Animator."
			);
		} else {
			prefabAttackPreviewIndex_ = std::clamp(
				prefabAttackPreviewIndex_,
				0,
				static_cast<int>(attackSet->attackDefinitions.size() - 1)
			);
			const SceneAttackDefinition& selectedAttack =
				attackSet->attackDefinitions[prefabAttackPreviewIndex_];
			if (ImGui::BeginCombo("Attack", selectedAttack.name.c_str())) {
				for (size_t index = 0;
					index < attackSet->attackDefinitions.size();
					++index) {
					const bool selected = static_cast<int>(index) ==
						prefabAttackPreviewIndex_;
					if (ImGui::Selectable(
						attackSet->attackDefinitions[index].name.c_str(),
						selected
					)) {
						prefabAttackPreviewIndex_ = static_cast<int>(index);
					}
				}
				ImGui::EndCombo();
			}
			const SceneAttackDefinition& attack =
				attackSet->attackDefinitions[prefabAttackPreviewIndex_];
			auto clipEntry = std::find_if(
				clips.begin(),
				clips.end(),
				[&attack](const ScenePrefabAnimationClip& candidate) {
					return candidate.name == attack.animation;
				}
			);
			if (clipEntry == clips.end()) {
				ImGui::TextDisabled(
					"Attack animation '%s' was not found.",
					attack.animation.c_str()
				);
			} else {
				const int attackClipIndex = static_cast<int>(
					std::distance(clips.begin(), clipEntry)
				);
				if (prefabAnimationPreviewClipIndex_ != attackClipIndex) {
					prefabAnimationPreviewClipIndex_ = attackClipIndex;
					prefabAnimationPreviewTime_ = 0.0f;
					prefabAnimationPreviewPlaying_ = false;
				}
				prefabAnimationPreviewActive_ = true;
			}
		}
	}

	prefabAnimationPreviewClipIndex_ = std::clamp(
		prefabAnimationPreviewClipIndex_,
		0,
		static_cast<int>(clips.size() - 1)
	);
	const char* clipPreview =
		clips[prefabAnimationPreviewClipIndex_].name.empty()
			? "(Unnamed Clip)"
			: clips[prefabAnimationPreviewClipIndex_].name.c_str();
	ImGui::BeginDisabled(prefabAttackPreviewMode_);
	if (ImGui::BeginCombo("Clip", clipPreview)) {
		for (size_t index = 0; index < clips.size(); ++index) {
			const bool selected =
				static_cast<int>(index) == prefabAnimationPreviewClipIndex_;
			const char* label = clips[index].name.empty()
				? "(Unnamed Clip)"
				: clips[index].name.c_str();
			ImGui::PushID(static_cast<int>(index));
			if (ImGui::Selectable(label, selected)) {
				prefabAnimationPreviewClipIndex_ = static_cast<int>(index);
				prefabSelectedEntityId_ = animatorEntry.entity->id;
				prefabAnimationPreviewTime_ = 0.0f;
				prefabAnimationPreviewPlaying_ = false;
				prefabAnimationPreviewActive_ = true;
				prefabTransformPoseAddTime_ = 0.0f;
				prefabTransformPoseStatus_.clear();
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Checkbox("Clip Focus", &prefabClipFocusEnabled_);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"When the Animator owner is selected, show only this Clip and its Attack."
		);
	}

	const ScenePrefabAnimationClip& clip =
		clips[prefabAnimationPreviewClipIndex_];
	const float duration = (std::max)(clip.duration, 0.001f);
	const SceneAttackDefinition* timelineAttack = nullptr;
	if (attackSet) {
		auto found = std::find_if(
			attackSet->attackDefinitions.begin(),
			attackSet->attackDefinitions.end(),
			[&clip](const SceneAttackDefinition& attack) {
				return attack.animation == clip.name;
			}
		);
		if (found != attackSet->attackDefinitions.end()) {
			timelineAttack = &*found;
		}
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Combat Rig Preview", &playerCombatPreviewEnabled_)) {
		playerCombatPreviewStatus_.clear();
	}
	if (playerCombatPreviewEnabled_) {
		if (!editorSession_ || !editorSession_->IsEditing()) {
			playerCombatPreviewStatus_ = "Open the title Scene in Edit mode to use Combat Rig Preview.";
		} else {
			const SceneDocument& scene = editorSession_->GetEditDocument();
			struct RigCandidate { uint64_t rootId; uint64_t weaponId; };
			std::vector<RigCandidate> candidates;
			for (const SceneEntity& weapon : scene.GetEntities()) {
				if (!FindEnabledComponent(weapon, "PrefabAnimator") ||
					!FindEnabledComponent(weapon, "AttackSet")) {
					continue;
				}
				const SceneEntity* root = scene.FindEntity(weapon.parentId);
				if (root) candidates.push_back({ root->id, weapon.id });
			}
			if (candidates.empty()) {
				playerCombatPreviewStatus_ = "No PlayerWeapon instance with Animator and AttackSet was found.";
			} else {
				auto selected = std::find_if(
					candidates.begin(), candidates.end(), [this](const RigCandidate& candidate) {
						return candidate.rootId == playerCombatPreviewRootId_ &&
							candidate.weaponId == playerCombatPreviewWeaponId_;
					}
				);
				if (selected == candidates.end()) {
					selected = candidates.begin();
					playerCombatPreviewRootId_ = selected->rootId;
					playerCombatPreviewWeaponId_ = selected->weaponId;
				}
				const SceneEntity* selectedRoot = scene.FindEntity(selected->rootId);
				const SceneEntity* selectedWeapon = scene.FindEntity(selected->weaponId);
				const std::string label = (selectedRoot ? selectedRoot->name : "Missing") +
					" / " + (selectedWeapon ? selectedWeapon->name : "Missing");
				if (ImGui::BeginCombo("Rig Source", label.c_str())) {
					for (const RigCandidate& candidate : candidates) {
						const SceneEntity* root = scene.FindEntity(candidate.rootId);
						const SceneEntity* weapon = scene.FindEntity(candidate.weaponId);
						const std::string candidateLabel =
							(root ? root->name : "Missing") + " / " +
							(weapon ? weapon->name : "Missing");
						if (ImGui::Selectable(
							candidateLabel.c_str(), candidate.weaponId == selected->weaponId
						)) {
							playerCombatPreviewRootId_ = candidate.rootId;
							playerCombatPreviewWeaponId_ = candidate.weaponId;
							playerCombatPreviewStatus_.clear();
						}
					}
					ImGui::EndCombo();
				}
			}
		}
		if (!playerCombatPreviewStatus_.empty()) {
			ImGui::TextDisabled("%s", playerCombatPreviewStatus_.c_str());
		}
	}
	static constexpr int kPreviewFrameRates[] = { 30, 60, 120 };
	const int previewFrameRate = std::clamp(
		prefabAnimationPreviewFrameRate_,
		kPreviewFrameRates[0],
		kPreviewFrameRates[2]
	);
	prefabAnimationPreviewFrameRate_ = previewFrameRate;
	const int previewFrameCount = (std::max)(
		1,
		static_cast<int>(std::ceil(duration * static_cast<float>(previewFrameRate)))
	);
	const auto frameToTime = [&](int frame) {
		return (std::min)(
			static_cast<float>(std::clamp(frame, 0, previewFrameCount)) /
				static_cast<float>(previewFrameRate),
			duration
		);
	};
	const auto timeToFrame = [&](float time) {
		return std::clamp(
			static_cast<int>(std::round(time * static_cast<float>(previewFrameRate))),
			0,
			previewFrameCount
		);
	};
	const auto snapPreviewTimeToFrame = [&]() {
		if (prefabAnimationPreviewSnapToFrames_) {
			prefabAnimationPreviewTime_ = frameToTime(
				timeToFrame(prefabAnimationPreviewTime_)
			);
		}
	};
	const auto evaluateDistanceEasedMotionProgress = [&](float time) {
		if (!timelineAttack) {
			return 0.0f;
		}
		const float activeDuration = (std::max)(timelineAttack->activeTime, 0.0001f);
		float progress = std::clamp(
			(time - timelineAttack->windup) / activeDuration,
			0.0f,
			1.0f
		);
		if (timelineAttack->motionEasing == "EaseOut") {
			return Math::EaseOutCubic(progress);
		}
		if (timelineAttack->motionEasing == "EaseIn") {
			return progress * progress * progress;
		}
		if (timelineAttack->motionEasing == "EaseInOut") {
			return progress < 0.5f
				? 4.0f * progress * progress * progress
				: 1.0f - std::pow(-2.0f * progress + 2.0f, 3.0f) * 0.5f;
		}
		return timelineAttack->motionEasing == "Linear"
			? progress
			: Math::SmoothStep(progress);
	};
	prefabAnimationPreviewTime_ = std::clamp(
		prefabAnimationPreviewTime_,
		0.0f,
		duration
	);
	if (prefabAnimationPreviewPlaying_) {
		prefabAnimationPreviewActive_ = true;
		if (prefabAnimationPreviewSnapToFrames_) {
			const float frameDuration = 1.0f / static_cast<float>(previewFrameRate);
			prefabAnimationPreviewFrameAccumulator_ += (std::max)(
				ImGui::GetIO().DeltaTime,
				0.0f
			);
			while (prefabAnimationPreviewFrameAccumulator_ >= frameDuration) {
				prefabAnimationPreviewFrameAccumulator_ -= frameDuration;
				const int nextFrame = timeToFrame(prefabAnimationPreviewTime_) + 1;
				if (nextFrame > previewFrameCount) {
					if (clip.loop) {
						prefabAnimationPreviewTime_ = 0.0f;
					} else {
						prefabAnimationPreviewTime_ = duration;
						prefabAnimationPreviewPlaying_ = false;
						prefabAnimationPreviewFrameAccumulator_ = 0.0f;
						break;
					}
				} else {
					prefabAnimationPreviewTime_ = frameToTime(nextFrame);
				}
			}
		} else {
			prefabAnimationPreviewTime_ += (std::max)(
				ImGui::GetIO().DeltaTime,
				0.0f
			);
			if (clip.loop) {
				prefabAnimationPreviewTime_ = std::fmod(
					prefabAnimationPreviewTime_,
					duration
				);
			} else if (prefabAnimationPreviewTime_ >= duration) {
				prefabAnimationPreviewTime_ = duration;
				prefabAnimationPreviewPlaying_ = false;
			}
		}
	}

	if (ImGui::Button(prefabAnimationPreviewPlaying_ ? "Pause" : "Play")) {
		if (
			!prefabAnimationPreviewPlaying_ &&
			!clip.loop &&
			prefabAnimationPreviewTime_ >= duration
		) {
			prefabAnimationPreviewTime_ = 0.0f;
		}
		prefabAnimationPreviewPlaying_ = !prefabAnimationPreviewPlaying_;
		prefabAnimationPreviewFrameAccumulator_ = 0.0f;
		prefabAnimationPreviewActive_ = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Stop")) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewTime_ = 0.0f;
		prefabAnimationPreviewFrameAccumulator_ = 0.0f;
		prefabAnimationPreviewActive_ = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset Pose")) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewTime_ = 0.0f;
		prefabAnimationPreviewFrameAccumulator_ = 0.0f;
		prefabAnimationPreviewActive_ = false;
	}
	ImGui::SameLine();
	ImGui::TextDisabled(
		prefabAnimationPreviewActive_ ? "Preview Pose" : "Authoring Pose"
	);

	if (ImGui::SliderFloat(
		"Time",
		&prefabAnimationPreviewTime_,
		0.0f,
		duration,
		"%.3f s"
	)) {
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
		snapPreviewTimeToFrame();
	}
	if (ImGui::BeginCombo("Preview FPS", (std::to_string(previewFrameRate) + " FPS").c_str())) {
		for (const int frameRate : kPreviewFrameRates) {
			const bool selected = frameRate == previewFrameRate;
			const std::string label = std::to_string(frameRate) + " FPS";
			if (ImGui::Selectable(label.c_str(), selected)) {
				prefabAnimationPreviewFrameRate_ = frameRate;
				prefabAnimationPreviewFrameAccumulator_ = 0.0f;
				const int selectedFrame = std::clamp(
					static_cast<int>(std::round(
						prefabAnimationPreviewTime_ * static_cast<float>(frameRate)
					)),
					0,
					(std::max)(1, static_cast<int>(std::ceil(
						duration * static_cast<float>(frameRate)
					)))
				);
				prefabAnimationPreviewTime_ = (std::min)(
					static_cast<float>(selectedFrame) / static_cast<float>(frameRate),
					duration
				);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::Checkbox("Frame Snap", &prefabAnimationPreviewSnapToFrames_);
	if (prefabAnimationPreviewSnapToFrames_) {
		snapPreviewTimeToFrame();
	}
	int previewFrame = timeToFrame(prefabAnimationPreviewTime_);
	if (ImGui::Button("< Frame")) {
		prefabAnimationPreviewTime_ = frameToTime(previewFrame - 1);
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Frame >")) {
		prefabAnimationPreviewTime_ = frameToTime(previewFrame + 1);
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
	}
	ImGui::SameLine();
	if (ImGui::DragInt("Frame", &previewFrame, 1.0f, 0, previewFrameCount)) {
		prefabAnimationPreviewTime_ = frameToTime(previewFrame);
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
	}

	const float timelineWidth = (std::max)(
		ImGui::GetContentRegionAvail().x,
		280.0f
	);
	const auto makeTrackLabel = [&](const SceneAnimationTrack& track) {
		const SceneEntity* target = track.targetEntityId != 0
			? document.FindEntity(track.targetEntityId)
			: nullptr;
		if (!target && !track.targetEntityName.empty()) {
			target = document.FindEntityByName(track.targetEntityName);
		}
		if (!target) {
			target = animatorEntry.entity;
		}
		const std::string targetName = target
			? target->name
			: std::string("Missing Target");
		return targetName + " / " + track.property;
	};
	float desiredLabelWidth = 120.0f;
	for (const SceneAnimationTrack& track : clip.tracks) {
		desiredLabelWidth = (std::max)(
			desiredLabelWidth,
			ImGui::CalcTextSize(makeTrackLabel(track).c_str()).x + 12.0f
		);
	}
	if (timelineAttack) {
		desiredLabelWidth = (std::max)(
			desiredLabelWidth,
			ImGui::CalcTextSize("Player Motion / Distance Eased").x + 12.0f
		);
		desiredLabelWidth = (std::max)(
			desiredLabelWidth,
			ImGui::CalcTextSize(
				("Effect / " + timelineAttack->name).c_str()
			).x + 12.0f
		);
	}
	const float maximumLabelWidth = (std::max)(
		120.0f,
		(std::min)(280.0f, timelineWidth - 160.0f)
	);
	const float labelWidth = std::clamp(
		desiredLabelWidth,
		120.0f,
		maximumLabelWidth
	);
	const float headerHeight = 24.0f;
	const float rowHeight = 24.0f;
	const size_t hitWindowCount = timelineAttack
		? timelineAttack->hitWindows.size()
		: 0;
	const size_t effectEventRowCount = timelineAttack &&
		!timelineAttack->effectEvents.empty()
		? size_t{ 1 }
		: size_t{ 0 };
	const size_t motionRowCount = timelineAttack ? size_t{ 1 } : size_t{ 0 };
	const size_t rowCount = (std::max)(
		clip.tracks.size() + hitWindowCount + effectEventRowCount + motionRowCount,
		size_t{ 1 }
	);
	const float timelineHeight = headerHeight + rowHeight * rowCount;
	const ImVec2 timelineOrigin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(
		"##PrefabAnimationTimeline",
		ImVec2(timelineWidth, timelineHeight),
		ImGuiButtonFlags_MouseButtonLeft
	);
	const float trackLeft = timelineOrigin.x + labelWidth;
	const float trackRight = timelineOrigin.x + timelineWidth;
	const float trackWidth = (std::max)(trackRight - trackLeft, 1.0f);
	if (
		ImGui::IsItemActive() &&
		ImGui::GetMousePos().x >= trackLeft
	) {
		const float amount = std::clamp(
			(ImGui::GetMousePos().x - trackLeft) / trackWidth,
			0.0f,
			1.0f
		);
		prefabAnimationPreviewTime_ = amount * duration;
		snapPreviewTimeToFrame();
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
	}
	if (
		ImGui::IsItemHovered() &&
		ImGui::GetMousePos().x >= trackLeft
	) {
		ImGui::SetItemTooltip(
			"Drag to seek | %.3f / %.3f s",
			prefabAnimationPreviewTime_,
			duration
		);
	}

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(
		timelineOrigin,
		ImVec2(trackRight, timelineOrigin.y + timelineHeight),
		IM_COL32(27, 30, 34, 255)
	);
	drawList->AddLine(
		ImVec2(trackLeft, timelineOrigin.y),
		ImVec2(trackLeft, timelineOrigin.y + timelineHeight),
		IM_COL32(82, 88, 96, 255)
	);
	for (int division = 0; division <= 5; ++division) {
		const float amount = static_cast<float>(division) / 5.0f;
		const float x = trackLeft + trackWidth * amount;
		drawList->AddLine(
			ImVec2(x, timelineOrigin.y + headerHeight),
			ImVec2(x, timelineOrigin.y + timelineHeight),
			IM_COL32(58, 63, 70, 255)
		);
		char timeLabel[32]{};
		std::snprintf(timeLabel, sizeof(timeLabel), "%.2f", duration * amount);
		drawList->AddText(
			ImVec2(x + 3.0f, timelineOrigin.y + 4.0f),
			IM_COL32(165, 172, 182, 255),
			timeLabel
		);
	}

	for (size_t trackIndex = 0; trackIndex < clip.tracks.size(); ++trackIndex) {
		const SceneAnimationTrack& track = clip.tracks[trackIndex];
		const float rowTop =
			timelineOrigin.y + headerHeight + rowHeight * trackIndex;
		const float rowCenter = rowTop + rowHeight * 0.5f;
		if ((trackIndex & 1u) != 0u) {
			drawList->AddRectFilled(
				ImVec2(timelineOrigin.x, rowTop),
				ImVec2(trackRight, rowTop + rowHeight),
				IM_COL32(35, 39, 44, 255)
			);
		}
		const SceneEntity* target = track.targetEntityId != 0
			? document.FindEntity(track.targetEntityId)
			: nullptr;
		if (!target && !track.targetEntityName.empty()) {
			target = document.FindEntityByName(track.targetEntityName);
		}
		if (!target) {
			target = animatorEntry.entity;
		}
		const std::string rowLabel = makeTrackLabel(track);
		const ImVec2 labelMin(timelineOrigin.x, rowTop);
		const ImVec2 labelMax(trackLeft - 4.0f, rowTop + rowHeight);
		drawList->PushClipRect(labelMin, labelMax, true);
		drawList->AddText(
			ImVec2(timelineOrigin.x + 5.0f, rowTop + 4.0f),
			IM_COL32(215, 220, 228, 255),
			rowLabel.c_str()
		);
		drawList->PopClipRect();
		if (ImGui::IsMouseHoveringRect(labelMin, labelMax)) {
			ImGui::SetTooltip("%s", rowLabel.c_str());
		}

		ImU32 keyColor = IM_COL32(75, 170, 255, 255);
		ImU32 activeColor = IM_COL32(80, 205, 120, 190);
		if (target && FindEnabledComponent(*target, "HitBox")) {
			activeColor = IM_COL32(255, 70, 45, 205);
		} else if (target && FindEnabledComponent(*target, "HurtBox")) {
			activeColor = IM_COL32(45, 190, 255, 205);
		}
		if (track.property == "Active" && !track.keyframes.empty()) {
			keyColor = activeColor;
			for (size_t keyIndex = 0;
				keyIndex < track.keyframes.size();
				++keyIndex) {
				const float startTime = keyIndex == 0
					? 0.0f
					: std::clamp(
						track.keyframes[keyIndex].time,
						0.0f,
						duration
					);
				const float endTime = keyIndex + 1 < track.keyframes.size()
					? std::clamp(
						track.keyframes[keyIndex + 1].time,
						0.0f,
						duration
					)
					: duration;
				if (
					track.keyframes[keyIndex].value.x < 0.5f ||
					endTime <= startTime
				) {
					continue;
				}
				drawList->AddRectFilled(
					ImVec2(
						trackLeft + trackWidth * (startTime / duration),
						rowCenter - 6.0f
					),
					ImVec2(
						trackLeft + trackWidth * (endTime / duration),
						rowCenter + 6.0f
					),
					activeColor,
					2.0f
				);
			}
		}
		for (const SceneAnimationKeyframe& keyframe : track.keyframes) {
			const float keyTime = std::clamp(
				keyframe.time,
				0.0f,
				duration
			);
			const float keyX = trackLeft + trackWidth * (keyTime / duration);
			drawList->AddCircleFilled(
				ImVec2(keyX, rowCenter),
				3.5f,
				keyColor
			);
		}
	}

	if (timelineAttack) {
		for (size_t windowIndex = 0;
			windowIndex < timelineAttack->hitWindows.size();
			++windowIndex) {
			const SceneAttackHitWindow& window =
				timelineAttack->hitWindows[windowIndex];
			const size_t rowIndex = clip.tracks.size() + windowIndex;
			const float rowTop =
				timelineOrigin.y + headerHeight + rowHeight * rowIndex;
			const float rowCenter = rowTop + rowHeight * 0.5f;
			if ((rowIndex & 1u) != 0u) {
				drawList->AddRectFilled(
					ImVec2(timelineOrigin.x, rowTop),
					ImVec2(trackRight, rowTop + rowHeight),
					IM_COL32(35, 39, 44, 255)
				);
			}
			const SceneEntity* hitBox = window.hitBoxEntityId != 0
				? document.FindEntity(window.hitBoxEntityId)
				: nullptr;
			if (!hitBox && !window.hitBoxEntityName.empty()) {
				hitBox = document.FindEntityByName(window.hitBoxEntityName);
			}
			const std::string targetName = hitBox
				? hitBox->name
				: std::string("StateMachine HitBox");
			const std::string rowLabel = "Hit / " + timelineAttack->name +
				" / " + targetName;
			const ImVec2 labelMin(timelineOrigin.x, rowTop);
			const ImVec2 labelMax(trackLeft - 4.0f, rowTop + rowHeight);
			drawList->PushClipRect(labelMin, labelMax, true);
			drawList->AddText(
				ImVec2(timelineOrigin.x + 5.0f, rowTop + 4.0f),
				IM_COL32(255, 183, 90, 255),
				rowLabel.c_str()
			);
			drawList->PopClipRect();
			const float startTime = std::clamp(window.startTime, 0.0f, duration);
			const float endTime = std::clamp(window.endTime, startTime, duration);
			const bool active = prefabAnimationPreviewTime_ >= startTime &&
				prefabAnimationPreviewTime_ < endTime;
			const ImU32 hitColor = active
				? IM_COL32(255, 92, 48, 245)
				: IM_COL32(220, 92, 42, 185);
			drawList->AddRectFilled(
				ImVec2(
					trackLeft + trackWidth * (startTime / duration),
					rowCenter - 6.0f
				),
				ImVec2(
					trackLeft + trackWidth * (endTime / duration),
					rowCenter + 6.0f
				),
				hitColor,
				2.0f
			);
			if (ImGui::IsMouseHoveringRect(
				labelMin,
				ImVec2(trackRight, rowTop + rowHeight)
			)) {
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					prefabAnimationPreviewTime_ = startTime;
					prefabAnimationPreviewPlaying_ = false;
					prefabAnimationPreviewActive_ = true;
				}
				ImGui::SetTooltip(
					"%s | %.3f - %.3f s | Damage %.1f | Poise %.1f | Knockback %.1f",
					rowLabel.c_str(),
					window.startTime,
					window.endTime,
					window.damage,
					window.poiseDamage,
					window.knockback
				);
			}
		}

		if (effectEventRowCount != 0) {
			const size_t rowIndex = clip.tracks.size() + hitWindowCount;
			const float rowTop =
				timelineOrigin.y + headerHeight + rowHeight * rowIndex;
			const float rowCenter = rowTop + rowHeight * 0.5f;
			if ((rowIndex & 1u) != 0u) {
				drawList->AddRectFilled(
					ImVec2(timelineOrigin.x, rowTop),
					ImVec2(trackRight, rowTop + rowHeight),
					IM_COL32(35, 39, 44, 255)
				);
			}
			const std::string rowLabel = "Effect / " + timelineAttack->name;
			const ImVec2 labelMin(timelineOrigin.x, rowTop);
			const ImVec2 labelMax(trackLeft - 4.0f, rowTop + rowHeight);
			drawList->PushClipRect(labelMin, labelMax, true);
			drawList->AddText(
				ImVec2(timelineOrigin.x + 5.0f, rowTop + 4.0f),
				IM_COL32(135, 224, 244, 255),
				rowLabel.c_str()
			);
			drawList->PopClipRect();
			for (size_t effectIndex = 0;
				effectIndex < timelineAttack->effectEvents.size();
				++effectIndex) {
				const SceneAttackEffectEvent& effect =
					timelineAttack->effectEvents[effectIndex];
				const float effectTime = std::clamp(effect.time, 0.0f, duration);
				const float effectX = trackLeft + trackWidth * (effectTime / duration);
				const ImVec2 markerMin(effectX - 6.0f, rowCenter - 6.0f);
				const ImVec2 markerMax(effectX + 6.0f, rowCenter + 6.0f);
				drawList->AddQuadFilled(
					ImVec2(effectX, rowCenter - 6.0f),
					ImVec2(effectX + 6.0f, rowCenter),
					ImVec2(effectX, rowCenter + 6.0f),
					ImVec2(effectX - 6.0f, rowCenter),
					IM_COL32(82, 205, 236, 245)
				);
				if (ImGui::IsMouseHoveringRect(markerMin, markerMax)) {
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
						prefabAnimationPreviewTime_ = effectTime;
						snapPreviewTimeToFrame();
						prefabAnimationPreviewPlaying_ = false;
						prefabAnimationPreviewActive_ = true;
					}
					ImGui::SetTooltip(
						"Effect Event %zu | %.3f s | %s",
						effectIndex + 1,
						effect.time,
						effect.groundEffectType.empty()
							? "Particle"
							: effect.groundEffectType.c_str()
					);
				}
			}
		}

		const size_t rowIndex = clip.tracks.size() + hitWindowCount + effectEventRowCount;
		const float rowTop = timelineOrigin.y + headerHeight + rowHeight * rowIndex;
		const float rowCenter = rowTop + rowHeight * 0.5f;
		if ((rowIndex & 1u) != 0u) {
			drawList->AddRectFilled(
				ImVec2(timelineOrigin.x, rowTop),
				ImVec2(trackRight, rowTop + rowHeight),
				IM_COL32(35, 39, 44, 255)
			);
		}
		const std::string motionLabel = "Player Motion / Distance Eased";
		const ImVec2 labelMin(timelineOrigin.x, rowTop);
		const ImVec2 labelMax(trackLeft - 4.0f, rowTop + rowHeight);
		drawList->PushClipRect(labelMin, labelMax, true);
		drawList->AddText(
			ImVec2(timelineOrigin.x + 5.0f, rowTop + 4.0f),
			IM_COL32(150, 215, 255, 255),
			motionLabel.c_str()
		);
		drawList->PopClipRect();
		const float motionStart = std::clamp(timelineAttack->windup, 0.0f, duration);
		const float motionEnd = std::clamp(
			timelineAttack->windup + timelineAttack->activeTime,
			motionStart,
			duration
		);
		const int firstMotionFrame = timeToFrame(motionStart);
		const int lastMotionFrame = timeToFrame(motionEnd);
		float maxMotionSpeed = 0.0f;
		for (int frame = firstMotionFrame; frame < lastMotionFrame; ++frame) {
			const float firstTime = frameToTime(frame);
			const float secondTime = frameToTime(frame + 1);
			const float deltaTime = secondTime - firstTime;
			if (deltaTime <= 0.000001f) { continue; }
			const float deltaProgress = evaluateDistanceEasedMotionProgress(secondTime) -
				evaluateDistanceEasedMotionProgress(firstTime);
			const float distance = std::sqrt(
				timelineAttack->forwardDistance * timelineAttack->forwardDistance +
				timelineAttack->sideDistance * timelineAttack->sideDistance
			) * std::abs(deltaProgress);
			maxMotionSpeed = (std::max)(maxMotionSpeed, distance / deltaTime);
		}
		for (int frame = firstMotionFrame; frame < lastMotionFrame; ++frame) {
			const float firstTime = frameToTime(frame);
			const float secondTime = frameToTime(frame + 1);
			const float deltaTime = secondTime - firstTime;
			if (deltaTime <= 0.000001f) { continue; }
			const float deltaProgress = evaluateDistanceEasedMotionProgress(secondTime) -
				evaluateDistanceEasedMotionProgress(firstTime);
			const float distance = std::sqrt(
				timelineAttack->forwardDistance * timelineAttack->forwardDistance +
				timelineAttack->sideDistance * timelineAttack->sideDistance
			) * std::abs(deltaProgress);
			const float speedRate = maxMotionSpeed > 0.000001f
				? std::clamp((distance / deltaTime) / maxMotionSpeed, 0.0f, 1.0f)
				: 0.0f;
			const ImU32 speedColor = IM_COL32(
				static_cast<int>(70.0f + 120.0f * speedRate),
				static_cast<int>(130.0f + 100.0f * speedRate),
				255,
				220
			);
			drawList->AddRectFilled(
				ImVec2(trackLeft + trackWidth * (firstTime / duration), rowCenter - 6.0f),
				ImVec2(trackLeft + trackWidth * (secondTime / duration), rowCenter + 6.0f),
				speedColor,
				1.0f
			);
		}
		if (ImGui::IsMouseHoveringRect(labelMin, ImVec2(trackRight, rowTop + rowHeight))) {
			const float progress = evaluateDistanceEasedMotionProgress(prefabAnimationPreviewTime_);
			const float previousTime = frameToTime((std::max)(previewFrame - 1, 0));
			const float previewDeltaTime = prefabAnimationPreviewTime_ - previousTime;
			const float previewDeltaProgress = progress -
				evaluateDistanceEasedMotionProgress(previousTime);
			const float previewDistance = std::sqrt(
				timelineAttack->forwardDistance * timelineAttack->forwardDistance +
				timelineAttack->sideDistance * timelineAttack->sideDistance
			) * std::abs(previewDeltaProgress);
			const float previewSpeed = previewDeltaTime > 0.000001f
				? previewDistance / previewDeltaTime
				: 0.0f;
			ImGui::SetTooltip(
				"Frame %d | Local X %.3f, Z %.3f | XZ Speed %.3f units/s",
				previewFrame,
				timelineAttack->sideDistance * progress,
				timelineAttack->forwardDistance * progress,
				previewSpeed
			);
		}
	}

	const float playheadX = trackLeft + trackWidth *
		(prefabAnimationPreviewTime_ / duration);
	drawList->AddLine(
		ImVec2(playheadX, timelineOrigin.y),
		ImVec2(playheadX, timelineOrigin.y + timelineHeight),
		IM_COL32(255, 215, 70, 255),
		2.0f
	);
	RebuildPrefabAnimationPreviewDocument();
}

void ImGuiManager::DrawPrefabTransformPoseInspector(SceneEntity& entity) {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}

	SceneDocument& document = prefabEditorSession_->GetDocument();
	SceneEntity* owner = document.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	SceneComponent* animator = owner
		? FindComponent(*owner, "PrefabAnimator")
		: nullptr;
	if (
		!animator ||
		!animator->enabled ||
		prefabAnimationPreviewClipIndex_ < 0 ||
		prefabAnimationPreviewClipIndex_ >=
			static_cast<int>(animator->prefabAnimationClips.size())
	) {
		return;
	}

	ScenePrefabAnimationClip& clip =
		animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_];
	const float duration = (std::max)(clip.duration, 0.001f);
	constexpr float kKeyTimeTolerance = 0.005f;
	constexpr float kRadiansToDegrees = 57.2957795f;
	constexpr float kDegreesToRadians = 0.0174532925f;

	const auto resolveTrackTargetId = [&](const SceneAnimationTrack& track) {
		if (track.targetEntityId != 0) {
			if (const SceneEntity* byId =
				document.FindEntity(track.targetEntityId)) {
				return byId->id;
			}
		}
		if (!track.targetEntityName.empty()) {
			if (const SceneEntity* byName =
				document.FindEntityByName(track.targetEntityName)) {
				return byName->id;
			}
		}
		return owner->id;
	};
	const auto propertyIndex = [](const std::string& property) {
		if (property == "LocalPosition") {
			return 0;
		}
		if (property == "LocalRotation") {
			return 1;
		}
		if (property == "LocalScale") {
			return 2;
		}
		return -1;
	};
	const auto propertyName = [](int index) -> const char* {
		return index == 0
			? "LocalPosition"
			: index == 1 ? "LocalRotation" : "LocalScale";
	};

	struct PoseKeyRef {
		int trackIndex = -1;
		int keyIndex = -1;
	};
	struct TransformPose {
		float time = 0.0f;
		PoseKeyRef keys[3];
	};
	struct TimedKeyRef {
		float time = 0.0f;
		int property = -1;
		int trackIndex = -1;
		int keyIndex = -1;
	};

	std::vector<TimedKeyRef> transformKeys;
	for (size_t trackIndex = 0; trackIndex < clip.tracks.size(); ++trackIndex) {
		const SceneAnimationTrack& track = clip.tracks[trackIndex];
		const int property = propertyIndex(track.property);
		if (property < 0 || resolveTrackTargetId(track) != entity.id) {
			continue;
		}
		for (size_t keyIndex = 0; keyIndex < track.keyframes.size(); ++keyIndex) {
			transformKeys.push_back({
				track.keyframes[keyIndex].time,
				property,
				static_cast<int>(trackIndex),
				static_cast<int>(keyIndex)
			});
		}
	}
	std::stable_sort(
		transformKeys.begin(),
		transformKeys.end(),
		[](const TimedKeyRef& left, const TimedKeyRef& right) {
			return left.time < right.time;
		}
	);

	std::vector<TransformPose> poses;
	for (const TimedKeyRef& key : transformKeys) {
		if (
			poses.empty() ||
			std::abs(poses.back().time - key.time) > kKeyTimeTolerance
		) {
			poses.push_back({});
			poses.back().time = key.time;
		}
		PoseKeyRef& destination = poses.back().keys[key.property];
		if (destination.trackIndex < 0) {
			destination.trackIndex = key.trackIndex;
			destination.keyIndex = key.keyIndex;
		}
	}

	const auto authoringValue = [&](int property) {
		return property == 0
			? entity.transform.translate
			: property == 1
				? MakeEulerFromQuaternion(entity.transform.rotate)
				: entity.transform.scale;
	};
	const auto resolvePoseValue = [&](const TransformPose& pose, int property) {
		const PoseKeyRef& key = pose.keys[property];
		if (key.trackIndex >= 0 && key.keyIndex >= 0) {
			return clip.tracks[key.trackIndex].keyframes[key.keyIndex].value;
		}

		Vector3 value = authoringValue(property);
		float latestTime = -1.0f;
		for (size_t trackIndex = 0;
			trackIndex < clip.tracks.size();
			++trackIndex) {
			const SceneAnimationTrack& track = clip.tracks[trackIndex];
			if (
				propertyIndex(track.property) != property ||
				resolveTrackTargetId(track) != entity.id
			) {
				continue;
			}
			for (const SceneAnimationKeyframe& keyframe : track.keyframes) {
				if (
					keyframe.time <= pose.time + kKeyTimeTolerance &&
					keyframe.time > latestTime
				) {
					latestTime = keyframe.time;
					value = keyframe.value;
				}
			}
		}
		return value;
	};
	const auto ensureTrack = [&](int property) {
		for (size_t trackIndex = 0;
			trackIndex < clip.tracks.size();
			++trackIndex) {
			SceneAnimationTrack& track = clip.tracks[trackIndex];
			if (
				track.property == propertyName(property) &&
				resolveTrackTargetId(track) == entity.id
			) {
				return static_cast<int>(trackIndex);
			}
		}
		SceneAnimationTrack track{};
		track.targetEntityId = entity.id;
		track.targetEntityName = entity.name;
		track.property = propertyName(property);
		clip.tracks.push_back(std::move(track));
		return static_cast<int>(clip.tracks.size() - 1);
	};
	const auto upsertKey = [&](int property, float time, const Vector3& value) {
		SceneAnimationTrack& track = clip.tracks[ensureTrack(property)];
		for (SceneAnimationKeyframe& keyframe : track.keyframes) {
			if (std::abs(keyframe.time - time) <= kKeyTimeTolerance) {
				keyframe.value = value;
				return;
			}
		}
		track.keyframes.push_back({ time, value });
		std::stable_sort(
			track.keyframes.begin(),
			track.keyframes.end(),
			[](const SceneAnimationKeyframe& left,
				const SceneAnimationKeyframe& right) {
				return left.time < right.time;
			}
		);
	};
	const auto sortTrack = [&](int trackIndex) {
		if (trackIndex < 0) {
			return;
		}
		std::stable_sort(
			clip.tracks[trackIndex].keyframes.begin(),
			clip.tracks[trackIndex].keyframes.end(),
			[](const SceneAnimationKeyframe& left,
				const SceneAnimationKeyframe& right) {
				return left.time < right.time;
			}
		);
	};
	const auto activatePreviewAt = [&](float time) {
		prefabAnimationPreviewTime_ = std::clamp(time, 0.0f, duration);
		prefabAnimationPreviewPlaying_ = false;
		prefabAnimationPreviewActive_ = true;
	};

	if (!DrawPersistentInspectorHeader(
		"prefab/" + prefabEditorSession_->GetFilePath() + "/" +
			std::to_string(entity.id) + "/TransformPoses",
		"Transform Poses###PrefabTransformPosesSection"
	)) {
		return;
	}
	ImGui::TextDisabled("Model Forward: -Z");
	ImGui::SameLine();
	ImGui::TextDisabled("%s / %s", owner->name.c_str(), clip.name.c_str());
	prefabTransformPoseAddTime_ = std::clamp(
		prefabTransformPoseAddTime_,
		0.0f,
		duration
	);
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::DragFloat(
		"New Pose Time",
		&prefabTransformPoseAddTime_,
		0.01f,
		0.0f,
		duration,
		"%.3f s"
	)) {
		prefabTransformPoseStatus_.clear();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Use Preview Time")) {
		prefabTransformPoseAddTime_ = std::clamp(
			prefabAnimationPreviewTime_,
			0.0f,
			duration
		);
		prefabTransformPoseStatus_.clear();
	}
	if (ImGui::Button("Add Pose")) {
		const float time = std::clamp(
			prefabTransformPoseAddTime_,
			0.0f,
			duration
		);
		const TransformPose* existingPose = nullptr;
		for (const TransformPose& pose : poses) {
			if (std::abs(pose.time - time) <= kKeyTimeTolerance) {
				existingPose = &pose;
				break;
			}
		}
		if (existingPose) {
			char message[160]{};
			std::snprintf(
				message,
				sizeof(message),
				"A Pose already exists at %.3f s. Choose another time or use Complete Pose.",
				existingPose->time
			);
			prefabTransformPoseStatus_ = message;
			activatePreviewAt(existingPose->time);
		} else {
			TransformPose newPose{};
			newPose.time = time;
			for (int property = 0; property < 3; ++property) {
				upsertKey(property, time, resolvePoseValue(newPose, property));
			}
			document.MarkDirty();
			activatePreviewAt(time);
			char message[96]{};
			std::snprintf(
				message,
				sizeof(message),
				"Added Transform Pose at %.3f s.",
				time
			);
			prefabTransformPoseStatus_ = message;
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Adds Position, Rotation, and Scale at New Pose Time.");
	if (!prefabTransformPoseStatus_.empty()) {
		ImGui::TextWrapped("%s", prefabTransformPoseStatus_.c_str());
	}
	std::string interpolationPreview;
	bool hasTransformTrack = false;
	bool mixedInterpolation = false;
	for (const SceneAnimationTrack& track : clip.tracks) {
		if (
			propertyIndex(track.property) < 0 ||
			resolveTrackTargetId(track) != entity.id
		) {
			continue;
		}
		const std::string easing = track.easing.empty()
			? "SmoothStep"
			: track.easing;
		if (!hasTransformTrack) {
			interpolationPreview = easing;
			hasTransformTrack = true;
		} else if (interpolationPreview != easing) {
			mixedInterpolation = true;
		}
	}
	if (mixedInterpolation) {
		interpolationPreview = "Mixed";
	}
	ImGui::BeginDisabled(!hasTransformTrack);
	if (ImGui::BeginCombo(
		"Default Easing (Transform Tracks)",
		interpolationPreview.empty() ? "SmoothStep" : interpolationPreview.c_str()
	)) {
		for (const char* easing : {
			"Linear", "EaseIn", "EaseOut", "EaseInOut", "SmoothStep"
		}) {
			if (ImGui::Selectable(easing, interpolationPreview == easing)) {
				for (SceneAnimationTrack& track : clip.tracks) {
					if (
						propertyIndex(track.property) >= 0 &&
						resolveTrackTargetId(track) == entity.id
					) {
						track.easing = easing;
					}
				}
				document.MarkDirty();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();

	if (poses.empty()) {
		ImGui::TextDisabled("No Transform Pose keys for the selected Entity.");
		return;
	}

	for (size_t poseIndex = 0; poseIndex < poses.size(); ++poseIndex) {
		const TransformPose& pose = poses[poseIndex];
		const bool complete =
			pose.keys[0].trackIndex >= 0 &&
			pose.keys[1].trackIndex >= 0 &&
			pose.keys[2].trackIndex >= 0;
		const bool hasNextPose = poseIndex + 1 < poses.size();
		const bool nextComplete = hasNextPose &&
			poses[poseIndex + 1].keys[0].trackIndex >= 0 &&
			poses[poseIndex + 1].keys[1].trackIndex >= 0 &&
			poses[poseIndex + 1].keys[2].trackIndex >= 0;
		ImGui::PushID(static_cast<int>(poseIndex));
		const char* state = complete ? "Transform Pose" : "Partial Pose";
		if (ImGui::TreeNodeEx(
			"Pose",
			ImGuiTreeNodeFlags_DefaultOpen,
			"%s  %.3f s",
			state,
			pose.time
		)) {
			float editedTime = pose.time;
			if (ImGui::DragFloat(
				"Time", &editedTime, 0.01f, 0.0f, duration, "%.3f s"
			)) {
				editedTime = std::clamp(editedTime, 0.0f, duration);
				for (const PoseKeyRef& key : pose.keys) {
					if (key.trackIndex >= 0 && key.keyIndex >= 0) {
						clip.tracks[key.trackIndex].keyframes[key.keyIndex].time = editedTime;
						sortTrack(key.trackIndex);
					}
				}
				document.MarkDirty();
				prefabAnimationPreviewTime_ = editedTime;
				prefabAnimationPreviewPlaying_ = false;
				prefabAnimationPreviewActive_ = true;
			}

			Vector3 position = resolvePoseValue(pose, 0);
			Vector3 rotationDegrees = resolvePoseValue(pose, 1);
			rotationDegrees.x *= kRadiansToDegrees;
			rotationDegrees.y *= kRadiansToDegrees;
			rotationDegrees.z *= kRadiansToDegrees;
			Vector3 scale = resolvePoseValue(pose, 2);
			if (ImGui::DragFloat3("Position", &position.x, 0.01f)) {
				upsertKey(0, pose.time, position);
				document.MarkDirty();
				activatePreviewAt(pose.time);
			}
			if (ImGui::DragFloat3("Rotation (Degrees)", &rotationDegrees.x, 0.1f)) {
				rotationDegrees.x *= kDegreesToRadians;
				rotationDegrees.y *= kDegreesToRadians;
				rotationDegrees.z *= kDegreesToRadians;
				upsertKey(1, pose.time, rotationDegrees);
				document.MarkDirty();
				activatePreviewAt(pose.time);
			}
			if (ImGui::DragFloat3("Scale", &scale.x, 0.01f)) {
				upsertKey(2, pose.time, scale);
				document.MarkDirty();
				activatePreviewAt(pose.time);
			}

			if (hasNextPose) {
				const TransformPose& nextPose = poses[poseIndex + 1];
				ImGui::SeparatorText("To Next Pose");
				ImGui::TextDisabled(
					"%.3f s -> %.3f s",
					pose.time,
					nextPose.time
				);
				if (!complete || !nextComplete) {
					ImGui::TextDisabled(
						"Complete both Poses to edit this interval."
					);
				} else {
					std::string segmentEasing;
					bool segmentEasingInitialized = false;
					bool mixedSegmentEasing = false;
					for (const PoseKeyRef& key : pose.keys) {
						const std::string& easing = clip.tracks[key.trackIndex]
							.keyframes[key.keyIndex].easingToNext;
						if (!segmentEasingInitialized) {
							segmentEasing = easing;
							segmentEasingInitialized = true;
						} else if (segmentEasing != easing) {
							mixedSegmentEasing = true;
						}
					}
					const char* segmentEasingPreview = mixedSegmentEasing
						? "Mixed"
						: segmentEasing.empty()
							? "Track Default"
							: segmentEasing.c_str();
					if (ImGui::BeginCombo(
						"Easing To Next",
						segmentEasingPreview
					)) {
						if (ImGui::Selectable(
							"Track Default",
							!mixedSegmentEasing && segmentEasing.empty()
						)) {
							for (const PoseKeyRef& key : pose.keys) {
								clip.tracks[key.trackIndex]
									.keyframes[key.keyIndex].easingToNext.clear();
							}
							document.MarkDirty();
							activatePreviewAt((pose.time + nextPose.time) * 0.5f);
						}
						for (const char* easing : {
							"Linear", "EaseIn", "EaseOut", "EaseInOut", "SmoothStep"
						}) {
							if (ImGui::Selectable(
								easing,
								!mixedSegmentEasing && segmentEasing == easing
							)) {
								for (const PoseKeyRef& key : pose.keys) {
									clip.tracks[key.trackIndex]
										.keyframes[key.keyIndex].easingToNext = easing;
								}
								document.MarkDirty();
								activatePreviewAt((pose.time + nextPose.time) * 0.5f);
							}
						}
						ImGui::EndCombo();
					}

					SceneAnimationKeyframe& positionStartKey =
						clip.tracks[pose.keys[0].trackIndex]
							.keyframes[pose.keys[0].keyIndex];
					ImGui::SetNextItemWidth(220.0f);
					if (ImGui::DragFloat3(
						"Position Bulge Offset",
						&positionStartKey.positionBulge.x,
						0.01f
					)) {
						document.MarkDirty();
						activatePreviewAt((pose.time + nextPose.time) * 0.5f);
					}
					if (ImGui::SmallButton("Reset Bulge")) {
						positionStartKey.positionBulge = {};
						document.MarkDirty();
						activatePreviewAt((pose.time + nextPose.time) * 0.5f);
					}
					ImGui::TextDisabled(
						"Local offset from the straight path at the interpolation midpoint."
		);
	}
			}

			if (!complete && ImGui::SmallButton("Complete Pose")) {
				for (int property = 0; property < 3; ++property) {
					if (pose.keys[property].trackIndex < 0) {
						upsertKey(
							property,
							pose.time,
							resolvePoseValue(pose, property)
						);
					}
				}
				document.MarkDirty();
				activatePreviewAt(pose.time);
			}
			if (!complete) {
				ImGui::SameLine();
				ImGui::TextDisabled("Missing Transform keys are kept unchanged.");
			}

			if (ImGui::SmallButton("Edit with Gizmo")) {
				prefabAnimationPreviewTime_ = pose.time;
				prefabAnimationPreviewPlaying_ = false;
				prefabAnimationPreviewActive_ = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Duplicate")) {
				float duplicateTime = (std::min)(pose.time + 0.1f, duration);
				if (std::abs(duplicateTime - pose.time) <= kKeyTimeTolerance) {
					duplicateTime = (std::max)(pose.time - 0.1f, 0.0f);
				}
				if (std::abs(duplicateTime - pose.time) > kKeyTimeTolerance) {
					for (int property = 0; property < 3; ++property) {
						upsertKey(
							property,
							duplicateTime,
							resolvePoseValue(pose, property)
						);
					}
					document.MarkDirty();
					prefabAnimationPreviewTime_ = duplicateTime;
					prefabAnimationPreviewPlaying_ = false;
					prefabAnimationPreviewActive_ = true;
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Delete")) {
				for (int property = 0; property < 3; ++property) {
					const PoseKeyRef& key = pose.keys[property];
					if (key.trackIndex >= 0 && key.keyIndex >= 0) {
						std::vector<SceneAnimationKeyframe>& keys =
							clip.tracks[key.trackIndex].keyframes;
						keys.erase(keys.begin() + key.keyIndex);
					}
				}
				document.MarkDirty();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	ImGui::TextDisabled(
		"Track Default is used when an interval has no Easing To Next override."
	);
}

bool ImGuiManager::WritePrefabAnimationGizmoKey(
	uint64_t entityId,
	const std::string& property,
	const Vector3& value
) {
	if (
		!prefabAnimationPreviewActive_ ||
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen()
	) {
		return false;
	}

	SceneDocument& document = prefabEditorSession_->GetDocument();
	SceneEntity* targetEntity = document.FindEntity(entityId);
	SceneEntity* owner = document.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	SceneComponent* animator = owner
		? FindComponent(*owner, "PrefabAnimator")
		: nullptr;
	if (
		!targetEntity ||
		!animator ||
		!animator->enabled ||
		prefabAnimationPreviewClipIndex_ < 0 ||
		prefabAnimationPreviewClipIndex_ >=
			static_cast<int>(animator->prefabAnimationClips.size())
	) {
		return false;
	}

	ScenePrefabAnimationClip& clip =
		animator->prefabAnimationClips[prefabAnimationPreviewClipIndex_];
	auto resolveTrackTargetId = [&](const SceneAnimationTrack& track) {
		if (track.targetEntityId != 0) {
			if (const SceneEntity* byId =
				document.FindEntity(track.targetEntityId)) {
				return byId->id;
			}
		}
		if (!track.targetEntityName.empty()) {
			if (const SceneEntity* byName =
				document.FindEntityByName(track.targetEntityName)) {
				return byName->id;
			}
		}
		return owner->id;
	};

	SceneAnimationTrack* destinationTrack = nullptr;
	for (SceneAnimationTrack& track : clip.tracks) {
		if (
			track.property == property &&
			resolveTrackTargetId(track) == entityId
		) {
			destinationTrack = &track;
			break;
		}
	}
	if (!destinationTrack) {
		SceneAnimationTrack track{};
		track.targetEntityId = targetEntity->id;
		track.targetEntityName = targetEntity->name;
		track.property = property;
		clip.tracks.push_back(std::move(track));
		destinationTrack = &clip.tracks.back();
	}

	const float keyTime = std::clamp(
		prefabAnimationPreviewTime_,
		0.0f,
		(std::max)(clip.duration, 0.0f)
	);
	constexpr float kKeyTimeTolerance = 0.005f;
	for (SceneAnimationKeyframe& keyframe : destinationTrack->keyframes) {
		if (std::abs(keyframe.time - keyTime) > kKeyTimeTolerance) {
			continue;
		}
		prefabAnimationPreviewTime_ = keyframe.time;
		keyframe.value = value;
		document.MarkDirty();
		return true;
	}

	destinationTrack->keyframes.push_back({ keyTime, value });
	std::stable_sort(
		destinationTrack->keyframes.begin(),
		destinationTrack->keyframes.end(),
		[](const SceneAnimationKeyframe& left,
			const SceneAnimationKeyframe& right) {
			return left.time < right.time;
		}
	);
	document.MarkDirty();
	return true;
}

void ImGuiManager::DrawPrefabGizmo(
	float x,
	float y,
	float width,
	float height
) {
	if (
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen() ||
		!prefabPreviewCameraValid_ ||
		prefabSelectedEntityId_ == 0 ||
		width <= 1.0f ||
		height <= 1.0f
	) {
		return;
	}

	SceneDocument& sourceDocument = prefabEditorSession_->GetDocument();
	const SceneDocument& stageDocument = GetPrefabStageDocument();
	SceneEntity* sourceEntity = sourceDocument.FindEntity(prefabSelectedEntityId_);
	const SceneEntity* stageEntity = stageDocument.FindEntity(
		prefabSelectedEntityId_
	);
	if (!sourceEntity || !stageEntity || sourceEntity->locked) {
		return;
	}
	if (
		prefabHitBoxSetupMode_ &&
		!FindEnabledComponent(*sourceEntity, "OBBCollider")
	) {
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(x + 8.0f, y + 8.0f),
			IM_COL32(255, 190, 80, 255),
			"HitBox Setup: select an Entity with a Collider."
		);
		return;
	}

	const SceneEntity* parentEntity = stageDocument.FindEntity(
		stageEntity->parentId
	);
	const Matrix4x4 parentWorld = parentEntity
		? ResolveSceneWorldMatrix(stageDocument, *parentEntity)
		: MakeIdentity4x4();
	Matrix4x4 worldMatrix = ResolveSceneWorldMatrix(
		stageDocument,
		*stageEntity
	);
	const ImGuizmo::OPERATION operation = gizmoOperation_ == 0
		? ImGuizmo::TRANSLATE
		: gizmoOperation_ == 1
			? ImGuizmo::ROTATE
			: ImGuizmo::SCALE;
	const ImGuizmo::MODE mode = gizmoLocalMode_
		? ImGuizmo::LOCAL
		: ImGuizmo::WORLD;
	if (
		!gizmoLocalMode_ &&
		gizmoOperation_ != 0 &&
		parentEntity &&
		HasNonUniformScale(parentWorld)
	) {
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(x + 8.0f, y + 8.0f),
			IM_COL32(255, 190, 80, 255),
			"World Rotate/Scale requires a uniformly scaled parent."
		);
		return;
	}

	const float snapValue = gizmoOperation_ == 0
		? gizmoTranslationSnap_
		: gizmoOperation_ == 1
			? gizmoRotationSnapDegrees_
			: gizmoScaleSnap_;
	const float snap[3] = { snapValue, snapValue, snapValue };
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(x, y, width, height);
	const bool changed = ImGuizmo::Manipulate(
		&prefabPreviewViewMatrix_.m[0][0],
		&prefabPreviewProjectionMatrix_.m[0][0],
		operation,
		mode,
		&worldMatrix.m[0][0],
		nullptr,
		gizmoSnapEnabled_ ? snap : nullptr
	);
	if (!changed || !ImGuizmo::IsUsing()) {
		return;
	}

	Matrix4x4 localMatrix = worldMatrix;
	if (parentEntity) {
		localMatrix = Multiply(worldMatrix, Inverse(parentWorld));
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
		return;
	}
	if (prefabAnimationPreviewActive_) {
		// Preview Documentは一時表示専用。操作値はSource Clipの現在時刻へ
		// 書き戻し、次のPreview再構築で表示へ反映する。
		prefabAnimationPreviewPlaying_ = false;
		const std::string property = gizmoOperation_ == 0
			? "LocalPosition"
			: gizmoOperation_ == 1
				? "LocalRotation"
				: "LocalScale";
		const Vector3 value = gizmoOperation_ == 0
			? localTranslate
			: gizmoOperation_ == 1
				? MakeEulerFromQuaternion(localRotate)
				: localScale;
		WritePrefabAnimationGizmoKey(
			sourceEntity->id,
			property,
			value
		);
		return;
	}
	if (gizmoOperation_ == 0) {
		sourceEntity->transform.translate = localTranslate;
	} else if (gizmoOperation_ == 1) {
		sourceEntity->transform.rotate = localRotate;
	} else {
		sourceEntity->transform.scale = localScale;
	}
	sourceDocument.MarkDirty();
}

bool ImGuiManager::PickPrefabEntity(
	float x,
	float y,
	float width,
	float height
) {
	if (
		!prefabEditorSession_ ||
		!prefabEditorSession_->IsOpen() ||
		!prefabPreviewCameraValid_ ||
		width <= 1.0f ||
		height <= 1.0f
	) {
		return false;
	}

	const ImVec2 mouse = ImGui::GetMousePos();
	const float normalizedX = (mouse.x - x) / width;
	const float normalizedY = (mouse.y - y) / height;
	if (
		normalizedX < 0.0f ||
		normalizedX > 1.0f ||
		normalizedY < 0.0f ||
		normalizedY > 1.0f
	) {
		return false;
	}

	const float ndcX = normalizedX * 2.0f - 1.0f;
	const float ndcY = 1.0f - normalizedY * 2.0f;
	const Matrix4x4 inverseViewProjection = Inverse(Multiply(
		prefabPreviewViewMatrix_,
		prefabPreviewProjectionMatrix_
	));
	const Vector3 nearPoint = TransformCoord(
		{ ndcX, ndcY, 0.0f },
		inverseViewProjection
	);
	const Vector3 farPoint = TransformCoord(
		{ ndcX, ndcY, 1.0f },
		inverseViewProjection
	);

	const SceneDocument& document = GetPrefabStageDocument();
	uint64_t bestEntityId = 0;
	float bestDistance = (std::numeric_limits<float>::max)();
	const Vector3 worldRayDirection = Math::Normalize(
		Math::Subtract(farPoint, nearPoint)
	);
	const auto considerHit = [&](uint64_t entityId, float worldDistance) {
		if (worldDistance < bestDistance) {
			bestDistance = worldDistance;
			bestEntityId = entityId;
		}
	};
	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.locked) {
			continue;
		}

		if (IsEntityActiveInHierarchy(document, entity)) {
			const SceneComponent* meshRenderer =
				FindEnabledComponent(entity, "MeshRenderer");
			if (meshRenderer && !meshRenderer->modelPath.empty()) {
				ModelManager::GetInstance()->LoadModel(meshRenderer->modelPath);
				Model* model = ModelManager::GetInstance()->FindModel(
					meshRenderer->modelPath
				);
				Vector3 localMin{};
				Vector3 localMax{};
				if (model && model->GetLocalBounds(localMin, localMax)) {
					Matrix4x4 modelWorld =
						ResolveSceneWorldMatrix(document, entity);
					if (!model->HasSkinning()) {
						modelWorld = Multiply(
							model->GetRootNodeLocalMatrix(),
							modelWorld
						);
					}
					const Matrix4x4 inverseWorld = Inverse(modelWorld);
					const Vector3 localRayOrigin =
						TransformCoord(nearPoint, inverseWorld);
					const Vector3 localRayFar =
						TransformCoord(farPoint, inverseWorld);
					const Vector3 localRayDirection = Math::Normalize(
						Math::Subtract(localRayFar, localRayOrigin)
					);
					float localDistance = 0.0f;
					if (IntersectRayAabb(
						localRayOrigin,
						localRayDirection,
						localMin,
						localMax,
						localDistance
					)) {
						const Vector3 localHit = Math::Add(
							localRayOrigin,
							Math::Multiply(localRayDirection, localDistance)
						);
						const Vector3 worldHit =
							TransformCoord(localHit, modelWorld);
						considerHit(
							entity.id,
							Math::Length(Math::Subtract(worldHit, nearPoint))
						);
					}
				}
			}
		}

		const SceneComponent* colliderComponent =
			FindEnabledComponent(entity, "OBBCollider");
		if (!colliderComponent) {
			continue;
		}
		const bool isCombatVolume =
			FindEnabledComponent(entity, "HitBox") != nullptr ||
			FindEnabledComponent(entity, "HurtBox") != nullptr;
		const bool colliderVisible = isCombatVolume
			? prefabPreviewShowCombatVolumes_ || prefabPreviewShowColliders_
			: prefabPreviewShowColliders_;
		if (!colliderVisible) {
			continue;
		}

		const Matrix4x4 colliderWorld =
			ResolveSceneWorldMatrix(document, entity);
		if (colliderComponent->colliderShape == "Sphere") {
			const Vector3 center = TransformCoord(
				colliderComponent->colliderOffset,
				colliderWorld
			);
			const float radius =
				(std::max)(colliderComponent->colliderSphereRadius, 0.001f) *
				GetMaxWorldAxisScale(colliderWorld);
			float worldDistance = 0.0f;
			if (IntersectRaySphere(
				nearPoint,
				worldRayDirection,
				center,
				radius,
				worldDistance
			)) {
				considerHit(entity.id, worldDistance);
			}
			continue;
		}

		const Matrix4x4 inverseWorld = Inverse(colliderWorld);
		const Vector3 localRayOrigin = TransformCoord(nearPoint, inverseWorld);
		const Vector3 localRayFar = TransformCoord(farPoint, inverseWorld);
		const Vector3 localRayDirection = Math::Normalize(
			Math::Subtract(localRayFar, localRayOrigin)
		);
		const Vector3 halfSize{
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.x), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.y), 0.001f),
			(std::max)(std::abs(colliderComponent->colliderSizeMultiplier.z), 0.001f)
		};
		const Vector3 localMin = Math::Subtract(
			colliderComponent->colliderOffset,
			halfSize
		);
		const Vector3 localMax = Math::Add(
			colliderComponent->colliderOffset,
			halfSize
		);
		float localDistance = 0.0f;
		if (IntersectRayAabb(
			localRayOrigin,
			localRayDirection,
			localMin,
			localMax,
			localDistance
		)) {
			const Vector3 localHit = Math::Add(
				localRayOrigin,
				Math::Multiply(localRayDirection, localDistance)
			);
			const Vector3 worldHit = TransformCoord(localHit, colliderWorld);
			considerHit(
				entity.id,
				Math::Length(Math::Subtract(worldHit, nearPoint))
			);
		}
	}

	prefabSelectedEntityId_ = bestEntityId;
	return bestEntityId != 0;
}

void ImGuiManager::DrawPrefabHierarchy() {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}
	SceneDocument& document = prefabEditorSession_->GetDocument();
	static std::string nestedPrefabStatus;
	std::string nestedPrefabDropPath;
	uint64_t nestedPrefabDropParentId = 0;
	std::string nestedPrefabOpenPath;
	const uint64_t selectedParentId = document.FindEntity(prefabSelectedEntityId_)
		? prefabSelectedEntityId_
		: 0;
	const bool hasRoot = std::any_of(
		document.GetEntities().begin(),
		document.GetEntities().end(),
		[](const SceneEntity& entity) { return entity.parentId == 0; }
	);
	ImGui::BeginDisabled(hasRoot);
	if (ImGui::Button("Create Root")) {
		SceneEntity& entity = document.CreateEntity("Entity");
		prefabSelectedEntityId_ = entity.id;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedParentId == 0);
	if (ImGui::Button("Create Child")) {
		SceneEntity& entity = document.CreateEntity("Entity", selectedParentId);
		prefabSelectedEntityId_ = entity.id;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedParentId == 0);
	if (ImGui::Button("Delete")) {
		document.RemoveEntity(selectedParentId);
		prefabSelectedEntityId_ = 0;
	}
	ImGui::EndDisabled();
	ImGui::Separator();
	if (!nestedPrefabStatus.empty()) {
		ImGui::TextWrapped("%s", nestedPrefabStatus.c_str());
	}

	std::function<void(uint64_t)> drawEntity;
	drawEntity = [&](uint64_t entityId) {
		const SceneEntity* entity = document.FindEntity(entityId);
		if (!entity) {
			return;
		}
		const bool hasChildren = std::any_of(
			document.GetEntities().begin(),
			document.GetEntities().end(),
			[entityId](const SceneEntity& candidate) {
				return candidate.parentId == entityId;
			}
		);
		ImGuiTreeNodeFlags flags =
			ImGuiTreeNodeFlags_OpenOnArrow |
			ImGuiTreeNodeFlags_SpanAvailWidth;
		if (!hasChildren) {
			flags |= ImGuiTreeNodeFlags_Leaf |
				ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}
		if (prefabSelectedEntityId_ == entityId) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		const char* prefabLabel = entity->prefabLinks.size() > 1
			? "[Nested] "
			: entity->prefabInstanceRootId != 0
				? "[Prefab] "
				: "";
		ImGui::PushID(static_cast<int>(entityId));
		const bool open = ImGui::TreeNodeEx(
			"##PrefabEntity",
			flags,
			"%s%s%s",
			entity->active ? "" : "(inactive) ",
			prefabLabel,
			entity->name.c_str()
		);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
			prefabSelectedEntityId_ = entityId;
		}
		if (ImGui::IsItemHovered() &&
			ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (!entity->prefabLinks.empty()) {
				// The last Link is the most specific Nested Prefab source.
				const ScenePrefabLink& source = entity->prefabLinks.back();
				nestedPrefabOpenPath = PrefabAssetRegistry::ResolvePath(
					source.assetId,
					source.sourcePath
				);
			}
		}
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload =
				ImGui::AcceptDragDropPayload("PROJECT_PREFAB_PATH")) {
				const char* droppedPath =
					static_cast<const char*>(payload->Data);
				if (droppedPath && droppedPath[0] != '\0') {
					nestedPrefabDropPath = droppedPath;
					nestedPrefabDropParentId = entityId;
				}
			}
			ImGui::EndDragDropTarget();
		}
		if (hasChildren && open) {
			for (const SceneEntity& child : document.GetEntities()) {
				if (child.parentId == entityId) {
					drawEntity(child.id);
				}
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	};

	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.parentId == 0) {
			drawEntity(entity.id);
		}
	}
	if (!nestedPrefabDropPath.empty()) {
		const uint64_t instanceId = document.InstantiatePrefab(
			nestedPrefabDropPath,
			nestedPrefabDropParentId
		);
		if (instanceId != 0) {
			prefabSelectedEntityId_ = instanceId;
			nestedPrefabStatus = "Nested Prefab added: " +
				nestedPrefabDropPath;
		} else {
			nestedPrefabStatus =
				"Failed to add Nested Prefab (missing asset or cycle): " +
				nestedPrefabDropPath;
		}
	}
	if (!nestedPrefabOpenPath.empty()) {
		RequestOpenPrefab(nestedPrefabOpenPath);
	}
}

void ImGuiManager::DrawPrefabInspector() {
	if (!prefabEditorSession_ || !prefabEditorSession_->IsOpen()) {
		return;
	}
	SceneDocument& document = prefabEditorSession_->GetDocument();
	SceneEntity* entity = document.FindEntity(prefabSelectedEntityId_);
	if (!entity) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"PrefabのEntityを選択してください。",
			"Select a Prefab Entity."
		));
		return;
	}
	const SceneEntity* clipFocusOwner = document.FindEntity(
		prefabAnimationPreviewOwnerEntityId_
	);
	const SceneComponent* clipFocusAnimator = clipFocusOwner
		? FindEnabledComponent(*clipFocusOwner, "PrefabAnimator")
		: nullptr;
	const bool clipFocusActive =
		prefabClipFocusEnabled_ &&
		clipFocusOwner == entity &&
		clipFocusAnimator &&
		prefabAnimationPreviewClipIndex_ >= 0 &&
		prefabAnimationPreviewClipIndex_ < static_cast<int>(
			clipFocusAnimator->prefabAnimationClips.size()
		);
	const std::string* clipFocusName = clipFocusActive
		? &clipFocusAnimator->prefabAnimationClips[
			prefabAnimationPreviewClipIndex_
		].name
		: nullptr;

	if (document.IsPrefabVariant()) {
		static std::string variantStatusDocumentPath;
		static std::string variantOverrideStatus;
		if (variantStatusDocumentPath != prefabEditorSession_->GetFilePath()) {
			variantStatusDocumentPath = prefabEditorSession_->GetFilePath();
			variantOverrideStatus.clear();
		}
		const std::string basePath = PrefabAssetRegistry::ResolvePath(
			document.GetVariantBaseAssetId(),
			document.GetVariantBasePath()
		);
		std::vector<ScenePrefabPropertyOverride> variantOverrides =
			document.CollectPrefabVariantOverrides();
		int applyVariantOverrideIndex = -1;
		int revertVariantOverrideIndex = -1;
		ImGui::SeparatorText("Variant Overrides");
		ImGui::TextDisabled(
			"Base: %s",
			basePath.empty() ? "Missing or ambiguous" : basePath.c_str()
		);
		ImGui::PushID("PrefabVariantOverrides");
		if (ImGui::TreeNodeEx(
			"Overrides",
			ImGuiTreeNodeFlags_DefaultOpen
		)) {
			for (size_t index = 0; index < variantOverrides.size(); ++index) {
				const ScenePrefabPropertyOverride& overrideValue =
					variantOverrides[index];
				const uint64_t targetEntityId =
					overrideValue.instanceEntityId != 0
						? overrideValue.instanceEntityId
						: overrideValue.entityLocalId;
				const SceneEntity* targetEntity =
					document.FindEntity(targetEntityId);
				const bool canModify =
					!basePath.empty() &&
					(!targetEntity || !targetEntity->locked);
				ImGui::PushID(static_cast<int>(index));
				ImGui::BulletText("%s", overrideValue.label.c_str());
				ImGui::Indent();
				ImGui::BeginDisabled(!canModify);
				if (ImGui::SmallButton("Apply to Base")) {
					applyVariantOverrideIndex = static_cast<int>(index);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Revert")) {
					revertVariantOverrideIndex = static_cast<int>(index);
				}
				ImGui::EndDisabled();
				ImGui::Unindent();
				ImGui::PopID();
			}
			if (variantOverrides.empty()) {
				ImGui::TextDisabled("No Entity or Component overrides.");
			}
			ImGui::TreePop();
		}
		ImGui::PopID();

		if (applyVariantOverrideIndex >= 0) {
			const ScenePrefabPropertyOverride overrideValue =
				variantOverrides[applyVariantOverrideIndex];
			if (document.ApplyPrefabVariantOverrideToBase(overrideValue)) {
				variantOverrideStatus = "Applied to Base: " +
					overrideValue.label;
				InvalidateProjectCache();
			} else {
				variantOverrideStatus = "Failed to apply to Base: " +
					overrideValue.label;
			}
		}
		if (revertVariantOverrideIndex >= 0) {
			const ScenePrefabPropertyOverride overrideValue =
				variantOverrides[revertVariantOverrideIndex];
			const uint64_t selectedId = entity->id;
			const bool removesSelectedBranch =
				overrideValue.kind == ScenePrefabOverrideKind::AddedEntity &&
				(
					selectedId == overrideValue.instanceEntityId ||
					document.IsDescendantOf(
						selectedId,
						overrideValue.instanceEntityId
					)
				);
			if (document.RevertPrefabVariantOverride(overrideValue)) {
				variantOverrideStatus = "Reverted: " + overrideValue.label;
				if (removesSelectedBranch ||
					!document.FindEntity(selectedId)) {
					const auto root = std::find_if(
						document.GetEntities().begin(),
						document.GetEntities().end(),
						[](const SceneEntity& candidate) {
							return candidate.parentId == 0;
						}
					);
					prefabSelectedEntityId_ =
						root == document.GetEntities().end() ? 0 : root->id;
				}
				return;
			}
			variantOverrideStatus = "Failed to revert: " +
				overrideValue.label;
		}
		if (!variantOverrideStatus.empty()) {
			ImGui::TextWrapped("%s", variantOverrideStatus.c_str());
		}
	}

	if (!entity->prefabLinks.empty()) {
		std::string nestedSourceOpenPath;
		ImGui::SeparatorText("Prefab Sources");
		for (size_t linkIndex = 0;
			linkIndex < entity->prefabLinks.size();
			++linkIndex) {
			const ScenePrefabLink& link = entity->prefabLinks[linkIndex];
			const std::string sourcePath = PrefabAssetRegistry::ResolvePath(
				link.assetId,
				link.sourcePath
			);
			const std::string displayPath = sourcePath.empty()
				? link.sourcePath
				: sourcePath;
			const std::string displayName = displayPath.empty()
				? "Missing Prefab"
				: PathToUtf8(PathFromUtf8(displayPath).filename());
			ImGui::PushID(static_cast<int>(linkIndex));
			ImGui::Text(
				"%zu. %s%s",
				linkIndex + 1,
				link.instanceRootId == entity->id ? "[Root] " : "",
				displayName.c_str()
			);
			ImGui::Indent();
			ImGui::TextDisabled(
				"Instance Root: %llu / Local Entity: %llu",
				static_cast<unsigned long long>(link.instanceRootId),
				static_cast<unsigned long long>(link.localId)
			);
			ImGui::BeginDisabled(sourcePath.empty());
			if (ImGui::SmallButton("Open")) {
				nestedSourceOpenPath = sourcePath;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Select Asset")) {
				SelectPrefabAssetInProject(sourcePath);
			}
			ImGui::EndDisabled();
			if (sourcePath.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(
					ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
					"Missing or ambiguous asset"
				);
			}
			ImGui::Unindent();
			ImGui::PopID();
		}
		if (!nestedSourceOpenPath.empty()) {
			RequestOpenPrefab(nestedSourceOpenPath);
			return;
		}
	}

	const std::vector<uint64_t> nestedInstanceRoots =
		document.CollectPrefabInstanceRoots(entity->id);
	if (
		prefabNestedTargetDocumentPath_ !=
			prefabEditorSession_->GetFilePath() ||
		std::find(
			nestedInstanceRoots.begin(),
			nestedInstanceRoots.end(),
			prefabNestedTargetRootId_
		) == nestedInstanceRoots.end()
	) {
		prefabNestedTargetDocumentPath_ =
			prefabEditorSession_->GetFilePath();
		prefabNestedTargetRootId_ = nestedInstanceRoots.empty()
			? 0
			: nestedInstanceRoots.front();
	}
	const uint64_t nestedInstanceRootId = prefabNestedTargetRootId_;
	if (nestedInstanceRootId != 0) {
		auto findInstanceLink = [](
			const SceneEntity* root,
			uint64_t rootId
		) -> const ScenePrefabLink* {
			if (!root) {
				return nullptr;
			}
			const auto found = std::find_if(
				root->prefabLinks.begin(),
				root->prefabLinks.end(),
				[rootId](const ScenePrefabLink& link) {
					return link.instanceRootId == rootId;
				}
			);
			return found == root->prefabLinks.end() ? nullptr : &(*found);
		};
		const SceneEntity* nestedInstanceRoot =
			document.FindEntity(nestedInstanceRootId);
		const ScenePrefabLink* nestedInstanceLink =
			findInstanceLink(nestedInstanceRoot, nestedInstanceRootId);
		const std::string nestedSourcePath = nestedInstanceLink
			? PrefabAssetRegistry::ResolvePath(
				nestedInstanceLink->assetId,
				nestedInstanceLink->sourcePath
			)
			: std::string{};
		static std::string nestedStatusDocumentPath;
		static uint64_t nestedStatusRootId = 0;
		static std::string nestedInstanceStatus;
		if (
			nestedStatusDocumentPath != prefabEditorSession_->GetFilePath() ||
			nestedStatusRootId != nestedInstanceRootId
		) {
			nestedStatusDocumentPath = prefabEditorSession_->GetFilePath();
			nestedStatusRootId = nestedInstanceRootId;
			nestedInstanceStatus.clear();
		}

		ImGui::SeparatorText("Nested Prefab Instance");
		if (nestedInstanceRoots.size() > 1) {
			const std::string currentTargetLabel = nestedSourcePath.empty()
				? "Missing Prefab"
				: PathToUtf8(PathFromUtf8(nestedSourcePath).filename());
			if (ImGui::BeginCombo("Apply Target", currentTargetLabel.c_str())) {
				for (uint64_t targetRootId : nestedInstanceRoots) {
					const SceneEntity* targetRoot =
						document.FindEntity(targetRootId);
					const ScenePrefabLink* targetLink =
						findInstanceLink(targetRoot, targetRootId);
					const std::string targetPath = targetLink
						? PrefabAssetRegistry::ResolvePath(
							targetLink->assetId,
							targetLink->sourcePath
						)
						: std::string{};
					const std::string targetName = targetPath.empty()
						? "Missing Prefab"
						: PathToUtf8(PathFromUtf8(targetPath).filename());
					const std::string targetLabel = targetName +
						" / Root " + std::to_string(targetRootId);
					const bool selected =
						targetRootId == nestedInstanceRootId;
					ImGui::PushID(static_cast<int>(targetRootId));
					if (ImGui::Selectable(targetLabel.c_str(), selected)) {
						prefabNestedTargetRootId_ = targetRootId;
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}
		ImGui::TextWrapped(
			"Target: %s",
			nestedSourcePath.empty()
				? "Missing or ambiguous asset"
				: nestedSourcePath.c_str()
		);
		ImGui::TextDisabled(
			"Instance Root: %llu",
			static_cast<unsigned long long>(nestedInstanceRootId)
		);

		std::vector<ScenePrefabPropertyOverride> nestedOverrides;
		int applyNestedOverrideIndex = -1;
		int revertNestedOverrideIndex = -1;
		ImGui::PushID("NestedPrefabInstance");
		if (ImGui::TreeNode("Overrides")) {
			const std::vector<std::string> overrideSummary =
				document.CollectPrefabInstanceOverrides(
					nestedInstanceRootId
				);
			nestedOverrides = document.CollectPrefabPropertyOverrides(
				nestedInstanceRootId
			);
			bool hasStatusMessage = false;
			for (const std::string& message : overrideSummary) {
				if (
					message.starts_with("Modified Entity:") ||
					message.starts_with("Added Entity:") ||
					message.starts_with("Removed Entity:") ||
					message.starts_with("Stale Entity:")
				) {
					continue;
				}
				hasStatusMessage = true;
				ImGui::BulletText("%s", message.c_str());
			}
			const bool canModifyNested =
				nestedInstanceRoot &&
				nestedInstanceLink &&
				!nestedInstanceRoot->locked &&
				!entity->locked &&
				!nestedSourcePath.empty();
			for (size_t index = 0; index < nestedOverrides.size(); ++index) {
				const ScenePrefabPropertyOverride& overrideValue =
					nestedOverrides[index];
				ImGui::PushID(static_cast<int>(index));
				ImGui::BulletText("%s", overrideValue.label.c_str());
				ImGui::Indent();
				ImGui::BeginDisabled(!canModifyNested);
				if (ImGui::SmallButton("Apply")) {
					applyNestedOverrideIndex = static_cast<int>(index);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Revert")) {
					revertNestedOverrideIndex = static_cast<int>(index);
				}
				ImGui::EndDisabled();
				ImGui::Unindent();
				ImGui::PopID();
			}
			if (nestedOverrides.empty() && !hasStatusMessage) {
				ImGui::TextDisabled("No overrides.");
			}
			ImGui::TreePop();
		}

		if (applyNestedOverrideIndex >= 0) {
			const ScenePrefabPropertyOverride overrideValue =
				nestedOverrides[applyNestedOverrideIndex];
			if (document.ApplyPrefabPropertyOverride(
				nestedInstanceRootId,
				overrideValue
			)) {
				nestedInstanceStatus = "Applied: " + overrideValue.label;
				InvalidateProjectCache();
				ImGui::PopID();
				return;
			}
			nestedInstanceStatus = "Failed to apply: " + overrideValue.label;
		}
		if (revertNestedOverrideIndex >= 0) {
			const ScenePrefabPropertyOverride overrideValue =
				nestedOverrides[revertNestedOverrideIndex];
			const uint64_t selectedId = entity->id;
			const bool removesSelectedBranch =
				(
					overrideValue.kind == ScenePrefabOverrideKind::AddedEntity ||
					overrideValue.kind == ScenePrefabOverrideKind::StaleEntity
				) &&
				(
					selectedId == overrideValue.instanceEntityId ||
					document.IsDescendantOf(
						selectedId,
						overrideValue.instanceEntityId
					)
				);
			if (document.RevertPrefabPropertyOverride(
				nestedInstanceRootId,
				overrideValue
			)) {
				nestedInstanceStatus = "Reverted: " + overrideValue.label;
				prefabSelectedEntityId_ = removesSelectedBranch
					? nestedInstanceRootId
					: selectedId;
				ImGui::PopID();
				return;
			}
			nestedInstanceStatus = "Failed to revert: " + overrideValue.label;
		}

		const bool canModifyNested =
			nestedInstanceRoot &&
			nestedInstanceLink &&
			!nestedInstanceRoot->locked &&
			!entity->locked &&
			!nestedSourcePath.empty();
		ImGui::BeginDisabled(!canModifyNested);
		if (ImGui::Button("Apply Instance")) {
			if (document.ApplyPrefabInstance(nestedInstanceRootId)) {
				nestedInstanceStatus =
					"Applied the Nested instance to its Prefab asset.";
				InvalidateProjectCache();
				ImGui::EndDisabled();
				ImGui::PopID();
				return;
			}
			nestedInstanceStatus = "Failed to apply the Nested instance.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Revert Instance")) {
			if (document.RevertPrefabInstance(nestedInstanceRootId)) {
				nestedInstanceStatus =
					"Reverted the Nested instance from its Prefab asset.";
				prefabSelectedEntityId_ = nestedInstanceRootId;
				ImGui::EndDisabled();
				ImGui::PopID();
				return;
			}
			nestedInstanceStatus = "Failed to revert the Nested instance.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Unpack")) {
			if (document.UnpackPrefabInstance(nestedInstanceRootId)) {
				nestedInstanceStatus = "Unpacked the Nested instance.";
			} else {
				nestedInstanceStatus = "Failed to unpack the Nested instance.";
			}
		}
		ImGui::EndDisabled();
		ImGui::PopID();
		if (!nestedInstanceStatus.empty()) {
			ImGui::TextWrapped("%s", nestedInstanceStatus.c_str());
		}
	}

	bool entityChanged = false;
	entityChanged |= InputTextString(SelectEditorText(
		editorLanguage_,
		"名前###PrefabEntityName",
		"Name###PrefabEntityName"
	), entity->name);
	entityChanged |= ImGui::Checkbox(SelectEditorText(
		editorLanguage_,
		"有効###PrefabEntityActive",
		"Active###PrefabEntityActive"
	), &entity->active);
	const std::string prefabInspectorKey = "prefab/" +
		prefabEditorSession_->GetFilePath() + "/" +
		std::to_string(entity->id) + "/";
	if (DrawPersistentInspectorHeader(
		prefabInspectorKey + "Transform",
		"Transform###PrefabTransformSection"
	)) {
	entityChanged |= ImGui::DragFloat3(
		SelectEditorText(editorLanguage_, "位置###PrefabTransformPosition", "Position###PrefabTransformPosition"),
		&entity->transform.translate.x,
		0.01f
	);
	Vector3 rotationEuler = MakeEulerFromQuaternion(entity->transform.rotate);
	if (ImGui::DragFloat3(
		SelectEditorText(editorLanguage_, "回転###PrefabTransformRotation", "Rotation###PrefabTransformRotation"),
		&rotationEuler.x,
		0.01f
	)) {
		entity->transform.rotate = MakeQuaternionFromEuler(rotationEuler);
		entityChanged = true;
	}
	entityChanged |= ImGui::DragFloat3(
		SelectEditorText(editorLanguage_, "スケール###PrefabTransformScale", "Scale###PrefabTransformScale"),
		&entity->transform.scale.x,
		0.01f
	);
	}
	if (entityChanged) {
		document.MarkDirty();
	}
	if (DrawPersistentInspectorHeader(
		prefabInspectorKey + "ComponentOverview",
		SelectEditorText(
			editorLanguage_,
			"Component概要###PrefabComponentOverviewSection",
			"Component Overview###PrefabComponentOverviewSection"
		)
	)) {
		DrawComponentSummary(
			*entity,
			prefabEditorSession_->GetFilePath(),
			clipFocusActive,
			prefabSummarySelectedComponentType_
		);
	}
	const bool simpleComponentInspector =
		componentInspectorMode_ == ComponentInspectorMode::Simple;
	DrawPrefabTransformPoseInspector(*entity);

	if (const SceneComponent* meshRenderer =
		FindEnabledComponent(*entity, "MeshRenderer")) {
		if (
			!meshRenderer->modelPath.empty() &&
			modelPreviewRenderedPath_ == meshRenderer->modelPath &&
			modelPreviewTexture_.ptr != 0
		) {
			if (DrawPersistentInspectorHeader(
				prefabInspectorKey + "Preview",
				"Preview###PrefabModelPreviewSection"
			)) {
			const float availableWidth = ImGui::GetContentRegionAvail().x;
			const float previewSize = std::clamp(availableWidth, 180.0f, 420.0f);
			ImGui::Image(
				ImTextureRef(static_cast<ImTextureID>(modelPreviewTexture_.ptr)),
				ImVec2(previewSize, previewSize)
			);
			if (ImGui::IsItemHovered()) {
				const ImGuiIO& io = ImGui::GetIO();
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
					modelPreviewYaw_ += io.MouseDelta.x * 0.01f;
					modelPreviewPitch_ = std::clamp(
						modelPreviewPitch_ + io.MouseDelta.y * 0.01f,
						-1.45f,
						1.45f
					);
				}
				if (io.MouseWheel != 0.0f) {
					modelPreviewZoom_ = std::clamp(
						modelPreviewZoom_ * (1.0f - io.MouseWheel * 0.1f),
						0.15f,
						8.0f
					);
				}
			}
			}
		}
	}

	int removeComponentIndex = -1;
	for (size_t componentIndex = 0;
		componentIndex < entity->components.size();
		++componentIndex) {
		SceneComponent& component = entity->components[componentIndex];
		if (
			clipFocusActive &&
			component.type != "AttackSet" &&
			component.type != "PrefabAnimator"
		) {
			continue;
		}
		if (
			simpleComponentInspector &&
			component.type != prefabSummarySelectedComponentType_
		) {
			continue;
		}
		ImGui::PushID(static_cast<int>(componentIndex));
		const std::string foldoutKey = MakeComponentFoldoutKey(
			prefabEditorSession_->GetFilePath(),
			entity->id,
			component.type
		);
		const auto savedFoldout = componentFoldoutStates_.find(foldoutKey);
		const bool wasComponentOpen = savedFoldout == componentFoldoutStates_.end()
			? true
			: savedFoldout->second;
		ImGui::SetNextItemOpen(wasComponentOpen, ImGuiCond_Always);
		const bool open = ImGui::CollapsingHeader(
			component.type.c_str(),
			ImGuiTreeNodeFlags_SpanAvailWidth
		);
		if (open != wasComponentOpen) {
			componentFoldoutStates_[foldoutKey] = open;
			SaveEditorSettings();
		}
		if (!open) {
			ImGui::PopID();
			continue;
		}
		bool changed = ImGui::Checkbox(SelectEditorText(
			editorLanguage_,
			"有効###PrefabComponentEnabled",
			"Enabled###PrefabComponentEnabled"
		), &component.enabled);
		if (component.type == "MeshRenderer") {
			const char* currentModel = component.modelPath.empty()
				? "None"
				: component.modelPath.c_str();
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Model"), currentModel)) {
				for (const std::string& modelPath : GetCachedModelAssetPaths()) {
					if (ImGui::Selectable(
						modelPath.c_str(), component.modelPath == modelPath
					)) {
						component.modelPath = modelPath;
						entity->modelPath = modelPath;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			changed |= ImGui::DragFloat3(
				LocalizedComponentWidgetLabel(editorLanguage_, "Visual Rotation (radians)"),
				&component.meshVisualRotation.x,
				0.01f
			);
		} else if (component.type == "Animator") {
			changed |= ImGui::Checkbox(
				LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"), &component.animatorPlayOnStart
			);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop"), &component.animatorLoop);
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Speed"), &component.animatorSpeed, 0.01f, 0.0f, 100.0f
			);
			changed |= ImGui::DragInt(
				LocalizedComponentWidgetLabel(editorLanguage_, "Default Clip"), &component.animatorDefaultClip, 1.0f, 0, 1024
			);
		} else if (component.type == "AudioSource") {
			changed |= DrawAudioClipAssetField(LocalizedComponentWidgetLabel(editorLanguage_, "Clip Path"), component.audioClipPath);
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Spatial Mode"), GetAudioSpatialModeDisplayName(component.audioSpatialMode))) {
				for (const auto& [value, label] : { std::pair{ "TwoD", "TwoD Stereo" }, std::pair{ "ThreeD", "ThreeD Point" }, std::pair{ "ThreeDPointDownmix", "ThreeD Point Downmix" }, std::pair{ "ThreeDStereoArea", "ThreeD Stereo Area" } }) {
					if (ImGui::Selectable(label, component.audioSpatialMode == value)) { component.audioSpatialMode = value; changed = true; }
				}
				ImGui::EndCombo();
			}
			if (IsThreeDAudioSpatialMode(component.audioSpatialMode)) {
				changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Minimum Distance"), &component.audioMinimumDistance, 0.05f, 0.0f, 10000.0f);
				changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Maximum Distance"), &component.audioMaximumDistance, 0.1f, 0.01f, 10000.0f);
				if (component.audioSpatialMode == "ThreeD") {
					ImGui::TextDisabled("ThreeD Point requires a mono clip.");
				} else if (component.audioSpatialMode == "ThreeDPointDownmix") {
					ImGui::TextDisabled("Stereo clips are downmixed to mono. Phase-opposed channels can cancel.");
					ImGui::TextDisabled("Decompress On Load avoids first-play decode and conversion work.");
				} else if (component.audioSpatialMode == "ThreeDStereoArea") {
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Area Width"), &component.audioStereoAreaWidth, 0.01f, 0.01f, 10000.0f);
					ImGui::TextDisabled("Stereo clip required. Width is the L/R spacing in Scene units.");
				}
				DrawAudioSpatialClipCompatibilityWarning(editorLanguage_, component);
			}
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Bus"), component.audioBus.c_str())) {
				for (const char* bus : { "BGM", "SFX", "UI", "Ambience" }) if (ImGui::Selectable(bus, component.audioBus == bus)) { component.audioBus = bus; changed = true; }
				ImGui::EndCombo();
			}
			changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Volume"), &component.audioVolume, 0.01f, 0.0f, 4.0f);
			changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Pitch"), &component.audioPitch, 0.01f, 0.01f, 8.0f);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop"), &component.audioLoop);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"), &component.audioPlayOnStart);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Stop On Disable"), &component.audioStopOnDisable);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Stream From Disk"), &component.audioStreamFromDisk);
			ImGui::BeginDisabled(component.audioStreamFromDisk);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Decompress On Load"), &component.audioDecompressOnLoad);
			ImGui::EndDisabled();
			const bool persistentBgmCompatible = component.audioStreamFromDisk && component.audioBus == "BGM" && component.audioSpatialMode == "TwoD";
			ImGui::BeginDisabled(!persistentBgmCompatible);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Persist Across Scenes"), &component.audioPersistAcrossScenes);
			ImGui::EndDisabled();
			if (component.audioBus == "BGM" && component.audioSpatialMode == "TwoD") {
				changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "BGM Fade Seconds"), &component.audioBgmFadeSeconds, 0.05f, 0.0f, 30.0f);
			}
			if (component.audioStreamFromDisk && (component.audioBus != "BGM" || component.audioSpatialMode != "TwoD")) {
				ImGui::TextDisabled("Stream From Disk requires TwoD and BGM Bus.");
			}
		} else if (component.type == "AudioListener") {
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Mode"), component.audioListenerMode.c_str())) {
				for (const char* mode : { "ActiveCamera", "Entity", "Hybrid" }) if (ImGui::Selectable(mode, component.audioListenerMode == mode)) { component.audioListenerMode = mode; changed = true; }
				ImGui::EndCombo();
			}
			ImGui::TextDisabled("Only one enabled listener is allowed per Scene.");
		} else if (component.type == "StatSet") {
			int removeStatIndex = -1;
			for (size_t statIndex = 0; statIndex < component.stats.size(); ++statIndex) {
				SceneStatDefinition& stat = component.stats[statIndex];
				ImGui::PushID(static_cast<int>(statIndex));
				const std::string statLabel = stat.displayName.empty()
					? stat.id
					: stat.displayName;
				if (ImGui::TreeNodeEx(
					"Stat",
					ImGuiTreeNodeFlags_DefaultOpen,
					"%s",
					statLabel.empty() ? "Stat" : statLabel.c_str()
				)) {
					changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Id"), stat.id);
					changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Display Name"), stat.displayName);
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Min"), &stat.minValue, 0.1f);
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Max"), &stat.maxValue, 0.1f);
					changed |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Initial"), &stat.initialValue, 0.1f
					);
					if (stat.maxValue < stat.minValue) {
						stat.maxValue = stat.minValue;
						changed = true;
					}
					const float clampedInitial = std::clamp(
						stat.initialValue,
						stat.minValue,
						stat.maxValue
					);
					if (clampedInitial != stat.initialValue) {
						stat.initialValue = clampedInitial;
						changed = true;
					}
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Statを削除###RemoveStat", "Remove Stat###RemoveStat"))) {
						removeStatIndex = static_cast<int>(statIndex);
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (removeStatIndex >= 0) {
				component.stats.erase(component.stats.begin() + removeStatIndex);
				changed = true;
			}
			if (ImGui::Button(SelectEditorText(editorLanguage_, "Statを追加###AddStat", "Add Stat###AddStat"))) {
				SceneStatDefinition stat{};
				stat.id = "stat" + std::to_string(component.stats.size() + 1);
				stat.displayName = stat.id;
				component.stats.push_back(std::move(stat));
				changed = true;
			}
		} else if (component.type == "OBBCollider") {
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Shape"), component.colliderShape.c_str())) {
				for (const char* shape : { "Box", "Sphere" }) {
					if (ImGui::Selectable(
						shape, component.colliderShape == shape
					)) {
						component.colliderShape = shape;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			changed |= ImGui::DragFloat3(
				LocalizedComponentWidgetLabel(editorLanguage_, "Offset"), &component.colliderOffset.x, 0.01f
			);
			if (component.colliderShape == "Sphere") {
				changed |= ImGui::DragFloat(
					LocalizedComponentWidgetLabel(editorLanguage_, "Radius"), &component.colliderSphereRadius, 0.01f, 0.001f, 10000.0f
				);
			} else {
				changed |= ImGui::DragFloat3(
					LocalizedComponentWidgetLabel(editorLanguage_, "Half Size"), &component.colliderSizeMultiplier.x, 0.01f
				);
			}
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Is Trigger"), &component.colliderIsTrigger);
			changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Collider Active"), &component.colliderActive);
			changed |= ImGui::Checkbox(
				LocalizedComponentWidgetLabel(editorLanguage_, "Debug Visible"), &component.colliderDebugVisible
			);
		} else if (component.type == "HitBox") {
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Damage"), &component.hitBoxDamage, 0.1f, 0.0f, 100000.0f
			);
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Poise Damage"), &component.hitBoxPoiseDamage, 0.1f, 0.0f, 100000.0f
			);
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Knockback"), &component.hitBoxKnockback, 0.1f, 0.0f, 100000.0f
			);
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Vertical Knockback"), &component.hitBoxVerticalKnockback,
				0.1f, 0.0f, 100000.0f
			);
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Hit Stop Duration"), &component.hitBoxHitStopDuration, 0.001f, 0.0f, 1.0f
			);
			changed |= InputTextString(
				LocalizedComponentWidgetLabel(editorLanguage_, "Reaction Tag"), component.hitBoxReactionTag
			);
			changed |= InputTextString(
				LocalizedComponentWidgetLabel(editorLanguage_, "Damage Stat"), component.hitBoxDamageStatId
			);
			changed |= InputTextString(
				LocalizedComponentWidgetLabel(editorLanguage_, "Poise Stat"), component.hitBoxPoiseStatId
			);
			changed |= ImGui::InputScalar(
				LocalizedComponentWidgetLabel(editorLanguage_, "Owner Entity Id"),
				ImGuiDataType_U64,
				&component.hitBoxOwnerEntityId
			);
			changed |= InputTextString(
				LocalizedComponentWidgetLabel(editorLanguage_, "Owner Entity Name"), component.hitBoxOwnerEntityName
			);
			changed |= ImGui::Checkbox(
				LocalizedComponentWidgetLabel(editorLanguage_, "Ignore Same Faction"), &component.hitBoxIgnoreSameFaction
			);
			ImGui::TextDisabled(SelectEditorText(editorLanguage_, "有効なTrigger Colliderと組み合わせて使用します。", "Use with an active Trigger Collider."));
		} else if (component.type == "HurtBox") {
			changed |= ImGui::DragFloat(
				LocalizedComponentWidgetLabel(editorLanguage_, "Damage Multiplier"),
				&component.hurtBoxDamageMultiplier,
				0.01f,
				0.0f,
				1000.0f
			);
			changed |= InputTextString(
				LocalizedComponentWidgetLabel(editorLanguage_, "Stats Entity Name"), component.hurtBoxStatsEntityName
			);
		} else if (component.type == "AgentBehavior") {
			const bool isGroundAgent =
				component.agentMovementMode == "GroundXZ";
			if (ImGui::BeginCombo(
				"Movement Mode",
				isGroundAgent ? "Ground XZ" : "Free 3D"
			)) {
				if (ImGui::Selectable("Free 3D", !isGroundAgent)) {
					component.agentMovementMode = "Free3D";
					changed = true;
				}
				if (ImGui::Selectable("Ground XZ", isGroundAgent)) {
					component.agentMovementMode = "GroundXZ";
					changed = true;
				}
				ImGui::EndCombo();
			}
			changed |= InputTextString("Group", component.agentGroupName);
			if (isGroundAgent) {
				changed |= ImGui::DragFloat(
					"Separation Radius",
					&component.agentSeparationRadius,
					0.05f,
					0.0f,
					100.0f
				);
				changed |= ImGui::DragFloat(
					"Separation Weight",
					&component.agentSeparationWeight,
					0.05f,
					0.0f,
					100.0f
				);
				changed |= ImGui::InputInt(
					"Neighbor Limit",
					&component.agentNeighborLimit
				);
				ImGui::TextDisabled(
					"Requires PhysicsBody. EnemyBehavior retains movement and rotation ownership."
				);
			}
		} else if (component.type == "HitReaction") {
			changed |= ImGui::DragFloat(
				"Knockback Multiplier",
				&component.hitReactionKnockbackMultiplier,
				0.01f, 0.0f, 100.0f
			);
			const char* reactionModePreview =
				component.hitReactionTriggerMode == "PoiseBreak"
				? "Poise Break" : "Minimum Damage";
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Reaction Trigger"), reactionModePreview)) {
				if (ImGui::Selectable(
					"Minimum Damage",
					component.hitReactionTriggerMode == "MinimumDamage"
				)) {
					component.hitReactionTriggerMode = "MinimumDamage";
					changed = true;
				}
				if (ImGui::Selectable(
					"Poise Break",
					component.hitReactionTriggerMode == "PoiseBreak"
				)) {
					component.hitReactionTriggerMode = "PoiseBreak";
					changed = true;
				}
				ImGui::EndCombo();
			}
			if (component.hitReactionTriggerMode == "PoiseBreak") {
				changed |= InputTextString(
					"Poise Stat", component.hitReactionPoiseStatId
				);
				changed |= ImGui::DragFloat(
					"Poise Recovery Delay",
					&component.hitReactionPoiseRecoveryDelay,
					0.05f, 0.0f, 60.0f
				);
			} else {
				changed |= ImGui::DragFloat(
					"Minimum Poise Damage",
					&component.hitReactionMinimumPoiseDamage,
					0.1f, 0.0f, 100000.0f
				);
			}
			changed |= InputTextString(
				"Hit State", component.hitReactionStateName
			);
		} else if (component.type == "DeathPresentation") {
			changed |= InputTextString(
				"Death State", component.deathPresentationStateName
			);
			changed |= ImGui::DragFloat(
				"Deactivate Delay",
				&component.deathPresentationDeactivateDelay,
				0.05f, 0.0f, 60.0f
			);
			changed |= InputTextString(
				"Death Effect Path", component.deathPresentationEffectPath
			);
		} else if (component.type == "EnemySpawner") {
				changed |= InputTextString(
					"Enemy Prefab", component.enemySpawnerPrefabPath
				);
				changed |= ImGui::DragInt(
					"Initial Count", &component.enemySpawnerInitialCount,
					1.0f, 0, 10000
				);
				changed |= ImGui::DragInt(
					"Max Alive", &component.enemySpawnerMaxAlive,
					1.0f, 0, 10000
				);
				changed |= ImGui::DragFloat(
					"Respawn Interval", &component.enemySpawnerInterval,
					0.05f, 0.0f, 3600.0f
				);
				changed |= ImGui::DragFloat(
					"Spawn Radius", &component.enemySpawnerRadius,
					0.1f, 0.0f, 10000.0f
				);
				changed |= ImGui::Checkbox(
					"Auto Start", &component.enemySpawnerAutoStart
				);
				component.enemySpawnerInitialCount = (std::max)(
					component.enemySpawnerInitialCount, 0
				);
				component.enemySpawnerMaxAlive = (std::max)(
					component.enemySpawnerMaxAlive,
					component.enemySpawnerInitialCount
				);
				component.enemySpawnerInterval = (std::max)(
					component.enemySpawnerInterval, 0.0f
				);
				component.enemySpawnerRadius = (std::max)(
					component.enemySpawnerRadius, 0.0f
				);
				ImGui::TextDisabled(
					"Runtime-only instances are reset to their prefab baseline before reuse."
				);
			} else if (component.type == "BoneAttachment") {
			SceneEntity* targetEntity = component.boneAttachmentTargetEntityId != 0
				? document.FindEntity(component.boneAttachmentTargetEntityId)
				: nullptr;
			if (!targetEntity && !component.boneAttachmentTargetEntityName.empty()) {
				targetEntity = document.FindEntityByName(
					component.boneAttachmentTargetEntityName
				);
			}
			SceneEntity* parentEntity = document.FindEntity(entity->parentId);
			SceneEntity* effectiveTarget = targetEntity ? targetEntity : parentEntity;
			const char* targetLabel = targetEntity
				? targetEntity->name.c_str()
				: "Parent / Auto";
			if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Target Entity"), targetLabel)) {
				if (ImGui::Selectable(
					"Parent / Auto",
					component.boneAttachmentTargetEntityId == 0 &&
						component.boneAttachmentTargetEntityName.empty()
				)) {
					component.boneAttachmentTargetEntityId = 0;
					component.boneAttachmentTargetEntityName.clear();
					component.boneAttachmentJointName.clear();
					changed = true;
				}
				for (const SceneEntity& candidate : document.GetEntities()) {
					if (
						candidate.id == entity->id ||
						!FindEnabledComponent(candidate, "MeshRenderer")
					) {
						continue;
					}
					if (ImGui::Selectable(
						candidate.name.c_str(),
						targetEntity && targetEntity->id == candidate.id
					)) {
						component.boneAttachmentTargetEntityId = candidate.id;
						component.boneAttachmentTargetEntityName = candidate.name;
						component.boneAttachmentJointName.clear();
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			const std::vector<std::string> jointNames = effectiveTarget
				? CollectEntityJointNames(*effectiveTarget)
				: std::vector<std::string>{};
			ImGui::BeginDisabled(jointNames.empty());
			changed |= DrawJointNameCombo(
				LocalizedComponentWidgetLabel(editorLanguage_, "Target Bone"), jointNames, component.boneAttachmentJointName
			);
			ImGui::EndDisabled();
			const bool matchesSourceBone =
				component.boneAttachmentAlignmentMode == "MatchSourceBone";
			if (ImGui::BeginCombo(
				LocalizedComponentWidgetLabel(editorLanguage_, "Alignment Mode"),
				matchesSourceBone ? "Match Weapon Bone" : "Manual Offset"
			)) {
				if (ImGui::Selectable("Manual Offset", !matchesSourceBone)) {
					component.boneAttachmentAlignmentMode = "ManualOffset";
					changed = true;
				}
				if (ImGui::Selectable("Match Weapon Bone", matchesSourceBone)) {
					component.boneAttachmentAlignmentMode = "MatchSourceBone";
					changed = true;
				}
				ImGui::EndCombo();
			}
			if (matchesSourceBone) {
				const std::vector<std::string> sourceJointNames =
					CollectEntityJointNames(*entity);
				ImGui::BeginDisabled(sourceJointNames.empty());
				changed |= DrawJointNameCombo(
					LocalizedComponentWidgetLabel(editorLanguage_, "Weapon Bone"),
					sourceJointNames,
					component.boneAttachmentSourceJointName
				);
				ImGui::EndDisabled();
				ImGui::TextDisabled(
					SelectEditorText(editorLanguage_, "Weapon Boneを対象Boneと正確に一致させます。", "The weapon bone is aligned exactly with the target bone.")
				);
			} else {
				ImGui::TextDisabled(
					SelectEditorText(editorLanguage_, "EntityのTransformをAttachmentのオフセットとして使用します。", "The Entity Transform is used as the attachment offset.")
				);
			}
			changed |= ImGui::Checkbox(
				LocalizedComponentWidgetLabel(editorLanguage_, "Inherit Bone Scale"), &component.boneAttachmentInheritScale
			);
		} else if (component.type == "AttackSet") {
			if (clipFocusName) {
				const bool hasFocusedAttack = std::any_of(
					component.attackDefinitions.begin(),
					component.attackDefinitions.end(),
					[&clipFocusName](const SceneAttackDefinition& attack) {
						return attack.animation == *clipFocusName;
					}
				);
				if (!hasFocusedAttack) {
				ImGui::TextDisabled(SelectEditorText(editorLanguage_, "このClipにはAttack Definitionがありません。", "No Attack Definition for this Clip."));
				}
			}
			auto hasHitBox = [](const SceneEntity& candidate) {
				return std::any_of(
					candidate.components.begin(),
					candidate.components.end(),
					[](const SceneComponent& candidateComponent) {
						return candidateComponent.type == "HitBox";
					}
				);
			};
			auto resolveHitBox = [&document](const SceneAttackHitWindow& window) {
				SceneEntity* hitBox = window.hitBoxEntityId != 0
					? document.FindEntity(window.hitBoxEntityId)
					: nullptr;
				if (!hitBox && !window.hitBoxEntityName.empty()) {
					hitBox = document.FindEntityByName(window.hitBoxEntityName);
				}
				return hitBox;
			};
			auto makeDedicatedHitBoxName = [&document](
				const SceneAttackDefinition& attack,
				size_t windowIndex
			) {
				const std::string base = attack.name + "_HitBox_" +
					std::to_string(windowIndex + 1);
				std::string result = base;
				for (uint32_t suffix = 2; document.FindEntityByName(result); ++suffix) {
					result = base + "_" + std::to_string(suffix);
				}
				return result;
			};
			auto copyLegacyPayloadToHitBox = [](
				SceneComponent& hitBox,
				const SceneAttackHitWindow& window
			) {
				hitBox.hitBoxDamage = window.damage;
				hitBox.hitBoxPoiseDamage = window.poiseDamage;
				hitBox.hitBoxKnockback = window.knockback;
				hitBox.hitBoxVerticalKnockback = window.verticalKnockback;
				hitBox.hitBoxHitStopDuration = window.hitStopDuration;
				hitBox.hitBoxReactionTag = window.reactionTag;
				hitBox.hitBoxKnockbackDirectionMode = window.knockbackDirectionMode;
				hitBox.hitBoxKnockbackLocalDirection = window.knockbackLocalDirection;
				hitBox.hitBoxHitPolicy = window.hitPolicy;
				hitBox.hitBoxTargetCooldown = window.targetCooldown;
			};
			auto isDedicatedHitBox = [&component](uint64_t entityId) {
				for (const SceneAttackDefinition& attack : component.attackDefinitions) {
					for (const SceneAttackHitWindow& window : attack.hitWindows) {
						if (window.payloadSource == "HitBox" &&
							window.hitBoxEntityId == entityId) {
							return true;
						}
					}
				}
				return false;
			};
			auto createDedicatedHitBox = [
				&document,
				&hasHitBox,
				&resolveHitBox,
				&makeDedicatedHitBoxName,
				&copyLegacyPayloadToHitBox,
				&isDedicatedHitBox
			](
				SceneAttackDefinition& attack,
				size_t windowIndex,
				bool copyWindowPayload
			) -> uint64_t {
				SceneAttackHitWindow& window = attack.hitWindows[windowIndex];
				SceneEntity* templateEntity = resolveHitBox(window);
				if (!templateEntity && !copyWindowPayload) {
					// 新規Windowは既存の専用HitBoxを連鎖複製せず、Dedicated
					// Windowから未参照のAuthoring HitBoxを基準Shapeとして使う。
					for (const SceneEntity& candidate : document.GetEntities()) {
						if (hasHitBox(candidate) && !isDedicatedHitBox(candidate.id)) {
							templateEntity = document.FindEntity(candidate.id);
							break;
						}
					}
				}
				if (!templateEntity) {
					for (const SceneAttackHitWindow& candidate : attack.hitWindows) {
						if (SceneEntity* candidateEntity = resolveHitBox(candidate)) {
							templateEntity = candidateEntity;
							break;
						}
					}
				}
				if (!templateEntity) {
					for (const SceneEntity& candidate : document.GetEntities()) {
						if (hasHitBox(candidate)) {
							templateEntity = document.FindEntity(candidate.id);
							break;
						}
					}
				}
				if (!templateEntity) {
					return 0;
				}
				const uint64_t dedicatedId = document.DuplicateEntity(templateEntity->id);
				SceneEntity* dedicated = document.FindEntity(dedicatedId);
				SceneComponent* dedicatedHitBox = dedicated
					? FindComponent(*dedicated, "HitBox") : nullptr;
				if (!dedicated || !dedicatedHitBox) {
					if (dedicatedId != 0) {
						document.RemoveEntity(dedicatedId);
					}
					return 0;
				}
				dedicated->name = makeDedicatedHitBoxName(attack, windowIndex);
				dedicated->active = false;
				if (!copyWindowPayload) {
					SceneComponent* collider = FindComponent(*dedicated, "OBBCollider");
					if (!collider) {
						document.RemoveEntity(dedicatedId);
						return 0;
					}
					dedicated->transform.scale = { 1.0f, 1.0f, 1.0f };
					collider->enabled = true;
					collider->colliderShape = "Box";
					collider->colliderOffset = { 0.0f, 0.0f, 0.0f };
					collider->colliderSizeMultiplier = { 0.5f, 0.5f, 0.5f };
					collider->colliderIsTrigger = true;
					collider->colliderActive = true;
				}
				else {
					copyLegacyPayloadToHitBox(*dedicatedHitBox, window);
					if (window.overrideHitBoxHalfSize) {
						if (SceneComponent* collider = FindComponent(*dedicated, "OBBCollider");
							collider && collider->enabled && collider->colliderShape == "Box") {
							collider->colliderSizeMultiplier = window.hitBoxHalfSize;
						}
					}
				}
				window.hitBoxEntityId = dedicatedId;
				window.hitBoxEntityName = dedicated->name;
				window.payloadSource = "HitBox";
				window.overrideHitBoxHalfSize = false;
				return dedicatedId;
			};
			int removeAttack = -1;
			for (size_t attackIndex = 0; attackIndex < component.attackDefinitions.size(); ++attackIndex) {
				SceneAttackDefinition& attack = component.attackDefinitions[attackIndex];
				if (clipFocusName && attack.animation != *clipFocusName) {
					continue;
				}
				ImGui::PushID(static_cast<int>(attackIndex));
				if (ImGui::TreeNodeEx("Attack", ImGuiTreeNodeFlags_DefaultOpen, "%s", attack.name.c_str())) {
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "基本情報", "Identity"));
					changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Name"), attack.name);
					changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Animation"), attack.animation);
					const SceneComponent* animator = FindEnabledComponent(
						*entity,
						"PrefabAnimator"
					);
					const ScenePrefabAnimationClip* animationClip = nullptr;
					if (animator) {
						auto foundClip = std::find_if(
							animator->prefabAnimationClips.begin(),
							animator->prefabAnimationClips.end(),
							[&attack](const ScenePrefabAnimationClip& clip) {
								return clip.name == attack.animation;
							}
						);
						if (foundClip != animator->prefabAnimationClips.end()) {
							animationClip = &*foundClip;
						}
					}
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "タイミング", "Timing"));
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Windup"), &attack.windup, 0.01f, 0.0f, 60.0f);
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Active Time"), &attack.activeTime, 0.01f, 0.0f, 60.0f);
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Recovery"), &attack.recovery, 0.01f, 0.0f, 60.0f);
					const float attackDuration = attack.windup +
						attack.activeTime + attack.recovery;
					if (animationClip) {
						const float durationDifference =
							std::abs(animationClip->duration - attackDuration);
						ImGui::TextDisabled(
							"Attack %.3f s / Clip %.3f s",
							attackDuration,
							animationClip->duration
						);
						if (durationDifference > 0.02f) {
							ImGui::TextColored(
								ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
								"Timing and Clip duration differ by %.3f s.",
								durationDifference
							);
						}
					} else if (!attack.animation.empty()) {
						ImGui::TextColored(
							ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
							"Animation '%s' was not found on this PrefabAnimator.",
							attack.animation.c_str()
						);
					}
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "移動", "Motion"));
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Forward Distance"), &attack.forwardDistance, 0.01f, -100.0f, 100.0f);
					changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Side Distance"), &attack.sideDistance, 0.01f, -100.0f, 100.0f);
					const char* facingPreview = "Fixed At Start";
					if (attack.facingMode == "InputDirection") facingPreview = "Input Direction";
					if (attack.facingMode == "TargetDirection") facingPreview = "Target Direction";
					if (attack.facingMode == "RotateByAngle") facingPreview = "Rotate By Angle";
					if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Facing"), facingPreview)) {
						for (const char* mode : {
							"FixedAtStart", "InputDirection", "TargetDirection", "RotateByAngle"
						}) {
							const bool selected = attack.facingMode == mode;
							const char* label = mode[0] == 'I'
								? "Input Direction"
								: mode[0] == 'T'
									? "Target Direction"
									: mode[0] == 'R'
										? "Rotate By Angle"
										: "Fixed At Start";
							if (ImGui::Selectable(label, selected)) {
								attack.facingMode = mode;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
					if (attack.facingMode == "TargetDirection") {
						const SceneEntity* target = attack.facingTargetEntityId != 0
							? document.FindEntity(attack.facingTargetEntityId)
							: nullptr;
						if (!target && !attack.facingTargetEntityName.empty()) {
							target = document.FindEntityByName(attack.facingTargetEntityName);
						}
						const char* targetPreview = target ? target->name.c_str() : "None (keep start facing)";
					if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Facing Target"), targetPreview)) {
							if (ImGui::Selectable("None (keep start facing)", !target)) {
								attack.facingTargetEntityId = 0;
								attack.facingTargetEntityName.clear();
								changed = true;
							}
							for (const SceneEntity& candidate : document.GetEntities()) {
								if (candidate.id == entity->id) { continue; }
								const bool selected = candidate.id == attack.facingTargetEntityId;
								const std::string label = candidate.name + " (" + std::to_string(candidate.id) + ")";
								if (ImGui::Selectable(label.c_str(), selected)) {
									attack.facingTargetEntityId = candidate.id;
									attack.facingTargetEntityName = candidate.name;
									changed = true;
								}
							}
							ImGui::EndCombo();
						}
					} else if (attack.facingMode == "RotateByAngle") {
						changed |= ImGui::DragFloat(
							SelectEditorText(editorLanguage_, "回転角度（Radians）###RotateAngle", "Rotate Angle (radians)###RotateAngle"), &attack.facingRotateAngle,
							0.01f, -25.1328f, 25.1328f
						);
					}
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Loop", "Loop"));
					changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop Enabled"), &attack.loopEnabled);
					ImGui::BeginDisabled(!attack.loopEnabled);
					changed |= ImGui::DragInt(
						LocalizedComponentWidgetLabel(editorLanguage_, "Loop Max Count (0 = Unlimited)"), &attack.loopMaxCount,
						1.0f, 0, 1000
					);
					changed |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Loop Safety Timeout"), &attack.loopSafetyTimeout,
						0.05f, 0.0f, 120.0f
					);
					ImGui::EndDisabled();
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Hit Window", "Hit Windows"));
					int removeWindow = -1;
					for (size_t windowIndex = 0; windowIndex < attack.hitWindows.size(); ++windowIndex) {
						SceneAttackHitWindow& window = attack.hitWindows[windowIndex];
						ImGui::PushID(static_cast<int>(windowIndex));
						if (ImGui::TreeNodeEx("Hit Window", ImGuiTreeNodeFlags_DefaultOpen, "Hit Window %zu", windowIndex + 1)) {
							changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Start"), &window.startTime, 0.01f, 0.0f, 60.0f);
							changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "End"), &window.endTime, 0.01f, window.startTime, 60.0f);
							const SceneEntity* selectedHitBox =
								window.hitBoxEntityId != 0
									? document.FindEntity(window.hitBoxEntityId)
									: nullptr;
							if (!selectedHitBox && !window.hitBoxEntityName.empty()) {
								selectedHitBox = document.FindEntityByName(window.hitBoxEntityName);
							}
							if (window.payloadSource == "HitBox") {
								ImGui::TextDisabled(SelectEditorText(editorLanguage_, "Payload Source: 専用HitBox", "Payload Source: Dedicated HitBox"));
								const char* hitBoxPreview = selectedHitBox && hasHitBox(*selectedHitBox)
									? selectedHitBox->name.c_str()
									: "Missing Dedicated HitBox";
						if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Dedicated HitBox"), hitBoxPreview)) {
									for (const SceneEntity& candidate : document.GetEntities()) {
										if (!hasHitBox(candidate)) { continue; }
										const bool selected = candidate.id == window.hitBoxEntityId;
										const std::string label = candidate.name + " (" +
											std::to_string(candidate.id) + ")";
										if (ImGui::Selectable(label.c_str(), selected)) {
											window.hitBoxEntityId = candidate.id;
											window.hitBoxEntityName = candidate.name;
											changed = true;
										}
									}
									ImGui::EndCombo();
								}
								if (selectedHitBox) {
									const SceneComponent* hitBox = FindComponent(*selectedHitBox, "HitBox");
									if (hitBox) {
										ImGui::TextDisabled(
											"Damage %.1f | Poise %.1f | Knockback %.1f | Vertical %.1f",
											hitBox->hitBoxDamage,
											hitBox->hitBoxPoiseDamage,
											hitBox->hitBoxKnockback,
											hitBox->hitBoxVerticalKnockback
										);
									}
									if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "専用HitBoxを選択###SelectDedicatedHitBox", "Select Dedicated HitBox###SelectDedicatedHitBox"))) {
										prefabSelectedEntityId_ = selectedHitBox->id;
									}
								}
							} else {
								ImGui::TextDisabled(SelectEditorText(editorLanguage_, "Payload Source: Window Legacy（新規編集前に移行してください）", "Payload Source: Window Legacy (migrate before new authoring)"));
							std::string hitBoxPreview = "StateMachine HitBox (Fallback)";
							if (selectedHitBox && hasHitBox(*selectedHitBox)) {
								hitBoxPreview = selectedHitBox->name;
							} else if (window.hitBoxEntityId != 0 || !window.hitBoxEntityName.empty()) {
								hitBoxPreview = "Missing HitBox";
							}
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "HitBox"), hitBoxPreview.c_str())) {
								const bool usesFallback = window.hitBoxEntityId == 0 &&
									window.hitBoxEntityName.empty();
								if (ImGui::Selectable("StateMachine HitBox (Fallback)", usesFallback)) {
									window.hitBoxEntityId = 0;
									window.hitBoxEntityName.clear();
									changed = true;
								}
								for (const SceneEntity& candidate : document.GetEntities()) {
									if (!hasHitBox(candidate)) { continue; }
									const bool selected = candidate.id == window.hitBoxEntityId;
									const std::string label = candidate.name + " (" + std::to_string(candidate.id) + ")";
									if (ImGui::Selectable(label.c_str(), selected)) {
										window.hitBoxEntityId = candidate.id;
										window.hitBoxEntityName = candidate.name;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							const SceneComponent* selectedCollider = selectedHitBox
								? FindEnabledComponent(*selectedHitBox, "OBBCollider")
								: nullptr;
							const bool canOverrideHalfSize = selectedCollider &&
								selectedCollider->colliderShape == "Box";
							if (!canOverrideHalfSize) {
								ImGui::TextDisabled(
									"Half Size Override requires the selected HitBox to have a Box OBBCollider."
								);
							}
							changed |= ImGui::Checkbox(
								LocalizedComponentWidgetLabel(editorLanguage_, "Override HitBox Half Size"), &window.overrideHitBoxHalfSize
							);
							ImGui::BeginDisabled(!canOverrideHalfSize);
							if (window.overrideHitBoxHalfSize) {
								changed |= ImGui::DragFloat3(
									LocalizedComponentWidgetLabel(editorLanguage_, "HitBox Half Size"), &window.hitBoxHalfSize.x,
									0.01f, 0.001f, 10000.0f
								);
								window.hitBoxHalfSize.x = (std::max)(window.hitBoxHalfSize.x, 0.001f);
								window.hitBoxHalfSize.y = (std::max)(window.hitBoxHalfSize.y, 0.001f);
								window.hitBoxHalfSize.z = (std::max)(window.hitBoxHalfSize.z, 0.001f);
							}
							ImGui::EndDisabled();
							ImGui::SeparatorText(SelectEditorText(editorLanguage_, "ダメージとReaction", "Damage & Reaction"));
							changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Damage"), &window.damage, 0.1f, 0.0f, 100000.0f);
							changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Poise Damage"), &window.poiseDamage, 0.1f, 0.0f, 100000.0f);
							changed |= ImGui::DragFloat(LocalizedComponentWidgetLabel(editorLanguage_, "Knockback"), &window.knockback, 0.1f, 0.0f, 100000.0f);
							changed |= ImGui::DragFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Vertical Knockback"), &window.verticalKnockback,
								0.1f, 0.0f, 100000.0f
							);
							changed |= ImGui::DragFloat(
								LocalizedComponentWidgetLabel(editorLanguage_, "Hit Stop Duration"), &window.hitStopDuration,
								0.001f, 0.0f, 1.0f
							);
							changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Reaction Tag"), window.reactionTag);
							struct HitPolicyOption {
								const char* value;
								const char* label;
							};
							static constexpr HitPolicyOption hitPolicies[] = {
								{ "OncePerActivation", "Once Per Activation" },
								{ "OncePerLoop", "Once Per Loop" },
								{ "TargetCooldown", "Target Cooldown" }
							};
							const char* policyPreview = "Once Per Activation";
							for (const HitPolicyOption& option : hitPolicies) {
								if (window.hitPolicy == option.value) {
									policyPreview = option.label;
									break;
								}
							}
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Hit Policy"), policyPreview)) {
								for (const HitPolicyOption& option : hitPolicies) {
									const bool selected = window.hitPolicy == option.value;
									if (ImGui::Selectable(option.label, selected)) {
										window.hitPolicy = option.value;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							if (window.hitPolicy == "TargetCooldown") {
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Target Cooldown"), &window.targetCooldown,
									0.01f, 0.0f, 60.0f
								);
							}
							struct DirectionModeOption {
								const char* value;
								const char* label;
							};
							static constexpr DirectionModeOption directionModes[] = {
								{ "RadialFromAttacker", "Radial from Attacker" },
								{ "AttackFacingLocal", "Attack Facing Local" },
								{ "HitBoxLocal", "HitBox Local" },
								{ "World", "World" }
							};
							const char* directionPreview = "Radial from Attacker";
							for (const DirectionModeOption& option : directionModes) {
								if (window.knockbackDirectionMode == option.value) {
									directionPreview = option.label;
									break;
								}
							}
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Direction Mode"), directionPreview)) {
								for (const DirectionModeOption& option : directionModes) {
									const bool selected = window.knockbackDirectionMode == option.value;
									if (ImGui::Selectable(option.label, selected)) {
										window.knockbackDirectionMode = option.value;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							const bool usesLocalDirection =
								window.knockbackDirectionMode != "RadialFromAttacker";
							ImGui::BeginDisabled(!usesLocalDirection);
							changed |= ImGui::DragFloat3(LocalizedComponentWidgetLabel(editorLanguage_, "Local Direction"), &window.knockbackLocalDirection.x, 0.01f);
							ImGui::EndDisabled();
							if (selectedHitBox && ImGui::SmallButton(SelectEditorText(editorLanguage_, "Windowから専用HitBoxを作成###CreateDedicatedHitBox", "Create Dedicated HitBox From Window###CreateDedicatedHitBox"))) {
								const uint64_t dedicatedId = createDedicatedHitBox(
									attack, windowIndex, true
								);
								if (dedicatedId != 0) {
									prefabSelectedEntityId_ = dedicatedId;
									changed = true;
								}
							}
							}
							if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Hit Windowを削除###RemoveHitWindow", "Remove Hit Window###RemoveHitWindow"))) removeWindow = static_cast<int>(windowIndex);
							ImGui::TreePop();
						}
						ImGui::PopID();
					}
					if (removeWindow >= 0) { attack.hitWindows.erase(attack.hitWindows.begin() + removeWindow); changed = true; }
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Hit Windowを追加###AddHitWindow", "Add Hit Window###AddHitWindow"))) {
						attack.hitWindows.push_back(SceneAttackHitWindow{});
						const size_t newWindowIndex = attack.hitWindows.size() - 1;
						const uint64_t dedicatedId = createDedicatedHitBox(
							attack, newWindowIndex, false
						);
						if (dedicatedId == 0) {
							attack.hitWindows.pop_back();
						} else {
							prefabSelectedEntityId_ = dedicatedId;
							changed = true;
						}
					}
					ImGui::SeparatorText(SelectEditorText(editorLanguage_, "Effect Event", "Effect Events"));
					int removeEffect = -1;
					for (size_t effectIndex = 0;
						effectIndex < attack.effectEvents.size(); ++effectIndex) {
						SceneAttackEffectEvent& effect = attack.effectEvents[effectIndex];
						ImGui::PushID(static_cast<int>(effectIndex));
						if (ImGui::TreeNodeEx(
							"Effect Event", ImGuiTreeNodeFlags_DefaultOpen,
							"Effect Event %zu", effectIndex + 1
						)) {
							changed |= ImGui::DragFloat(
							LocalizedComponentWidgetLabel(editorLanguage_, "Time"), &effect.time, 0.01f, 0.0f, 60.0f
							);
							changed |= InputTextString(
							LocalizedComponentWidgetLabel(editorLanguage_, "Particle Effect Path"), effect.particleEffectPath
							);
							const SceneEntity* selectedSpawn = effect.spawnEntityId != 0
								? document.FindEntity(effect.spawnEntityId)
								: nullptr;
							if (!selectedSpawn && !effect.spawnEntityName.empty()) {
								selectedSpawn = document.FindEntityByName(effect.spawnEntityName);
							}
							const char* spawnPreview = selectedSpawn
								? selectedSpawn->name.c_str()
								: "AttackSet (Fallback)";
						if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Spawn Entity"), spawnPreview)) {
								if (ImGui::Selectable(
									"AttackSet (Fallback)", effect.spawnEntityId == 0 &&
									effect.spawnEntityName.empty()
								)) {
									effect.spawnEntityId = 0;
									effect.spawnEntityName.clear();
									changed = true;
								}
								for (const SceneEntity& candidate : document.GetEntities()) {
									const bool selected = candidate.id == effect.spawnEntityId;
									const std::string label = candidate.name + " (" +
										std::to_string(candidate.id) + ")";
									if (ImGui::Selectable(label.c_str(), selected)) {
										effect.spawnEntityId = candidate.id;
										effect.spawnEntityName = candidate.name;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							changed |= ImGui::DragFloat3(
							LocalizedComponentWidgetLabel(editorLanguage_, "Local Offset"), &effect.localOffset.x, 0.01f
							);
							const char* groundEffectTypes[] = {
								"None", "Prefab", "ProceduralCrack"
							};
							int groundEffectTypeIndex = effect.groundEffectType == "Prefab"
								? 1
								: effect.groundEffectType == "ProceduralCrack" ? 2 : 0;
							if (ImGui::Combo(
							LocalizedComponentWidgetLabel(editorLanguage_, "Ground Effect Type"),
								&groundEffectTypeIndex,
								groundEffectTypes,
								IM_ARRAYSIZE(groundEffectTypes)
							)) {
								effect.groundEffectType =
									groundEffectTypes[groundEffectTypeIndex];
								changed = true;
							}
							if (groundEffectTypeIndex != 0) {
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Ground Probe Distance"), &effect.groundProbeDistance,
									0.05f, 0.0f, 100.0f
								);
							}
							if (groundEffectTypeIndex == 1) {
								changed |= InputTextString(
									LocalizedComponentWidgetLabel(editorLanguage_, "Ground Prefab Path"), effect.groundPrefabPath
								);
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Ground Prefab Lifetime"), &effect.groundPrefabLifetime,
									0.05f, 0.0f, 60.0f
								);
							} else if (groundEffectTypeIndex == 2) {
								int primaryBranchCount = static_cast<int>(
									effect.groundCrackPrimaryBranchCount
								);
								int segmentsPerBranch = static_cast<int>(
									effect.groundCrackSegmentsPerBranch
								);
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Crack Radius"), &effect.groundCrackRadius,
									0.05f, 0.0f, 100.0f
								);
								if (ImGui::DragInt(
									LocalizedComponentWidgetLabel(editorLanguage_, "Primary Branch Count"), &primaryBranchCount, 1.0f, 1, 24
								)) {
									effect.groundCrackPrimaryBranchCount =
										static_cast<uint32_t>(primaryBranchCount);
									changed = true;
								}
								if (ImGui::DragInt(
									LocalizedComponentWidgetLabel(editorLanguage_, "Segments Per Branch"), &segmentsPerBranch, 1.0f, 1, 12
								)) {
									effect.groundCrackSegmentsPerBranch =
										static_cast<uint32_t>(segmentsPerBranch);
									changed = true;
								}
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Branch Probability"), &effect.groundCrackBranchProbability,
									0.01f, 0.0f, 1.0f
								);
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Crack Width"), &effect.groundCrackWidth,
									0.005f, 0.0f, 10.0f
								);
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Crack Lifetime"), &effect.groundCrackLifetime,
									0.05f, 0.0f, 60.0f
								);
								changed |= ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Crack Surface Offset"), &effect.groundCrackSurfaceOffset,
									0.001f, 0.0f, 1.0f
								);
							}
							if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Effect Eventを削除###RemoveEffectEvent", "Remove Effect Event###RemoveEffectEvent"))) {
								removeEffect = static_cast<int>(effectIndex);
							}
							ImGui::TreePop();
						}
						ImGui::PopID();
					}
					if (removeEffect >= 0) {
						attack.effectEvents.erase(
							attack.effectEvents.begin() + removeEffect
						);
						changed = true;
					}
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Effect Eventを追加###AddEffectEvent", "Add Effect Event###AddEffectEvent"))) {
						attack.effectEvents.push_back(SceneAttackEffectEvent{});
						changed = true;
					}
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Attackを削除###RemoveAttack", "Remove Attack###RemoveAttack"))) removeAttack = static_cast<int>(attackIndex);
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (removeAttack >= 0) { component.attackDefinitions.erase(component.attackDefinitions.begin() + removeAttack); changed = true; }
			if (!clipFocusName && ImGui::Button(SelectEditorText(editorLanguage_, "Attackを追加###AddAttack", "Add Attack###AddAttack"))) {
				component.attackDefinitions.push_back(SceneAttackDefinition{});
				changed = true;
			}
		} else if (component.type == "PrefabAnimator") {
			int removeClipIndex = -1;
			for (size_t clipIndex = 0;
				clipIndex < component.prefabAnimationClips.size();
				++clipIndex) {
				if (
					clipFocusActive &&
					static_cast<int>(clipIndex) != prefabAnimationPreviewClipIndex_
				) {
					continue;
				}
				ScenePrefabAnimationClip& clip =
					component.prefabAnimationClips[clipIndex];
				ImGui::PushID(static_cast<int>(clipIndex));
				if (ImGui::TreeNodeEx(
					"Clip", ImGuiTreeNodeFlags_DefaultOpen, "%s", clip.name.c_str()
				)) {
					changed |= InputTextString(LocalizedComponentWidgetLabel(editorLanguage_, "Clip Name"), clip.name);
					changed |= ImGui::DragFloat(
						LocalizedComponentWidgetLabel(editorLanguage_, "Duration"), &clip.duration, 0.01f, 0.001f, 3600.0f
					);
					changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Loop"), &clip.loop);
					changed |= ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Play On Start"), &clip.playOnStart);
					int removeTrackIndex = -1;
					for (size_t trackIndex = 0;
						trackIndex < clip.tracks.size();
						++trackIndex) {
						SceneAnimationTrack& track = clip.tracks[trackIndex];
						ImGui::PushID(static_cast<int>(trackIndex));
						if (ImGui::TreeNodeEx(
							"Track", ImGuiTreeNodeFlags_DefaultOpen, "%s", track.property.c_str()
						)) {
							const SceneEntity* trackTarget = track.targetEntityId != 0
								? document.FindEntity(track.targetEntityId)
								: entity;
							if (ImGui::BeginCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Target"),
								trackTarget ? trackTarget->name.c_str() : "Self"
							)) {
								if (ImGui::Selectable("Self", track.targetEntityId == 0)) {
									track.targetEntityId = 0;
									track.targetEntityName.clear();
									changed = true;
								}
								for (const SceneEntity& candidate : document.GetEntities()) {
									if (ImGui::Selectable(
										candidate.name.c_str(),
										track.targetEntityId == candidate.id
									)) {
										track.targetEntityId = candidate.id;
										track.targetEntityName = candidate.name;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							if (ImGui::BeginCombo(LocalizedComponentWidgetLabel(editorLanguage_, "Property"), track.property.c_str())) {
								for (const char* property : {
									"LocalPosition", "LocalRotation", "LocalScale", "Active"
								}) {
									if (ImGui::Selectable(
										property, track.property == property
									)) {
										track.property = property;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							if (track.property != "Active" && ImGui::BeginCombo(
								LocalizedComponentWidgetLabel(editorLanguage_, "Easing"),
								track.easing.empty() ? "SmoothStep" : track.easing.c_str()
							)) {
								for (const char* easing : {
									"Linear", "EaseIn", "EaseOut", "EaseInOut",
									"SmoothStep"
								}) {
									if (ImGui::Selectable(easing, track.easing == easing)) {
										track.easing = easing;
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
							int removeKeyIndex = -1;
							bool keyframeTimeChanged = false;
							for (size_t keyIndex = 0;
								keyIndex < track.keyframes.size();
								++keyIndex) {
								SceneAnimationKeyframe& key = track.keyframes[keyIndex];
								ImGui::PushID(static_cast<int>(keyIndex));
								if (ImGui::DragFloat(
									LocalizedComponentWidgetLabel(editorLanguage_, "Time"), &key.time, 0.01f, 0.0f, clip.duration
								)) {
									key.time = std::clamp(key.time, 0.0f, clip.duration);
									keyframeTimeChanged = true;
									changed = true;
								}
								if (track.property == "Active") {
									bool activeValue = key.value.x >= 0.5f;
									if (ImGui::Checkbox(LocalizedComponentWidgetLabel(editorLanguage_, "Active Value"), &activeValue)) {
										key.value.x = activeValue ? 1.0f : 0.0f;
										changed = true;
									}
								} else {
									changed |= ImGui::DragFloat3(
										track.property == "LocalRotation"
											? LocalizedComponentWidgetLabel(editorLanguage_, "Euler Value (Radians)")
											: LocalizedComponentWidgetLabel(editorLanguage_, "Value"),
										&key.value.x,
										0.01f
									);
								}
								ImGui::SameLine();
								if (ImGui::SmallButton("X")) {
									removeKeyIndex = static_cast<int>(keyIndex);
								}
								ImGui::PopID();
							}
							if (removeKeyIndex >= 0) {
								track.keyframes.erase(
									track.keyframes.begin() + removeKeyIndex
								);
								changed = true;
							}
							if (keyframeTimeChanged) {
								std::stable_sort(
									track.keyframes.begin(),
									track.keyframes.end(),
									[](const SceneAnimationKeyframe& left,
										const SceneAnimationKeyframe& right) {
										return left.time < right.time;
									}
								);
							}
							if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Keyframeを追加###AddKeyframe", "Add Keyframe###AddKeyframe"))) {
								SceneAnimationKeyframe keyframe = track.keyframes.empty()
									? SceneAnimationKeyframe{}
									: track.keyframes.back();
								keyframe.time = track.keyframes.empty()
									? 0.0f
									: (std::min)(keyframe.time + 0.1f, clip.duration);
								keyframe.easingToNext.clear();
								keyframe.positionBulge = {};
								track.keyframes.push_back(keyframe);
								changed = true;
							}
							ImGui::SameLine();
							if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Trackを削除###RemoveTrack", "Remove Track###RemoveTrack"))) {
								removeTrackIndex = static_cast<int>(trackIndex);
							}
							ImGui::TreePop();
						}
						ImGui::PopID();
					}
					if (removeTrackIndex >= 0) {
						clip.tracks.erase(clip.tracks.begin() + removeTrackIndex);
						changed = true;
					}
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Trackを追加###AddTrack", "Add Track###AddTrack"))) {
						clip.tracks.push_back(SceneAnimationTrack{});
						changed = true;
					}
					ImGui::SameLine();
					if (ImGui::SmallButton(SelectEditorText(editorLanguage_, "Clipを削除###RemoveClip", "Remove Clip###RemoveClip"))) {
						removeClipIndex = static_cast<int>(clipIndex);
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (removeClipIndex >= 0) {
				component.prefabAnimationClips.erase(
					component.prefabAnimationClips.begin() + removeClipIndex
				);
				changed = true;
			}
			if (ImGui::Button(SelectEditorText(editorLanguage_, "Clipを追加###AddClip", "Add Clip###AddClip"))) {
				component.prefabAnimationClips.push_back(ScenePrefabAnimationClip{});
				changed = true;
			}
		}

		if (changed) {
			document.MarkDirty();
		}
		if (ImGui::SmallButton(SelectEditorText(
			editorLanguage_,
			"Componentを削除###PrefabComponentRemove",
			"Remove Component###PrefabComponentRemove"
		))) {
			removeComponentIndex = static_cast<int>(componentIndex);
		}
		ImGui::Separator();
		ImGui::PopID();
	}
	if (removeComponentIndex >= 0) {
		const std::string type = entity->components[removeComponentIndex].type;
		document.RemoveComponent(entity->id, type);
	}

	ImGui::SeparatorText(SelectEditorText(
		editorLanguage_,
		"Componentを追加",
		"Add Component"
	));
	ImGui::BeginDisabled(entity->folder);
	if (ImGui::Button(
		SelectEditorText(
			editorLanguage_,
			"Componentを追加...##OpenPrefabComponentPicker",
			"Add Components...##OpenPrefabComponentPicker"
		),
		ImVec2(-1.0f, 0.0f)
	)) {
		OpenComponentPicker(
			prefabComponentPicker_,
			ComponentPickerTarget::Prefab,
			document,
			prefabEditorSession_->GetFilePath(),
			entity->id
		);
	}
	ImGui::EndDisabled();
	if (entity->folder) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"FolderへComponentは追加できません。",
			"Components cannot be added to a folder."
		));
	}
}

void ImGuiManager::DrawFishingScoreAttackConsoleWindow() {
	if (ImGuiWindow* inspectorWindow = ImGui::FindWindowByName("Inspector")) {
		if (ImGuiDockNode* dockNode = inspectorWindow->DockNode) {
			ImGui::SetNextWindowDockID(dockNode->ID, ImGuiCond_FirstUseEver);
		}
	}
	ImGui::Begin(
		"Fishing Score Attack Console###FishingScoreAttackConsole",
		&showFishingScoreAttackConsole_
	);
	if (!editorSession_) {
		ImGui::TextDisabled("Scene editor is not available.");
		ImGui::End();
		return;
	}

	SceneDocument& document = editorSession_->GetActiveDocument();
	const bool canEditScene = editorSession_->IsEditing();
	const auto text = [this](const char* japanese, const char* english) {
		return SelectEditorText(editorLanguage_, japanese, english);
	};
	ImGui::TextColored(
		canEditScene
			? ImVec4(0.35f, 0.85f, 0.4f, 1.0f)
			: ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
		canEditScene ? text("編集モード", "Edit mode") : text("読み取り専用（プレイ／一時停止）", "Read-only (Play/Pause)")
	);

	std::vector<uint64_t> directorIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (FindEnabledComponent(entity, "FishingScoreAttackDirector")) {
			directorIds.push_back(entity.id);
		}
	}
	if (directorIds.empty()) {
		ImGui::TextColored(
			ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
			text("このSceneにFishingScoreAttackDirectorがありません。", "FishingScoreAttackDirector is not configured in this Scene.")
		);
		ImGui::End();
		return;
	}
	if (std::find(directorIds.begin(), directorIds.end(), fishingConsoleDirectorEntityId_) == directorIds.end()) {
		fishingConsoleDirectorEntityId_ = directorIds.front();
	}
	if (directorIds.size() > 1) {
		ImGui::TextColored(
			ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
			text("有効なDirectorが複数あります。", "Multiple active Directors found.")
		);
	}
	SceneEntity* directorEntity = document.FindEntity(fishingConsoleDirectorEntityId_);
	SceneComponent* director = directorEntity
		? FindComponent(*directorEntity, "FishingScoreAttackDirector")
		: nullptr;
	if (!director || !director->enabled) {
		ImGui::TextDisabled("%s", text("選択中のDirectorを取得できません。", "Selected Director is unavailable."));
		ImGui::End();
		return;
	}
	if (directorIds.size() > 1) {
		const std::string preview = BuildEntityHierarchyLabel(document, *directorEntity);
		if (ImGui::BeginCombo("Director", preview.c_str())) {
			for (uint64_t entityId : directorIds) {
				SceneEntity* candidate = document.FindEntity(entityId);
				if (!candidate) {
					continue;
				}
				const bool selected = entityId == fishingConsoleDirectorEntityId_;
				const std::string label = BuildEntityHierarchyLabel(document, *candidate);
				if (ImGui::Selectable(label.c_str(), selected)) {
					fishingConsoleDirectorEntityId_ = entityId;
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
	}

	auto revealInspector = [this](uint64_t entityId) {
		if (entityId == 0) {
			return;
		}
		selectedEntityIds_.clear();
		selectedEntityIds_.insert(entityId);
		selectedEntityId_ = entityId;
		showInspector_ = true;
		revealInspectorRequested_ = true;
	};
	if (ImGui::SmallButton(text("DirectorをInspectorで開く", "Open Director Inspector"))) {
		revealInspector(fishingConsoleDirectorEntityId_);
	}

	bool changed = false;
	const bool directorCanEdit = canEditScene && !directorEntity->locked;
	ImGui::BeginDisabled(!directorCanEdit);
		auto drawReference = [this, &document, &changed, &revealInspector, &text](
		const char* label, uint64_t& entityId, const char* requiredType
	) {
		// Fish rows reuse the same controls; scope every label/button by its reference label.
		ImGui::PushID(label);
		SceneEntity* selected = entityId != 0 ? document.FindEntity(entityId) : nullptr;
		const std::string preview = selected
			? BuildEntityHierarchyLabel(document, *selected)
			: text("未設定", "Not assigned");
		if (ImGui::BeginCombo(label, preview.c_str())) {
			for (const SceneEntity& candidate : document.GetEntities()) {
				if (!FindEnabledComponent(candidate, requiredType)) {
					continue;
				}
				const bool isSelected = candidate.id == entityId;
				const std::string candidateLabel = BuildEntityHierarchyLabel(document, candidate);
				if (ImGui::Selectable(candidateLabel.c_str(), isSelected)) {
					entityId = candidate.id;
					changed = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(text("Inspectorで開く", "Open Inspector"))) {
			revealInspector(entityId);
		}
		ImGui::PopID();
	};
	const int fishCountUpperBound = (std::max)(
		1,
		static_cast<int>((std::min)(
			director->fishingFishEntityIds.size(),
			static_cast<size_t>((std::numeric_limits<int>::max)())
		))
	);
	if (directorCanEdit && director->fishingMaxSelectableFishCount > fishCountUpperBound) {
		director->fishingMaxSelectableFishCount = fishCountUpperBound;
		changed = true;
	}

	if (ImGui::TreeNodeEx("Game###FishingConsoleGame", ImGuiTreeNodeFlags_DefaultOpen, text("ゲーム", "Game"))) {
		changed |= ImGui::DragFloat(text("制限時間（秒）", "Duration Seconds"), &director->fishingDurationSeconds, 0.1f, 0.1f, 3600.0f);
		changed |= ImGui::SliderInt(text("魚の最大数", "Maximum Fish Count"), &director->fishingMaxSelectableFishCount, 1, fishCountUpperBound);
		changed |= DrawSceneInputExpressionEditor(
			text("魚数決定入力", "Fish Count Confirm Input"),
			director->fishingConfirmInputExpression,
			director->fishingConfirmInput,
			editorLanguage_
		);
		changed |= ImGui::Checkbox(text("プレイ時にシードをランダム化", "Randomize Seed On Play"), &director->fishingRandomizeSeedOnPlay);
		changed |= ImGui::InputInt(text("ランダムシード", "Random Seed"), &director->fishingRandomSeed);
		drawReference(text("プレイヤー", "Player"), director->fishingPlayerEntityId, "PlayerBehavior");
		drawReference(text("水域", "Water Volume"), director->fishingWaterVolumeEntityId, "WaterVolume");
		drawReference(text("釣り針スポーン範囲", "Hook Spawn Area"), director->fishingHookSpawnAreaEntityId, "FishingHookSpawnArea");
		drawReference(text("釣り針プール", "Hook Pool"), director->fishingHookPoolEntityId, "FishingHookPool");
		int removeFishIndex = -1;
		for (size_t fishIndex = 0; fishIndex < director->fishingFishEntityIds.size(); ++fishIndex) {
			ImGui::PushID(static_cast<int>(fishIndex));
			drawReference(
				(text("魚 ", "Fish ") + std::to_string(fishIndex + 1)).c_str(),
				director->fishingFishEntityIds[fishIndex],
				"AgentBehavior"
			);
			ImGui::SameLine();
			if (ImGui::SmallButton(text("削除", "Remove"))) {
				removeFishIndex = static_cast<int>(fishIndex);
			}
			ImGui::PopID();
		}
		if (removeFishIndex >= 0) {
			director->fishingFishEntityIds.erase(
				director->fishingFishEntityIds.begin() + removeFishIndex
			);
			director->fishingMaxSelectableFishCount = (std::max)(
				1,
				(std::min)(
					director->fishingMaxSelectableFishCount,
					static_cast<int>(director->fishingFishEntityIds.size())
				)
			);
			changed = true;
		}
		if (ImGui::SmallButton(text("魚を追加", "Add Fish"))) {
			director->fishingFishEntityIds.push_back(0);
			changed = true;
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Score###FishingConsoleScore", ImGuiTreeNodeFlags_DefaultOpen, text("スコア", "Score"))) {
		changed |= ImGui::Checkbox(text("区間別釣り針設定を使用", "Use Hook Band Settings"), &director->fishingUseHookBandSettings);
		changed |= ImGui::DragFloat(text("釣り針スコア単位", "Hook Score Unit"), &director->fishingHookScoreUnit, 10.0f, 0.001f, 1000000000.0f);
		changed |= ImGui::DragFloat(text("魚数倍率（基礎）", "Fish Multiplier Base"), &director->fishingFishMultiplierBase, 0.05f, 0.0f, 100000.0f);
		changed |= ImGui::DragFloat(text("魚1匹追加ごとの倍率", "Fish Multiplier Per Additional Fish"), &director->fishingFishMultiplierPerAdditionalFish, 0.05f, 0.0f, 100000.0f);
		if (director->fishingUseHookBandSettings) {
			const size_t rankCountBefore = director->fishingHookRanks.size();
			if (directorCanEdit) {
				EnsureFishingHookRanks(*director);
			}
			if (rankCountBefore != director->fishingHookRanks.size()) {
				changed = true;
			}
			const bool ranksReady = director->fishingHookRanks.size() == 10;
			ImGui::SeparatorText(text("ランク定義", "Hook Rank Definitions"));
			if (!ranksReady) {
				ImGui::TextDisabled(
					"%s",
					text("ランク定義が10個未満です。編集モードで初期化してください。", "Fewer than ten rank definitions are available. Initialize them in Edit mode.")
				);
			}
			for (size_t tier = 0; ranksReady && tier < 10; ++tier) {
				ImGui::PushID(static_cast<int>(tier));
				SceneFishingHookRankDefinition& rank = director->fishingHookRanks[tier];
				ImGui::Text(text("ランク %zu", "Rank %zu"), tier + 1);
				changed |= InputTextString(text("ID", "Stable ID"), rank.id);
				changed |= InputTextString(text("名前", "Display Name"), rank.displayName);
				const char* currentModel = rank.modelPath.empty() ? "None" : rank.modelPath.c_str();
				if (ImGui::BeginCombo(text("モデル", "Model"), currentModel)) {
					if (ImGui::Selectable("None", rank.modelPath.empty())) {
						rank.modelPath.clear();
						changed = true;
					}
					for (const std::string& modelPath : GetCachedModelAssetPaths()) {
						if (ImGui::Selectable(modelPath.c_str(), rank.modelPath == modelPath)) {
							rank.modelPath = modelPath;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROJECT_MODEL_PATH")) {
						const char* droppedPath = static_cast<const char*>(payload->Data);
						if (droppedPath && droppedPath[0] != '\0') {
							rank.modelPath = GetModelPathRelativeToResources(droppedPath);
							changed = true;
						}
					}
					ImGui::EndDragDropTarget();
				}
				const char* currentIconTexture = rank.iconTexturePath.empty()
					? "None" : rank.iconTexturePath.c_str();
				if (ImGui::BeginCombo(text("アイコンテクスチャ", "Icon Texture"), currentIconTexture)) {
					if (ImGui::Selectable("None", rank.iconTexturePath.empty())) {
						rank.iconTexturePath.clear();
						changed = true;
					}
					for (const std::string& texturePath : GetCachedTextureAssetPaths()) {
						if (ImGui::Selectable(texturePath.c_str(), rank.iconTexturePath == texturePath)) {
							rank.iconTexturePath = texturePath;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROJECT_TEXTURE_PATH")) {
						const char* droppedPath = static_cast<const char*>(payload->Data);
						if (droppedPath && droppedPath[0] != '\0') {
							rank.iconTexturePath = GetProjectResourcePath(droppedPath);
							changed = true;
						}
					}
					ImGui::EndDragDropTarget();
				}
				changed |= ImGui::DragFloat(
					text("得点倍率", "Score Multiplier"), &rank.scoreMultiplier,
					0.05f, 0.0f, 100000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp
				);
				changed |= ImGui::ColorEdit4(text("色", "Color"), &rank.color.x);
				ImGui::PopID();
			}
		}
		const int maximumFish = (std::max)(
			1,
			(std::min)(director->fishingMaxSelectableFishCount, fishCountUpperBound)
		);
		fishingConsolePreviewFishCount_ = std::clamp(fishingConsolePreviewFishCount_, 1, maximumFish);
		ImGui::SliderInt(text("プレビュー魚数", "Preview Fish Count"), &fishingConsolePreviewFishCount_, 1, maximumFish);
		const double fishMultiplier = (std::max)(
			0.0,
			static_cast<double>(director->fishingFishMultiplierBase) +
			static_cast<double>(fishingConsolePreviewFishCount_ - 1) *
			static_cast<double>(director->fishingFishMultiplierPerAdditionalFish)
		);
		ImGui::TextDisabled(
			text("区間方式: 単位 × 区間倍率 × ランク得点倍率 × 魚数倍率 (%.3f)", "Band mode: unit x distance multiplier x rank score multiplier x fish multiplier (%.3f)"),
			fishMultiplier
		);
		if (ImGui::BeginTable(
			"FishingConsoleScoreTable", 11,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX
		)) {
			ImGui::TableSetupColumn("Band");
			for (int tier = 1; tier <= 10; ++tier) {
				const float scoreMultiplier = tier <= static_cast<int>(director->fishingHookRanks.size())
					? director->fishingHookRanks[static_cast<size_t>(tier - 1)].scoreMultiplier
					: 0.0f;
				const std::string rankName = tier <= static_cast<int>(director->fishingHookRanks.size()) &&
					!director->fishingHookRanks[static_cast<size_t>(tier - 1)].displayName.empty()
					? director->fishingHookRanks[static_cast<size_t>(tier - 1)].displayName
					: "R" + std::to_string(tier);
				const std::string label = rankName + " x" + std::to_string(scoreMultiplier);
				ImGui::TableSetupColumn(label.c_str());
			}
			ImGui::TableHeadersRow();
			for (size_t bandIndex = 0; bandIndex < director->fishingHookBands.size(); ++bandIndex) {
				const SceneFishingHookBandSettings& band = director->fishingHookBands[bandIndex];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("Band %zu (x%.2f)", bandIndex, band.distanceMultiplier);
				for (int tier = 1; tier <= 10; ++tier) {
					ImGui::TableSetColumnIndex(tier);
					const double rankScoreMultiplier = tier <= static_cast<int>(director->fishingHookRanks.size())
						? static_cast<double>(director->fishingHookRanks[
							static_cast<size_t>(tier - 1)
						].scoreMultiplier)
						: 0.0;
					const double score = static_cast<double>(director->fishingHookScoreUnit) *
						static_cast<double>(band.distanceMultiplier) *
						rankScoreMultiplier * fishMultiplier;
					ImGui::Text("%.1f", score);
				}
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	const char* bandsTitle = director->fishingUseHookBandSettings
		? text("区間ごとの釣り針設定", "Hook Settings by Band")
		: text("旧方式の区間設定", "Legacy Distance Settings");
	if (ImGui::TreeNodeEx("Bands###FishingConsoleBands", ImGuiTreeNodeFlags_DefaultOpen, bandsTitle)) {
		if (!director->fishingUseHookBandSettings) {
			changed |= ImGui::DragInt(text("区間数", "Distance Band Count"), &director->fishingDistanceBandCount, 1.0f, 1, 32);
			changed |= ImGui::SliderInt(text("区間ごとの釣り針数", "Hooks Per Distance Band"), &director->fishingHooksPerDistanceBand, 1, 4);
			changed |= ImGui::DragFloat(text("倍率の基準値", "Multiplier Base"), &director->fishingDistanceMultiplierBase, 0.05f, 0.0f, 100.0f);
			changed |= ImGui::DragFloat(text("倍率の増加値", "Multiplier Step"), &director->fishingDistanceMultiplierStep, 0.05f, 0.0f, 100.0f);
		}
		if (director->fishingUseHookBandSettings && ImGui::Button(text("推奨5区間設定を適用", "Apply Recommended 5-Band Template"))) {
			director->fishingHookBands = {
				{ 0.0f, 0, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
				{ 1.0f, 4, { 40.0f, 35.0f, 25.0f, 2.0f, 2.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f } },
				{ 1.2f, 3, { 8.0f, 8.0f, 8.0f, 24.0f, 24.0f, 24.0f, 2.0f, 2.0f, 1.0f, 1.0f } },
				{ 1.4f, 2, { 6.0f, 6.0f, 6.0f, 20.0f, 20.0f, 20.0f, 10.0f, 10.0f, 1.0f, 1.0f } },
				{ 1.6f, 2, { 3.0f, 3.0f, 3.0f, 10.0f, 10.0f, 10.0f, 24.0f, 24.0f, 4.0f, 9.0f } }
			};
			changed = true;
		}
		for (size_t bandIndex = 0;
			director->fishingUseHookBandSettings && bandIndex < director->fishingHookBands.size();
			++bandIndex) {
			SceneFishingHookBandSettings& band = director->fishingHookBands[bandIndex];
			ImGui::PushID(static_cast<int>(bandIndex));
			if (ImGui::TreeNodeEx("Band", ImGuiTreeNodeFlags_DefaultOpen, text("区間 %zu", "Band %zu"), bandIndex)) {
				changed |= ImGui::DragFloat(text("区間倍率", "Distance Multiplier"), &band.distanceMultiplier, 0.05f, 0.0f, 100.0f);
				changed |= ImGui::SliderInt(text("釣り針数", "Hook Count"), &band.hookCount, 0, 30);
				if (band.hookMultiplierWeights.size() != 10 && directorCanEdit) {
					band.hookMultiplierWeights.resize(10, 0.0f);
					changed = true;
				}
				float totalWeight = 0.0f;
				for (float weight : band.hookMultiplierWeights) totalWeight += (std::max)(weight, 0.0f);
				for (size_t tier = 0; tier < 10; ++tier) {
					const float weight = tier < band.hookMultiplierWeights.size() ? band.hookMultiplierWeights[tier] : 0.0f;
					if (tier < band.hookMultiplierWeights.size()) {
						changed |= ImGui::DragFloat(("x" + std::to_string(tier + 1)).c_str(), &band.hookMultiplierWeights[tier], 0.1f, 0.0f, 100000.0f);
					} else {
						ImGui::TextDisabled("x%zu unavailable", tier + 1);
					}
					ImGui::SameLine();
					ImGui::Text("%.1f%%", totalWeight > 0.0f ? (std::max)(weight, 0.0f) / totalWeight * 100.0f : 0.0f);
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("HookPool###FishingConsoleHookPool", ImGuiTreeNodeFlags_DefaultOpen, text("釣り針プール・釣り針別スコア", "Hook Pool & Per-Hook Score"))) {
		SceneEntity* poolEntity = document.FindEntity(director->fishingHookPoolEntityId);
		SceneComponent* pool = poolEntity ? FindComponent(*poolEntity, "FishingHookPool") : nullptr;
		if (!poolEntity || !pool || !pool->enabled) {
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", text("釣り針プールが未設定、または無効です。", "Hook Pool is not assigned or is disabled."));
		} else {
			for (size_t entryIndex = 0; entryIndex < pool->fishingHookPoolEntries.size(); ++entryIndex) {
				SceneFishingHookPoolEntry& entry = pool->fishingHookPoolEntries[entryIndex];
				ImGui::PushID(static_cast<int>(entryIndex));
				ImGui::Text(text("エントリ %zu", "Entry %zu"), entryIndex + 1);
				SceneEntity* hookEntity = document.FindEntity(entry.hookEntityId);
				SceneComponent* hook = hookEntity ? FindComponent(*hookEntity, "FishingHook") : nullptr;
				if (!hookEntity || !hook || !hook->enabled) {
					ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", text("FishingHook Entityがありません。", "Missing FishingHook Entity"));
				} else {
					ImGui::SameLine();
					ImGui::TextUnformatted(hookEntity->name.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(text("釣り針をInspectorで開く", "Open Hook Inspector"))) {
						revealInspector(hookEntity->id);
					}
					if (!director->fishingUseHookBandSettings) {
						ImGui::BeginDisabled(!canEditScene || hookEntity->locked);
						changed |= ImGui::DragInt(text("旧方式の基礎スコア", "Legacy Base Score"), &hook->fishingHookBaseScore, 10.0f, 0, 1000000000);
						ImGui::EndDisabled();
					}
				}
				if (!director->fishingUseHookBandSettings) {
					ImGui::BeginDisabled(!canEditScene || poolEntity->locked);
					for (size_t bandIndex = 0; bandIndex < entry.weightsByDistanceBand.size(); ++bandIndex) {
						changed |= ImGui::DragFloat(("Band " + std::to_string(bandIndex) + " Weight").c_str(), &entry.weightsByDistanceBand[bandIndex], 0.1f, 0.0f, 100000.0f);
					}
					ImGui::EndDisabled();
				} else {
					ImGui::TextDisabled("%s", text("区間方式ではHook Poolの旧式設定は使用されません。", "Legacy Hook Pool settings are unused in Band mode."));
				}
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Appearance###FishingConsoleAppearance", ImGuiTreeNodeFlags_DefaultOpen, text("釣り針の表示", "Hook Appearance"))) {
		changed |= ImGui::DragFloat(text("発光強度", "Color Emissive Intensity"), &director->fishingHookColorEmissiveIntensity, 0.05f, 0.0f, 100.0f);
		ImGui::TextDisabled(
			"%s",
			text("ランクごとの色は「スコア > ランク定義」で設定します。", "Per-rank colors are configured under Score > Hook Rank Definitions.")
		);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Sharks###FishingConsoleSharks", ImGuiTreeNodeFlags_DefaultOpen, text("サメ", "Sharks"))) {
		for (SceneEntity& entity : document.GetEntities()) {
			SceneComponent* shark = FindComponent(entity, "FishingShark");
			if (!shark || !shark->enabled) continue;
			ImGui::PushID(static_cast<int>(entity.id));
			ImGui::TextUnformatted(entity.name.c_str());
			ImGui::BeginDisabled(!canEditScene || entity.locked);
			changed |= ImGui::DragInt("Penalty Score", &shark->fishingSharkPenaltyScore, 10.0f, 0, 1000000000);
			changed |= ImGui::DragFloat("Hit Cooldown Seconds", &shark->fishingSharkHitCooldownSeconds, 0.05f, 0.0f, 3600.0f);
			changed |= ImGui::DragFloat("Path Randomness", &shark->fishingSharkPathRandomness, 0.01f, 0.0f, 1.0f);
			changed |= ImGui::DragFloat("Wander Move Speed", &shark->fishingSharkWanderMoveSpeed, 0.1f, 0.0f, 10000.0f);
			changed |= ImGui::DragFloat("Wander Maximum Turn Rate", &shark->fishingSharkWanderMaximumTurnRate, 0.05f, 0.0f, 1000.0f);
			changed |= ImGui::DragFloat("Obstacle Avoidance Distance", &shark->fishingSharkObstacleAvoidanceDistance, 0.1f, 0.0f, 10000.0f);
			changed |= ImGui::DragFloat("Obstacle Avoidance Strength", &shark->fishingSharkObstacleAvoidanceStrength, 0.01f, 0.0f, 1.0f);
			changed |= ImGui::DragFloat("Obstacle Avoidance Response", &shark->fishingSharkObstacleAvoidanceResponse, 0.1f, 0.0f, 1000.0f);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton(text("サメをInspectorで開く", "Open Shark Inspector"))) {
				revealInspector(entity.id);
			}
			ImGui::Separator();
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("HudFormation###FishingConsoleHudFormation", ImGuiTreeNodeFlags_DefaultOpen, text("HUD・群れ", "HUD & Formation"))) {
		changed |= ImGui::Checkbox(text("群れのアウトラインを表示", "Formation Outline Visible"), &director->fishingFormationOutlineVisible);
		changed |= ImGui::ColorEdit4(text("群れのアウトライン色", "Formation Outline Color"), &director->fishingFormationOutlineColor.x);
		changed |= ImGui::DragFloat(text("枠の発光強度", "Formation Outline Bloom Intensity"), &director->fishingFormationOutlineBloomIntensity, 0.1f, 0.0f, 32.0f);
		changed |= ImGui::DragFloat(text("群れのアウトラインYオフセット", "Formation Outline Y Offset"), &director->fishingFormationOutlineYOffset, 0.01f, -100.0f, 100.0f);
		changed |= ImGui::SliderInt(text("群れのアウトライン分割数", "Formation Outline Segments"), &director->fishingFormationOutlineSegments, 12, 128);
		changed |= InputTextString(text("魚数表示プレフィックス", "Fish Count Prefix"), director->fishingFishCountPrefix);
		changed |= InputTextString(text("時間表示プレフィックス", "Timer Prefix"), director->fishingTimerPrefix);
		changed |= InputTextString(text("スコア表示プレフィックス", "Score Prefix"), director->fishingScorePrefix);
		changed |= InputTextString(text("倍率表示プレフィックス", "Multiplier Prefix"), director->fishingMultiplierPrefix);
		ImGui::TreePop();
	}
	ImGui::EndDisabled();
	if (changed) document.MarkDirty();
	ImGui::End();
}

void ImGuiManager::DrawInputSettingsWindow() {
	if (ImGuiWindow* inspectorWindow = ImGui::FindWindowByName("Inspector")) {
		if (ImGuiDockNode* dockNode = inspectorWindow->DockNode) {
			ImGui::SetNextWindowDockID(dockNode->ID, ImGuiCond_FirstUseEver);
		}
	}
	ImGui::Begin(
		SelectEditorText(
			editorLanguage_,
			"入力設定###InputSettingsWindow",
			"Input Settings###InputSettingsWindow"
		),
		&showInputSettings_
	);
	if (!editorSession_) {
		ImGui::TextDisabled("%s", SelectEditorText(
			editorLanguage_,
			"Sceneエディターを利用できません。",
			"Scene editor is not available."
		));
		ImGui::End();
		return;
	}

	SceneDocument& document = editorSession_->GetEditDocument();
	const bool canEdit = editorSession_->IsEditing();
	ImGui::TextColored(
		canEdit
			? ImVec4(0.35f, 0.85f, 0.4f, 1.0f)
			: ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
		canEdit
			? SelectEditorText(editorLanguage_, "編集モード", "Edit mode")
			: SelectEditorText(editorLanguage_, "読み取り専用（プレイ／一時停止）", "Read-only (Play/Pause)")
	);
	ImGui::TextDisabled("%s", SelectEditorText(
		editorLanguage_,
		"入力式はグループ間と条件内でAny（OR）／All（AND）を個別に設定できます。",
		"Input expressions support independent Any (OR) / All (AND) modes between groups and within each group."
	));

	bool changed = false;
	for (SceneEntity& entity : document.GetEntities()) {
		if (entity.folder) {
			continue;
		}
		const std::string entityLabel = BuildEntityHierarchyLabel(document, entity);
		for (size_t componentIndex = 0; componentIndex < entity.components.size(); ++componentIndex) {
			SceneComponent& component = entity.components[componentIndex];
			if (!component.enabled) {
				continue;
			}
			ImGui::PushID(&entity);
			ImGui::PushID(static_cast<int>(componentIndex));
			const bool componentCanEdit = canEdit && !entity.locked;
			if (component.type == "EventTrigger") {
				bool hasInputBinding = false;
				for (const SceneEventBinding& binding : component.eventBindings) {
					hasInputBinding = hasInputBinding ||
						binding.triggerType == "OnKeyPressed" ||
						binding.triggerType == "OnFishingScoreAttackResultInput";
				}
				if (hasInputBinding && ImGui::TreeNodeEx(
					"EventTriggerInputs",
					ImGuiTreeNodeFlags_DefaultOpen,
					"%s / %s",
					entityLabel.c_str(),
					SelectEditorText(editorLanguage_, "Event入力", "Event Inputs")
				)) {
					ImGui::BeginDisabled(!componentCanEdit);
					for (size_t bindingIndex = 0; bindingIndex < component.eventBindings.size(); ++bindingIndex) {
						SceneEventBinding& binding = component.eventBindings[bindingIndex];
						if (binding.triggerType != "OnKeyPressed" &&
							binding.triggerType != "OnFishingScoreAttackResultInput") {
							continue;
						}
						ImGui::PushID(static_cast<int>(bindingIndex));
						changed |= DrawSceneInputExpressionEditor(
							binding.triggerType.c_str(),
							binding.inputExpression,
							binding.triggerKey,
							editorLanguage_
						);
						ImGui::PopID();
					}
					ImGui::EndDisabled();
					ImGui::TreePop();
				}
			} else if (component.type == "FishingScoreAttackDirector") {
				if (ImGui::TreeNodeEx(
					"FishingDirectorInputs",
					ImGuiTreeNodeFlags_DefaultOpen,
					"%s / %s",
					entityLabel.c_str(),
					SelectEditorText(editorLanguage_, "Fishing入力", "Fishing Inputs")
				)) {
					ImGui::BeginDisabled(!componentCanEdit);
					changed |= DrawSceneInputExpressionEditor(
						SelectEditorText(editorLanguage_, "魚数決定入力", "Fish Count Confirm Input"),
						component.fishingConfirmInputExpression,
						component.fishingConfirmInput,
						editorLanguage_
					);
					ImGui::EndDisabled();
					ImGui::TreePop();
				}
			} else if (component.type == "PlayerBehavior") {
				if (ImGui::TreeNodeEx(
					"PlayerInputs",
					ImGuiTreeNodeFlags_DefaultOpen,
					"%s / %s",
					entityLabel.c_str(),
					SelectEditorText(editorLanguage_, "Player入力", "Player Inputs")
				)) {
					ImGui::BeginDisabled(!componentCanEdit);
					if (ImGui::BeginCombo(
						SelectEditorText(editorLanguage_, "入力方式###InputSettingsMode", "Input Mode###InputSettingsMode"),
						component.playerInputMode.c_str()
					)) {
						for (const char* mode : { "KeyboardMouse", "Gamepad", "Both" }) {
							if (ImGui::Selectable(mode, component.playerInputMode == mode)) {
								component.playerInputMode = mode;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
					const bool gamepadMode = component.playerInputMode == "Gamepad" ||
						component.playerInputMode == "Both";
					ImGui::BeginDisabled(!gamepadMode);
					changed |= ImGui::SliderFloat(
						SelectEditorText(editorLanguage_, "ゲームパッドデッドゾーン###InputSettingsDeadzone", "Gamepad Deadzone###InputSettingsDeadzone"),
						&component.playerGamepadDeadzone,
						0.0f,
						0.95f
					);
					ImGui::EndDisabled();
					component.playerGamepadDeadzone = std::clamp(
						component.playerGamepadDeadzone,
						0.0f,
						0.95f
					);
					ImGui::EndDisabled();
					ImGui::TreePop();
				}
			} else if (component.type == "SceneTransition") {
				ImGui::TextDisabled(
					"%s: %s",
					entityLabel.c_str(),
					SelectEditorText(editorLanguage_, "旧SceneTransition入力（読み取り専用）", "Legacy SceneTransition input (read-only)")
				);
				ImGui::Text("%s", component.sceneTransitionTriggerKey.c_str());
			}
			ImGui::PopID();
			ImGui::PopID();
		}
	}
	if (changed) {
		document.MarkDirty();
	}
	ImGui::End();
}

void ImGuiManager::DrawConsoleWindow() {
	SystemPerformanceMonitor& performanceMonitor =
		SystemPerformanceMonitor::GetInstance();
	performanceMonitor.Update();
	const SystemPerformanceMonitor::Snapshot& performance =
		performanceMonitor.GetSnapshot();
	const ParticleManager::RuntimeStats& particleStats =
		ParticleManager::GetInstance()->GetRuntimeStats();
	const GpuParticle::RuntimeInfo& gpuParticleInfo =
		particleStats.gpuParticle;
	const float fps = ImGui::GetIO().Framerate;
	const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;

	ImGui::Begin("Console", &showConsole_);
	ImGui::TextColored(
		ImVec4(0.45f, 0.8f, 0.55f, 1.0f),
		"Ready"
	);
	ImGui::SameLine();
	ImGui::TextDisabled("%.1f FPS / %.3f ms", fps, frameMs);

	ImGui::SeparatorText("System Load");
	if (performance.cpuSupported) {
		ImGui::Text(
			"CPU: %.1f%% system / %.1f%% process",
			performance.systemCpuUsage,
			performance.processCpuUsage
		);
		ImGui::TextDisabled(
			"Process CPU: %.1f%% of one logical core / %u logical cores",
			performance.processCpuOneCoreUsage,
			performance.logicalProcessorCount
		);
	} else {
		ImGui::TextDisabled("CPU counters are collecting...");
	}

	if (performance.gpuSupported) {
		ImGui::Text(
			"GPU Engine: %.1f%% system / %.1f%% process",
			performance.gpuUsage,
			performance.processGpuUsage
		);
		ImGui::Text(
			"GPU 3D: %.1f%% system / %.1f%% process",
			performance.gpu3DUsage,
			performance.processGpu3DUsage
		);
		ImGui::Text(
			"GPU Compute: %.1f%% system / %.1f%% process",
			performance.gpuComputeUsage,
			performance.processGpuComputeUsage
		);
		ImGui::Text(
			"GPU Copy: %.1f%% system / %.1f%% process",
			performance.gpuCopyUsage,
			performance.processGpuCopyUsage
		);
		if (performance.gpuRawEngineUsage > 100.0f ||
			performance.processGpuRawEngineUsage > 100.0f) {
			ImGui::TextDisabled(
				"Raw GPU engine sum: %.1f%% system / %.1f%% process",
				performance.gpuRawEngineUsage,
				performance.processGpuRawEngineUsage
			);
		}
		ImGui::TextDisabled(
			"GPU engines sampled: %u system / %u process",
			performance.gpuEngineSampleCount,
			performance.processGpuEngineSampleCount
		);
	} else {
		ImGui::TextDisabled("%s", performance.gpuStatus.c_str());
	}

	ImGui::SeparatorText("Particles");
	ImGui::Text(
		"Particle CPU Update: %.3f ms / Total: %.3f ms",
		particleStats.cpuParticleUpdateMs,
		particleStats.totalParticleUpdateMs
	);
	ImGui::Text(
		"CPU Particles: %u active / %u instanced",
		particleStats.cpuParticleActiveCount,
		particleStats.cpuParticleInstanceCount
	);
	ImGui::Text(
		"GpuParticle CPU Update: %.3f ms",
		particleStats.gpuParticleCpuUpdateMs
	);
	ImGui::Text(
		"GpuParticle: %s / %s / %u instances",
		particleStats.gpuParticleEnabled ? "enabled" : "disabled",
		gpuParticleInfo.initialized ? "initialized" : "not initialized",
		particleStats.gpuParticleInstanceCount
	);
	ImGui::Text(
		"GpuParticle Emit: %u / Max: %u / Flags: 0x%X",
		gpuParticleInfo.emitCount,
		gpuParticleInfo.maxParticles,
		gpuParticleInfo.emitFlags
	);
	ImGui::Text(
		"GpuParticle Frequency: %.3f / Timer: %.3f",
		gpuParticleInfo.frequency,
		gpuParticleInfo.frequencyTime
	);
	ImGui::End();
}

bool ImGuiManager::IsSceneViewInputActive() {
	return sceneViewInputActive_;
}
