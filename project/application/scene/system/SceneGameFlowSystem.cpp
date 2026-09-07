// 役割: GameFlowの保存値を読取り、既存Systemへ渡す値型Requestだけを生成する。
#include "SceneGameFlowSystem.h"

#include "SceneEnemySpawnerSystem.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {
	const SceneComponent* FindDirector(const SceneDocument& document, uint64_t& entityId) {
		const SceneComponent* found = nullptr;
		entityId = 0;
		for (const SceneEntity& entity : document.GetEntities()) {
			const SceneComponent* component = SceneEntityQuery::FindEnabledComponent(entity, "GameFlowDirector");
			if (!component) continue;
			if (found) return nullptr;
			found = component;
			entityId = entity.id;
		}
		return found;
	}

	bool HasComponent(const SceneDocument& document, uint64_t entityId, const char* type) {
		const SceneEntity* entity = document.FindEntity(entityId);
		return entity && SceneEntityQuery::FindEnabledComponent(*entity, type);
	}

	bool HasActiveComponent(const SceneDocument& document, uint64_t entityId, const char* type) {
		const SceneEntity* entity = document.FindEntity(entityId);
		return entity && entity->active && SceneEntityQuery::FindEnabledComponent(*entity, type);
	}

	bool HasMotionClip(const SceneDocument& document, uint64_t entityId, const std::string& clipId) {
		const SceneEntity* entity = document.FindEntity(entityId);
		const SceneComponent* motion = entity ? SceneEntityQuery::FindEnabledComponent(*entity, "TextMotion") : nullptr;
		return motion && std::any_of(motion->textMotionClips.begin(), motion->textMotionClips.end(), [&clipId](const SceneTextMotionClip& clip) { return clip.id == clipId; });
	}

	int CalculateRemainingEnemies(
		const SceneComponent& director,
		const SceneEnemySpawnerSystem& spawnerSystem,
		int phaseIndex,
		const std::vector<uint64_t>& activeWaveGenerations
	) {
		if (phaseIndex < 0 || phaseIndex >= static_cast<int>(director.gameFlowPhases.size())) return 0;
		const SceneGameFlowPhase& phase = director.gameFlowPhases[phaseIndex];
		long long remaining = 0;
		for (size_t index = 0; index < phase.waves.size(); ++index) {
			const SceneGameFlowWave& wave = phase.waves[index];
			int waveRemaining = wave.count;
			if (index < activeWaveGenerations.size()) {
				const SceneEnemySpawnerSystem::FiniteWaveStatus status = spawnerSystem.GetFiniteWaveStatus(wave.spawnerEntityId);
				if (status.generation == activeWaveGenerations[index]) {
					const int defeated = (std::clamp)(status.activatedCount - status.activeCount, 0, (std::max)(status.requestedCount, 0));
					waveRemaining = (std::max)(status.requestedCount - defeated, 0);
				}
			}
			remaining += waveRemaining;
		}
		return static_cast<int>((std::min)(remaining, static_cast<long long>((std::numeric_limits<int>::max)())));
	}
}

SceneGameFlowResult SceneGameFlowSystem::Update(
	SceneDocument& document,
	SceneEnemySpawnerSystem& spawnerSystem,
	float deltaTime,
	bool advance
) {
	SceneGameFlowResult result{};
	uint64_t foundDirectorId = 0;
	const SceneComponent* director = FindDirector(document, foundDirectorId);
	if (!director) {
		if (foundDirectorId != 0 || directorEntityId_ != 0) { state_ = SceneGameFlowState::Faulted; gameplayAllowed_ = false; result.gameplayAllowed = false; }
		else Clear();
		return result;
	}
	result.hasDirector = true;
	if (!advance) {
		result.gameplayAllowed = gameplayAllowed_;
		const auto addText = [&result](uint64_t entityId, std::string text) {
			if (entityId != 0) {
				result.textRequests.push_back({ entityId, std::move(text) });
			}
		};
		if (state_ == SceneGameFlowState::Countdown) {
			addText(
				director->gameFlowCountdownTextEntityId,
				std::to_string(countdownValue_)
			);
		} else if (state_ == SceneGameFlowState::StartCue) {
			addText(
				director->gameFlowCountdownTextEntityId,
				director->gameFlowStartCueText
			);
		} else {
			addText(director->gameFlowCountdownTextEntityId, {});
		}
		if (
			state_ == SceneGameFlowState::PhaseActive ||
			state_ == SceneGameFlowState::PhaseGap ||
			state_ == SceneGameFlowState::ResultDelay
		) {
			const int labelIndex = state_ == SceneGameFlowState::PhaseActive
				? phaseIndex_
				: phaseIndex_ - 1;
			addText(
				director->gameFlowPhaseTextEntityId,
				labelIndex >= 0 &&
				labelIndex < static_cast<int>(director->gameFlowPhases.size())
					? director->gameFlowPhases[labelIndex].label
					: std::string{}
			);
		} else {
			addText(director->gameFlowPhaseTextEntityId, {});
		}
		if (
			state_ == SceneGameFlowState::PhaseActive ||
			state_ == SceneGameFlowState::PhaseGap ||
			state_ == SceneGameFlowState::ResultDelay
		) {
			const double displayedSeconds = std::floor(
				elapsedSeconds_ / director->gameFlowTimerDisplayStepSeconds
			) * director->gameFlowTimerDisplayStepSeconds;
			addText(
				director->gameFlowTimerTextEntityId,
				director->gameFlowTimerPrefix + FormatTimer(displayedSeconds, false)
			);
		} else {
			addText(director->gameFlowTimerTextEntityId, {});
		}
		if (state_ == SceneGameFlowState::PhaseActive) {
			addText(
				director->gameFlowRemainingTextEntityId,
				director->gameFlowRemainingPrefix + std::to_string(
					CalculateRemainingEnemies(
						*director,
						spawnerSystem,
						phaseIndex_,
						activeWaveGenerations_
					)
				)
			);
		} else if (
			state_ == SceneGameFlowState::PhaseGap ||
			state_ == SceneGameFlowState::ResultDelay
		) {
			addText(
				director->gameFlowRemainingTextEntityId,
				director->gameFlowRemainingPrefix + "0"
			);
		} else {
			addText(director->gameFlowRemainingTextEntityId, {});
		}
		if (state_ == SceneGameFlowState::Result) {
			addText(
				director->gameFlowResultTimeTextEntityId,
				director->gameFlowResultPrefix + FormatTimer(elapsedSeconds_, true)
			);
		}
		return result;
	}
	if (directorEntityId_ != foundDirectorId) {
		Clear();
		directorEntityId_ = foundDirectorId;
	}
	if (!director->gameFlowAutoStart && state_ == SceneGameFlowState::Inactive) {
		result.gameplayAllowed = false;
		gameplayAllowed_ = false;
		return result;
	}
	if (state_ == SceneGameFlowState::Inactive) state_ = SceneGameFlowState::Preflight;
	if (state_ == SceneGameFlowState::Preflight) {
		std::string diagnostic;
		if (!Preflight(document, foundDirectorId, diagnostic)) state_ = SceneGameFlowState::Faulted;
		else {
			state_ = SceneGameFlowState::Countdown;
			countdownValue_ = director->gameFlowCountdownStart;
			result.motionRequests.push_back({ director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId });
		}
	}
	stateElapsedSeconds_ += (std::max)(deltaTime, 0.0f);
	if (timerRunning_) elapsedSeconds_ += (std::max)(deltaTime, 0.0f);
	if (state_ == SceneGameFlowState::Countdown) {
		const int expected = director->gameFlowCountdownStart - static_cast<int>(stateElapsedSeconds_ / director->gameFlowCountdownStepSeconds);
		while (countdownValue_ > (std::max)(expected, 0)) {
			--countdownValue_;
			if (countdownValue_ > 0) result.motionRequests.push_back({ director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId });
		}
		if (countdownValue_ <= 0) {
			state_ = SceneGameFlowState::StartCue;
			stateElapsedSeconds_ = 0.0f;
			result.motionRequests.push_back({ director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId });
		}
	} else if (state_ == SceneGameFlowState::StartCue && stateElapsedSeconds_ >= director->gameFlowStartCueSeconds) {
		state_ = SceneGameFlowState::PhaseActive;
		stateElapsedSeconds_ = 0.0f;
		timerRunning_ = true;
		EnterPhase(document, *director, result);
	} else if (state_ == SceneGameFlowState::PhaseActive) {
		bool failed = false;
		bool complete = !activeWaveGenerations_.empty();
		for (size_t index = 0; index < activeWaveGenerations_.size(); ++index) {
			const SceneEnemySpawnerSystem::FiniteWaveStatus status = spawnerSystem.GetFiniteWaveStatus(director->gameFlowPhases[phaseIndex_].waves[index].spawnerEntityId);
			failed |= status.state == SceneEnemySpawnerSystem::FiniteWaveState::Failed;
			complete &= status.state == SceneEnemySpawnerSystem::FiniteWaveState::Complete && status.generation == activeWaveGenerations_[index];
		}
		if (failed) state_ = SceneGameFlowState::Faulted;
		else if (complete) {
			++phaseIndex_;
			stateElapsedSeconds_ = 0.0f;
			if (phaseIndex_ >= static_cast<int>(director->gameFlowPhases.size())) { state_ = SceneGameFlowState::ResultDelay; timerRunning_ = false; }
			else state_ = SceneGameFlowState::PhaseGap;
		}
	} else if (state_ == SceneGameFlowState::PhaseGap && stateElapsedSeconds_ >= director->gameFlowInterPhaseDelaySeconds) {
		state_ = SceneGameFlowState::PhaseActive;
		stateElapsedSeconds_ = 0.0f;
		EnterPhase(document, *director, result);
	} else if (state_ == SceneGameFlowState::ResultDelay && stateElapsedSeconds_ >= director->gameFlowResultRevealDelaySeconds) {
		state_ = SceneGameFlowState::Result;
		result.entityRequests.push_back({ director->gameFlowResultRootEntityId, true });
		result.motionRequests.push_back({ director->gameFlowResultTimeTextEntityId, director->gameFlowResultMotionClipId });
	}
	gameplayAllowed_ = state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap;
	result.gameplayAllowed = gameplayAllowed_;
	if (state_ == SceneGameFlowState::Faulted) {
		for (const SceneGameFlowPhase& phase : director->gameFlowPhases) for (const SceneGameFlowWave& wave : phase.waves) spawnerSystem.StopFiniteWave(wave.spawnerEntityId);
	}
	const auto addText = [&result](uint64_t entityId, std::string text) {
		if (entityId != 0) result.textRequests.push_back({ entityId, std::move(text) });
	};
	if (state_ == SceneGameFlowState::Countdown) addText(director->gameFlowCountdownTextEntityId, std::to_string(countdownValue_));
	else if (state_ == SceneGameFlowState::StartCue) addText(director->gameFlowCountdownTextEntityId, director->gameFlowStartCueText);
	else addText(director->gameFlowCountdownTextEntityId, {});

	if (state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay) {
		const int labelIndex = state_ == SceneGameFlowState::PhaseActive ? phaseIndex_ : phaseIndex_ - 1;
		addText(director->gameFlowPhaseTextEntityId, labelIndex >= 0 && labelIndex < static_cast<int>(director->gameFlowPhases.size()) ? director->gameFlowPhases[labelIndex].label : std::string{});
	} else addText(director->gameFlowPhaseTextEntityId, {});

	if (state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay) {
		const double displayedSeconds = std::floor(elapsedSeconds_ / director->gameFlowTimerDisplayStepSeconds) * director->gameFlowTimerDisplayStepSeconds;
		addText(director->gameFlowTimerTextEntityId, director->gameFlowTimerPrefix + FormatTimer(displayedSeconds, false));
	} else addText(director->gameFlowTimerTextEntityId, {});

	if (state_ == SceneGameFlowState::PhaseActive) {
		addText(director->gameFlowRemainingTextEntityId, director->gameFlowRemainingPrefix + std::to_string(CalculateRemainingEnemies(*director, spawnerSystem, phaseIndex_, activeWaveGenerations_)));
	} else if (state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay) {
		addText(director->gameFlowRemainingTextEntityId, director->gameFlowRemainingPrefix + "0");
	} else addText(director->gameFlowRemainingTextEntityId, {});

	if (state_ == SceneGameFlowState::Result) addText(director->gameFlowResultTimeTextEntityId, director->gameFlowResultPrefix + FormatTimer(elapsedSeconds_, true));
	return result;
}

bool SceneGameFlowSystem::Preflight(const SceneDocument& document, uint64_t directorEntityId, std::string& diagnostic) const {
	const SceneEntity* entity = document.FindEntity(directorEntityId);
	const SceneComponent* director = entity ? SceneEntityQuery::FindEnabledComponent(*entity, "GameFlowDirector") : nullptr;
	if (!director || director->gameFlowCountdownStart < 1 || director->gameFlowCountdownStart > 9 || director->gameFlowPhases.empty() ||
		!std::isfinite(director->gameFlowCountdownStepSeconds) || !std::isfinite(director->gameFlowStartCueSeconds) ||
		!std::isfinite(director->gameFlowInterPhaseDelaySeconds) || !std::isfinite(director->gameFlowResultRevealDelaySeconds) ||
		!std::isfinite(director->gameFlowTimerDisplayStepSeconds) || director->gameFlowCountdownStepSeconds <= 0.0f ||
		director->gameFlowStartCueSeconds < 0.0f || director->gameFlowInterPhaseDelaySeconds < 0.0f ||
		director->gameFlowResultRevealDelaySeconds < 0.0f || director->gameFlowTimerDisplayStepSeconds <= 0.0f) { diagnostic = "GameFlowDirector values are invalid"; return false; }
	if (!HasComponent(document, director->gameFlowCountdownTextEntityId, "TextRenderer") || !HasMotionClip(document, director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId) || !HasComponent(document, director->gameFlowPhaseTextEntityId, "TextRenderer") || !HasMotionClip(document, director->gameFlowPhaseTextEntityId, director->gameFlowPhaseMotionClipId) || !HasComponent(document, director->gameFlowTimerTextEntityId, "TextRenderer") || !document.FindEntity(director->gameFlowResultRootEntityId) || !HasComponent(document, director->gameFlowResultTimeTextEntityId, "TextRenderer") || !HasMotionClip(document, director->gameFlowResultTimeTextEntityId, director->gameFlowResultMotionClipId) || (director->gameFlowRemainingTextEntityId != 0 && !HasActiveComponent(document, director->gameFlowRemainingTextEntityId, "TextRenderer"))) { diagnostic = "GameFlowDirector references are invalid"; return false; }
	const uint64_t textIds[] = { director->gameFlowCountdownTextEntityId, director->gameFlowPhaseTextEntityId, director->gameFlowTimerTextEntityId, director->gameFlowRemainingTextEntityId, director->gameFlowResultTimeTextEntityId };
	std::unordered_set<uint64_t> textEntityIds;
	for (const uint64_t textId : textIds) if (textId != 0 && !textEntityIds.insert(textId).second) { diagnostic = "GameFlow text targets overlap"; return false; }
	const SceneEntity* resultRoot = document.FindEntity(director->gameFlowResultRootEntityId);
	if (resultRoot->id == directorEntityId || document.IsDescendantOf(directorEntityId, resultRoot->id)) { diagnostic = "GameFlow result root is invalid"; return false; }
	std::unordered_set<std::string> phaseIds;
	for (const SceneGameFlowPhase& phase : director->gameFlowPhases) {
		if (phase.id.empty() || phase.waves.empty() || !phaseIds.insert(phase.id).second) { diagnostic = "GameFlow phase is invalid"; return false; }
		std::unordered_set<uint64_t> spawnerIds;
		for (const SceneGameFlowWave& wave : phase.waves) {
			const SceneEntity* spawnerEntity = document.FindEntity(wave.spawnerEntityId);
			const SceneComponent* spawner = spawnerEntity ? SceneEntityQuery::FindEnabledComponent(*spawnerEntity, "EnemySpawner") : nullptr;
			if (!spawner || spawner->enemySpawnerAutoStart || spawner->enemySpawnerPrefabPath.empty() || wave.count <= 0 || !spawnerIds.insert(wave.spawnerEntityId).second) { diagnostic = "GameFlow wave is invalid"; return false; }
		}
	}
	return true;
}

void SceneGameFlowSystem::EnterPhase(const SceneDocument&, const SceneComponent& director, SceneGameFlowResult& result) {
	if (phaseIndex_ < 0 || phaseIndex_ >= static_cast<int>(director.gameFlowPhases.size())) return;
	const SceneGameFlowPhase& phase = director.gameFlowPhases[phaseIndex_];
	result.motionRequests.push_back({ director.gameFlowPhaseTextEntityId, director.gameFlowPhaseMotionClipId });
	activeWaveGenerations_.clear();
	for (const SceneGameFlowWave& wave : phase.waves) { const uint64_t generation = nextGeneration_++; activeWaveGenerations_.push_back(generation); result.waveRequests.push_back({ wave.spawnerEntityId, generation, wave.count }); }
}

std::string SceneGameFlowSystem::FormatTimer(double seconds, bool centiseconds) {
	const double clamped = (std::min)(seconds, 5999.99);
	const int minutes = static_cast<int>(clamped / 60.0);
	const double remainder = clamped - minutes * 60.0;
	char buffer[32]{};
	if (centiseconds) std::snprintf(buffer, sizeof(buffer), "%02d:%05.2f", minutes, remainder);
	else std::snprintf(buffer, sizeof(buffer), "%02d:%04.1f", minutes, remainder);
	return buffer;
}

void SceneGameFlowSystem::Clear() { state_ = SceneGameFlowState::Inactive; directorEntityId_ = 0; phaseIndex_ = 0; countdownValue_ = 0; nextGeneration_ = 1; activeWaveGenerations_.clear(); elapsedSeconds_ = 0.0; stateElapsedSeconds_ = 0.0f; timerRunning_ = false; gameplayAllowed_ = true; }
