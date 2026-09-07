// 役割: State遷移を遅延確定し、移動・攻撃・被弾・死亡の組み込み行動を提供する。
#include "SceneStateMachineSystem.h"

#include "SceneAttackRunnerSystem.h"
#include "ScenePrefabAnimationSystem.h"
#include "../../../engine/3d/Object3d.h"
#include "../../../engine/io/Input.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/math/Quaternion.h"
#include "../../../engine/physics/PhysicsBody.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../player/Player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace {
	const SceneStateParameter* FindParameter(
		const SceneStateDefinition& state,
		const std::string& name
	) {
		const auto found = std::find_if(
			state.parameters.begin(),
			state.parameters.end(),
			[&](const SceneStateParameter& parameter) {
				return parameter.name == name;
			}
		);
		return found == state.parameters.end() ? nullptr : &*found;
	}

	BYTE ResolveKey(const std::string& keyName) {
		std::string key = keyName;
		std::transform(
			key.begin(), key.end(), key.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::toupper(character));
			}
		);
		if (key == "ENTER") return DIK_RETURN;
		if (key == "SPACE") return DIK_SPACE;
		if (key == "ESCAPE") return DIK_ESCAPE;
		if (key == "TAB") return DIK_TAB;
		if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
			static const BYTE letters[] = {
				DIK_A, DIK_B, DIK_C, DIK_D, DIK_E, DIK_F, DIK_G,
				DIK_H, DIK_I, DIK_J, DIK_K, DIK_L, DIK_M, DIK_N,
				DIK_O, DIK_P, DIK_Q, DIK_R, DIK_S, DIK_T, DIK_U,
				DIK_V, DIK_W, DIK_X, DIK_Y, DIK_Z
			};
			return letters[key[0] - 'A'];
		}
		return 0;
	}

	bool ResolveMouseButton(
		const std::string& inputName,
		Input::MouseButton& button
	) {
		std::string normalized;
		normalized.reserve(inputName.size());
		for (const unsigned char character : inputName) {
			if (std::isalnum(character)) {
				normalized.push_back(static_cast<char>(std::toupper(character)));
			}
		}
		if (
			normalized == "MOUSELEFT" ||
			normalized == "LEFTMOUSE" ||
			normalized == "LMB"
		) {
			button = Input::MouseButton::Left;
			return true;
		}
		if (
			normalized == "MOUSERIGHT" ||
			normalized == "RIGHTMOUSE" ||
			normalized == "RMB"
		) {
			button = Input::MouseButton::Right;
			return true;
		}
		if (
			normalized == "MOUSEMIDDLE" ||
			normalized == "MIDDLEMOUSE" ||
			normalized == "MMB"
		) {
			button = Input::MouseButton::Middle;
			return true;
		}
		return false;
	}

	std::string GetAttackInput(StateContext& context) {
		const std::string input = context.GetString("AttackInput");
		return input.empty()
			? context.GetString("AttackKey", "J")
			: input;
	}

	const SceneAttackDefinition* ResolveAttackDefinition(StateContext& context) {
		SceneEntity* target = context.GetEntityParameter("AnimationTarget");
		const SceneComponent* attackSet = target
			? SceneEntityQuery::FindEnabledComponent(*target, "AttackSet")
			: nullptr;
		if (!attackSet) { return nullptr; }
		const std::string name = context.GetString("AttackName", context.GetString("Animation"));
		auto found = std::find_if(attackSet->attackDefinitions.begin(), attackSet->attackDefinitions.end(),
			[&](const SceneAttackDefinition& attack) { return attack.name == name; });
		return found == attackSet->attackDefinitions.end() ? nullptr : &*found;
	}


	float ApplyStateEasing(float value, const std::string& easing) {
		value = std::clamp(value, 0.0f, 1.0f);
		if (easing == "Linear") {
			return value;
		}
		if (easing == "EaseIn") {
			return value * value * value;
		}
		if (easing == "EaseOut") {
			return Math::EaseOutCubic(value);
		}
		if (easing == "EaseInOut") {
			return value < 0.5f
				? 4.0f * value * value * value
				: 1.0f - std::pow(-2.0f * value + 2.0f, 3.0f) * 0.5f;
		}
		return Math::SmoothStep(value);
	}

	void ResolvePlanarAttackAxes(
		StateContext& context,
		Vector3& forward,
		Vector3& right
	) {
		Matrix4x4 rotation = MakeRotateMatrix(context.GetEntity().transform.rotate);
		const Matrix4x4& basis = context.GetRuntimeObject()
			? context.GetRuntimeObject()->GetWorldMatrix()
			: rotation;
		forward = { basis.m[2][0], 0.0f, basis.m[2][2] };
		right = { basis.m[0][0], 0.0f, basis.m[0][2] };
		forward = Math::Length(forward) > 0.0001f
			? Math::Normalize(forward)
			: Vector3{ 0.0f, 0.0f, 1.0f };
		right = Math::Length(right) > 0.0001f
			? Math::Normalize(right)
			: Vector3{ 1.0f, 0.0f, 0.0f };
	}

	void PlayReturnPrefabAnimation(StateContext& context) {
		SceneEntity* animationTarget =
			context.GetEntityParameter("AnimationTarget");
		const std::string returnAnimation =
			context.GetString("ReturnAnimation", "Idle");
		if (!animationTarget || returnAnimation.empty()) {
			return;
		}
		context.PlayPrefabAnimation(
			animationTarget,
			returnAnimation,
			true,
			(std::max)(context.GetFloat("ReturnAnimationBlend", 0.15f), 0.0f)
		);
	}

	const SceneStateDefinition* FindState(
		const SceneComponent& machine,
		const std::string& stateName
	) {
		const auto found = std::find_if(
			machine.stateMachineStates.begin(),
			machine.stateMachineStates.end(),
			[&](const SceneStateDefinition& state) {
				return state.name == stateName;
			}
		);
		return found == machine.stateMachineStates.end() ? nullptr : &*found;
	}

	class BuiltinIdleState final : public IEntityStateAction {
	public:
		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
			const std::string attackState = context.GetString("AttackState");
			if (
				!attackState.empty() &&
				context.IsInputTriggered(GetAttackInput(context))
			) {
				context.StopHorizontalMovement();
				context.RequestState(attackState);
				return;
			}
			if (context.HasMoveInput()) {
				context.RequestState(context.GetString("MoveState", "Move"));
			}
		}
	};

	class BuiltinMoveState final : public IEntityStateAction {
	public:
		void Update(StateContext& context, float) override {
			const std::string attackState = context.GetString("AttackState");
			if (
				!attackState.empty() &&
				context.IsInputTriggered(GetAttackInput(context))
			) {
				context.StopHorizontalMovement();
				context.RequestState(attackState);
				return;
			}
			Input* input = Input::GetInstance();
			Vector3 move{};
			if (input && input->PushKey(DIK_W)) move.z += 1.0f;
			if (input && input->PushKey(DIK_S)) move.z -= 1.0f;
			if (input && input->PushKey(DIK_A)) move.x -= 1.0f;
			if (input && input->PushKey(DIK_D)) move.x += 1.0f;
			if (Math::Length(move) <= 0.0001f) {
				context.StopHorizontalMovement();
				context.RequestState(context.GetString("IdleState", "Idle"));
				return;
			}
			// PlayerBehaviorはCamera相対移動を先に計算済みなので、その速度を維持する。
			if (!SceneEntityQuery::HasComponent(
				context.GetEntity(), "PlayerBehavior"
			)) {
				move = Math::Multiply(
					Math::Normalize(move),
					(std::max)(context.GetFloat("Speed", 6.0f), 0.0f)
				);
				context.SetHorizontalVelocity(move.x, move.z);
			}
		}
	};

	class BuiltinMeleeAttackState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			context.SetEntityActive(context.GetEntityParameter("HitBox"), false);
			const std::string animation = context.GetString("Animation");
			if (!animation.empty()) {
				SceneEntity* animationTarget =
					context.GetEntityParameter("AnimationTarget");
				if (animationTarget) {
					context.PlayPrefabAnimation(
						animationTarget,
						animation,
						true,
						(std::max)(
							context.GetFloat("AnimationBlendIn", 0.1f),
							0.0f
						)
					);
				} else {
					context.PlayAnimation(animation, true);
				}
			}
		}

		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
			const float windup = (std::max)(context.GetFloat("Windup", 0.15f), 0.0f);
			const float activeTime = (std::max)(
				context.GetFloat("ActiveTime", 0.2f), 0.0f
			);
			const float recovery = (std::max)(
				context.GetFloat("Recovery", 0.35f), 0.0f
			);
			const float time = context.GetStateTime();
			context.SetEntityActive(
				context.GetEntityParameter("HitBox"),
				time >= windup && time < windup + activeTime
			);
			if (time >= windup + activeTime + recovery) {
				PlayReturnPrefabAnimation(context);
				context.RequestState(context.GetString("ReturnState", "Idle"));
			}
		}

		void Exit(StateContext& context) override {
			context.SetEntityActive(context.GetEntityParameter("HitBox"), false);
		}
	};

	class BuiltinMeleeComboAttackState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			comboQueued_ = false;
			lastMotionAmount_ = 0.0f;
			activeHitWindowIndex_ = -1;
			activeHitBox_ = nullptr;
			hasAttackDefinition_ = false;
			runnerAttack_ = false;
			SceneEntity* animationTarget =
				context.GetEntityParameter("AnimationTarget");
			if (
				animationTarget &&
				ResolveAttackDefinition(context) &&
				context.StartAttack(
					animationTarget,
					context.GetString("AttackName", context.GetString("Animation"))
				)
			) {
				runnerAttack_ = true;
				return;
			}
			// 攻撃中のCamera操作で突進軌道が曲がらないよう、開始時の向きを固定する。
			ResolvePlanarAttackAxes(context, forward_, right_);
			context.SetEntityActive(context.GetEntityParameter("HitBox"), false);
			const std::string animation = hasAttackDefinition_
				? attackDefinition_.animation
				: context.GetString("Animation");
			if (animation.empty()) {
				return;
			}
			SceneEntity* legacyAnimationTarget =
				context.GetEntityParameter("AnimationTarget");
			if (legacyAnimationTarget) {
				context.PlayPrefabAnimation(
					legacyAnimationTarget,
					animation,
					true,
					(std::max)(
						context.GetFloat("AnimationBlendIn", 0.1f),
						0.0f
					)
				);
			} else {
				context.PlayAnimation(animation, true);
			}
		}

		void Update(StateContext& context, float deltaTime) override {
			context.StopHorizontalMovement();
			if (runnerAttack_) {
				const float time = context.GetAttackTime();
				const float transitionTime = (std::max)(
					context.GetFloat("ComboTransitionTime", context.GetAttackDuration()),
					0.0f
				);
				const float windowStart = std::clamp(
					context.GetFloat("ComboWindowStart", 0.0f),
					0.0f,
					transitionTime
				);
				const float windowEnd = std::clamp(
					context.GetFloat("ComboWindowEnd", transitionTime),
					windowStart,
					transitionTime
				);
				const float inputStart = (std::max)(
					windowStart - (std::max)(context.GetFloat("ComboInputBuffer", 0.15f), 0.0f),
					0.0f
				);
				const std::string nextState = context.GetString("NextState");
				if (
					!nextState.empty() &&
					time >= inputStart &&
					time <= windowEnd &&
					context.IsInputTriggered(GetAttackInput(context))
				) {
					comboQueued_ = true;
				}
				if (!context.IsAttackFinished() && time < transitionTime) {
					return;
				}
				if (comboQueued_ && !nextState.empty()) {
					context.RequestState(nextState);
					return;
				}
				PlayReturnPrefabAnimation(context);
				context.RequestState(context.GetString("ReturnState", "Idle"));
				return;
			}
			const float windup = hasAttackDefinition_ ? attackDefinition_.windup : (std::max)(context.GetFloat("Windup", 0.15f), 0.0f);
			const float activeTime = hasAttackDefinition_ ? attackDefinition_.activeTime : (std::max)(context.GetFloat("ActiveTime", 0.2f), 0.0f);
			const float recovery = hasAttackDefinition_ ? attackDefinition_.recovery : (std::max)(context.GetFloat("Recovery", 0.35f), 0.0f);
			const float stateDuration = windup + activeTime + recovery;
			const float time = context.GetStateTime();
			SceneEntity* fallbackHitBox = context.GetEntityParameter("HitBox");
			if (hasAttackDefinition_ && !attackDefinition_.hitWindows.empty()) {
				const auto activeWindow = std::find_if(
					attackDefinition_.hitWindows.begin(),
					attackDefinition_.hitWindows.end(),
					[time](const SceneAttackHitWindow& window) {
						return time >= window.startTime && time < window.endTime;
					}
				);
				const int activeWindowIndex = activeWindow == attackDefinition_.hitWindows.end()
					? -1
					: static_cast<int>(activeWindow - attackDefinition_.hitWindows.begin());
				SceneEntity* hitBox = activeWindowIndex >= 0
					? context.ResolveEntityReference(
						activeWindow->hitBoxEntityId,
						activeWindow->hitBoxEntityName
					)
					: nullptr;
				if (!hitBox && activeWindowIndex >= 0) {
					hitBox = fallbackHitBox;
				}
				if (activeHitBox_ && activeHitBox_ != hitBox) {
					context.SetEntityActive(activeHitBox_, false);
				}
				activeHitBox_ = hitBox;
				context.SetEntityActive(hitBox, activeWindowIndex >= 0);
				if (hitBox && activeWindowIndex >= 0) {
					for (SceneComponent& component : hitBox->components) {
						if (component.type != "HitBox") { continue; }
						if (activeWindowIndex != activeHitWindowIndex_) {
							++component.hitBoxAttackWindowSerial;
						}
						component.hitBoxDamage = activeWindow->damage;
						component.hitBoxPoiseDamage = activeWindow->poiseDamage;
						component.hitBoxKnockback = activeWindow->knockback;
						component.hitBoxReactionTag = activeWindow->reactionTag;
						component.hitBoxKnockbackDirectionMode =
							activeWindow->knockbackDirectionMode;
						component.hitBoxKnockbackLocalDirection =
							activeWindow->knockbackLocalDirection;
						break;
					}
				}
				activeHitWindowIndex_ = activeWindowIndex;
			} else if (!context.GetBool("AnimateHitBox", false)) {
				context.SetEntityActive(
					fallbackHitBox,
					time >= windup && time < windup + activeTime
				);
			}

			const float motionStart = hasAttackDefinition_ ? windup : (std::max)(context.GetFloat("MotionStart", 0.0f), 0.0f);
			const float motionDuration = hasAttackDefinition_ ? (std::max)(attackDefinition_.activeTime, 0.0001f) : (std::max)(context.GetFloat("MotionDuration", stateDuration), 0.0001f);
			const float motionProgress = ApplyStateEasing(
				(time - motionStart) / motionDuration,
				hasAttackDefinition_ ? attackDefinition_.motionEasing : context.GetString("MotionEasing", "SmoothStep")
			);
			const float motionDelta = motionProgress - lastMotionAmount_;
			lastMotionAmount_ = motionProgress;
			if (deltaTime > 0.0001f && std::abs(motionDelta) > 0.000001f) {
				// Transformを直接進めず速度へ変換し、既存Physicsの衝突解決を維持する。
				const Vector3 displacement = Math::Add(
					Math::Multiply(
						forward_,
						(hasAttackDefinition_ ? attackDefinition_.forwardDistance : context.GetFloat("ForwardDistance")) * motionDelta
					),
					Math::Multiply(
						right_,
						(hasAttackDefinition_ ? attackDefinition_.sideDistance : context.GetFloat("SideDistance")) * motionDelta
					)
				);
				context.SetHorizontalVelocity(
					displacement.x / deltaTime,
					displacement.z / deltaTime
				);
			}

			const float transitionTime = (std::max)(
				context.GetFloat("ComboTransitionTime", stateDuration),
				0.0f
			);
			const float windowStart = std::clamp(
				context.GetFloat("ComboWindowStart", windup),
				0.0f,
				transitionTime
			);
			const float windowEnd = std::clamp(
				context.GetFloat("ComboWindowEnd", transitionTime),
				windowStart,
				transitionTime
			);
			const float inputBuffer = (std::max)(
				context.GetFloat("ComboInputBuffer", 0.15f),
				0.0f
			);
			const float inputStart = (std::max)(
				windowStart - inputBuffer,
				0.0f
			);
			const std::string nextState = context.GetString("NextState");
			if (
				!nextState.empty() &&
				time >= inputStart &&
				time <= windowEnd &&
				context.IsInputTriggered(GetAttackInput(context))
			) {
				comboQueued_ = true;
			}
			if (time < transitionTime) {
				return;
			}

			if (comboQueued_ && !nextState.empty()) {
				context.RequestState(nextState);
				return;
			}
			// 入力予約がない場合だけIdle Clipへ戻す。次段予約時は現在Poseから
			// 次Attack Clipへ直接Cross Fadeし、コンボの連続性を維持する。
			PlayReturnPrefabAnimation(context);
			context.RequestState(context.GetString("ReturnState", "Idle"));
		}

		void Exit(StateContext& context) override {
			if (runnerAttack_) {
				context.StopAttack();
				runnerAttack_ = false;
				return;
			}
			context.SetEntityActive(activeHitBox_, false);
			context.SetEntityActive(context.GetEntityParameter("HitBox"), false);
			activeHitBox_ = nullptr;
		}

	private:
		bool comboQueued_ = false;
		bool runnerAttack_ = false;
		bool hasAttackDefinition_ = false;
		SceneAttackDefinition attackDefinition_{};
		int activeHitWindowIndex_ = -1;
		SceneEntity* activeHitBox_ = nullptr;
		float lastMotionAmount_ = 0.0f;
		Vector3 forward_{ 0.0f, 0.0f, 1.0f };
		Vector3 right_{ 1.0f, 0.0f, 0.0f };
	};

	class BuiltinReactionAnimationState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			context.StopHorizontalMovement();
			Play(context, "Animation", "Hit");
		}

		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
			const float duration = (std::max)(
				context.GetFloat("Duration", 0.2f),
				0.0f
			);
			if (context.GetStateTime() < duration) {
				return;
			}
			Play(context, "ReturnAnimation", "Idle");
			context.RequestState(context.GetString("ReturnState", "Idle"));
		}

	private:
		static void Play(
			StateContext& context,
			const char* parameter,
			const char* fallback
		) {
			const std::string animation = context.GetString(parameter, fallback);
			if (animation.empty()) {
				return;
			}
			if (SceneEntity* target = context.GetEntityParameter("AnimationTarget")) {
				context.PlayPrefabAnimation(
					target,
					animation,
					true,
					(std::max)(context.GetFloat("AnimationBlend", 0.05f), 0.0f)
				);
			} else {
				context.PlayAnimation(animation, true);
			}
		}
	};

	class BuiltinSpinLoopAttackState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			SceneEntity* target = context.GetEntityParameter("AnimationTarget");
			started_ = target && context.StartAttack(
				target, context.GetString("AttackName", "SpinLoop")
			);
		}
		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
			if (!started_) {
				context.RequestState(context.GetString("FailureState", "SpinFinish"));
				return;
			}
			const bool held = context.IsInputDown(
				context.GetString("Input", "MouseLeft")
			);
			context.SetAttackLoopRequested(held);
			if (!held || context.IsAttackFinished()) {
				context.RequestState(context.GetString("FinishState", "SpinFinish"));
			}
		}
		void Exit(StateContext& context) override { context.StopAttack(); }
	private:
		bool started_ = false;
	};

	class BuiltinSpinStartAttackState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			SceneEntity* target = context.GetEntityParameter("AnimationTarget");
			started_ = target && context.StartAttack(
				target, context.GetString("AttackName", "SpinStart")
			);
		}
		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
			if (!started_) {
				context.RequestState(context.GetString("FailureState", "Idle"));
				return;
			}
			if (!context.IsAttackFinished()) {
				return;
			}
			const bool held = context.IsInputDown(
				context.GetString("Input", "MouseLeft")
			);
			context.RequestState(context.GetString(
				held ? "LoopState" : "FinishState",
				held ? "SpinLoop" : "SpinFinish"
			));
		}
	private:
		bool started_ = false;
	};

	class BuiltinDeathAnimationState final : public IEntityStateAction {
	public:
		void Enter(StateContext& context) override {
			context.StopHorizontalMovement();
			const std::string animation = context.GetString("Animation", "Dead");
			if (animation.empty()) {
				return;
			}
			if (SceneEntity* target = context.GetEntityParameter("AnimationTarget")) {
				context.PlayPrefabAnimation(
					target,
					animation,
					true,
					(std::max)(context.GetFloat("AnimationBlend", 0.05f), 0.0f)
				);
			} else {
				context.PlayAnimation(animation, true);
			}
		}

		void Update(StateContext& context, float) override {
			context.StopHorizontalMovement();
		}
	};

	class BuiltinPassiveState final : public IEntityStateAction {
	public:
		void Update(StateContext&, float) override {}
	};

	void RegisterBuiltinStates() {
		EntityStateRegistry& registry = EntityStateRegistry::GetInstance();
		if (!registry.Contains("Builtin.Passive")) {
			registry.Register<BuiltinPassiveState>("Builtin.Passive");
		}
		if (!registry.Contains("Builtin.Idle")) {
			registry.Register<BuiltinIdleState>("Builtin.Idle");
		}
		if (!registry.Contains("Builtin.Move")) {
			registry.Register<BuiltinMoveState>("Builtin.Move");
		}
		if (!registry.Contains("Builtin.MeleeAttack")) {
			registry.Register<BuiltinMeleeAttackState>("Builtin.MeleeAttack");
		}
		if (!registry.Contains("Builtin.MeleeComboAttack")) {
			registry.Register<BuiltinMeleeComboAttackState>(
				"Builtin.MeleeComboAttack"
			);
		}
		if (!registry.Contains("Builtin.ReactionAnimation")) {
			registry.Register<BuiltinReactionAnimationState>(
				"Builtin.ReactionAnimation"
			);
		}
		if (!registry.Contains("Builtin.SpinLoopAttack")) {
			registry.Register<BuiltinSpinLoopAttackState>("Builtin.SpinLoopAttack");
		}
		if (!registry.Contains("Builtin.SpinStartAttack")) {
			registry.Register<BuiltinSpinStartAttackState>("Builtin.SpinStartAttack");
		}
		if (!registry.Contains("Builtin.DeathAnimation")) {
			registry.Register<BuiltinDeathAnimationState>(
				"Builtin.DeathAnimation"
			);
		}
	}
}

StateContext::StateContext(
	SceneDocument& document,
	SceneEntity& entity,
	Object3d* object,
	PhysicsBody* body,
	SceneAttackRunnerSystem& attackRunnerSystem,
	ScenePrefabAnimationSystem& prefabAnimationSystem,
	const SceneStateDefinition& state,
	float stateTime,
	std::string& requestedState
) :
	document_(document),
	entity_(entity),
	object_(object),
	body_(body),
	attackRunnerSystem_(attackRunnerSystem),
	prefabAnimationSystem_(prefabAnimationSystem),
	state_(state),
	stateTime_(stateTime),
	requestedState_(requestedState) {}

SceneEntity& StateContext::GetEntity() const { return entity_; }

float StateContext::GetFloat(const std::string& name, float fallback) const {
	const SceneStateParameter* parameter = FindParameter(state_, name);
	return parameter ? parameter->floatValue : fallback;
}

int StateContext::GetInt(const std::string& name, int fallback) const {
	const SceneStateParameter* parameter = FindParameter(state_, name);
	return parameter ? parameter->intValue : fallback;
}

bool StateContext::GetBool(const std::string& name, bool fallback) const {
	const SceneStateParameter* parameter = FindParameter(state_, name);
	return parameter ? parameter->boolValue : fallback;
}

std::string StateContext::GetString(
	const std::string& name,
	const std::string& fallback
) const {
	const SceneStateParameter* parameter = FindParameter(state_, name);
	return parameter ? parameter->stringValue : fallback;
}

SceneEntity* StateContext::GetEntityParameter(const std::string& name) const {
	const SceneStateParameter* parameter = FindParameter(state_, name);
	if (!parameter) {
		return nullptr;
	}
	return ResolveEntityReference(parameter->entityId, parameter->entityName);
}

SceneEntity* StateContext::ResolveEntityReference(
	uint64_t entityId,
	const std::string& entityName
) const {
	if (entityId != 0) {
		if (SceneEntity* entity = document_.FindEntity(entityId)) {
			return entity;
		}
	}
	return entityName.empty()
		? nullptr
		: document_.FindEntityByName(entityName);
}

bool StateContext::IsKeyDown(const std::string& keyName) const {
	Input* input = Input::GetInstance();
	const BYTE key = ResolveKey(keyName);
	return input && key != 0 && input->PushKey(key);
}

bool StateContext::IsKeyTriggered(const std::string& keyName) const {
	Input* input = Input::GetInstance();
	const BYTE key = ResolveKey(keyName);
	return input && key != 0 && input->TriggerKey(key);
}

bool StateContext::IsInputDown(const std::string& inputName) const {
	Input* input = Input::GetInstance();
	if (!input) {
		return false;
	}
	Input::MouseButton mouseButton{};
	if (ResolveMouseButton(inputName, mouseButton)) {
		return input->PushMouse(mouseButton);
	}
	const BYTE key = ResolveKey(inputName);
	return key != 0 && input->PushKey(key);
}

bool StateContext::IsInputTriggered(const std::string& inputName) const {
	Input* input = Input::GetInstance();
	if (!input) {
		return false;
	}
	Input::MouseButton mouseButton{};
	if (ResolveMouseButton(inputName, mouseButton)) {
		return input->TriggerMouse(mouseButton);
	}
	const BYTE key = ResolveKey(inputName);
	return key != 0 && input->TriggerKey(key);
}

bool StateContext::HasMoveInput() const {
	Input* input = Input::GetInstance();
	return input && (
		input->PushKey(DIK_W) || input->PushKey(DIK_A) ||
		input->PushKey(DIK_S) || input->PushKey(DIK_D)
	);
}

void StateContext::SetHorizontalVelocity(float x, float z) {
	if (body_) {
		body_->velocity.x = x;
		body_->velocity.z = z;
	}
}

void StateContext::StopHorizontalMovement() {
	SetHorizontalVelocity(0.0f, 0.0f);
}

void StateContext::SetEntityActive(SceneEntity* entity, bool active) {
	if (entity) {
		entity->active = active;
	}
}

bool StateContext::PlayAnimation(const std::string& clipName, bool restart) {
	if (object_) {
		for (size_t index = 0; index < object_->GetAnimationClipCount(); ++index) {
			if (object_->GetAnimationClipName(index) == clipName) {
				return object_->PlayAnimation(index, 0.1f, restart);
			}
		}
	}
	return prefabAnimationSystem_.Play(
		document_, entity_.id, clipName, restart
	);
}

bool StateContext::PlayPrefabAnimation(
	SceneEntity* target,
	const std::string& clipName,
	bool restart,
	float transitionDuration
) {
	return target && prefabAnimationSystem_.Play(
		document_, target->id, clipName, restart, transitionDuration
	);
}

bool StateContext::StartAttack(
	SceneEntity* attackSetEntity,
	const std::string& attackName
) {
	return attackSetEntity && attackRunnerSystem_.Start(
		document_, entity_.id, attackSetEntity->id, attackName
	);
}

void StateContext::StopAttack() {
	attackRunnerSystem_.Stop(document_, entity_.id);
}

bool StateContext::IsAttackFinished() const {
	return attackRunnerSystem_.IsFinished(entity_.id);
}

float StateContext::GetAttackTime() const {
	return attackRunnerSystem_.GetTime(entity_.id);
}

float StateContext::GetAttackDuration() const {
	return attackRunnerSystem_.GetDuration(entity_.id);
}

void StateContext::SetAttackLoopRequested(bool requested) {
	attackRunnerSystem_.SetLoopRequested(entity_.id, requested);
}

void StateContext::RequestState(const std::string& stateName) {
	if (!stateName.empty()) {
		requestedState_ = stateName;
	}
}

EntityStateRegistry& EntityStateRegistry::GetInstance() {
	static EntityStateRegistry instance;
	return instance;
}

void EntityStateRegistry::Register(const std::string& actionId, Factory factory) {
	if (!actionId.empty() && factory) {
		factories_[actionId] = std::move(factory);
	}
}

bool EntityStateRegistry::Contains(const std::string& actionId) const {
	return factories_.contains(actionId);
}

std::unique_ptr<IEntityStateAction> EntityStateRegistry::Create(
	const std::string& actionId
) const {
	const auto found = factories_.find(actionId);
	return found == factories_.end() ? nullptr : found->second();
}

SceneStateMachineSystem::SceneStateMachineSystem() {
	RegisterBuiltinStates();
}

void SceneStateMachineSystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	Player* player,
	SceneAttackRunnerSystem& attackRunnerSystem,
	ScenePrefabAnimationSystem& prefabAnimationSystem,
	float deltaTime,
	const std::function<bool(uint64_t)>& shouldProcess
) {
	auto findBinding = [&](uint64_t entityId) -> const SceneRuntimeObjectBinding* {
		const auto found = std::find_if(
			bindings.begin(), bindings.end(),
			[entityId](const SceneRuntimeObjectBinding& binding) {
				return binding.entity && binding.entity->id == entityId;
			}
		);
		return found == bindings.end() ? nullptr : &*found;
	};
	auto makeContext = [&] (
		SceneEntity& entity,
		const SceneStateDefinition& state,
		float stateTime,
		std::string& requestedState
	) {
		const SceneRuntimeObjectBinding* binding = findBinding(entity.id);
		Object3d* object = binding ? binding->object : nullptr;
		PhysicsBody* body = binding ? binding->body : nullptr;
		if (player && object && player->GetObject() == object) {
			body = &player->GetPhysicsBody();
		}
		return StateContext(
			document, entity, object, body, attackRunnerSystem, prefabAnimationSystem,
			state, stateTime, requestedState
		);
	};
	auto transition = [&] (
		SceneEntity& entity,
		const SceneComponent& machine,
		Runtime& runtime,
		const std::string& nextState
	) {
		const SceneStateDefinition* next = FindState(machine, nextState);
		if (!next || next->name == runtime.currentState) {
			return;
		}
		std::string ignoredRequest;
		if (runtime.action) {
			if (const SceneStateDefinition* current =
				FindState(machine, runtime.currentState)) {
				StateContext context = makeContext(
					entity, *current, runtime.stateTime, ignoredRequest
				);
				runtime.action->Exit(context);
			}
		}
		runtime.currentState = next->name;
		runtime.stateTime = 0.0f;
		runtime.action = EntityStateRegistry::GetInstance().Create(next->actionId);
		runtime.initialized = true;
		if (runtime.action) {
			StateContext context = makeContext(
				entity, *next, 0.0f, ignoredRequest
			);
			runtime.action->Enter(context);
		}
	};

	std::unordered_set<uint64_t> requiredEntities;
	for (SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* machine =
			SceneEntityQuery::FindComponent(entity, "StateMachine");
		if (!machine) {
			continue;
		}
		requiredEntities.insert(entity.id);
		Runtime& runtime = runtimes_[entity.id];
		const bool active = machine->enabled &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, entity);
		if (!active) {
			if (machine->stateMachineResetOnDisable) {
				if (runtime.action) {
					if (const SceneStateDefinition* current =
						FindState(*machine, runtime.currentState)) {
						std::string ignoredRequest;
						StateContext context = makeContext(
							entity,
							*current,
							runtime.stateTime,
							ignoredRequest
						);
						runtime.action->Exit(context);
					}
				}
				runtime = {};
			}
			continue;
		}
		if (shouldProcess && !shouldProcess(entity.id)) {
			continue;
		}
		if (!runtime.initialized) {
			const std::string initial = machine->stateMachineInitialState.empty() &&
				!machine->stateMachineStates.empty()
				? machine->stateMachineStates.front().name
				: machine->stateMachineInitialState;
			transition(entity, *machine, runtime, initial);
		}
		if (const auto external = externalRequests_.find(entity.id);
			external != externalRequests_.end()) {
			transition(entity, *machine, runtime, external->second);
			externalRequests_.erase(external);
		}
		const SceneStateDefinition* current =
			FindState(*machine, runtime.currentState);
		if (!current) {
			continue;
		}
		std::string requestedState;
		if (runtime.action) {
			StateContext context = makeContext(
				entity, *current, runtime.stateTime, requestedState
			);
			runtime.action->Update(context, deltaTime);
		}
		runtime.stateTime += (std::max)(deltaTime, 0.0f);
		if (!requestedState.empty()) {
			transition(entity, *machine, runtime, requestedState);
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

bool SceneStateMachineSystem::RequestState(
	uint64_t entityId,
	const std::string& stateName
) {
	if (entityId == 0 || stateName.empty()) {
		return false;
	}
	externalRequests_[entityId] = stateName;
	return true;
}

const std::string* SceneStateMachineSystem::GetCurrentState(
	uint64_t entityId
) const {
	const auto found = runtimes_.find(entityId);
	return found == runtimes_.end() || !found->second.initialized
		? nullptr
		: &found->second.currentState;
}

void SceneStateMachineSystem::ResetEntity(uint64_t entityId) {
	runtimes_.erase(entityId);
	externalRequests_.erase(entityId);
}

void SceneStateMachineSystem::Clear() {
	runtimes_.clear();
	externalRequests_.clear();
}
