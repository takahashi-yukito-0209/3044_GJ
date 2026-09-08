// 役割: SceneEntityとRuntime側の描画・衝突・物理オブジェクトを接続する。
#pragma once

#include <cstdint>

class Collider;
class Object3d;
struct PhysicsBody;
struct SceneEntity;

struct SceneRuntimeObjectBinding {
	uint64_t entityId = 0; // SceneDocumentから現在のEntityを引き直すためのID。
	SceneEntity* entity = nullptr;
	Object3d* object = nullptr;
	Collider* collider = nullptr;
	PhysicsBody* body = nullptr;
};
