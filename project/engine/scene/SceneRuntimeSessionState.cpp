// 役割: Play中だけ有効なFishing Result recordの保存とgeneration採番を実装する。
#include "SceneRuntimeSessionState.h"

#include <limits>
#include <utility>

void SceneRuntimeSessionState::Clear() {
	results_.clear();
	nextGenerations_.clear();
}

void SceneRuntimeSessionState::BeginFishingRun(const std::string& channelId) {
	if (channelId.empty()) {
		return;
	}
	results_.erase(channelId);
}

uint64_t SceneRuntimeSessionState::PublishFishingResult(
	SceneFishingResultRecord record
) {
	if (record.channelId.empty()) {
		return 0;
	}

	uint64_t& nextGeneration = nextGenerations_[record.channelId];
	if (nextGeneration == (std::numeric_limits<uint64_t>::max)()) {
		nextGeneration = 1;
	} else {
		++nextGeneration;
	}
	record.generation = nextGeneration;
	const uint64_t generation = record.generation;
	results_[record.channelId] = std::move(record);
	return generation;
}

const SceneFishingResultRecord* SceneRuntimeSessionState::FindFishingResult(
	const std::string& channelId
) const {
	if (channelId.empty()) {
		return nullptr;
	}
	const auto found = results_.find(channelId);
	return found == results_.end() ? nullptr : &found->second;
}
