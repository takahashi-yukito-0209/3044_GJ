// 役割: GameFlowDirectorのRuntime state、phase進行、gameplay許可と値型Requestを所有する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SceneDocument;
class SceneEnemySpawnerSystem;

enum class SceneGameFlowState { Inactive, Preflight, Countdown, StartCue, PhaseActive, PhaseGap, ResultDelay, Result, Faulted };

struct SceneGameFlowTextRequest { uint64_t entityId = 0; std::string text; };
struct SceneGameFlowMotionRequest { uint64_t entityId = 0; std::string clipId; };
struct SceneGameFlowWaveRequest { uint64_t spawnerEntityId = 0; uint64_t generation = 0; int count = 0; };
struct SceneGameFlowEntityRequest { uint64_t entityId = 0; bool active = false; };

struct SceneGameFlowResult {
	bool hasDirector = false;
	bool gameplayAllowed = true;
	std::vector<SceneGameFlowTextRequest> textRequests;
	std::vector<SceneGameFlowMotionRequest> motionRequests;
	std::vector<SceneGameFlowWaveRequest> waveRequests;
	std::vector<SceneGameFlowEntityRequest> entityRequests;
};

class SceneGameFlowSystem {
public:
	SceneGameFlowResult Update(
		SceneDocument& document,
		SceneEnemySpawnerSystem& spawnerSystem,
		float deltaTime,
		bool advance
	);
	bool IsGameplayAllowed() const { return gameplayAllowed_; }
	void Clear();

private:
	bool Preflight(const SceneDocument& document, uint64_t directorEntityId, std::string& diagnostic) const;
	void EnterPhase(const SceneDocument& document, const struct SceneComponent& director, SceneGameFlowResult& result);
	static std::string FormatTimer(double seconds, bool centiseconds);

	SceneGameFlowState state_ = SceneGameFlowState::Inactive;
	uint64_t directorEntityId_ = 0;
	int phaseIndex_ = 0;
	int countdownValue_ = 0;
	uint64_t nextGeneration_ = 1;
	std::vector<uint64_t> activeWaveGenerations_;
	double elapsedSeconds_ = 0.0;
	float stateElapsedSeconds_ = 0.0f;
	bool timerRunning_ = false;
	bool gameplayAllowed_ = true;
};
