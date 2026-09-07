// 役割: Play中のScene遷移を越えて一時的なResult recordを保持する。
#pragma once

#include "../math/Vector4.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SceneFishingResultRankRecord {
	std::string rankId;
	std::string displayName;
	float scoreMultiplier = 1.0f;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	std::string iconTexturePath;
	uint64_t catchCount = 0;
	uint64_t fishWeightedCount = 0;
};

struct SceneFishingResultRecord {
	std::string channelId;
	uint64_t generation = 0;
	std::string sourceSceneId;
	uint64_t sourceSceneInstanceId = 0;
	uint64_t sourceDirectorEntityId = 0;
	int activeRankCount = 0;
	long long totalScore = 0;
	double elapsedSeconds = 0.0;
	bool hasWinner = false;
	std::string winningRankId;
	int winningRankIndex = -1;
	uint64_t winningFishWeightedCount = 0;
	std::vector<SceneFishingResultRankRecord> ranks;
};

class SceneRuntimeSessionState final {
public:
	void Clear();
	void BeginFishingRun(const std::string& channelId);
	uint64_t PublishFishingResult(SceneFishingResultRecord record);
	const SceneFishingResultRecord* FindFishingResult(
		const std::string& channelId
	) const;

private:
	std::unordered_map<std::string, SceneFishingResultRecord> results_;
	std::unordered_map<std::string, uint64_t> nextGenerations_;
};
