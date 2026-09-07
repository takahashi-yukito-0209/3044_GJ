// 役割: StateMachine ComponentとC++で登録された行動をEntity単位で実行する。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../SceneRuntimeObjectBinding.h"

class Object3d;
class Player;
class SceneAttackRunnerSystem;
class SceneDocument;
class ScenePrefabAnimationSystem;
struct PhysicsBody;
struct SceneEntity;
struct SceneStateDefinition;

class StateContext {
public:
	SceneEntity& GetEntity() const;
	Object3d* GetRuntimeObject() const { return object_; }
	PhysicsBody* GetPhysicsBody() const { return body_; }
	float GetStateTime() const { return stateTime_; }

	float GetFloat(const std::string& name, float fallback = 0.0f) const;
	int GetInt(const std::string& name, int fallback = 0) const;
	bool GetBool(const std::string& name, bool fallback = false) const;
	std::string GetString(
		const std::string& name,
		const std::string& fallback = {}
	) const;
	SceneEntity* GetEntityParameter(const std::string& name) const;
	SceneEntity* ResolveEntityReference(
		uint64_t entityId,
		const std::string& entityName
	) const;

	bool IsKeyDown(const std::string& keyName) const;
	bool IsKeyTriggered(const std::string& keyName) const;
	bool IsInputDown(const std::string& inputName) const;
	bool IsInputTriggered(const std::string& inputName) const;
	bool HasMoveInput() const;
	void SetHorizontalVelocity(float x, float z);
	void StopHorizontalMovement();
	void SetEntityActive(SceneEntity* entity, bool active);
	bool PlayAnimation(const std::string& clipName, bool restart = true);
	bool PlayPrefabAnimation(
		SceneEntity* target,
		const std::string& clipName,
		bool restart = true,
		float transitionDuration = 0.0f
	);
	bool StartAttack(SceneEntity* attackSetEntity, const std::string& attackName);
	void StopAttack();
	bool IsAttackFinished() const;
	float GetAttackTime() const;
	float GetAttackDuration() const;
	void SetAttackLoopRequested(bool requested);
	void RequestState(const std::string& stateName);

private:
	friend class SceneStateMachineSystem;
	StateContext(
		SceneDocument& document,
		SceneEntity& entity,
		Object3d* object,
		PhysicsBody* body,
		SceneAttackRunnerSystem& attackRunnerSystem,
		ScenePrefabAnimationSystem& prefabAnimationSystem,
		const SceneStateDefinition& state,
		float stateTime,
		std::string& requestedState
	);

	SceneDocument& document_;
	SceneEntity& entity_;
	Object3d* object_ = nullptr;
	PhysicsBody* body_ = nullptr;
	SceneAttackRunnerSystem& attackRunnerSystem_;
	ScenePrefabAnimationSystem& prefabAnimationSystem_;
	const SceneStateDefinition& state_;
	float stateTime_ = 0.0f;
	std::string& requestedState_;
};

class IEntityStateAction {
public:
	virtual ~IEntityStateAction() = default;
	virtual void Enter(StateContext&) {}
	virtual void Update(StateContext&, float deltaTime) = 0;
	virtual void Exit(StateContext&) {}
};

class EntityStateRegistry {
public:
	using Factory = std::function<std::unique_ptr<IEntityStateAction>()>;

	static EntityStateRegistry& GetInstance();
	void Register(const std::string& actionId, Factory factory);
	bool Contains(const std::string& actionId) const;
	std::unique_ptr<IEntityStateAction> Create(
		const std::string& actionId
	) const;

	template<class T>
	void Register(const std::string& actionId) {
		Register(actionId, []() { return std::make_unique<T>(); });
	}

private:
	std::unordered_map<std::string, Factory> factories_;
};

class SceneStateMachineSystem {
public:
	SceneStateMachineSystem();
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		Player* player,
		SceneAttackRunnerSystem& attackRunnerSystem,
		ScenePrefabAnimationSystem& prefabAnimationSystem,
		float deltaTime,
		const std::function<bool(uint64_t)>& shouldProcess
	);
	bool RequestState(uint64_t entityId, const std::string& stateName);
	const std::string* GetCurrentState(uint64_t entityId) const;
	void ResetEntity(uint64_t entityId);
	void Clear();

private:
	struct Runtime {
		std::string currentState;
		float stateTime = 0.0f;
		std::unique_ptr<IEntityStateAction> action;
		bool initialized = false;
	};

	std::unordered_map<uint64_t, Runtime> runtimes_;
	std::unordered_map<uint64_t, std::string> externalRequests_;
};
