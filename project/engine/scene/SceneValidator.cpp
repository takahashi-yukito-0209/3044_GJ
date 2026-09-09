// 役割: Entity ID、Hierarchy、Entity参照、Scene遷移先の整合性を検証する。
#include "SceneValidator.h"

#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneEntityQuery.h"
#include "SceneInputKey.h"
#include "SceneTransformResolver.h"
#include "../Audio/Audio.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
	bool HasErrors(const std::vector<SceneValidationIssue>& issues) {
		for (const SceneValidationIssue& issue : issues) {
			if (issue.severity == SceneValidationSeverity::Error) {
				return true;
			}
		}
		return false;
	}

	float WorldAxisScale(const Matrix4x4& matrix, uint32_t axis) {
		return std::sqrt(
			matrix.m[axis][0] * matrix.m[axis][0] +
			matrix.m[axis][1] * matrix.m[axis][1] +
			matrix.m[axis][2] * matrix.m[axis][2]
		);
	}

	bool IsValidAudioSpatialMode(const std::string& mode) {
		return mode == "TwoD" || mode == "ThreeD" ||
			mode == "ThreeDPointDownmix" || mode == "ThreeDStereoArea";
	}

	bool IsValidPauseDomain(const std::string& domain) {
		return domain == "Gameplay" || domain == "Physics" ||
			domain == "GameplayInput" || domain == "WorldAnimation" ||
			domain == "WorldEffects" || domain == "Audio";
	}

	bool IsValidScreenOverlayScaleMode(const std::string& mode) {
		return mode == "Inherit" || mode == "Legacy" ||
			mode == "Fit" || mode == "Cover";
	}

	std::string DescribeAudioChannelMismatch(
		const std::string& spatialMode,
		uint32_t channelCount
	) {
		if (spatialMode == "ThreeD") {
			if (channelCount == 2) {
				return "AudioSource clip has 2 channels, but ThreeD Point requires mono. "
					"Use ThreeD Point Downmix or ThreeD Stereo Area.";
			}
			return "AudioSource clip has " + std::to_string(channelCount) +
				" channels, but ThreeD Point requires mono.";
		}
		if (channelCount == 1) {
			return "AudioSource clip has mono, but " + spatialMode +
				" requires stereo. Use ThreeD Point.";
		}
		return "AudioSource clip has " + std::to_string(channelCount) +
			" channels, but " + spatialMode + " requires stereo.";
	}

	bool SamePathComponent(
		const std::filesystem::path& left,
		const std::filesystem::path& right
	) {
		std::wstring leftValue = left.wstring();
		std::wstring rightValue = right.wstring();
		std::transform(leftValue.begin(), leftValue.end(), leftValue.begin(), std::towlower);
		std::transform(rightValue.begin(), rightValue.end(), rightValue.begin(), std::towlower);
		return leftValue == rightValue;
	}

	bool IsPathWithin(
		const std::filesystem::path& root,
		const std::filesystem::path& candidate
	) {
		const std::filesystem::path normalizedRoot = root.lexically_normal();
		const std::filesystem::path normalizedCandidate = candidate.lexically_normal();
		auto rootIt = normalizedRoot.begin();
		auto candidateIt = normalizedCandidate.begin();
		const auto rootEnd = normalizedRoot.end();
		const auto candidateEnd = normalizedCandidate.end();
		for (; rootIt != rootEnd; ++rootIt, ++candidateIt) {
			if (candidateIt == candidateEnd || !SamePathComponent(*rootIt, *candidateIt)) {
				return false;
			}
		}
		return true;
	}

	bool IsResourceFontPathShape(const std::string& value) {
		const std::filesystem::path path = StringUtility::ToPath(value);
		if (path.empty() || path.is_absolute() || path.has_root_name()) {
			return false;
		}
		for (const std::filesystem::path& component : path) {
			if (component == "..") {
				return false;
			}
		}
		const std::filesystem::path normalizedPath = path.lexically_normal();
		auto iterator = normalizedPath.begin();
		const auto end = normalizedPath.end();
		if (iterator == end || !SamePathComponent(*iterator, std::filesystem::path("resources"))) {
			return false;
		}
		++iterator;
		if (iterator == end || !SamePathComponent(*iterator, std::filesystem::path("fonts"))) {
			return false;
		}
		const std::wstring extension = path.extension().wstring();
		std::wstring lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), std::towlower);
		return normalizedPath.filename() != normalizedPath &&
			(lowerExtension == L".ttf" || lowerExtension == L".otf" || lowerExtension == L".ttc");
	}

	bool IsResourceRelativeModelPath(const std::string& path) {
		if (path.empty()) {
			return true;
		}
		return path.find(':') == std::string::npos &&
			path.front() != '/' && path.front() != '\\' &&
			path.find("..") == std::string::npos;
	}

	template <typename AddIssue>
	void ValidateSceneInputExpression(
		const SceneInputExpression& expression,
		uint64_t entityId,
		const std::string& label,
		AddIssue&& addIssue
	) {
		if (expression.mode != "Any" && expression.mode != "All") {
			addIssue(
				SceneValidationSeverity::Error,
				entityId,
				label + " has an unsupported expression mode: " + expression.mode
			);
		}
		if (expression.groups.empty()) {
			addIssue(
				SceneValidationSeverity::Warning,
				entityId,
				label + " has no input groups and is disabled"
			);
		}
		for (size_t groupIndex = 0;
			groupIndex < expression.groups.size();
			++groupIndex) {
			const SceneInputGroup& group = expression.groups[groupIndex];
			if (group.mode != "Any" && group.mode != "All") {
				addIssue(
					SceneValidationSeverity::Error,
					entityId,
					label + " group " + std::to_string(groupIndex) +
						" has an unsupported mode: " + group.mode
				);
			}
			if (group.terms.empty()) {
				addIssue(
					SceneValidationSeverity::Warning,
					entityId,
					label + " group " + std::to_string(groupIndex) +
						" has no terms and is disabled"
				);
			}
			std::unordered_set<std::string> terms;
			int pressedTermCount = 0;
			for (const SceneInputTerm& term : group.terms) {
				if (!IsSupportedSceneInput(term.input)) {
					addIssue(
						SceneValidationSeverity::Error,
						entityId,
						label + " has an unsupported input: " + term.input
					);
				}
				if (term.phase != "Pressed" && term.phase != "Held") {
					addIssue(
						SceneValidationSeverity::Error,
						entityId,
						label + " has an unsupported input phase: " + term.phase
					);
				}
				if (term.phase == "Pressed") {
					++pressedTermCount;
				}
				const std::string duplicateKey = term.input + "\n" + term.phase;
				if (!terms.insert(duplicateKey).second) {
					addIssue(
						SceneValidationSeverity::Warning,
						entityId,
						label + " contains a duplicate input term: " + term.input
					);
				}
			}
			if (group.mode == "All" && pressedTermCount > 1) {
				addIssue(
					SceneValidationSeverity::Warning,
					entityId,
					label + " group " + std::to_string(groupIndex) +
						" requires multiple Pressed inputs in one frame"
				);
			}
		}
	}
}

bool SceneValidator::ValidateDocument(
	const SceneDocument& document,
	const SceneCatalog* catalog,
	const std::string& sceneId,
	const std::string& filePath,
	std::vector<SceneValidationIssue>& issues
) {
	const size_t firstIssueIndex = issues.size();
	auto addIssue = [
		&issues,
		&sceneId,
		&filePath
	](SceneValidationSeverity severity, uint64_t entityId, std::string message) {
		issues.push_back({ severity, sceneId, filePath, entityId, std::move(message) });
	};

	std::unordered_map<uint64_t, const SceneEntity*> entitiesById;
	std::unordered_map<uint64_t, size_t> entityOrderById;
	for (size_t entityIndex = 0; entityIndex < document.GetEntities().size(); ++entityIndex) {
		const SceneEntity& entity = document.GetEntities()[entityIndex];
		entityOrderById.emplace(entity.id, entityIndex);
		if (entity.id == 0) {
			addIssue(
				SceneValidationSeverity::Error,
				0,
				"Entity ID must be greater than zero"
			);
			continue;
		}
		if (!entitiesById.emplace(entity.id, &entity).second) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Duplicate Entity ID"
			);
		}
	}

	std::unordered_set<uint64_t> reportedCycles;
	uint32_t directionalLightCount = 0;
	uint32_t pointLightCount = 0;
	uint32_t spotLightCount = 0;
	uint32_t spotShadowCount = 0;
	uint32_t cameraSwitcherCount = 0;
	uint32_t activeAudioListenerCount = 0;
	uint64_t firstAudioListenerEntityId = 0;
	uint32_t persistentBgmPlayOnStartCount = 0;
	uint64_t firstPersistentBgmEntityId = 0;
	uint32_t fishingDirectorCount = 0;
	uint64_t firstFishingDirectorEntityId = 0;
	uint32_t fishingResultPresenterCount = 0;
	uint64_t firstFishingResultPresenterEntityId = 0;
	uint32_t screenOverlayCanvasCount = 0;
	uint64_t firstScreenOverlayCanvasEntityId = 0;
	uint32_t pauseControllerCount = 0;
	std::unordered_map<uint64_t, std::unordered_set<uint64_t>> prefabLocalIds;
	std::unordered_map<std::string, uint64_t> activeLeaderControllers;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		for (const SceneComponent& component : entity.components) {
			if (!component.enabled || component.type != "ScreenOverlayCanvas") {
				continue;
			}
			++screenOverlayCanvasCount;
			if (firstScreenOverlayCanvasEntityId == 0) {
				firstScreenOverlayCanvasEntityId = entity.id;
			}
			if (!std::isfinite(component.screenOverlayCanvasReferenceSize.x) ||
				!std::isfinite(component.screenOverlayCanvasReferenceSize.y) ||
				component.screenOverlayCanvasReferenceSize.x <= 0.0f ||
				component.screenOverlayCanvasReferenceSize.y <= 0.0f) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"ScreenOverlayCanvas referenceSize must contain finite positive values"
				);
			}
		}
	}
	for (const SceneTeamSettings& team : document.GetTeams()) {
		if (!team.agentFormationCapsuleEnabled) {
			if (team.agentFormationCapsuleScaleWithActiveMembers) {
				addIssue(
					SceneValidationSeverity::Error,
					0,
					"Dynamic Team formation capsule scaling requires an enabled formation capsule: " + team.name
				);
			}
			continue;
		}
		if (!std::isfinite(team.agentFormationCapsuleRadius) ||
			team.agentFormationCapsuleRadius <= 0.0f ||
			!std::isfinite(team.agentFormationCapsuleHalfSegmentLength) ||
			team.agentFormationCapsuleHalfSegmentLength < 0.0f) {
			addIssue(
				SceneValidationSeverity::Error,
				0,
				"Enabled Team formation capsule has invalid dimensions: " + team.name
			);
		}
	}
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		for (const SceneComponent& component : entity.components) {
			if (!component.enabled || component.type != "AgentTeamLeaderController") {
				continue;
			}
			const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
			if (!team || team->name.empty()) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"AgentTeamLeaderController requires an owning Team"
				);
				continue;
			}
			const bool inserted = activeLeaderControllers.emplace(
				team->name,
				entity.id
			).second;
			if (!inserted) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Team has multiple active AgentTeamLeaderController Entities: " +
						team->name
				);
			}
		}
	}
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!entity.teamName.empty() && !document.FindTeam(entity.teamName)) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Entity references an unknown Team: " + entity.teamName
			);
		}
		if (entity.parentId == entity.id && entity.id != 0) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Entity cannot be its own parent"
			);
		} else if (entity.parentId != 0 && !entitiesById.contains(entity.parentId)) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Parent Entity does not exist: " + std::to_string(entity.parentId)
			);
		}
		std::unordered_set<uint64_t> componentLocalIds;
		for (const SceneComponent& component : entity.components) {
			if (component.localId == 0) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Component local ID must be greater than zero: " +
						component.type
				);
			} else if (!componentLocalIds.insert(component.localId).second) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Entity contains a duplicate Component local ID: " +
						std::to_string(component.localId)
				);
			}
		}
		const bool hasPrefabMetadata =
			!entity.prefabLinks.empty() ||
			!entity.prefabAssetId.empty() ||
			!entity.prefabSourcePath.empty() ||
			entity.prefabInstanceRootId != 0 ||
			entity.prefabLocalId != 0;
		if (hasPrefabMetadata) {
			if (
				(
					entity.prefabAssetId.empty() &&
					entity.prefabSourcePath.empty()
				) ||
				entity.prefabInstanceRootId == 0 ||
				entity.prefabLocalId == 0
			) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Prefab instance metadata is incomplete"
				);
			} else {
				const auto root = entitiesById.find(entity.prefabInstanceRootId);
				if (root == entitiesById.end()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance root does not exist: " +
						std::to_string(entity.prefabInstanceRootId)
					);
				} else {
					const bool assetLinkMatches = !entity.prefabAssetId.empty()
						? root->second->prefabAssetId == entity.prefabAssetId
						: root->second->prefabAssetId.empty() &&
							root->second->prefabSourcePath ==
								entity.prefabSourcePath;
					if (
						root->second->prefabInstanceRootId !=
							entity.prefabInstanceRootId ||
						!assetLinkMatches
					) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance root metadata does not match"
					);
					}
				}
				if (!prefabLocalIds[entity.prefabInstanceRootId].insert(
					entity.prefabLocalId
				).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance contains a duplicate local Entity ID"
					);
				}
			}
		}
		if (!entity.prefabLinks.empty()) {
			const ScenePrefabLink& active = entity.prefabLinks.front();
			if (
				active.assetId != entity.prefabAssetId ||
				active.sourcePath != entity.prefabSourcePath ||
				active.instanceRootId != entity.prefabInstanceRootId ||
				active.localId != entity.prefabLocalId
			) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Active Prefab link does not match compatibility metadata"
				);
			}
			for (size_t linkIndex = 1;
				linkIndex < entity.prefabLinks.size();
				++linkIndex) {
				const ScenePrefabLink& link = entity.prefabLinks[linkIndex];
				if (
					(link.assetId.empty() && link.sourcePath.empty()) ||
					link.instanceRootId == 0 ||
					link.localId == 0
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab link metadata is incomplete"
					);
					continue;
				}
				const auto root = entitiesById.find(link.instanceRootId);
				if (root == entitiesById.end()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab instance root does not exist: " +
						std::to_string(link.instanceRootId)
					);
				} else {
					const auto rootLink = std::find_if(
						root->second->prefabLinks.begin(),
						root->second->prefabLinks.end(),
						[&link](const ScenePrefabLink& candidate) {
							return candidate.instanceRootId ==
								link.instanceRootId;
						}
					);
					const bool assetMatches =
						rootLink != root->second->prefabLinks.end() &&
						(!link.assetId.empty()
							? rootLink->assetId == link.assetId
							: rootLink->assetId.empty() &&
								rootLink->sourcePath == link.sourcePath);
					if (!assetMatches) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Nested Prefab root metadata does not match"
						);
					}
				}
				if (!prefabLocalIds[link.instanceRootId].insert(
					link.localId
				).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab contains a duplicate local Entity ID"
					);
				}
			}
		}

		std::unordered_set<uint64_t> visited;
		const SceneEntity* current = &entity;
		while (current && current->parentId != 0) {
			if (!visited.insert(current->id).second) {
				if (reportedCycles.insert(entity.id).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Hierarchy contains a parent cycle"
					);
				}
				break;
			}
			const auto parent = entitiesById.find(current->parentId);
			current = parent == entitiesById.end() ? nullptr : parent->second;
		}

		bool activeInHierarchy = entity.active;
		std::unordered_set<uint64_t> activeVisited;
		const SceneEntity* activeParent = &entity;
		while (activeInHierarchy && activeParent && activeParent->parentId != 0) {
			if (!activeVisited.insert(activeParent->id).second) {
				activeInHierarchy = false;
				break;
			}
			const auto parent = entitiesById.find(activeParent->parentId);
			activeParent = parent == entitiesById.end() ? nullptr : parent->second;
			if (activeParent && !activeParent->active) {
				activeInHierarchy = false;
			}
		}
		for (const SceneComponent& component : entity.components) {
			auto validateEntityReference = [
				&entitiesById,
				&addIssue,
				&entity
			](uint64_t targetId, const char* label) {
				if (targetId != 0 && !entitiesById.contains(targetId)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						std::string(label) + " Entity does not exist: " +
						std::to_string(targetId)
					);
				}
			};
			if (component.type == "PauseController") {
				if (component.enabled && activeInHierarchy &&
					++pauseControllerCount > 1) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Scene can contain only one active PauseController"
					);
				}
				std::unordered_set<std::string> profileIds;
				for (const ScenePauseProfile& profile : component.pauseProfiles) {
					if (profile.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PauseController contains an empty Profile Id"
						);
					} else if (!profileIds.insert(profile.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PauseController contains a duplicate Profile Id: " +
								profile.id
						);
					}
					if (profile.label.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"PauseController Profile has an empty label: " +
								profile.id
						);
					}
					std::unordered_set<std::string> domains;
					for (const std::string& domain : profile.pausedDomains) {
						if (!IsValidPauseDomain(domain)) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"PauseController Profile has an unsupported domain: " + domain
							);
						} else if (!domains.insert(domain).second) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"PauseController Profile contains a duplicate domain: " + domain
							);
						}
					}
				}
			} else if (component.type == "ProcessPolicy") {
				if (
					component.processMode != "Inherit" &&
					component.processMode != "Pausable" &&
					component.processMode != "WhenPaused" &&
					component.processMode != "Always" &&
					component.processMode != "Disabled"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ProcessPolicy has an unsupported processMode: " +
							component.processMode
					);
				}
			} else if (component.type == "MeshRenderer") {
				if (
					!std::isfinite(component.meshVisualRotation.x) ||
					!std::isfinite(component.meshVisualRotation.y) ||
					!std::isfinite(component.meshVisualRotation.z)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"MeshRenderer visualRotation must contain finite values"
					);
				}
			} else if (component.type == "AudioSource") {
				const bool validSpatialMode = IsValidAudioSpatialMode(
					component.audioSpatialMode
				);
				if (!validSpatialMode) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"AudioSource has an invalid spatialMode: " +
							component.audioSpatialMode
					);
				}
				if (
					component.audioSpatialMode != "TwoD" &&
					(
						component.audioMinimumDistance < 0.0f ||
						component.audioMinimumDistance >= component.audioMaximumDistance
					)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"3D AudioSource minimumDistance must be >= 0 and below maximumDistance"
					);
				}
				if (
					component.audioSpatialMode == "ThreeDStereoArea" &&
					(!std::isfinite(component.audioStereoAreaWidth) ||
						component.audioStereoAreaWidth <= 0.0f)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThreeD Stereo Area width must be finite and greater than zero"
					);
				}
				const bool streamCompatibilityInvalid =
					component.audioStreamFromDisk &&
					(component.audioSpatialMode != "TwoD" || component.audioBus != "BGM");
				if (streamCompatibilityInvalid) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Stream From Disk requires TwoD spatial mode and BGM Bus"
					);
				}
				if (
					validSpatialMode && component.audioSpatialMode != "TwoD" &&
					!component.audioClipPath.empty() && !streamCompatibilityInvalid
				) {
					if (Audio* audio = Audio::GetInstance();
						audio && audio->CanProbeAudioFileMetadata()) {
						AudioFileMetadata metadata{};
						std::string metadataError;
						if (!audio->TryGetAudioFileMetadata(
							component.audioClipPath.c_str(), metadata, &metadataError
						)) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"AudioSource clip metadata could not be read: " + metadataError
							);
						} else {
							const uint32_t requiredChannels =
								component.audioSpatialMode == "ThreeD" ? 1 : 2;
							if (metadata.channelCount != requiredChannels) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									DescribeAudioChannelMismatch(
										component.audioSpatialMode,
										metadata.channelCount
									)
								);
							}
						}
					}
				}
				if (component.audioPersistAcrossScenes && !component.audioStreamFromDisk) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Persist Across Scenes requires Stream From Disk"
					);
				}
				if (!std::isfinite(component.audioBgmFadeSeconds) || component.audioBgmFadeSeconds < 0.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"BGM Fade Seconds must be finite and greater than or equal to zero"
					);
				}
				if (
					component.enabled && activeInHierarchy &&
					component.audioPlayOnStart && component.audioPersistAcrossScenes &&
					component.audioStreamFromDisk &&
					component.audioSpatialMode == "TwoD" && component.audioBus == "BGM"
				) {
					++persistentBgmPlayOnStartCount;
					if (firstPersistentBgmEntityId == 0) {
						firstPersistentBgmEntityId = entity.id;
					}
				}
			} else if (component.type == "AudioListener") {
				if (
					component.audioListenerMode != "ActiveCamera" &&
					component.audioListenerMode != "Entity" &&
					component.audioListenerMode != "Hybrid"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"AudioListener has an invalid mode: " +
							component.audioListenerMode
					);
				}
				if (component.enabled && activeInHierarchy) {
					++activeAudioListenerCount;
					if (firstAudioListenerEntityId == 0) {
						firstAudioListenerEntityId = entity.id;
					}
				}
			} else if (component.type == "SpriteRenderer") {
				if (
					component.spriteRenderSpace != "ScreenOverlay" &&
					component.spriteRenderSpace != "Scene2D"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SpriteRenderer has an unknown renderSpace: " +
							component.spriteRenderSpace
					);
				}
				if (!IsValidScreenOverlayScaleMode(component.screenOverlayScaleMode)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SpriteRenderer has an unknown screenOverlayScaleMode: " +
							component.screenOverlayScaleMode
					);
				} else if (component.spriteRenderSpace == "Scene2D" &&
					(component.screenOverlayScaleMode == "Fit" ||
						component.screenOverlayScaleMode == "Cover")) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SpriteRenderer Fit/Cover scale mode requires ScreenOverlay renderSpace"
					);
				} else if (component.spriteRenderSpace == "ScreenOverlay" &&
					(component.screenOverlayScaleMode == "Fit" ||
						component.screenOverlayScaleMode == "Cover") &&
					screenOverlayCanvasCount == 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SpriteRenderer explicit Fit/Cover scale mode requires an active ScreenOverlayCanvas"
					);
				}
			} else if (component.type == "TextRenderer") {
				if (
					component.textRenderSpace != "ScreenOverlay" &&
					component.textRenderSpace != "Scene2D"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown renderSpace: " +
						component.textRenderSpace
					);
				}
				if (component.screenOverlayScaleMode != "Inherit" &&
					component.screenOverlayScaleMode != "Legacy" &&
					component.screenOverlayScaleMode != "Fit") {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown screenOverlayScaleMode: " +
							component.screenOverlayScaleMode
					);
				} else if (component.textRenderSpace == "Scene2D" &&
					component.screenOverlayScaleMode == "Fit") {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer Fit scale mode requires ScreenOverlay renderSpace"
					);
				} else if (component.textRenderSpace == "ScreenOverlay" &&
					component.screenOverlayScaleMode == "Fit" &&
					screenOverlayCanvasCount == 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer explicit Fit scale mode requires an active ScreenOverlayCanvas"
					);
				}
				if (component.textFontFamily.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"TextRenderer fontFamily is empty; runtime fallback will be used"
					);
				}
				if (component.textFontSize < 1.0f || component.textFontSize > 512.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer fontSize must be between 1 and 512"
					);
				}
				if (
					component.textFontWeight != "Regular" &&
					component.textFontWeight != "Bold"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown fontWeight: " +
							component.textFontWeight
					);
				}
				if (
					component.textFontStyle != "Normal" &&
					component.textFontStyle != "Italic"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown fontStyle: " +
							component.textFontStyle
					);
				}
				if (
					component.textHorizontalAlignment != "Left" &&
					component.textHorizontalAlignment != "Center" &&
					component.textHorizontalAlignment != "Right"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown horizontalAlignment");
				}
				if (
					component.textVerticalAlignment != "Top" &&
					component.textVerticalAlignment != "Center" &&
					component.textVerticalAlignment != "Bottom"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown verticalAlignment");
				}
				if (
					component.textWrapMode != "NoWrap" &&
					component.textWrapMode != "Word"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown wrapMode");
				}
				if (
					component.textOverflowMode != "Overflow" &&
					component.textOverflowMode != "Clip" &&
					component.textOverflowMode != "Ellipsis"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown overflowMode");
				}
				if (
					component.textLayoutSize.x < 0.0f ||
					component.textLayoutSize.y < 0.0f ||
					component.textLineSpacing < 0.1f ||
					component.textOpacity < 0.0f || component.textOpacity > 1.0f
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
					"TextRenderer contains an invalid layout or opacity value");
				}
				if (component.textHasPlacementProfiles) {
					const auto validPlacement = [](const Text2DPlacement& placement, bool overlay) {
						return placement.scale.x > 0.0f && placement.scale.y > 0.0f &&
							(!overlay || (placement.viewportAnchor.x >= 0.0f && placement.viewportAnchor.x <= 1.0f &&
								placement.viewportAnchor.y >= 0.0f && placement.viewportAnchor.y <= 1.0f));
					};
					if (!validPlacement(component.textOverlayPlacement, true) ||
						!validPlacement(component.textScene2DPlacement, false)) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextRenderer contains an invalid 2D placement profile");
					}
				}
			} else if (component.type == "TextMotion") {
				const SceneComponent* textRenderer =
					SceneEntityQuery::FindEnabledComponent(entity, "TextRenderer");
				if (!textRenderer) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextMotion requires an enabled TextRenderer on the same Entity"
					);
				}
				std::unordered_set<std::string> clipIds;
				for (const SceneTextMotionClip& clip : component.textMotionClips) {
					if (clip.id.empty() || !clipIds.insert(clip.id).second) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion contains an empty or duplicate clip Id");
					}
					if (clip.keyframes.size() < 2) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion clip requires at least two keyframes: " + clip.id);
					}
					float previousTime = -1.0f;
					for (const SceneTextMotionKeyframe& keyframe : clip.keyframes) {
						const bool validEasing =
							keyframe.easingToNext == "Linear" ||
							keyframe.easingToNext == "EaseIn" ||
							keyframe.easingToNext == "EaseOut" ||
							keyframe.easingToNext == "EaseInOut" ||
							keyframe.easingToNext == "SmoothStep";
						if (!std::isfinite(keyframe.timeSeconds) ||
							!std::isfinite(keyframe.positionOffset.x) ||
							!std::isfinite(keyframe.positionOffset.y) ||
							!std::isfinite(keyframe.rotationOffset) ||
							!std::isfinite(keyframe.scaleMultiplier.x) ||
							!std::isfinite(keyframe.scaleMultiplier.y) ||
							!std::isfinite(keyframe.opacityMultiplier) ||
							keyframe.timeSeconds < 0.0f ||
							keyframe.timeSeconds <= previousTime ||
							keyframe.scaleMultiplier.x <= 0.0f ||
							keyframe.scaleMultiplier.y <= 0.0f ||
							keyframe.opacityMultiplier < 0.0f ||
							keyframe.opacityMultiplier > 1.0f || !validEasing) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"TextMotion clip contains an invalid keyframe: " + clip.id);
							break;
						}
						previousTime = keyframe.timeSeconds;
					}
					if (!clip.keyframes.empty() &&
						(clip.keyframes.front().timeSeconds != 0.0f ||
							clip.keyframes.back().timeSeconds <= 0.0f)) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion clip must start at zero and end after zero: " +
								clip.id);
					}
				}
			} else if (component.type == "SpriteMotion") {
				const SceneComponent* spriteRenderer =
					SceneEntityQuery::FindEnabledComponent(entity, "SpriteRenderer");
				if (!spriteRenderer) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SpriteMotion requires an enabled SpriteRenderer on the same Entity"
					);
				}
				std::unordered_set<std::string> clipIds;
				bool startClipFound = false;
				for (const SceneSpriteMotionClip& clip : component.spriteMotionClips) {
					if (clip.id.empty() || !clipIds.insert(clip.id).second) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"SpriteMotion contains an empty or duplicate clip Id");
					}
					if (clip.id == component.spriteMotionStartClipId) {
						startClipFound = true;
					}
					if (clip.keyframes.size() < 2) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"SpriteMotion clip requires at least two keyframes: " + clip.id);
					}
					float previousTime = -1.0f;
					for (const SceneSpriteMotionKeyframe& keyframe : clip.keyframes) {
						const bool validEasing =
							keyframe.easingToNext == "Linear" ||
							keyframe.easingToNext == "EaseIn" ||
							keyframe.easingToNext == "EaseOut" ||
							keyframe.easingToNext == "EaseInOut" ||
							keyframe.easingToNext == "SmoothStep";
						if (!std::isfinite(keyframe.timeSeconds) ||
							!std::isfinite(keyframe.positionOffset.x) ||
							!std::isfinite(keyframe.positionOffset.y) ||
							!std::isfinite(keyframe.rotationOffset) ||
							!std::isfinite(keyframe.scaleMultiplier.x) ||
							!std::isfinite(keyframe.scaleMultiplier.y) ||
							!std::isfinite(keyframe.opacityMultiplier) ||
							keyframe.timeSeconds < 0.0f ||
							keyframe.timeSeconds <= previousTime ||
							keyframe.scaleMultiplier.x <= 0.0f ||
							keyframe.scaleMultiplier.y <= 0.0f ||
							keyframe.opacityMultiplier < 0.0f ||
							keyframe.opacityMultiplier > 1.0f || !validEasing) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"SpriteMotion clip contains an invalid keyframe: " + clip.id);
							break;
						}
						previousTime = keyframe.timeSeconds;
					}
					if (!clip.keyframes.empty() &&
						(clip.keyframes.front().timeSeconds != 0.0f ||
							clip.keyframes.back().timeSeconds <= 0.0f)) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"SpriteMotion clip must start at zero and end after zero: " +
								clip.id);
					}
				}
				if (component.spriteMotionPlayOnStart &&
					(component.spriteMotionStartClipId.empty() || !startClipFound)) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"SpriteMotion playOnStart requires a valid startClipId");
				}
			} else if (component.type == "Light") {
				if (
					component.lightType != "Directional" &&
					component.lightType != "Point" &&
					component.lightType != "Spot"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Light has an unknown lightType: " + component.lightType
					);
					continue;
				}
				if (!component.enabled || !activeInHierarchy) {
					continue;
				}
				if (component.lightType == "Directional") {
					++directionalLightCount;
					if (directionalLightCount > 1) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first active Directional Light is rendered"
						);
					}
				} else if (component.lightType == "Point") {
					++pointLightCount;
					if (pointLightCount > 16) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first 16 active Point Lights are rendered"
						);
					}
				} else {
					++spotLightCount;
					if (spotLightCount > 8) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first 8 active Spot Lights are rendered"
						);
					}
					if (component.lightCastsShadow) {
						++spotShadowCount;
						if (spotShadowCount > 4) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Only the first 4 active Spot Light shadows are rendered"
							);
						}
					}
				}
			} else if (component.type == "MonitorRenderer") {
				validateEntityReference(
					component.monitorCameraEntityId,
					"Monitor camera"
				);
			} else if (component.type == "CameraSwitcher") {
				if (component.enabled && activeInHierarchy) {
					++cameraSwitcherCount;
					if (cameraSwitcherCount > 1) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first active CameraSwitcher is used"
						);
					}
				}
				std::unordered_set<uint64_t> registeredCameraIds;
				for (const SceneCameraSwitchEntry& entry :
					component.cameraSwitchEntries) {
					validateEntityReference(
						entry.cameraEntityId,
						"CameraSwitcher camera"
					);
					const SceneEntity* cameraEntity = entry.cameraEntityId != 0
						? document.FindEntity(entry.cameraEntityId)
						: nullptr;
					if (
						!entry.cameraEntityName.empty() &&
						(!cameraEntity ||
							cameraEntity->name != entry.cameraEntityName)
					) {
						cameraEntity = document.FindEntityByName(
							entry.cameraEntityName
						);
					}
					if (!cameraEntity) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"CameraSwitcher contains an unresolved camera"
						);
						continue;
					}
					const bool hasCamera = std::any_of(
						cameraEntity->components.begin(),
						cameraEntity->components.end(),
						[](const SceneComponent& candidate) {
							return candidate.enabled && candidate.type == "Camera";
						}
					);
					if (!hasCamera) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"CameraSwitcher target has no enabled Camera: " +
								cameraEntity->name
						);
					} else if (!registeredCameraIds.insert(cameraEntity->id).second) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"CameraSwitcher contains a duplicate camera: " +
								cameraEntity->name
						);
					}
				}
				if (component.cameraSwitchEntries.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"CameraSwitcher has no registered cameras"
					);
				}
			} else if (component.type == "ThirdPersonCamera") {
				const bool hasCamera = std::any_of(
					entity.components.begin(),
					entity.components.end(),
					[](const SceneComponent& candidate) {
						return candidate.enabled && candidate.type == "Camera";
					}
				);
				if (!hasCamera) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPersonCamera requires Camera on the same Entity"
					);
				}
				validateEntityReference(
					component.thirdPersonTargetEntityId,
					"ThirdPerson target"
				);
				if (
					component.thirdPersonTargetEntityId == 0 &&
					!component.thirdPersonTargetEntityName.empty() &&
					!document.FindEntityByName(
						component.thirdPersonTargetEntityName
					)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPerson target name cannot be resolved: " +
							component.thirdPersonTargetEntityName
					);
				}
				if (
					component.thirdPersonYawReference != "World" &&
					component.thirdPersonYawReference != "Target"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPersonCamera has an unknown yawReference: " +
							component.thirdPersonYawReference
					);
				}
			} else if (component.type == "PlayerBehavior") {
				if (
					component.playerInputMode != "KeyboardMouse" &&
					component.playerInputMode != "Gamepad" &&
					component.playerInputMode != "Both"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"PlayerBehavior has an unsupported inputMode: " +
							component.playerInputMode
					);
				}
				if (
					component.textFontSource != "System" &&
					component.textFontSource != "Resource"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown fontSource: " +
							component.textFontSource
					);
				} else if (component.textFontSource == "Resource") {
					if (
						component.textFontResourcePath.empty() ||
						!IsResourceFontPathShape(component.textFontResourcePath)
					) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"TextRenderer Resource font path must be a relative .ttf, .otf, or .ttc under resources/fonts"
						);
					} else {
						const std::filesystem::path projectRoot =
							EditableResourcePath::FindProjectRoot();
						const std::filesystem::path fontRoot =
							projectRoot / "resources" / "fonts";
						const std::filesystem::path filePath =
							EditableResourcePath::Resolve(
								StringUtility::ToPath(component.textFontResourcePath)
							);
						std::error_code pathError;
						const std::filesystem::path canonicalRoot =
							std::filesystem::weakly_canonical(fontRoot, pathError);
						const std::filesystem::path canonicalFile =
							std::filesystem::weakly_canonical(filePath, pathError);
						if (pathError || !IsPathWithin(canonicalRoot, canonicalFile)) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"TextRenderer Resource font path resolves outside resources/fonts"
							);
						} else if (
							!std::filesystem::is_regular_file(filePath, pathError) ||
							pathError
						) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"TextRenderer Resource font file is missing; runtime fallback may be used"
							);
						}
					}
				}
				if (
					!std::isfinite(component.playerGamepadDeadzone) ||
					component.playerGamepadDeadzone < 0.0f ||
					component.playerGamepadDeadzone > 0.95f
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"PlayerBehavior has an invalid gamepadDeadzone"
					);
				}
			} else if (component.type == "AgentBehavior") {
				validateEntityReference(
					component.agentBoundsEntityId,
					"Agent bounds"
				);
				validateEntityReference(
					component.agentAttractorEntityId,
					"Agent attractor"
				);
				if (!std::isfinite(component.agentMemberMinimumDistance) ||
					component.agentMemberMinimumDistance < 0.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"AgentBehavior member minimum distance must be finite and non-negative"
					);
				}
			} else if (component.type == "EntityReference") {
				if (component.entityReferenceName.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference referenceName is empty"
					);
				}
				const SceneEntityReference& reference =
					component.entityReferenceTarget;
				if (reference.entityId == 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference target Entity ID is empty"
					);
				} else if (
					reference.sceneId.empty() ||
					reference.sceneId == sceneId
				) {
					validateEntityReference(
						reference.entityId,
						"EntityReference target"
					);
				}
				if (
					reference.sceneId.empty() &&
					!reference.instanceKey.empty()
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference instanceKey requires a target sceneId"
					);
				} else if (
					!reference.sceneId.empty() &&
					catalog &&
					!catalog->Find(reference.sceneId)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference target Scene is not registered: " +
						reference.sceneId
					);
				}
			} else if (component.type == "SceneTransition") {
				if (component.sceneTransitionTargetSceneId.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SceneTransition targetSceneId is empty"
					);
				} else if (catalog &&
					!catalog->Find(component.sceneTransitionTargetSceneId)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SceneTransition target is not registered: " +
						component.sceneTransitionTargetSceneId
					);
				}
			} else if (component.type == "StatSet") {
				std::unordered_set<std::string> statIds;
				for (const SceneStatDefinition& stat : component.stats) {
					if (stat.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StatSet contains an empty Stat Id"
						);
					} else if (!statIds.insert(stat.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StatSet contains a duplicate Stat Id: " + stat.id
						);
					}
					if (stat.maxValue < stat.minValue) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Stat max is below min: " + stat.id
						);
					}
				}
			} else if (component.type == "EventTrigger") {
				auto validateCameraEventTarget = [
					&addIssue,
					&document,
					&entity
				](
					uint64_t targetId,
					const std::string& targetName,
					const char* componentName,
					const char* label,
					bool checkPathPoints,
					bool allowInactiveTarget = false
				) {
					if (targetId == 0 && targetName.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							std::string(label) + " target is not set"
						);
						return;
					}
					const SceneEntity* target = targetId != 0
						? document.FindEntity(targetId)
						: nullptr;
					if (!target && !targetName.empty()) {
						target = document.FindEntityByName(targetName);
					}
					const SceneComponent* targetComponent = target
						? SceneEntityQuery::FindEnabledComponent(
							*target, componentName
						)
						: nullptr;
					if (
						!target || !targetComponent ||
						(!allowInactiveTarget &&
							!SceneEntityQuery::IsEntityActiveInHierarchy(document, *target))
					) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) +
								" target is unresolved, inactive, or disabled"
						);
						return;
					}
					if (!checkPathPoints) {
						return;
					}
					const bool hasPoint = std::any_of(
						document.GetEntities().begin(),
						document.GetEntities().end(),
						[&document, target](const SceneEntity& candidate) {
							return candidate.parentId == target->id &&
								SceneEntityQuery::IsEntityActiveInHierarchy(
									document, candidate
								) &&
								SceneEntityQuery::FindEnabledComponent(
									candidate, "CameraPathPoint"
								);
						}
					);
					if (!hasPoint) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"CameraPath Event target has no active CameraPathPoint"
						);
					}
					if (!targetComponent->cameraPathTargetCameraName.empty()) {
						const SceneEntity* targetCamera = document.FindEntityByName(
							targetComponent->cameraPathTargetCameraName
						);
						if (
							!targetCamera ||
							!SceneEntityQuery::IsEntityActiveInHierarchy(
								document, *targetCamera
							) ||
							!SceneEntityQuery::FindEnabledComponent(
								*targetCamera, "Camera"
							)
						) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"CameraPath Event target has an unresolved target Camera"
							);
						}
					}
				};
				auto resolveEventTarget = [&document](
					uint64_t targetId,
					const std::string& targetName
				) -> const SceneEntity* {
					const SceneEntity* target = targetId != 0
						? document.FindEntity(targetId)
						: nullptr;
					if (!target && !targetName.empty()) {
						target = document.FindEntityByName(targetName);
					}
					return target;
				};
				auto hasActiveParentHierarchy = [&document](const SceneEntity& target) {
					std::unordered_set<uint64_t> visited;
					const SceneEntity* current = &target;
					while (current->parentId != 0) {
						if (!visited.insert(current->id).second) {
							return false;
						}
						current = document.FindEntity(current->parentId);
						if (!current || !current->active) {
							return false;
						}
					}
					return true;
				};
				std::unordered_set<uint64_t> onStartActivatedEntityIds;
				if (component.enabled && activeInHierarchy) {
					for (const SceneEventBinding& binding : component.eventBindings) {
						if (binding.triggerType != "OnStart") {
							continue;
						}
						for (const SceneEventAction& action : binding.actions) {
							if (action.type != "SetEntityActive" || !action.active) {
								continue;
							}
							const SceneEntity* target = resolveEventTarget(
								action.targetEntityId,
								action.targetEntityName
							);
							if (target && hasActiveParentHierarchy(*target)) {
								onStartActivatedEntityIds.insert(target->id);
							}
						}
					}
				}
				auto isActivatedOnStart = [
					&onStartActivatedEntityIds,
					&resolveEventTarget
				](uint64_t targetId, const std::string& targetName) {
					const SceneEntity* target = resolveEventTarget(targetId, targetName);
					return target && onStartActivatedEntityIds.contains(target->id);
				};
				auto validateConditionExpression = [
				&addIssue,
				&entity,
				&document,
				&resolveEventTarget
			](const SceneEventConditionExpression& expression) {
				if (expression.mode != "Any") {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Event condition expression mode must be Any"
					);
				}
				if (expression.groups.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Event condition expression requires at least one group"
					);
				}
				for (const SceneEventConditionGroup& group : expression.groups) {
					if (group.mode != "All") {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Event condition group mode must be All"
						);
					}
					if (group.terms.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Event condition group requires at least one term"
						);
					}
					for (const SceneEventConditionTerm& term : group.terms) {
						const bool needsTarget =
							term.type == "StatCompare" ||
							term.type == "EntityActive" ||
							term.type == "StateEquals" ||
							term.type == "PauseActive" ||
							term.type == "PositionWithin";
						const bool isFishingResultCondition =
							term.type == "FishingResultAvailable" ||
							term.type == "FishingResultHasWinner" ||
							term.type == "FishingResultWinnerEquals";
						const SceneEntity* target = resolveEventTarget(
							term.targetEntityId, term.targetEntityName
						);
						if (needsTarget && !target) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Event condition target is unresolved"
							);
							continue;
						}
						if (term.type == "StatCompare") {
							if (term.statId.empty() || !target ||
								!SceneEntityQuery::FindEnabledComponent(*target, "StatSet")) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"StatCompare condition requires a StatSet target and statId"
								);
							}
							if (
								term.statComparison != "LessOrEqual" &&
								term.statComparison != "Less" &&
								term.statComparison != "Equal" &&
								term.statComparison != "Greater" &&
								term.statComparison != "GreaterOrEqual"
							) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"StatCompare condition has an invalid comparison");
							}
						} else if (term.type == "EntityActive") {
							if (!target) continue;
						} else if (term.type == "StateEquals") {
							if (term.stateName.empty() || !target ||
								!SceneEntityQuery::FindEnabledComponent(*target, "StateMachine")) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"StateEquals condition requires a StateMachine target and stateName");
							}
						} else if (term.type == "PauseActive") {
							if (!target || !SceneEntityQuery::FindEnabledComponent(*target, "PauseController")) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"PauseActive condition requires a PauseController target");
							}
						} else if (term.type == "PositionWithin") {
							if (term.radius < 0.0f) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"PositionWithin condition radius must be non-negative");
							}
						} else if (term.type == "InputExpression") {
							if (!term.inputExpression) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"InputExpression condition requires inputExpression");
							} else {
								ValidateSceneInputExpression(
									*term.inputExpression, entity.id,
									"Event condition input expression", addIssue
								);
							}
						} else if (isFishingResultCondition) {
							if (term.fishingResultChannelId.empty()) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"Fishing result condition requires a channelId"
								);
							}
							if (term.type == "FishingResultWinnerEquals" &&
								term.fishingResultRankId.empty()) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingResultWinnerEquals requires a rankId"
								);
							}
						} else {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"Event condition has unknown type: " + term.type);
						}
					}
				}
			};
				for (const SceneEventBinding& binding : component.eventBindings) {
					if (binding.priority < -1000 || binding.priority > 1000) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"Event binding priority must be between -1000 and 1000");
					}
					if (binding.conditionExpression) {
						validateConditionExpression(*binding.conditionExpression);
					}
					if (binding.triggerType == "OnStateEntered") {
						const SceneEntity* target = resolveEventTarget(
							binding.targetEntityId, binding.targetEntityName
						);
						if (binding.stateName.empty() || !target ||
							!SceneEntityQuery::FindEnabledComponent(*target, "StateMachine")) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"OnStateEntered requires a StateMachine target and stateName");
						}
					}
					const bool triggerUsesTarget =
						binding.triggerType == "OnStatReachedMin" ||
						binding.triggerType == "OnStatCompare" ||
						binding.triggerType == "OnPositionReached";
					if (triggerUsesTarget) {
						validateEntityReference(
							binding.targetEntityId,
							"Event target"
						);
					}
					if (binding.triggerType == "OnFishingScoreAttackResultInput") {
						const SceneEntity* target = resolveEventTarget(
							binding.targetEntityId,
							binding.targetEntityName
						);
						if (
							!target ||
							!SceneEntityQuery::FindEnabledComponent(
								*target, "FishingScoreAttackDirector"
							) ||
							!SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)
						) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"OnFishingScoreAttackResultInput target is unresolved, inactive, or disabled"
							);
						}
						if (binding.inputExpression) {
							ValidateSceneInputExpression(
								*binding.inputExpression,
								entity.id,
								"OnFishingScoreAttackResultInput expression",
								addIssue
							);
						} else if (!IsSupportedSceneInput(binding.triggerKey)) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"OnFishingScoreAttackResultInput has an unsupported input: " +
									binding.triggerKey
							);
						}
					}
					if (binding.triggerType == "OnFishingResultPublished" &&
						binding.fishingResultChannelId.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"OnFishingResultPublished requires a channelId"
						);
					}
					if (binding.triggerType == "OnKeyPressed" &&
						binding.inputExpression) {
						ValidateSceneInputExpression(
							*binding.inputExpression,
							entity.id,
							"OnKeyPressed expression",
							addIssue
						);
					} else if (
						binding.triggerType == "OnKeyPressed" &&
						!IsSupportedSceneInput(binding.triggerKey)
					) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"OnKeyPressed Event has an unsupported key: " +
								binding.triggerKey
						);
					}
					if (binding.triggerType == "OnCameraPathCompleted") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"CameraPath",
							"OnCameraPathCompleted",
							true
						);
					}
					if (binding.triggerType == "OnAudioFinished") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"AudioSource",
							"OnAudioFinished",
							false
						);
					}
					if (binding.triggerType == "OnTextMotionCompleted") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"TextMotion",
							"OnTextMotionCompleted",
							false,
							isActivatedOnStart(
								binding.targetEntityId,
								binding.targetEntityName
							)
						);
						const SceneEntity* target = binding.targetEntityId != 0
							? document.FindEntity(binding.targetEntityId) : nullptr;
						if (!target && !binding.targetEntityName.empty()) {
							target = document.FindEntityByName(binding.targetEntityName);
						}
						const SceneComponent* motion = target
							? SceneEntityQuery::FindEnabledComponent(*target, "TextMotion")
							: nullptr;
						if (motion && !binding.textMotionClipId.empty() &&
							!std::any_of(
								motion->textMotionClips.begin(),
								motion->textMotionClips.end(),
								[&binding](const SceneTextMotionClip& clip) {
									return clip.id == binding.textMotionClipId;
								}
							)) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"OnTextMotionCompleted clip Id is unresolved: " +
									binding.textMotionClipId);
						}
					}
					for (size_t actionIndex = 0;
						actionIndex < binding.actions.size();
						++actionIndex) {
						const SceneEventAction& action = binding.actions[actionIndex];
						const bool actionUsesTarget =
							action.type == "ModifyStat" ||
							action.type == "SetEntityActive" ||
							action.type == "InstantiatePrefab" ||
							action.type == "ChangeState" ||
							action.type == "PlayAudio" ||
							action.type == "StopAudio" ||
							action.type == "PauseAudio" ||
							action.type == "ResumeAudio" ||
								action.type == "PlayTextMotion" ||
								action.type == "StopTextMotion" ||
								action.type == "ResetTextMotion" ||
								action.type == "SetPauseState";
						if (action.type == "AdjustFishingFishCount") {
							const SceneEntity* target = resolveEventTarget(
								action.targetEntityId,
								action.targetEntityName
							);
							if (
								!target ||
								!SceneEntityQuery::FindEnabledComponent(
									*target,
									"FishingScoreAttackDirector"
								) ||
								!SceneEntityQuery::IsEntityActiveInHierarchy(
									document,
									*target
								)
							) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"AdjustFishingFishCount target is unresolved, inactive, or disabled"
								);
							}
							if (
								!std::isfinite(action.value) ||
								(action.value != 1.0f && action.value != -1.0f)
							) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"AdjustFishingFishCount value must be +1 or -1"
								);
							}
						}
						if (actionUsesTarget) {
							validateEntityReference(
								action.targetEntityId,
								"Event action target"
							);
						}
						if (action.type == "SetPauseState") {
							const SceneEntity* target = resolveEventTarget(
								action.targetEntityId,
								action.targetEntityName
							);
							const SceneComponent* controller = target
								? SceneEntityQuery::FindEnabledComponent(
									*target, "PauseController"
								)
								: nullptr;
							if (
								!target || !controller ||
								!SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)
							) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"SetPauseState target is unresolved, inactive, or disabled"
								);
							} else if (action.pauseProfileId.empty() ||
								!std::any_of(
									controller->pauseProfiles.begin(),
									controller->pauseProfiles.end(),
									[&action](const ScenePauseProfile& profile) {
										return profile.id == action.pauseProfileId;
									}
								)) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"SetPauseState profile cannot be resolved: " +
										action.pauseProfileId
								);
							}
							if (
								action.pauseOperation != "Pause" &&
								action.pauseOperation != "Resume" &&
								action.pauseOperation != "Toggle"
							) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"SetPauseState has an unsupported operation: " +
										action.pauseOperation
								);
							}
							if (action.pauseRequestId.empty()) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"SetPauseState requires a non-empty request Id"
								);
							}
						}
						if (
							(action.type == "PlayAudio" || action.type == "StopAudio" ||
								action.type == "PauseAudio" || action.type == "ResumeAudio")
						) {
							validateCameraEventTarget(
								action.targetEntityId, action.targetEntityName, "AudioSource",
								action.type.c_str(), false
							);
						}
						if (
							action.type == "PlayCameraPath" ||
							action.type == "StopCameraPath"
						) {
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"CameraPath",
								action.type.c_str(),
								action.type == "PlayCameraPath"
							);
						} else if (action.type == "SelectCamera") {
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"Camera",
								"SelectCamera",
								false
							);
						}
						if (
							action.type == "PlayTextMotion" ||
							action.type == "StopTextMotion" ||
							action.type == "ResetTextMotion"
						) {
							const bool activatedByEarlierOnStartAction =
								binding.triggerType == "OnStart" &&
								std::any_of(
									binding.actions.begin(),
									binding.actions.begin() + actionIndex,
									[&action](const SceneEventAction& priorAction) {
										if (priorAction.type != "SetEntityActive" ||
											!priorAction.active) {
											return false;
										}
										if (action.targetEntityId != 0) {
											return priorAction.targetEntityId == action.targetEntityId;
										}
										return !action.targetEntityName.empty() &&
											priorAction.targetEntityName == action.targetEntityName;
									}
								);
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"TextMotion",
								action.type.c_str(),
								false,
								action.type == "PlayTextMotion" &&
									(binding.triggerType == "OnStart"
										? activatedByEarlierOnStartAction
										: isActivatedOnStart(
											action.targetEntityId,
											action.targetEntityName
										))
							);
							if (action.type == "PlayTextMotion") {
								const SceneEntity* target = action.targetEntityId != 0
									? document.FindEntity(action.targetEntityId) : nullptr;
								if (!target && !action.targetEntityName.empty()) {
									target = document.FindEntityByName(action.targetEntityName);
								}
								const SceneComponent* motion = target
									? SceneEntityQuery::FindEnabledComponent(*target, "TextMotion")
									: nullptr;
								const bool hasClip = motion && std::any_of(
									motion->textMotionClips.begin(),
									motion->textMotionClips.end(),
									[&action](const SceneTextMotionClip& clip) {
										return clip.id == action.textMotionClipId;
									}
								);
								if (!hasClip) {
									addIssue(SceneValidationSeverity::Error, entity.id,
										"PlayTextMotion clip Id is unresolved: " +
											action.textMotionClipId);
								}
							}
						}
						if (
							action.type == "SetPostProcessProfile" ||
							action.type == "NextPostProcessProfile"
						) {
							const SceneEntity* manager =
								action.postProcessManagerEntityId != 0
								? document.FindEntity(action.postProcessManagerEntityId)
								: nullptr;
							if (!manager && !action.postProcessManagerEntityName.empty()) {
								manager = document.FindEntityByName(
									action.postProcessManagerEntityName
								);
							}
							const SceneComponent* managerComponent = nullptr;
							if (manager) {
								const auto componentIterator = std::find_if(
									manager->components.begin(),
									manager->components.end(),
									[](const SceneComponent& candidate) {
										return candidate.enabled &&
											candidate.type == "PostProcessProfileManager";
									}
								);
								if (componentIterator != manager->components.end()) {
									managerComponent = &*componentIterator;
								}
							}
							if (!managerComponent) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"Event PostProcess action has an unresolved or disabled manager"
								);
							} else if (action.type == "NextPostProcessProfile" &&
								managerComponent->postProcessProfiles.empty()) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									"NextPostProcessProfile manager has no profiles"
								);
							} else if (action.type == "SetPostProcessProfile") {
								const bool profileExists = std::any_of(
									managerComponent->postProcessProfiles.begin(),
									managerComponent->postProcessProfiles.end(),
									[&action](const ScenePostProcessProfile& profile) {
										return profile.id == action.postProcessProfileId;
									}
								);
								if (!profileExists) {
									addIssue(
										SceneValidationSeverity::Error,
										entity.id,
										"SetPostProcessProfile profile cannot be resolved: " +
											action.postProcessProfileId
									);
								}
							}
						}
						if (
							action.type == "ChangeState" &&
							action.stateName.empty()
						) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"ChangeState action has an empty state name"
							);
						}
						if (
							action.type == "SceneTransition" &&
							!action.sceneId.empty() &&
							catalog &&
							!catalog->Find(action.sceneId)
						) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Event SceneTransition target is not registered: " +
									action.sceneId
							);
						}
					}
				}
			} else if (component.type == "PostProcessProfileManager") {
				std::unordered_set<std::string> profileIds;
				for (const ScenePostProcessProfile& profile :
					component.postProcessProfiles) {
					if (profile.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PostProcessProfileManager contains an empty Profile Id"
						);
					} else if (!profileIds.insert(profile.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PostProcessProfileManager contains a duplicate Profile Id: " +
								profile.id
						);
					}
					if (profile.label.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"PostProcessProfileManager Profile has an empty label: " +
								profile.id
						);
					}
					const ScenePostProcessSettings& settings = profile.settings;
					if (
						settings.pixelationBlockSize < 1 ||
						settings.pixelationBlockSize > 64 ||
						settings.motionBlurStrength < 0.0f ||
						settings.motionBlurStrength > 1.0f ||
						settings.motionBlurSamples < 2 ||
						settings.motionBlurSamples > 32 ||
						settings.motionBlurMaxRadius < 0.0f ||
						settings.motionBlurMaxRadius > 64.0f ||
						settings.chromaticAberrationCenter.x < 0.0f ||
						settings.chromaticAberrationCenter.x > 1.0f ||
						settings.chromaticAberrationCenter.y < 0.0f ||
						settings.chromaticAberrationCenter.y > 1.0f ||
						settings.chromaticAberrationIntensity < 0.0f ||
						settings.chromaticAberrationFalloff <= 0.0f
					) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Post Process settings are outside the supported range: " + profile.id
						);
					}
				}
			} else if (component.type == "AttackSet") {
				for (const SceneAttackDefinition& attack : component.attackDefinitions) {
					for (size_t windowIndex = 0;
						windowIndex < attack.hitWindows.size(); ++windowIndex) {
						const SceneAttackHitWindow& window =
							attack.hitWindows[windowIndex];
						if (window.payloadSource != "HitBox") {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Attack '" + attack.name + "' Hit Window " +
									std::to_string(windowIndex + 1) +
									" uses WindowLegacy payload"
							);
							continue;
						}
						const SceneEntity* hitBox = window.hitBoxEntityId != 0
							? document.FindEntity(window.hitBoxEntityId)
							: nullptr;
						if (!hitBox && !window.hitBoxEntityName.empty()) {
							hitBox = document.FindEntityByName(window.hitBoxEntityName);
						}
						if (!hitBox) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Attack '" + attack.name + "' Hit Window " +
									std::to_string(windowIndex + 1) +
									" has no resolvable Dedicated HitBox"
							);
							continue;
						}
						const bool hasEnabledHitBox = std::any_of(
							hitBox->components.begin(), hitBox->components.end(),
							[](const SceneComponent& candidate) {
								return candidate.enabled && candidate.type == "HitBox";
							}
						);
						const bool hasEnabledTrigger = std::any_of(
							hitBox->components.begin(), hitBox->components.end(),
							[](const SceneComponent& candidate) {
								return candidate.enabled && candidate.type == "OBBCollider" &&
									candidate.colliderIsTrigger;
							}
						);
						if (!hasEnabledHitBox || !hasEnabledTrigger) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Attack '" + attack.name + "' Dedicated HitBox '" +
									hitBox->name +
									" requires enabled HitBox and Trigger Collider"
							);
						}
						if (hitBox->active) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Attack '" + attack.name + "' Dedicated HitBox '" +
									hitBox->name + "' is saved active"
							);
						}
					}
					for (size_t leftIndex = 0;
						leftIndex < attack.hitWindows.size(); ++leftIndex) {
						const SceneAttackHitWindow& left = attack.hitWindows[leftIndex];
						if (left.payloadSource != "HitBox" || left.hitBoxEntityId == 0) {
							continue;
						}
						for (size_t rightIndex = leftIndex + 1;
							rightIndex < attack.hitWindows.size(); ++rightIndex) {
							const SceneAttackHitWindow& right = attack.hitWindows[rightIndex];
							if (
								right.payloadSource == "HitBox" &&
								left.hitBoxEntityId == right.hitBoxEntityId &&
								left.startTime < right.endTime &&
								right.startTime < left.endTime
							) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									"Attack '" + attack.name + "' has overlapping Dedicated HitBox Windows " +
										std::to_string(leftIndex + 1) + " and " +
										std::to_string(rightIndex + 1)
								);
							}
						}
					}
				}
			} else if (component.type == "StateMachine") {
				std::unordered_set<std::string> stateNames;
				for (const SceneStateDefinition& state :
					component.stateMachineStates) {
					if (state.name.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StateMachine contains an empty state name"
						);
					} else if (!stateNames.insert(state.name).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StateMachine contains a duplicate state: " + state.name
						);
					}
					if (state.actionId.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"State has an empty Action Id: " + state.name
						);
					}
					std::unordered_set<std::string> parameterNames;
					for (const SceneStateParameter& parameter : state.parameters) {
						if (!parameterNames.insert(parameter.name).second) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"State contains a duplicate parameter: " +
									state.name + "." + parameter.name
							);
						}
						validateEntityReference(
							parameter.entityId,
							"State parameter"
						);
					}
				}
				if (
					component.stateMachineStates.empty() ||
					!stateNames.contains(component.stateMachineInitialState)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"StateMachine initial state cannot be resolved"
					);
				}
			} else if (
				component.type == "HitBox" ||
				component.type == "HurtBox"
			) {
				const SceneComponent* collider = nullptr;
				for (const SceneComponent& candidate : entity.components) {
					if (candidate.type == "OBBCollider" && candidate.enabled) {
						collider = &candidate;
						break;
					}
				}
				if (!collider || !collider->colliderIsTrigger) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						component.type +
							" requires an enabled Trigger Collider on the same Entity"
					);
				}
				if (component.type == "HitBox") {
					validateEntityReference(
						component.hitBoxOwnerEntityId,
						"HitBox owner"
					);
				} else {
					validateEntityReference(
						component.hurtBoxStatsEntityId,
						"HurtBox stats owner"
					);
				}
			} else if (component.type == "BoneAttachment") {
				validateEntityReference(
					component.boneAttachmentTargetEntityId,
					"BoneAttachment target"
				);
				if (component.boneAttachmentJointName.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment jointName is empty"
					);
				}
				if (
					component.boneAttachmentAlignmentMode != "ManualOffset" &&
					component.boneAttachmentAlignmentMode != "MatchSourceBone"
				) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment alignmentMode is invalid"
					);
				} else if (
					component.boneAttachmentAlignmentMode == "MatchSourceBone" &&
					component.boneAttachmentSourceJointName.empty()
				) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment sourceJointName is empty"
					);
				}
			} else if (component.type == "EnemyBehavior") {
				validateEntityReference(
					component.enemyTargetEntityId,
					"Enemy target"
				);
				validateEntityReference(
					component.enemyAttackHitBoxEntityId,
					"Enemy attack HitBox"
				);
			} else if (component.type == "Projectile") {
				validateEntityReference(
					component.projectileHomingTargetEntityId,
					"Projectile homing target"
				);
			} else if (component.type == "FishingScoreAttackDirector") {
				if (component.fishingConfirmInputExpression) {
					ValidateSceneInputExpression(
						*component.fishingConfirmInputExpression,
						entity.id,
						"FishingScoreAttackDirector confirm input expression",
						addIssue
					);
				} else if (!IsSupportedSceneInput(component.fishingConfirmInput)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector has an unsupported fish count confirm input: " +
							component.fishingConfirmInput
					);
				}
				if (
					!std::isfinite(component.fishingFormationOutlineColor.x) ||
					!std::isfinite(component.fishingFormationOutlineColor.y) ||
					!std::isfinite(component.fishingFormationOutlineColor.z) ||
					!std::isfinite(component.fishingFormationOutlineColor.w) ||
					component.fishingFormationOutlineColor.x < 0.0f ||
					component.fishingFormationOutlineColor.x > 1.0f ||
					component.fishingFormationOutlineColor.y < 0.0f ||
					component.fishingFormationOutlineColor.y > 1.0f ||
					component.fishingFormationOutlineColor.z < 0.0f ||
					component.fishingFormationOutlineColor.z > 1.0f ||
					component.fishingFormationOutlineColor.w < 0.0f ||
					component.fishingFormationOutlineColor.w > 1.0f ||
					!std::isfinite(component.fishingFormationOutlineBloomIntensity) ||
					component.fishingFormationOutlineBloomIntensity < 0.0f ||
					component.fishingFormationOutlineBloomIntensity > 32.0f ||
					!std::isfinite(component.fishingFormationOutlineYOffset) ||
					component.fishingFormationOutlineSegments < 12 ||
					component.fishingFormationOutlineSegments > 128
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector has invalid formation outline settings"
					);
				}
				if (
					component.fishingFormationParticlePointCount < 12 ||
					component.fishingFormationParticlePointCount > 128 ||
					!std::isfinite(component.fishingFormationParticleStartSize) ||
					component.fishingFormationParticleStartSize < 0.01f ||
					component.fishingFormationParticleStartSize > 5.0f ||
					!std::isfinite(component.fishingFormationParticleEndSize) ||
					component.fishingFormationParticleEndSize < 0.01f ||
					component.fishingFormationParticleEndSize > 5.0f ||
					component.fishingFormationParticleCountPerEmission < 1 ||
					component.fishingFormationParticleCountPerEmission > 16 ||
					!std::isfinite(component.fishingFormationParticleEmitterSpread) ||
					component.fishingFormationParticleEmitterSpread < 0.0f ||
					component.fishingFormationParticleEmitterSpread > 0.5f ||
					!std::isfinite(component.fishingFormationParticleLifetime) ||
					component.fishingFormationParticleLifetime < 0.1f ||
					component.fishingFormationParticleLifetime > 3.0f ||
					!std::isfinite(component.fishingFormationParticleStartColor.x) ||
					!std::isfinite(component.fishingFormationParticleStartColor.y) ||
					!std::isfinite(component.fishingFormationParticleStartColor.z) ||
					!std::isfinite(component.fishingFormationParticleStartColor.w) ||
					component.fishingFormationParticleStartColor.x < 0.0f ||
					component.fishingFormationParticleStartColor.x > 1.0f ||
					component.fishingFormationParticleStartColor.y < 0.0f ||
					component.fishingFormationParticleStartColor.y > 1.0f ||
					component.fishingFormationParticleStartColor.z < 0.0f ||
					component.fishingFormationParticleStartColor.z > 1.0f ||
					component.fishingFormationParticleStartColor.w < 0.0f ||
					component.fishingFormationParticleStartColor.w > 1.0f ||
					!std::isfinite(component.fishingFormationParticleEndColor.x) ||
					!std::isfinite(component.fishingFormationParticleEndColor.y) ||
					!std::isfinite(component.fishingFormationParticleEndColor.z) ||
					!std::isfinite(component.fishingFormationParticleEndColor.w) ||
					component.fishingFormationParticleEndColor.x < 0.0f ||
					component.fishingFormationParticleEndColor.x > 1.0f ||
					component.fishingFormationParticleEndColor.y < 0.0f ||
					component.fishingFormationParticleEndColor.y > 1.0f ||
					component.fishingFormationParticleEndColor.z < 0.0f ||
					component.fishingFormationParticleEndColor.z > 1.0f ||
					component.fishingFormationParticleEndColor.w < 0.0f ||
					component.fishingFormationParticleEndColor.w > 1.0f ||
					!std::isfinite(component.fishingFormationParticleEmissiveIntensity) ||
					component.fishingFormationParticleEmissiveIntensity < 0.0f ||
					component.fishingFormationParticleEmissiveIntensity > 8.0f
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector has invalid formation particle settings"
					);
				}
				auto validateFishingComponentReference = [
					&addIssue,
					&document,
					&entity
				](uint64_t targetId, const char* componentType, const char* label) {
					if (targetId == 0) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) + " Entity is not set"
						);
						return;
					}
					const SceneEntity* target = document.FindEntity(targetId);
					if (!target || !SceneEntityQuery::FindEnabledComponent(*target, componentType)) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) + " does not reference an enabled " + componentType
						);
					}
				};
				validateFishingComponentReference(
					component.fishingPlayerEntityId,
					"PlayerBehavior",
					"Fishing Player"
				);
				validateFishingComponentReference(
					component.fishingWaterVolumeEntityId,
					"WaterVolume",
					"Fishing Water Volume"
				);
				validateFishingComponentReference(
					component.fishingHookSpawnAreaEntityId,
					"FishingHookSpawnArea",
					"Fishing Hook Spawn Area"
				);
				validateFishingComponentReference(
					component.fishingHookPoolEntityId,
					"FishingHookPool",
					"Fishing Hook Pool"
				);
				const std::array<uint64_t, 4> boundaryWallIds = {
					component.fishingBoundaryNegativeXWallEntityId,
					component.fishingBoundaryPositiveXWallEntityId,
					component.fishingBoundaryNegativeZWallEntityId,
					component.fishingBoundaryPositiveZWallEntityId
				};
				const bool anyBoundaryWall = std::any_of(
					boundaryWallIds.begin(), boundaryWallIds.end(),
					[](uint64_t entityId) { return entityId != 0; }
				);
				const bool allBoundaryWalls = std::all_of(
					boundaryWallIds.begin(), boundaryWallIds.end(),
					[](uint64_t entityId) { return entityId != 0; }
				);
				if (anyBoundaryWall && !allBoundaryWalls) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Fishing boundary walls must be all configured or all unset"
					);
				} else if (allBoundaryWalls) {
					std::unordered_set<uint64_t> uniqueBoundaryWalls;
					for (uint64_t boundaryWallId : boundaryWallIds) {
						const SceneEntity* boundaryWall = document.FindEntity(
							boundaryWallId
						);
						const SceneComponent* boundaryCollider = boundaryWall
							? SceneEntityQuery::FindEnabledComponent(
								*boundaryWall, "OBBCollider"
							)
							: nullptr;
						if (!uniqueBoundaryWalls.insert(boundaryWallId).second ||
							!boundaryWall ||
							!SceneEntityQuery::IsEntityActiveInHierarchy(
								document, *boundaryWall
							) ||
							!boundaryCollider ||
							!boundaryCollider->colliderActive ||
							boundaryCollider->colliderIsTrigger ||
							boundaryCollider->colliderShape != "Box") {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Fishing boundary walls require unique active non-trigger Box Colliders"
							);
							break;
						}
					}
				}
				if (
					component.fishingUseFormationCapsuleCollision ||
					component.fishingFormationOutlineVisible
				) {
					const SceneEntity* player = document.FindEntity(
						component.fishingPlayerEntityId
					);
					const SceneTeamSettings* playerTeam = player
						? document.ResolveEntityTeam(*player)
						: nullptr;
					if (!playerTeam || !playerTeam->agentFormationCapsuleEnabled) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Fishing formation features require the Player Team formation capsule"
						);
					}
				}
				if (component.fishingFishEntityIds.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector has no fish entities"
					);
				}
				if (component.fishingMaxSelectableFishCount < 1 ||
					static_cast<size_t>(component.fishingMaxSelectableFishCount) >
						component.fishingFishEntityIds.size()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector max fish count is invalid"
					);
				}
				if (!std::isfinite(component.fishingDurationSeconds) ||
					component.fishingDurationSeconds <= 0.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector contains invalid duration"
					);
				}
				if (!component.fishingUseHookBandSettings && (
					component.fishingDistanceBandCount <= 0 ||
					component.fishingHooksPerDistanceBand < 1 ||
					component.fishingHooksPerDistanceBand > 4 ||
					!std::isfinite(component.fishingDistanceMultiplierBase) ||
					component.fishingDistanceMultiplierBase < 0.0f ||
					!std::isfinite(component.fishingDistanceMultiplierStep) ||
					component.fishingDistanceMultiplierStep < 0.0f
				)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector contains invalid legacy multiplier settings"
					);
				}
				if (component.fishingUseHookBandSettings) {
					if (component.fishingHookBands.size() != 5) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector requires exactly five hook bands"
						);
					}
					for (size_t bandIndex = 0;
						bandIndex < component.fishingHookBands.size(); ++bandIndex) {
						const SceneFishingHookBandSettings& band =
							component.fishingHookBands[bandIndex];
						if (band.hookMultiplierWeights.size() != 10) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector hook band must have exactly ten tier weights"
							);
						}
						if (!std::isfinite(band.distanceMultiplier) ||
							band.distanceMultiplier < 0.0f ||
							band.hookCount < 0) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector hook band has invalid multiplier or hook count"
							);
						}
						if ((bandIndex == 0 && band.hookCount != 0) ||
							(bandIndex > 0 && band.hookCount <= 0)) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector hook band has an invalid hook density"
							);
						}
						float activeWeight = 0.0f;
						for (size_t tierIndex = 0;
							tierIndex < band.hookMultiplierWeights.size(); ++tierIndex) {
							const float weight = band.hookMultiplierWeights[tierIndex];
							if (!std::isfinite(weight) || weight < 0.0f) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingScoreAttackDirector hook band has an invalid tier weight"
								);
								break;
							}
							if (tierIndex < static_cast<size_t>(component.fishingHookRankCount)) {
								activeWeight += weight;
							}
						}
						if (band.hookCount > 0 && activeWeight <= 0.0f) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector hook band has no selectable tier"
							);
						}
					}
					if (!std::isfinite(component.fishingHookScoreUnit) ||
						component.fishingHookScoreUnit <= 0.0f ||
						component.fishingHookRankCount < 1 ||
						component.fishingHookRankCount > 10 ||
						!std::isfinite(component.fishingFishMultiplierBase) ||
						component.fishingFishMultiplierBase < 0.0f ||
						!std::isfinite(component.fishingFishMultiplierPerAdditionalFish) ||
						component.fishingFishMultiplierPerAdditionalFish < 0.0f ||
						component.fishingHookRanks.size() != 10 ||
						!std::isfinite(component.fishingHookLegendIconSize.x) ||
						!std::isfinite(component.fishingHookLegendIconSize.y) ||
						component.fishingHookLegendIconSize.x <= 0.0f ||
						component.fishingHookLegendIconSize.y <= 0.0f ||
						!std::isfinite(component.fishingHookLegendLayoutCenter.x) ||
						!std::isfinite(component.fishingHookLegendLayoutCenter.y) ||
						!std::isfinite(component.fishingHookLegendColumnSpacing) ||
						component.fishingHookLegendColumnSpacing <= 0.0f ||
						!std::isfinite(component.fishingHookLegendRowSpacing) ||
						component.fishingHookLegendRowSpacing <= 0.0f ||
						!std::isfinite(component.fishingHookLegendIconOffset.x) ||
						!std::isfinite(component.fishingHookLegendIconOffset.y) ||
						!std::isfinite(component.fishingHookColorEmissiveIntensity) ||
						component.fishingHookColorEmissiveIntensity < 0.0f) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector contains invalid hook score settings"
						);
					}
					std::unordered_set<std::string> rankIds;
					for (const SceneFishingHookRankDefinition& rank : component.fishingHookRanks) {
						if (rank.id.empty() || !rankIds.insert(rank.id).second ||
							!std::isfinite(rank.scoreMultiplier) ||
							!IsResourceRelativeModelPath(rank.modelPath) ||
							!IsResourceRelativeModelPath(rank.iconTexturePath)) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector contains an invalid hook rank definition"
							);
							break;
						}
						const Vector4& color = rank.color;
						if (!std::isfinite(color.x) || !std::isfinite(color.y) ||
							!std::isfinite(color.z) || !std::isfinite(color.w) ||
							color.x < 0.0f || color.x > 1.0f ||
							color.y < 0.0f || color.y > 1.0f ||
							color.z < 0.0f || color.z > 1.0f ||
							color.w < 0.0f || color.w > 1.0f) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector contains an invalid hook rank color"
							);
							break;
						}
					}
					if (component.fishingHookLegendIconEntityIds.size() != 10) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector requires ten hook legend Icon entities"
						);
					}
					if (component.fishingHookLegendVisible) {
						validateFishingComponentReference(
							component.fishingHookLegendTitleTextEntityId,
							"TextRenderer",
							"Hook legend title"
						);
						if (component.fishingHookLegendTextEntityIds.size() != 10) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector requires ten hook legend Text entities"
							);
						}
						std::unordered_set<uint64_t> legendTextIds;
						if (component.fishingHookLegendTitleTextEntityId != 0) {
							legendTextIds.insert(component.fishingHookLegendTitleTextEntityId);
						}
						for (size_t tierIndex = 0;
							tierIndex < component.fishingHookLegendTextEntityIds.size(); ++tierIndex) {
							if (tierIndex >= static_cast<size_t>(component.fishingHookRankCount)) {
								continue;
							}
							const uint64_t textId = component.fishingHookLegendTextEntityIds[tierIndex];
							validateFishingComponentReference(textId, "TextRenderer", "Hook legend");
							if (textId != 0 && !legendTextIds.insert(textId).second) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingScoreAttackDirector contains a duplicate hook legend Text Entity"
								);
							}
						}
						std::unordered_set<uint64_t> legendIconIds;
						for (size_t tierIndex = 0;
							tierIndex < component.fishingHookLegendIconEntityIds.size();
							++tierIndex) {
							if (tierIndex >= static_cast<size_t>(component.fishingHookRankCount)) {
								continue;
							}
							const uint64_t iconId =
								component.fishingHookLegendIconEntityIds[tierIndex];
							if (iconId == 0) {
								if (tierIndex < component.fishingHookRanks.size() &&
									!component.fishingHookRanks[tierIndex].iconTexturePath.empty()) {
									addIssue(
										SceneValidationSeverity::Error,
										entity.id,
										"FishingScoreAttackDirector hook rank icon has no Icon Entity"
									);
								}
								continue;
							}
							const SceneEntity* iconEntity = document.FindEntity(iconId);
							const SceneComponent* iconRenderer = iconEntity
								? SceneEntityQuery::FindEnabledComponent(*iconEntity, "SpriteRenderer")
								: nullptr;
							if (!iconRenderer || iconRenderer->spriteRenderSpace != "ScreenOverlay") {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									tierIndex < component.fishingHookRanks.size() &&
										!component.fishingHookRanks[tierIndex].iconTexturePath.empty()
									? "FishingScoreAttackDirector hook rank icon requires a valid ScreenOverlay Icon Entity"
									: "FishingScoreAttackDirector hook legend Icon must reference an enabled ScreenOverlay SpriteRenderer"
								);
							}
							if (!legendIconIds.insert(iconId).second) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingScoreAttackDirector contains a duplicate hook legend Icon Entity"
								);
							}
						}
					}
					if (component.fishingHookLegendAutoLayout &&
						!component.fishingHookLegendVisible) {
						std::unordered_set<uint64_t> layoutEntityIds;
						const size_t activeRankCount = static_cast<size_t>(
							std::clamp(component.fishingHookRankCount, 1, 10)
						);
						for (size_t tierIndex = 0; tierIndex < activeRankCount; ++tierIndex) {
							const uint64_t textId = tierIndex < component.fishingHookLegendTextEntityIds.size()
								? component.fishingHookLegendTextEntityIds[tierIndex] : 0;
							const uint64_t iconId = tierIndex < component.fishingHookLegendIconEntityIds.size()
								? component.fishingHookLegendIconEntityIds[tierIndex] : 0;
							const auto validateLayoutLink = [
								&addIssue, &document, &layoutEntityIds, entityId = entity.id
							](uint64_t linkedId, const char* label, const char* requiredType) {
								const SceneEntity* linkedEntity = linkedId != 0
									? document.FindEntity(linkedId) : nullptr;
								if (!linkedEntity || !SceneEntityQuery::FindEnabledComponent(
									*linkedEntity, requiredType
								)) {
									addIssue(
										SceneValidationSeverity::Warning,
										entityId,
										std::string("FishingScoreAttackDirector auto layout link is missing: ") + label
									);
									return;
								}
								if (!layoutEntityIds.insert(linkedId).second) {
									addIssue(
										SceneValidationSeverity::Warning,
										entityId,
										"FishingScoreAttackDirector auto layout link is duplicated"
									);
								}
							};
							validateLayoutLink(textId, "Hook legend text", "TextRenderer");
							validateLayoutLink(iconId, "Hook legend icon", "SpriteRenderer");
						}
					}
					if (component.fishingUseHookBandSettings) {
						const SceneEntity* poolEntity = document.FindEntity(
							component.fishingHookPoolEntityId
						);
						const SceneComponent* pool = poolEntity
							? SceneEntityQuery::FindEnabledComponent(*poolEntity, "FishingHookPool")
							: nullptr;
						if (pool) {
							std::unordered_set<uint64_t> validHookIds;
							for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
								const SceneEntity* hook = document.FindEntity(entry.hookEntityId);
								if (hook && SceneEntityQuery::FindEnabledComponent(*hook, "FishingHook")) {
									validHookIds.insert(entry.hookEntityId);
								}
							}
							size_t requiredHookCount = 0;
							for (const SceneFishingHookBandSettings& band : component.fishingHookBands) {
								if (band.hookCount > 0) {
									requiredHookCount += static_cast<size_t>(band.hookCount);
								}
							}
							if (requiredHookCount > validHookIds.size()) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingHookPool has fewer unique valid entries than the new hook band settings require"
								);
							}
						}
					}
				}
				const auto hasFiniteVector2 = [](const Vector2& value) {
					return std::isfinite(value.x) && std::isfinite(value.y);
				};
				const auto hasFiniteVector3 = [](const Vector3& value) {
					return std::isfinite(value.x) &&
						std::isfinite(value.y) &&
						std::isfinite(value.z);
				};
				if (!hasFiniteVector3(component.fishingHookRankBubbleWorldOffset) ||
					!hasFiniteVector2(component.fishingHookRankBubbleScreenOffset) ||
					!hasFiniteVector2(component.fishingHookRankBubbleSize) ||
					component.fishingHookRankBubbleSize.x <= 0.0f ||
					component.fishingHookRankBubbleSize.y <= 0.0f ||
					!hasFiniteVector2(component.fishingHookRankBubbleIconBaseSize) ||
					component.fishingHookRankBubbleIconBaseSize.x <= 0.0f ||
					component.fishingHookRankBubbleIconBaseSize.y <= 0.0f ||
					!hasFiniteVector2(component.fishingHookRankBubbleIconBaseOffset) ||
					(!component.fishingHookRankBubbleTexturePath.empty() &&
						!IsResourceRelativeModelPath(component.fishingHookRankBubbleTexturePath))) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingScoreAttackDirector contains invalid hook rank bubble settings"
					);
				}
				for (const SceneFishingHookRankDefinition& rank : component.fishingHookRanks) {
					if (!hasFiniteVector2(rank.bubbleIconScale) ||
						rank.bubbleIconScale.x <= 0.0f ||
						rank.bubbleIconScale.y <= 0.0f ||
						!hasFiniteVector2(rank.bubbleIconOffset) ||
						(!rank.iconTexturePath.empty() &&
							!IsResourceRelativeModelPath(rank.iconTexturePath))) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector contains invalid hook rank bubble icon settings"
						);
						break;
					}
				}
				if (component.fishingHookRankBubbleVisible) {
					if (component.fishingHookRankBubbleTexturePath.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector requires a hook rank bubble texture when visible"
						);
					}
					const SceneEntity* poolEntity = document.FindEntity(
						component.fishingHookPoolEntityId
					);
					const SceneComponent* pool = poolEntity
						? SceneEntityQuery::FindEnabledComponent(*poolEntity, "FishingHookPool")
						: nullptr;
					if (!pool) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector requires a valid FishingHookPool for hook rank bubbles"
						);
					} else {
						std::unordered_set<uint64_t> bubbleSpriteIds;
						size_t firstRankIconOrder = (std::numeric_limits<size_t>::max)();
						size_t lastBubbleOrder = 0;
						bool hasBubbleOrder = false;
						bool hasRankIconOrder = false;
						for (const SceneFishingHookPoolEntry& entry : pool->fishingHookPoolEntries) {
							const SceneEntity* hookEntity = document.FindEntity(entry.hookEntityId);
							const SceneComponent* hook = hookEntity
								? SceneEntityQuery::FindEnabledComponent(*hookEntity, "FishingHook")
								: nullptr;
							if (!hook) {
								continue;
							}
							const uint64_t bubbleId = hook->fishingHookBubbleSpriteEntityId;
							const uint64_t rankIconId = hook->fishingHookRankIconSpriteEntityId;
							const auto validateBubbleSprite = [
								&addIssue, &document, entityId = entity.id
							](uint64_t spriteId, const char* label) {
								const SceneEntity* spriteEntity = spriteId != 0
									? document.FindEntity(spriteId) : nullptr;
								const SceneComponent* sprite = spriteEntity
									? SceneEntityQuery::FindEnabledComponent(*spriteEntity, "SpriteRenderer")
									: nullptr;
								if (!sprite || sprite->spriteRenderSpace != "ScreenOverlay") {
									addIssue(
										SceneValidationSeverity::Error,
										entityId,
										std::string("FishingScoreAttackDirector hook rank bubble requires a valid ScreenOverlay ") + label
									);
									return false;
								}
								return true;
							};
							if (!validateBubbleSprite(bubbleId, "bubble Sprite Entity") ||
								!validateBubbleSprite(rankIconId, "rank icon Sprite Entity")) {
								continue;
							}
							if (bubbleId == rankIconId ||
								!bubbleSpriteIds.insert(bubbleId).second ||
								!bubbleSpriteIds.insert(rankIconId).second) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"FishingScoreAttackDirector hook rank bubble Sprite Entity is duplicated"
								);
							}
							const auto bubbleOrder = entityOrderById.find(bubbleId);
							const auto rankIconOrder = entityOrderById.find(rankIconId);
							if (bubbleOrder != entityOrderById.end()) {
								lastBubbleOrder = (std::max)(lastBubbleOrder, bubbleOrder->second);
								hasBubbleOrder = true;
							}
							if (rankIconOrder != entityOrderById.end()) {
								firstRankIconOrder = (std::min)(firstRankIconOrder, rankIconOrder->second);
								hasRankIconOrder = true;
							}
						}
						if (hasBubbleOrder && hasRankIconOrder && lastBubbleOrder >= firstRankIconOrder) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingScoreAttackDirector requires all hook rank bubble Sprites before rank icon Sprites"
							);
						}
					}
				}
				if (!component.fishingUseHookBandSettings &&
					component.fishingDistanceBandCount > 0 &&
					component.fishingHooksPerDistanceBand > 0) {
					const SceneEntity* poolEntity = document.FindEntity(
						component.fishingHookPoolEntityId
					);
					const SceneComponent* pool = poolEntity
						? SceneEntityQuery::FindEnabledComponent(
							*poolEntity, "FishingHookPool"
						)
						: nullptr;
					const size_t requiredHookCount = static_cast<size_t>(
						component.fishingDistanceBandCount
					) * static_cast<size_t>(component.fishingHooksPerDistanceBand);
					if (pool && pool->fishingHookPoolEntries.size() < requiredHookCount) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingHookPool has fewer entries than the required active hook count"
						);
					}
				}
				std::unordered_set<uint64_t> fishIds;
				for (const uint64_t fishId : component.fishingFishEntityIds) {
					validateEntityReference(fishId, "Fishing fish");
					if (!fishIds.insert(fishId).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingScoreAttackDirector contains a duplicate fish Entity"
						);
					}
				}
				for (const uint64_t textId : {
					component.fishingFishCountTextEntityId,
					component.fishingTimerTextEntityId,
					component.fishingScoreTextEntityId,
					component.fishingMultiplierTextEntityId,
					component.fishingResultTextEntityId
				}) {
					if (textId == 0) {
						continue;
					}
					validateFishingComponentReference(textId, "TextRenderer", "Fishing HUD");
				}
				if (component.enabled && activeInHierarchy) {
					++fishingDirectorCount;
					if (firstFishingDirectorEntityId == 0) {
						firstFishingDirectorEntityId = entity.id;
					}
				}
			} else if (component.type == "FishingResultTracker") {
				if (component.fishingResultChannelId.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultTracker requires a channelId"
					);
				}
				if (component.fishingResultTieBreakMode != "HigherRank" &&
					component.fishingResultTieBreakMode != "LowerRank") {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultTracker has an invalid tie-break mode"
					);
				}
				const SceneComponent* fishingDirector =
					SceneEntityQuery::FindEnabledComponent(
						entity, "FishingScoreAttackDirector"
					);
				if (!fishingDirector) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultTracker requires a FishingScoreAttackDirector on the same Entity"
					);
				} else if (!fishingDirector->fishingUseHookBandSettings) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultTracker requires hook band settings on FishingScoreAttackDirector"
					);
				}
				const size_t trackerCount = static_cast<size_t>(std::count_if(
					entity.components.begin(), entity.components.end(),
					[](const SceneComponent& candidate) {
						return candidate.enabled &&
							candidate.type == "FishingResultTracker";
					}
				));
				if (trackerCount > 1) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Entity contains multiple active FishingResultTracker components"
					);
				}
			} else if (component.type == "FishingResultPresenter") {
				auto validatePresenterReference = [
					&addIssue,
					&document,
					&entity
				](uint64_t targetId, const char* componentType, const char* label) {
					if (targetId == 0) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) + " Entity is not set"
						);
						return;
					}
					const SceneEntity* target = document.FindEntity(targetId);
					if (!target || !SceneEntityQuery::FindEnabledComponent(
						*target, componentType
					)) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) +
								" does not reference an enabled " + componentType
						);
					}
				};
				if (component.fishingResultPresentationChannelId.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultPresenter requires a channelId"
					);
				}
				validatePresenterReference(
					component.fishingResultPresentationBackgroundEntityId,
					"SpriteRenderer",
					"Fishing Result Background"
				);
				validatePresenterReference(
					component.fishingResultPresentationScoreTextEntityId,
					"TextRenderer",
					"Fishing Result Score Text"
				);
				if (component.fishingResultPresentationCenterPanelEntityId != 0) {
					validatePresenterReference(
						component.fishingResultPresentationCenterPanelEntityId,
						"SpriteRenderer",
						"Fishing Result Center Panel"
					);
				}
				if (component.fishingResultPresentationFallbackVariantId.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultPresenter requires a fallback variant ID"
					);
				}
				std::unordered_set<std::string> variantIds;
				for (const SceneFishingResultVisualVariant& variant :
					component.fishingResultPresentationVariants) {
					if (variant.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter contains an empty variant ID"
						);
					} else if (!variantIds.insert(variant.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter contains a duplicate variant ID: " +
								variant.id
						);
					}
					if (variant.backgroundTexturePath.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter variants require a background texture"
						);
					}
					if (!variant.centerPanelTexturePath.empty() &&
						component.fishingResultPresentationCenterPanelEntityId == 0) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter center panel texture requires a center panel Entity"
						);
					}
				}
				if (!component.fishingResultPresentationFallbackVariantId.empty() &&
					!variantIds.contains(
						component.fishingResultPresentationFallbackVariantId
					)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultPresenter fallback variant ID does not resolve"
					);
				}
				if (component.fishingResultPresentationIncludeSharkInWinnerSelection &&
					!variantIds.contains(
						component.fishingResultPresentationSharkVariantId
					)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingResultPresenter shark variant ID does not resolve"
					);
				}
				std::unordered_set<uint64_t> decorationEntityIds;
				for (const SceneFishingResultDecorationEntry& decoration :
					component.fishingResultPresentationDecorations) {
					validatePresenterReference(
						decoration.spriteEntityId,
						"SpriteRenderer",
						"Fishing Result Decoration"
					);
					if (decoration.spriteEntityId != 0 &&
						!decorationEntityIds.insert(
							decoration.spriteEntityId
						).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter contains a duplicate decoration Entity"
						);
					}
					if (!std::isfinite(decoration.minScaleMultiplier) ||
						!std::isfinite(decoration.maxScaleMultiplier) ||
						decoration.minScaleMultiplier <= 0.0f ||
						decoration.minScaleMultiplier > decoration.maxScaleMultiplier ||
						!std::isfinite(decoration.periodSeconds) ||
						decoration.periodSeconds <= 0.0f ||
						!std::isfinite(decoration.phaseOffset) ||
						decoration.phaseOffset < 0.0f ||
						decoration.phaseOffset >= 1.0f) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingResultPresenter has invalid decoration pulse settings"
						);
					}
				}
				if (component.enabled && activeInHierarchy) {
					++fishingResultPresenterCount;
					if (firstFishingResultPresenterEntityId == 0) {
						firstFishingResultPresenterEntityId = entity.id;
					}
				}
			} else if (component.type == "FishingHookSpawnArea") {
				if (!std::isfinite(component.fishingSpawnHalfSizeX) ||
					!std::isfinite(component.fishingSpawnHalfSizeZ) ||
					component.fishingSpawnHalfSizeX <= 0.0f ||
					component.fishingSpawnHalfSizeZ <= 0.0f ||
					!std::isfinite(component.fishingSpawnMinimumDistance) ||
					component.fishingSpawnMinimumDistance < 0.0f ||
					component.fishingSpawnMaxAttempts <= 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingHookSpawnArea contains invalid bounds"
					);
				}
			} else if (component.type == "FishingHookPool") {
				std::unordered_set<uint64_t> hookIds;
				const SceneComponent* fishingDirector = nullptr;
				for (const SceneEntity& candidateEntity : document.GetEntities()) {
					for (const SceneComponent& candidateComponent : candidateEntity.components) {
						if (
							candidateComponent.enabled &&
							candidateComponent.type == "FishingScoreAttackDirector" &&
							candidateComponent.fishingHookPoolEntityId == entity.id
						) {
							fishingDirector = &candidateComponent;
							break;
						}
					}
					if (fishingDirector) {
						break;
					}
				}
				if (component.fishingHookPoolEntries.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingHookPool has no entries"
					);
				}
				for (const SceneFishingHookPoolEntry& entry : component.fishingHookPoolEntries) {
					validateEntityReference(entry.hookEntityId, "Fishing hook");
					const SceneEntity* hook = document.FindEntity(entry.hookEntityId);
					if (!hook || !SceneEntityQuery::FindEnabledComponent(*hook, "FishingHook")) {
						continue;
					}
					if (!hookIds.insert(entry.hookEntityId).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingHookPool contains a duplicate hook Entity"
						);
					}
					if (
						fishingDirector &&
						!fishingDirector->fishingUseHookBandSettings &&
						static_cast<int>(entry.weightsByDistanceBand.size()) !=
							fishingDirector->fishingDistanceBandCount
					) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingHookPool entry has a mismatched distance band count"
						);
					}
					for (const float weight : entry.weightsByDistanceBand) {
						if (!std::isfinite(weight) || weight < 0.0f) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingHookPool contains an invalid distance weight"
							);
							break;
						}
					}
				}
				if (fishingDirector && !fishingDirector->fishingUseHookBandSettings) {
					for (int bandIndex = 0;
						bandIndex < fishingDirector->fishingDistanceBandCount;
						++bandIndex) {
						float totalWeight = 0.0f;
						for (const SceneFishingHookPoolEntry& entry : component.fishingHookPoolEntries) {
							if (bandIndex < static_cast<int>(entry.weightsByDistanceBand.size())) {
								totalWeight += entry.weightsByDistanceBand[bandIndex];
							}
						}
						if (totalWeight <= 0.0f) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"FishingHookPool has no selectable hook for a distance band"
							);
						}
					}
				}
			} else if (component.type == "FishingHook") {
				if (component.fishingHookBaseScore < 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingHook base score must be greater than or equal to zero"
					);
				}
				const SceneComponent* collider =
					SceneEntityQuery::FindEnabledComponent(entity, "OBBCollider");
				if (!collider || !collider->colliderIsTrigger) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingHook requires an enabled Trigger Collider"
					);
				}
			} else if (component.type == "FishingShark") {
				if (!std::isfinite(component.fishingSharkRadiusX) ||
					component.fishingSharkRadiusX <= 0.0f ||
					!std::isfinite(component.fishingSharkRadiusZ) ||
					component.fishingSharkRadiusZ <= 0.0f ||
					!std::isfinite(component.fishingSharkAngularSpeed) ||
					!std::isfinite(component.fishingSharkInitialPhase) ||
					component.fishingSharkPenaltyScore < 0 ||
					!std::isfinite(component.fishingSharkHitCooldownSeconds) ||
					component.fishingSharkHitCooldownSeconds < 0.0f ||
					!std::isfinite(component.fishingSharkPathRandomness) ||
					component.fishingSharkPathRandomness < 0.0f ||
					component.fishingSharkPathRandomness > 1.0f ||
					!std::isfinite(component.fishingSharkWanderMoveSpeed) ||
					component.fishingSharkWanderMoveSpeed < 0.0f ||
					!std::isfinite(component.fishingSharkWanderMaximumTurnRate) ||
					component.fishingSharkWanderMaximumTurnRate < 0.0f ||
					!std::isfinite(component.fishingSharkObstacleAvoidanceDistance) ||
					component.fishingSharkObstacleAvoidanceDistance < 0.0f ||
					!std::isfinite(component.fishingSharkObstacleAvoidanceStrength) ||
					component.fishingSharkObstacleAvoidanceStrength < 0.0f ||
					component.fishingSharkObstacleAvoidanceStrength > 1.0f ||
					!std::isfinite(component.fishingSharkObstacleAvoidanceResponse) ||
					component.fishingSharkObstacleAvoidanceResponse < 0.0f ||
					!std::isfinite(component.fishingSharkPatrolRouteRebuildIntervalSeconds) ||
					component.fishingSharkPatrolRouteRebuildIntervalSeconds <= 0.0f ||
					!std::isfinite(component.fishingSharkNavigationCellSize) ||
					component.fishingSharkNavigationCellSize <= 0.0f ||
					!std::isfinite(component.fishingSharkWaypointAcceptanceDistance) ||
					component.fishingSharkWaypointAcceptanceDistance <= 0.0f ||
					!std::isfinite(component.fishingSharkObstacleClearance) ||
					component.fishingSharkObstacleClearance < 0.0f ||
					!std::isfinite(component.fishingSharkDetectionDistance) ||
					component.fishingSharkDetectionDistance < 0.0f ||
					!std::isfinite(component.fishingSharkLoseDistance) ||
					component.fishingSharkLoseDistance <
						component.fishingSharkDetectionDistance ||
					!std::isfinite(component.fishingSharkDetectionDelaySeconds) ||
					component.fishingSharkDetectionDelaySeconds < 0.0f ||
					!std::isfinite(component.fishingSharkLostTargetDelaySeconds) ||
					component.fishingSharkLostTargetDelaySeconds < 0.0f ||
					!std::isfinite(component.fishingSharkReacquireCooldownSeconds) ||
					component.fishingSharkReacquireCooldownSeconds < 0.0f ||
					!std::isfinite(component.fishingSharkChaseMoveSpeed) ||
					component.fishingSharkChaseMoveSpeed < 0.0f ||
					!std::isfinite(component.fishingSharkChaseMaximumTurnRate) ||
					component.fishingSharkChaseMaximumTurnRate <= 0.0f ||
					!std::isfinite(component.fishingSharkChaseRouteRebuildIntervalSeconds) ||
					component.fishingSharkChaseRouteRebuildIntervalSeconds <= 0.0f ||
					!std::isfinite(component.fishingSharkAlertColor.x) ||
					!std::isfinite(component.fishingSharkAlertColor.y) ||
					!std::isfinite(component.fishingSharkAlertColor.z) ||
					!std::isfinite(component.fishingSharkAlertColor.w) ||
					component.fishingSharkAlertColor.x < 0.0f ||
					component.fishingSharkAlertColor.x > 1.0f ||
					component.fishingSharkAlertColor.y < 0.0f ||
					component.fishingSharkAlertColor.y > 1.0f ||
					component.fishingSharkAlertColor.z < 0.0f ||
					component.fishingSharkAlertColor.z > 1.0f ||
					component.fishingSharkAlertColor.w < 0.0f ||
					component.fishingSharkAlertColor.w > 1.0f ||
					!std::isfinite(component.fishingSharkAlertEmissiveIntensity) ||
					component.fishingSharkAlertEmissiveIntensity < 0.0f ||
					component.fishingSharkAlertEmissiveIntensity > 8.0f ||
					!std::isfinite(component.fishingSharkAlertPulseSpeed) ||
					component.fishingSharkAlertPulseSpeed < 0.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingShark contains invalid patrol or penalty settings"
					);
				}
				const SceneComponent* collider =
					SceneEntityQuery::FindEnabledComponent(entity, "OBBCollider");
				if (!collider || !collider->colliderIsTrigger) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingShark requires an enabled Trigger OBB Collider"
					);
				}
				bool hasFishingDirector = false;
				for (const SceneEntity& candidate : document.GetEntities()) {
					if (SceneEntityQuery::FindEnabledComponent(
						candidate, "FishingScoreAttackDirector"
					)) {
						hasFishingDirector = true;
						break;
					}
				}
				if (!hasFishingDirector) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingShark requires a FishingScoreAttackDirector"
					);
				}
			} else if (component.type == "FishingObstacle") {
				const SceneComponent* meshRenderer =
					SceneEntityQuery::FindEnabledComponent(entity, "MeshRenderer");
				const SceneComponent* collider =
					SceneEntityQuery::FindEnabledComponent(entity, "OBBCollider");
				if (!meshRenderer || meshRenderer->modelPath.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingObstacle requires an enabled MeshRenderer with a model"
					);
				}
				if (!collider || !collider->colliderActive || collider->colliderIsTrigger) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingObstacle requires an active non-trigger Box or Sphere Collider"
					);
					continue;
				}
				if (collider->colliderShape != "Box" &&
					collider->colliderShape != "Sphere") {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"FishingObstacle collider shape must be Box or Sphere"
					);
					continue;
				}
				if (collider->colliderShape == "Sphere") {
					if (!std::isfinite(collider->colliderSphereRadius) ||
						collider->colliderSphereRadius <= 0.0f) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingObstacle Sphere radius must be finite and greater than zero"
						);
						continue;
					}
					const Matrix4x4 world =
						SceneTransformResolver::ResolveSceneWorldMatrix(document, entity);
					const float scaleX = WorldAxisScale(world, 0);
					const float scaleY = WorldAxisScale(world, 1);
					const float scaleZ = WorldAxisScale(world, 2);
					const float maxScale = (std::max)(
						scaleX, (std::max)(scaleY, scaleZ)
					);
					const float minScale = (std::min)(
						scaleX, (std::min)(scaleY, scaleZ)
					);
					const float effectiveRadius =
						collider->colliderSphereRadius * maxScale;
					if (!std::isfinite(maxScale) || maxScale <= 0.0f ||
						!std::isfinite(effectiveRadius) || effectiveRadius <= 0.0f) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"FishingObstacle Sphere world radius is invalid"
						);
						continue;
					}
					if (maxScale - minScale > maxScale * 0.01f) {
						std::ostringstream message;
						message << "FishingObstacle Sphere uses the largest World axis scale; effective radius is "
							<< effectiveRadius << ".";
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							message.str()
						);
					}
				}
			}
		}
	}
	std::unordered_set<std::string> obstacleProfileModelPaths;
	for (const SceneFishingObstacleColliderProfile& profile :
		document.GetFishingObstacleSettings().colliderProfiles) {
		const bool hasFiniteValues =
			std::isfinite(profile.colliderOffset.x) &&
			std::isfinite(profile.colliderOffset.y) &&
			std::isfinite(profile.colliderOffset.z) &&
			std::isfinite(profile.colliderRotation.x) &&
			std::isfinite(profile.colliderRotation.y) &&
			std::isfinite(profile.colliderRotation.z) &&
			std::isfinite(profile.colliderSizeMultiplier.x) &&
			std::isfinite(profile.colliderSizeMultiplier.y) &&
			std::isfinite(profile.colliderSizeMultiplier.z) &&
			std::isfinite(profile.colliderSphereRadius);
		if (profile.modelPath.empty() || !hasFiniteValues ||
			profile.colliderSizeMultiplier.x <= 0.0f ||
			profile.colliderSizeMultiplier.y <= 0.0f ||
			profile.colliderSizeMultiplier.z <= 0.0f ||
			profile.colliderSphereRadius <= 0.0f) {
			addIssue(
				SceneValidationSeverity::Error,
				0,
				"Scene FishingObstacle settings contain an invalid model collider profile"
			);
		} else if (!obstacleProfileModelPaths.insert(profile.modelPath).second) {
			addIssue(
				SceneValidationSeverity::Error,
				0,
				"Scene FishingObstacle settings contain duplicate model collider profiles"
			);
		}
	}
	if (fishingDirectorCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstFishingDirectorEntityId,
			"Scene contains multiple active FishingScoreAttackDirector components"
		);
	}
	if (fishingResultPresenterCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstFishingResultPresenterEntityId,
			"Scene contains multiple active FishingResultPresenter components"
		);
	}
	if (screenOverlayCanvasCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstScreenOverlayCanvasEntityId,
			"Scene contains multiple active ScreenOverlayCanvas components"
		);
	}
	if (activeAudioListenerCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstAudioListenerEntityId,
			"Scene contains multiple active AudioListener components"
		);
	}
	if (persistentBgmPlayOnStartCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstPersistentBgmEntityId,
			"Scene contains multiple Play On Start Persistent BGM AudioSources"
		);
	}

	for (size_t index = firstIssueIndex; index < issues.size(); ++index) {
		if (issues[index].severity == SceneValidationSeverity::Error) {
			return false;
		}
	}
	return true;
}

bool SceneValidator::ValidateCatalog(
	const SceneCatalog& catalog,
	std::vector<SceneValidationIssue>& issues
) {
	issues.clear();
	for (const SceneDescriptor& descriptor : catalog.GetScenes()) {
		SceneDocument document;
		if (!document.Load(descriptor.filePath)) {
			issues.push_back({
				SceneValidationSeverity::Error,
				descriptor.id,
				descriptor.filePath,
				0,
				"Scene could not be loaded: " + document.GetLastLoadError()
			});
			continue;
		}
		if (!document.GetLastLoadError().empty()) {
			issues.push_back({
				SceneValidationSeverity::Warning,
				descriptor.id,
				descriptor.filePath,
				0,
				document.GetLastLoadError()
			});
		} else if (document.IsDirty()) {
			issues.push_back({
				SceneValidationSeverity::Warning,
				descriptor.id,
				descriptor.filePath,
				0,
				"Scene JSON was migrated in memory; save it in the Editor"
			});
		}
		ValidateDocument(
			document,
			&catalog,
			descriptor.id,
			descriptor.filePath,
			issues
		);
		for (const SceneEntity& entity : document.GetEntities()) {
			for (const SceneComponent& component : entity.components) {
				if (
					component.type != "EntityReference" ||
					component.entityReferenceTarget.entityId == 0 ||
					component.entityReferenceTarget.sceneId.empty() ||
					component.entityReferenceTarget.sceneId == descriptor.id
				) {
					continue;
				}
				const SceneDescriptor* targetDescriptor = catalog.Find(
					component.entityReferenceTarget.sceneId
				);
				if (!targetDescriptor) {
					continue;
				}
				SceneDocument targetDocument;
				if (
					targetDocument.Load(targetDescriptor->filePath) &&
					!targetDocument.FindEntity(
						component.entityReferenceTarget.entityId
					)
				) {
					issues.push_back({
						SceneValidationSeverity::Error,
						descriptor.id,
						descriptor.filePath,
						entity.id,
						"EntityReference target Entity does not exist in Scene " +
							targetDescriptor->id + ": " +
							std::to_string(
								component.entityReferenceTarget.entityId
							)
					});
				}
			}
		}
	}
	return !HasErrors(issues);
}

std::string SceneValidator::FormatIssues(
	const std::vector<SceneValidationIssue>& issues,
	size_t maxIssueCount
) {
	std::ostringstream output;
	const size_t count = (std::min)(issues.size(), maxIssueCount);
	for (size_t index = 0; index < count; ++index) {
		const SceneValidationIssue& issue = issues[index];
		if (index != 0) {
			output << '\n';
		}
		output << (issue.severity == SceneValidationSeverity::Error
			? "Error"
			: "Warning");
		if (!issue.sceneId.empty()) {
			output << " [" << issue.sceneId << "]";
		}
		if (issue.entityId != 0) {
			output << " Entity " << issue.entityId;
		}
		output << ": " << issue.message;
	}
	if (issues.size() > count) {
		output << '\n' << "... and " << (issues.size() - count) << " more";
	}
	return output.str();
}
