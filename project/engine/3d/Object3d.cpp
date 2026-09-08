// 役割: Object3dの更新、アニメーション遷移、モデル描画を実装する。
#include "Object3d.h"
#include "Object3dCommon.h"
#include "../base/DirectXCommon.h"
#include "../2d/TextureManager.h"
#include "ModelManager.h"
#include "Model.h"
#include "Camera.h"
#include "SrvManager.h"
#include "../debug/DebugRenderer.h"
#include <algorithm>
#include <cmath>

namespace {

Vector4 MultiplyColor(const Vector4& left, const Vector4& right) {
	return {
		left.x * right.x,
		left.y * right.y,
		left.z * right.z,
		left.w * right.w
	};
}

bool IsSameColor(const Vector4& left, const Vector4& right) {
	return left.x == right.x && left.y == right.y &&
		left.z == right.z && left.w == right.w;
}

} // namespace

void Object3d::Initialize(Object3dCommon* object3dCommon){
	this->object3dCommon = object3dCommon;

	CreateTransformationMatrixResource();
	CreateCameraResource();
	CreateShadowTransformationMatrixResource();
	environmentTextureFilePath_ = "resources/rostock_laage_airport_4k.dds";
	TextureManager::GetInstance()->LoadTexture(environmentTextureFilePath_);

	//Transform変数を作る
	transform = { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };
	this->camera = object3dCommon->GetDefaultCamera();
}

void Object3d::Update(){
	UpdateInternal();
}

void Object3d::UpdateAnimation(float deltaTime) {
	if (
		deltaTime > 0.0f &&
		animationPlayer_.IsEnabled() &&
		animationPlayer_.IsPlaying()
	) {
		animationPoseDirty_ = true;
	}
	animationPlayer_.Update(deltaTime);
}

void Object3d::UpdateForCamera(Camera* camera) {
	this->camera = camera;
	UpdateInternal();
}

void Object3d::UpdateInternal() {
	objectWorldMatrix_ = transform.useQuaternionRotation
		? MakeAffineMatrix(
			transform.scale,
			transform.quaternionRotate,
			transform.translate
		)
		: MakeAffineMatrix(
			transform.scale,
			transform.rotate,
			transform.translate
		);
	if (hasParentMatrixOverride_) {
		objectWorldMatrix_ = Multiply(
			objectWorldMatrix_,
			parentMatrixOverride_
		);
	} else if (parent_) {
		objectWorldMatrix_ = Multiply(
			objectWorldMatrix_,
			parent_->objectWorldMatrix_
		);
	}
	const Matrix4x4& worldMatrix = objectWorldMatrix_;

	// ModelのRootNode行列を適用する
	Matrix4x4 modelWorldMatrix = worldMatrix;
	if (model) {
		Matrix4x4 localMatrix = model->HasSkinning()
			? MakeIdentity4x4()
			: model->GetRootNodeLocalMatrix();
		if (animationPoseDirty_) {
			UpdateAnimationPose();
		}

		if (skeleton_.IsValid()) {
			if (!model->HasSkinning()) {
				localMatrix =
					skeleton_.joints[skeleton_.root].skeletonSpaceMatrix;
			}
		}
		modelWorldMatrix = Multiply(
			Multiply(localMatrix, visualLocalRotationMatrix_),
			worldMatrix
		);
	}

	Matrix4x4 worldViewProjectionMatrix;
	if (camera) {
		const Matrix4x4& viewProjectionMatrix = camera->GetViewProjectionMatrix();
		worldViewProjectionMatrix = Multiply(modelWorldMatrix, viewProjectionMatrix);
	}
	else {
		worldViewProjectionMatrix = modelWorldMatrix;
	}

	transformationMatrixData->WVP = worldViewProjectionMatrix;
	transformationMatrixData->World = modelWorldMatrix;

	if (camera) {
		cameraData->worldPosition = camera->GetTranslate();
	}
}

void Object3d::SetVisualLocalRotation(const Vector3& rotation) {
	visualLocalRotationMatrix_ = MakeAffineMatrix(
		{ 1.0f, 1.0f, 1.0f },
		rotation,
		{ 0.0f, 0.0f, 0.0f }
	);
}

void Object3d::SetModel(Model* model) {
	this->model = model;
	CreateMaterialResources();
	animationPlayer_.SetAnimations(
		model ? &model->GetAnimations() : nullptr
	);
	skeleton_ = model
		? CreateSkeleton(model->GetRootNode())
		: Skeleton{};
	skinCluster_.reset();
	if (
		model &&
		model->HasSkinning() &&
		skeleton_.IsValid()
	) {
		skinCluster_ = std::make_unique<SkinCluster>();
		skinCluster_->Initialize(
			object3dCommon->GetDxCommon(),
			SrvManager::GetInstance(),
			skeleton_,
			*model
		);
	}
	animationPoseDirty_ = true;
	skinningDispatchPending_ = true;
}

void Object3d::UpdateAnimationPose() {
	if (!model || !skeleton_.IsValid()) {
		animationPoseDirty_ = false;
		return;
	}

	const Animation* animation = animationPlayer_.GetCurrentAnimation();
	if (animationPlayer_.IsEnabled() && animation) {
		if (const Animation* previous =
			animationPlayer_.GetPreviousAnimation()) {
			ApplyAnimationBlend(
				skeleton_,
				*previous,
				animationPlayer_.GetPreviousTime(),
				*animation,
				animationPlayer_.GetTime(),
				animationPlayer_.GetBlendWeight()
			);
		} else {
			ApplyAnimation(
				skeleton_,
				*animation,
				animationPlayer_.GetTime()
			);
		}
	} else {
		ResetSkeletonPose(skeleton_);
	}

	UpdateSkeleton(skeleton_);
	if (skinCluster_) {
		skinCluster_->Update(skeleton_);
		skinningDispatchPending_ = true;
	}
	animationPoseDirty_ = false;
}

void Object3d::DispatchSkinningIfNeeded() {
	if (
		!skinningDispatchPending_ ||
		!skinCluster_ ||
		!skinCluster_->IsValid()
	) {
		return;
	}
	object3dCommon->DispatchSkinning(*skinCluster_);
	skinningDispatchPending_ = false;
}

void Object3d::SetRotateQuaternion(const Quaternion& rotate) {
	transform.quaternionRotate = Normalize(rotate);
	transform.useQuaternionRotation = true;
}

void Object3d::Draw(){
	if (!model) {
		return;
	}

	auto* commandList = object3dCommon->GetDxCommon()->GetCommandList();
	if (skinCluster_ && skinCluster_->IsValid()) {
		DispatchSkinningIfNeeded();
		object3dCommon->SetCommonRenderState(cullMode_);
	}
	else {
		object3dCommon->SetCommonRenderState(cullMode_);
	}

	// 座標変換行列CBufferの場所を設定
	commandList->SetGraphicsRootConstantBufferView(1, transformationMatrixResource->GetGPUVirtualAddress());

	commandList->SetGraphicsRootConstantBufferView(4, cameraResource->GetGPUVirtualAddress());

	commandList->SetGraphicsRootDescriptorTable(
		7,
		TextureManager::GetInstance()->GetSrvHandleGPU(environmentTextureFilePath_)
	);

	if (skinCluster_ && skinCluster_->IsValid()) {
		model->DrawWithVertexBufferAndMaterialSlots(
			skinCluster_->GetSkinnedVertexBufferView(),
			materialResourcesForDraw_,
			materialTexturesForDraw_
		);
	}
	else {
		model->DrawWithMaterialSlots(
			materialResourcesForDraw_,
			materialTexturesForDraw_
		);
	}
}

void Object3d::DrawShadow(const Matrix4x4& lightViewProjection) {
	if (!model) {
		return;
	}
	if (materialDissolveAmount_ >= 0.98f) {
		return;
	}

	shadowTransformationMatrixData->WVP = Multiply(transformationMatrixData->World, lightViewProjection);

	auto* commandList = object3dCommon->GetDxCommon()->GetCommandList();
	if (skinCluster_ && skinCluster_->IsValid()) {
		DispatchSkinningIfNeeded();
	}
	// スキニング済み頂点Bufferを渡すため、ShadowMap.VS と同じ入力レイアウトを使う。
	// SkinningShadowMap.VS は未スキニング頂点/Influence/Paletteを前提にしており、
	// ここで選ぶと未バインドの入力を参照して巨大な三角形をShadowMapへ書き込む。
	object3dCommon->SetShadowRenderState(cullMode_);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		shadowTransformationMatrixResource->GetGPUVirtualAddress()
	);

	if (skinCluster_ && skinCluster_->IsValid()) {
		model->DrawForShadowWithVertexBuffer(
			skinCluster_->GetSkinnedVertexBufferView()
		);
	}
	else {
		model->DrawForShadow();
	}
}

void Object3d::DrawSkeletonDebug(
	bool drawJointNames,
	bool drawJointAxes,
	float jointRadius,
	float axisLength
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!skeleton_.IsValid() || !camera || !transformationMatrixData) {
		return;
	}

	DebugRenderer* debugRenderer = DebugRenderer::GetInstance();
	const Vector4 lineColor = { 1.0f, 0.65f, 0.12f, 1.0f };
	const Vector4 jointColor = { 0.2f, 0.85f, 1.0f, 1.0f };
	const Vector4 rootColor = { 1.0f, 0.95f, 0.35f, 1.0f };
	const Matrix4x4& objectWorldMatrix = objectWorldMatrix_;

	std::vector<Matrix4x4> jointWorldMatrices(skeleton_.joints.size());
	for (const Joint& joint : skeleton_.joints) {
		jointWorldMatrices[joint.index] = Multiply(
			joint.skeletonSpaceMatrix,
			objectWorldMatrix
		);
	}

	for (const Joint& joint : skeleton_.joints) {
		const Matrix4x4& jointWorldMatrix =
			jointWorldMatrices[joint.index];
		const Vector3 jointPosition = {
			jointWorldMatrix.m[3][0],
			jointWorldMatrix.m[3][1],
			jointWorldMatrix.m[3][2]
		};

		debugRenderer->AddSphere(
			jointPosition,
			joint.index == skeleton_.root
				? jointRadius * 1.35f
				: jointRadius,
			joint.index == skeleton_.root ? rootColor : jointColor
		);
		if (drawJointNames) {
			// Screen座標への投影はScene画像Rectを所有するImGuiManagerへ委譲する。
			debugRenderer->AddWorldLabel(
				jointPosition,
				joint.name,
				{ 1.0f, 1.0f, 1.0f, 1.0f }
			);
		}

		if (drawJointAxes) {
			debugRenderer->AddAxis(jointWorldMatrix, axisLength);
		}

		if (joint.parent.has_value()) {
			const Matrix4x4& parentWorldMatrix =
				jointWorldMatrices[*joint.parent];
			const Vector3 parentPosition = {
				parentWorldMatrix.m[3][0],
				parentWorldMatrix.m[3][1],
				parentWorldMatrix.m[3][2]
			};
			debugRenderer->AddLine(
				parentPosition,
				jointPosition,
				lineColor
			);
		}
	}

#else
	(void)drawJointNames;
	(void)drawJointAxes;
	(void)jointRadius;
	(void)axisLength;
#endif
}

void Object3d::SetModel(const std::string& filePath){
//モデルを検索してセットする
	SetModel(ModelManager::GetInstance()->FindModel(filePath));
}

bool Object3d::HasAnimation() const {
	return animationPlayer_.HasAnimations();
}

float Object3d::GetAnimationDuration() const {
	return animationPlayer_.GetDuration();
}

size_t Object3d::GetAnimationClipCount() const {
	return model ? model->GetAnimations().size() : 0;
}

const std::string& Object3d::GetAnimationClipName(size_t clipIndex) const {
	static const std::string emptyName;
	if (!model || clipIndex >= model->GetAnimations().size()) {
		return emptyName;
	}
	return model->GetAnimations()[clipIndex].name;
}

void Object3d::SetEnvironmentMap(const std::string& textureFilePath, float coefficient) {
	environmentTextureFilePath_ = textureFilePath;
	if (!environmentTextureFilePath_.empty()) {
		TextureManager::GetInstance()->LoadTexture(environmentTextureFilePath_);
	}
	SetEnvironmentCoefficient(coefficient);
}

void Object3d::SetEnvironmentCoefficient(float coefficient) {
	cameraData->environmentCoefficient = std::clamp(coefficient, 0.0f, 1.0f);
}

void Object3d::SetColor(const Vector4& color) {
	if (IsSameColor(materialColorMultiplier_, color)) {
		return;
	}
	materialColorMultiplier_ = color;
	UpdateMaterialResources();
}

bool Object3d::TryGetJointWorldMatrix(
	const std::string& jointName,
	Matrix4x4& worldMatrix
) const {
	if (!skeleton_.IsValid()) {
		return false;
	}
	const auto found = skeleton_.jointMap.find(jointName);
	if (
		found == skeleton_.jointMap.end() ||
		found->second < 0 ||
		found->second >= static_cast<int32_t>(skeleton_.joints.size())
	) {
		return false;
	}
	worldMatrix = Multiply(
		skeleton_.joints[found->second].skeletonSpaceMatrix,
		objectWorldMatrix_
	);
	return true;
}

void Object3d::SetEnableLighting(bool enableLighting) {
	if (materialEnableLighting_ == enableLighting) {
		return;
	}
	materialEnableLighting_ = enableLighting;
	UpdateMaterialResources();
}

void Object3d::SetEmissive(float intensity, const Vector4& color) {
	const float clampedIntensity = (std::max)(0.0f, intensity);
	if (materialEmissiveIntensity_ == clampedIntensity &&
		IsSameColor(materialEmissiveColor_, color)) {
		return;
	}
	materialEmissiveIntensity_ = clampedIntensity;
	materialEmissiveColor_ = color;
	UpdateMaterialResources();
}

void Object3d::SetDissolve(
	float amount,
	float edgeWidth,
	float noiseScale
) {
	const float clampedAmount = std::clamp(amount, 0.0f, 1.0f);
	const float clampedEdgeWidth = (std::max)(edgeWidth, 0.0f);
	const float clampedNoiseScale = (std::max)(noiseScale, 0.001f);
	if (materialDissolveAmount_ == clampedAmount &&
		materialDissolveEdgeWidth_ == clampedEdgeWidth &&
		materialDissolveNoiseScale_ == clampedNoiseScale) {
		return;
	}
	materialDissolveAmount_ = clampedAmount;
	materialDissolveEdgeWidth_ = clampedEdgeWidth;
	materialDissolveNoiseScale_ = clampedNoiseScale;
	UpdateMaterialResources();
}

void Object3d::CreateTransformationMatrixResource(){
	//WVP用リソースのリソースを作る。Matrix4x4 1つ分のサイズを用意する
	transformationMatrixResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(TransformationMatrix));
	//書き込むためのアドレス取得
	transformationMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixData));
	//単位行列を書き込んでおく
	transformationMatrixData->WVP = MakeIdentity4x4();
	transformationMatrixData->World = MakeIdentity4x4();
}

void Object3d::CreateShadowTransformationMatrixResource() {
	shadowTransformationMatrixResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(ShadowTransformationMatrix));
	shadowTransformationMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&shadowTransformationMatrixData));
	shadowTransformationMatrixData->WVP = MakeIdentity4x4();
}

void Object3d::CreateCameraResource() {
	cameraResource = *&object3dCommon->GetDxCommon()->CreateBufferResource(sizeof(CameraForGPU));
	cameraResource->Map(0, nullptr, reinterpret_cast<void**>(&cameraData));
	cameraData->worldPosition = { 0.0f, 0.0f, -10.0f };
	cameraData->environmentCoefficient = 0.0f;
}

void Object3d::SetMaterialOverrides(
	const std::vector<MaterialOverride>& overrides
) {
	for (MaterialSlotResource& materialSlot : materialSlots_) {
		materialSlot.colorOverrideEnabled = false;
		materialSlot.textureOverride = {};
		for (const MaterialOverride& override : overrides) {
			if (!override.enabled || override.materialName.empty()) {
				continue;
			}
			const size_t materialIndex = &materialSlot - materialSlots_.data();
			if (!model || materialIndex >= model->GetMaterialSlots().size() ||
				model->GetMaterialSlots()[materialIndex].name != override.materialName) {
				continue;
			}
			materialSlot.colorOverrideEnabled = override.colorOverrideEnabled;
			materialSlot.colorOverride = override.color;
			if (!override.texturePath.empty() &&
				TextureManager::GetInstance()->LoadTexture(override.texturePath)) {
				materialSlot.textureOverride =
					TextureManager::GetInstance()->GetSrvHandleGPU(override.texturePath);
			}
			break;
		}
	}
	UpdateMaterialResources();
}

void Object3d::SetTextureOverride(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
	if (textureOverrideHandle_.ptr == handle.ptr) {
		return;
	}
	textureOverrideHandle_ = handle;
	UpdateMaterialResources();
}

void Object3d::ClearTextureOverride() {
	if (textureOverrideHandle_.ptr == 0) {
		return;
	}
	textureOverrideHandle_ = {};
	UpdateMaterialResources();
}

void Object3d::CreateMaterialResources() {
	materialSlots_.clear();
	materialResourcesForDraw_.clear();
	materialTexturesForDraw_.clear();
	if (!model) {
		return;
	}

	const std::vector<Model::MaterialSlot>& modelMaterials =
		model->GetMaterialSlots();
	materialSlots_.resize(modelMaterials.size());
	for (size_t index = 0; index < modelMaterials.size(); ++index) {
		MaterialSlotResource& materialSlot = materialSlots_[index];
		materialSlot.resource = *&object3dCommon->GetDxCommon()
			->CreateBufferResource(sizeof(Material));
		materialSlot.resource->Map(
			0, nullptr, reinterpret_cast<void**>(&materialSlot.data)
		);
		materialSlot.modelColor = modelMaterials[index].baseColor;
	}
	UpdateMaterialResources();
}

void Object3d::UpdateMaterialResources() {
	materialResourcesForDraw_.resize(materialSlots_.size());
	materialTexturesForDraw_.resize(materialSlots_.size());
	for (size_t index = 0; index < materialSlots_.size(); ++index) {
		MaterialSlotResource& materialSlot = materialSlots_[index];
		if (!materialSlot.data) {
			continue;
		}
		const Vector4 baseColor = materialSlot.colorOverrideEnabled
			? materialSlot.colorOverride
			: materialSlot.modelColor;
		materialSlot.data->color = MultiplyColor(
			baseColor,
			materialColorMultiplier_
		);
		materialSlot.data->enableLighting = materialEnableLighting_ ? 1 : 0;
		materialSlot.data->emissiveIntensity = materialEmissiveIntensity_;
		materialSlot.data->uvTransform = MakeIdentity4x4();
		materialSlot.data->emissiveColor = materialEmissiveColor_;
		materialSlot.data->shininess = 40.0f;
		materialSlot.data->dissolveAmount = materialDissolveAmount_;
		materialSlot.data->dissolveEdgeWidth = materialDissolveEdgeWidth_;
		materialSlot.data->dissolveNoiseScale = materialDissolveNoiseScale_;
		materialResourcesForDraw_[index] = materialSlot.resource;
		materialTexturesForDraw_[index] = textureOverrideHandle_.ptr != 0
			? textureOverrideHandle_
			: materialSlot.textureOverride;
	}
}
