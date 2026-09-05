// 役割: スプライトの頂点更新と描画コマンド設定を実装する。
#include "Sprite.h"
#include "SpriteCommon.h"
#include "../base/DirectXCommon.h"
#include "TextureManager.h"
void Sprite::Initialize(SpriteCommon* spriteCommon, std::string textureFilePath){
	transform = { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };
	this->spriteCommon_ = spriteCommon;
	MakeVertexData();
	MakeMaterialData();
	MakeTransformationMatrixData();
	this->textureFilePath_ = textureFilePath;
	AdjustTextureSize();
}

void Sprite::Update(){
	Update(WinApp::kClientWidth, WinApp::kClientHeight);
}

void Sprite::Update(uint32_t viewportWidth, uint32_t viewportHeight){

	float left = 0.0f - anchorPoint.x;
	float right = 1.0f - anchorPoint.x;
	float top = 0.0f - anchorPoint.y;
	float bottom = 1.0f - anchorPoint.y;

	//左右反転
	if(isFlipX_){
		left = -left;
		right = -right;
	}
	//上下反転
	if(isFlipY_){
		top = -top;
		bottom = -bottom;
	}

	const DirectX::TexMetadata& metadata =
		TextureManager::GetInstance()->GetMetaData(textureFilePath_);
	float tex_left = textureLeftTop_.x / metadata.width;
	float tex_right = (textureLeftTop_.x + textureSize_.x) / metadata.width;
	float tex_top = textureLeftTop_.y / metadata.height;
	float tex_bottom = (textureLeftTop_.y + textureSize_.y) / metadata.height;

	// 左下
	vertexData[0].position = { left, bottom, 0.0f, 1.0f };
	vertexData[0].texcoord = { tex_left,tex_bottom };
	vertexData[0].normal = { 0.0f,0.0f,-1.0f };
	// 上
	vertexData[1] = {left,  top, 0.0f, 1.0f };
	vertexData[1].texcoord = { tex_left,tex_top };
	vertexData[1].normal = { 0.0f,0.0f,-1.0f };
	// 右下
	vertexData[2] = { right, bottom, 0.0f, 1.0f };
	vertexData[2].texcoord = { tex_right,tex_bottom };
	vertexData[2].normal = { 0.0f,0.0f,-1.0f };

	//二枚目
	// 左下
	vertexData[3].position = { right, top, 0.0f, 1.0f };
	vertexData[3].texcoord = { tex_left,tex_top };
	vertexData[3].normal = { 0.0f,0.0f,-1.0f };
	// 上
	vertexData[4] = { right,  top, 0.0f, 1.0f };
	vertexData[4].texcoord = { tex_right,tex_top };
	vertexData[4].normal = { 0.0f,0.0f,-1.0f };
	// 右下
	vertexData[5] = { right, bottom, 0.0f, 1.0f };
	vertexData[5].texcoord = { tex_right,tex_bottom };
	vertexData[5].normal = { 0.0f,0.0f,-1.0f };

	vertexData[0].normal = { 0.0f,0.0f,-1.0f };

	indexResource->Map(0, nullptr, reinterpret_cast<void**>(&indexData));
	indexData[0] = 0; indexData[1] = 1; indexData[2] = 2;
	indexData[3] = 1; indexData[4] = 4; indexData[5] = 2;

	transform.translate = { position.x,position.y,0.0f };
	transform.rotate = { 0.0f,0.0f,rotation };
	transform.scale = { size.x,size.y,1.0f };

	Matrix4x4 worldMatrixSprite = MakeAffineMatrix(transform.scale, transform.rotate, transform.translate);
	Matrix4x4 viewMatrixSprite = MakeIdentity4x4();
	Matrix4x4 projectionMatrixSprite = MakeOrthographicMatrix(
		0.0f,
		0.0f,
		static_cast<float>(viewportWidth),
		static_cast<float>(viewportHeight),
		0.0f,
		100.0f
	);
	Matrix4x4 worldViewProjectionMatrixSprite = Multiply(worldMatrixSprite, Multiply(viewMatrixSprite, projectionMatrixSprite));

	transformationMatrixData->WVP = worldViewProjectionMatrixSprite;
	transformationMatrixData->World = MakeIdentity4x4();
}

void Sprite::Draw(){
	spriteCommon_->GetDxCommon()->GetCommandList()->IASetVertexBuffers(0, 1, &vertexBufferView); // VBVを設定
	spriteCommon_->GetDxCommon()->GetCommandList()->IASetIndexBuffer(&indexBufferView); // IBVを設定

	//TransformationMatrixCBufferの場所を限定
	spriteCommon_->GetDxCommon()->GetCommandList()->SetGraphicsRootConstantBufferView(1, transformationMatrixResource->GetGPUVirtualAddress());

	spriteCommon_->GetDxCommon()->GetCommandList()->SetGraphicsRootConstantBufferView(0, materialResource->GetGPUVirtualAddress());

	spriteCommon_->GetDxCommon()->GetCommandList()->SetGraphicsRootDescriptorTable(2, TextureManager::GetInstance()->GetSrvHandleGPU(textureFilePath_));
	//描画
	spriteCommon_->GetDxCommon()->GetCommandList()->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

void Sprite::MakeVertexData(){
	//VertexResourceを作る
	vertexResource = *&spriteCommon_->GetDxCommon()->CreateBufferResource(sizeof(VertexData) * 6);
	//IndexResourceを作る
	indexResource = *&spriteCommon_->GetDxCommon()->CreateBufferResource(sizeof(uint32_t) * 6);

	//VertexBufferViewを作成する
	// リソースの先頭のアドレスから使う
	vertexBufferView.BufferLocation = vertexResource->GetGPUVirtualAddress();
	// 使用するリソースのサイズは頂点3つ分のサイズ
	vertexBufferView.SizeInBytes = sizeof(VertexData) * 6;
	// 1頂点あたりのサイズ
	vertexBufferView.StrideInBytes = sizeof(VertexData);

	//IndexBufferViewを作成する
	indexBufferView.BufferLocation = indexResource->GetGPUVirtualAddress();
	//使用するリソースのインデックスは6つ分のサイズ
	indexBufferView.SizeInBytes = sizeof(uint32_t) * 6;
	//インデックスはuint32_tとする
	indexBufferView.Format = DXGI_FORMAT_R32_UINT;

	//VertexResourceにデータを書き込むためのアドレスを取得してvertexDataに割り当て
	vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));
	//IndexxResourceにデータを書き込むためのアドレスを取得してindexDataに割り当てる
	indexResource->Map(0, nullptr, reinterpret_cast<void**>(&indexData));

}

void Sprite::MakeMaterialData(){
	//マテリアルリソースを作る
	materialResource = *&spriteCommon_->GetDxCommon()->CreateBufferResource(sizeof(Material));
	//書き込むためのアドレスを取得
	materialResource->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
	//今回は赤を書き込んでる
	materialData->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	//ライティングの有無
	materialData->enableLighting = false;
	//uvTransform行列を単位行列で初期化
	materialData->uvTransform = MakeIdentity4x4();
}

void Sprite::MakeTransformationMatrixData(){
	//WVP用リソースのリソースを作る。Matrix4x4 1つ分のサイズを用意する
	transformationMatrixResource = *&spriteCommon_->GetDxCommon()->CreateBufferResource(sizeof(TransformationMatrix));
	//書き込むためのアドレス取得
	transformationMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixData));
	//単位行列を書き込んでおく
	transformationMatrixData->WVP = MakeIdentity4x4();
	transformationMatrixData->World = MakeIdentity4x4();
}

void Sprite::AdjustTextureSize(){
	const DirectX::TexMetadata& metadata = TextureManager::GetInstance()->GetMetaData(textureFilePath_);

	textureSize_.x = static_cast<float>(metadata.width);
	textureSize_.y = static_cast<float>(metadata.height);
	//画像サイズをテクスチャサイズに合わせる
	size = textureSize_;
}
