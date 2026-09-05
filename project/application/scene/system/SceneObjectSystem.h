// 役割: SceneEntityに対応するModel/Spriteの実行時オブジェクトを所有・同期する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../SceneRuntimeObjectBinding.h"
#include "../../../engine/collision/OBBCollider.h"
#include "../../../engine/collision/SphereCollider.h"
#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector4.h"
#include "../../../engine/physics/PhysicsBody.h"

class Camera;
class Object3d;
class SceneDocument;
class ScenePhysicsSystem;
class Sprite;

struct SceneSpriteRuntimeOverride {
	uint64_t entityId = 0;
	std::string texturePath;
	Vector2 size = { 0.0f, 0.0f };
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool visible = false;
};

// Scene由来のObject3dとSpriteを一意に所有する。
// BuildBindingsが返すポインタは次のSyncModelsまたはFinalizeまでだけ有効。
class SceneObjectSystem {
public:
	struct ModelRuntime {
		std::unique_ptr<Object3d> object;
		std::string modelPath;
		std::string materialOverrideSignature;
		bool hasRenderer = false;
		bool animatorInitialized = false;
		bool hasAnimator = false;
		bool animatorAutoPlayAllowed = false;
		bool animatorPlayOnStart = true;
		bool animatorLoop = true;
		float animatorSpeed = 1.0f;
		int animatorDefaultClip = 0;
		float animatorTransitionDuration = 0.2f;
		std::string animatorBlendCurve = "SmoothStep";
		OBBCollider boxCollider;
		SphereCollider sphereCollider;
		Collider* collider = nullptr;
		bool hasCollider = false;
		Vector4 colliderDebugColor = { 0.2f, 0.95f, 0.7f, 1.0f };
		bool colliderDebugVisible = true;
		std::string colliderDebugDrawMode = "Wireframe";
		uint32_t colliderDebugSegments = 16;
		PhysicsBody physicsBody;
		bool hasPhysicsBody = false;
	};

	SceneObjectSystem();
	~SceneObjectSystem();

	void SyncModels(
		SceneDocument* document,
		ScenePhysicsSystem& physicsSystem,
		float deltaTime,
		bool allowAnimatorAutoPlay,
		bool editing
	);
	void SyncSprites(const SceneDocument* document);
	void ClearSpriteOverrides();
	void SetSpriteRuntimeOverride(
		const SceneSpriteRuntimeOverride& overrideValue
	);
	// Agent・Physics・Environmentへ渡す非所有参照を、Object同期直後に再構築する。
	void BuildBindings(
		SceneDocument& document,
		std::vector<SceneRuntimeObjectBinding>& bindings
	);

	void ApplyRenderCamera(Camera* camera);
	void PrepareModelDraw() const;
	void DrawModels(
		const SceneDocument& document,
		uint64_t skipEntityId,
		bool hidePlayerModel
	) const;
	void DrawSprites(
		const SceneDocument& document,
		uint64_t skipEntityId
	) const;
	bool HasScreenOverlaySprites(const SceneDocument& document) const;
	void DrawScreenOverlaySprites(
		const SceneDocument& document,
		uint32_t viewportWidth,
		uint32_t viewportHeight
	) const;
	void CollectShadowCasters(
		const SceneDocument& document,
		bool hidePlayerModel,
		std::vector<Object3d*>& shadowCasters
	) const;

	Object3d* FindObject(uint64_t entityId) const;
	Object3d* FindObjectByName(
		const SceneDocument* document,
		const char* name
	) const;
	ModelRuntime* FindModelRuntime(uint64_t entityId);
	const ModelRuntime* FindModelRuntime(uint64_t entityId) const;
	void Finalize();

private:
	struct SpriteRuntime {
		std::unique_ptr<Sprite> sprite;
		std::string texturePath;
	};

	void ClearModels();
	void ClearSprites();

	std::unordered_map<uint64_t, ModelRuntime> models_;
	std::unordered_map<uint64_t, SpriteRuntime> sprites_;
	std::unordered_map<uint64_t, SceneSpriteRuntimeOverride> spriteOverrides_;
};
