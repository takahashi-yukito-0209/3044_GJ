// 役割: 水面メッシュと水中ポストエフェクトの描画を実装する。
#include "WaterSurfaceRenderer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include <d3dcompiler.h>

#include "../3d/Camera.h"
#include "../base/DirectXCommon.h"
#include "../base/RenderFormats.h"
#include "../math/Math.h"
#include "../utility/Logger.h"

void WaterSurfaceRenderer::Initialize(
	DirectXCommon* dxCommon,
	uint32_t gridResolution
) {
	dxCommon_ = dxCommon;
	assert(dxCommon_);

	CreateRootSignature();
	CreateGraphicsPipeline();
	CreateResources((std::clamp)(gridResolution, 4u, 160u));
	CreateFoamPipeline();
	CreateFoamResources();
}

void WaterSurfaceRenderer::Finalize() {
	surfaceData_ = nullptr;
	foamVertices_ = nullptr;
	foamCameraData_ = nullptr;
	foamCameraResource_.Reset();
	foamVertexResource_.Reset();
	surfaceResource_.Reset();
	vertexResource_.Reset();
	foamPipelineState_.Reset();
	foamRootSignature_.Reset();
	pipelineState_.Reset();
	rootSignature_.Reset();
	vertexBufferView_ = {};
	foamVertexBufferView_ = {};
	vertexCount_ = 0;
	foamDrawSlot_ = 0;
	time_ = 0.0f;
	dxCommon_ = nullptr;
}

void WaterSurfaceRenderer::Update(float deltaTime) {
	time_ += deltaTime;
	foamDrawSlot_ = 0;
}

void WaterSurfaceRenderer::Draw(
	const Camera* camera,
	const Vector3& center,
	const Vector3& halfSize,
	const Settings& settings,
	const std::vector<FoamEmitter>& foamEmitters
) {
	if (
		!settings.enabled ||
		!dxCommon_ ||
		!camera ||
		!surfaceData_ ||
		vertexCount_ == 0
	) {
		return;
	}

	const float safeHalfX = (std::max)(halfSize.x, 0.05f);
	const float safeHalfY = (std::max)(halfSize.y, 0.05f);
	const float safeHalfZ = (std::max)(halfSize.z, 0.05f);
	const float span = (std::max)(safeHalfX, safeHalfZ);
	const float waveFitScale = (std::clamp)(span / 8.0f, 0.45f, 1.6f);
	const float amplitudeScale = settings.waveScale * waveFitScale;
	const float wavelengthScale = (std::max)(waveFitScale, 0.65f);
	const Vector3 cameraPosition = camera->GetTranslate();

	surfaceData_->viewProjection = camera->GetViewProjectionMatrix();
	surfaceData_->centerTime = { center.x, center.y, center.z, time_ };
	surfaceData_->halfSizeAlpha = {
		safeHalfX,
		safeHalfY,
		safeHalfZ,
		(std::clamp)(settings.alpha, 0.0f, 1.0f)
	};
	surfaceData_->cameraPositionFresnel = {
		cameraPosition.x,
		cameraPosition.y,
		cameraPosition.z,
		(std::max)(settings.fresnelPower, 0.1f)
	};
	surfaceData_->waveA = {
		0.86f,
		0.32f,
		0.12f * amplitudeScale,
		5.8f * wavelengthScale
	};
	surfaceData_->waveB = {
		-0.28f,
		0.96f,
		0.075f * amplitudeScale,
		3.1f * wavelengthScale
	};
	surfaceData_->waveC = {
		0.58f,
		-0.74f,
		0.045f * amplitudeScale,
		1.65f * wavelengthScale
	};
	surfaceData_->baseColor = settings.baseColor;
	surfaceData_->highlightColorNormal = {
		settings.highlightColor.x,
		settings.highlightColor.y,
		settings.highlightColor.z,
		(std::clamp)(settings.normalStrength, 0.0f, 2.0f)
	};
	const uint32_t foamEmitterCount = (std::min)(
		static_cast<uint32_t>(foamEmitters.size()),
		8u
	);
	for (uint32_t index = 0; index < 8; ++index) {
		surfaceData_->foamEmitters[index] = {};
		surfaceData_->foamOrientations[index] = {};
	}
	for (uint32_t index = 0; index < foamEmitterCount; ++index) {
		const FoamEmitter& emitter = foamEmitters[index];
		surfaceData_->foamEmitters[index] = {
			emitter.center.x,
			emitter.center.z,
			(std::max)(emitter.radiusX, 0.25f),
			(std::max)(emitter.radiusZ, 0.25f)
		};
		surfaceData_->foamOrientations[index] = {
			std::sin(emitter.yaw), std::cos(emitter.yaw), 0, 0
		};
	}
	surfaceData_->foamInfo = { static_cast<float>(foamEmitterCount), 0, 0, 0 };

	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(rootSignature_.Get());
	commandList->SetPipelineState(pipelineState_.Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		surfaceResource_->GetGPUVirtualAddress()
	);
	commandList->DrawInstanced(vertexCount_, 1, 0, 0);
}

void WaterSurfaceRenderer::DrawFoam(
	const Camera* camera,
	const std::vector<FoamEmitter>& emitters
) {
	if (
		!camera ||
		!foamVertices_ ||
		!foamCameraData_ ||
		!foamRootSignature_ ||
		!foamPipelineState_ ||
		emitters.empty() ||
		foamDrawSlot_ >= kFoamDrawSlotCount
	) {
		return;
	}

	const uint32_t emitterCount = (std::min)(
		static_cast<uint32_t>(emitters.size()),
		kMaxFoamEmitters
	);
	uint32_t vertexCount = 0;
	const uint32_t slot = foamDrawSlot_++;
	FoamVertex* vertices = foamVertices_ +
		static_cast<size_t>(slot) * kFoamVerticesPerSlot;
	auto addVertex = [&vertices, &vertexCount](
		const Vector3& position,
		const Vector4& color
	) {
		vertices[vertexCount++] = { position, color };
	};

	for (uint32_t emitterIndex = 0; emitterIndex < emitterCount; ++emitterIndex) {
		const FoamEmitter& emitter = emitters[emitterIndex];
		const float radiusX = (std::max)(emitter.radiusX, 0.25f);
		const float radiusZ = (std::max)(emitter.radiusZ, 0.25f);
		const float phase = static_cast<float>(emitter.seed % 628u) * 0.01f;
		for (uint32_t segment = 0; segment < kFoamSegmentCount; ++segment) {
			const float segmentNoise = std::sin(
				static_cast<float>(segment) * 5.37f + phase
			);
			// いくつかの区間を消して、均一な輪ではない泡の切れ目を作る。
			if (segmentNoise < -0.28f) {
				continue;
			}
			const float angle0 = 6.2831853f *
				(static_cast<float>(segment) / kFoamSegmentCount) + phase;
			const float angle1 = 6.2831853f *
				(static_cast<float>(segment + 1) / kFoamSegmentCount) + phase;
			const float width = (std::clamp)(
				(std::min)(radiusX, radiusZ) * (0.075f + segmentNoise * 0.022f),
				0.08f,
				0.42f
			);
			auto point = [&emitter, radiusX, radiusZ](float angle, float inset) {
				return Vector3{
					emitter.center.x + std::cos(angle) * (radiusX + inset),
					emitter.center.y,
					emitter.center.z + std::sin(angle) * (radiusZ + inset)
				};
			};
			const Vector3 inner0 = point(angle0, -width * 0.4f);
			const Vector3 inner1 = point(angle1, -width * 0.4f);
			const Vector3 outer0 = point(angle0, width);
			const Vector3 outer1 = point(angle1, width);
			const Vector4 innerColor{ 1.2f, 1.35f, 1.45f, 0.25f };
			const Vector4 outerColor{ 1.45f, 1.6f, 1.7f, 0.78f };
			addVertex(inner0, innerColor);
			addVertex(outer0, outerColor);
			addVertex(inner1, innerColor);
			addVertex(inner1, innerColor);
			addVertex(outer0, outerColor);
			addVertex(outer1, outerColor);
		}
	}
	if (vertexCount == 0) {
		return;
	}

	FoamCameraData* cameraData = reinterpret_cast<FoamCameraData*>(
		foamCameraData_ + static_cast<size_t>(slot) * kFoamCameraSlotSize
	);
	cameraData->viewProjection = camera->GetViewProjectionMatrix();
	D3D12_VERTEX_BUFFER_VIEW vertexView = foamVertexBufferView_;
	vertexView.BufferLocation += static_cast<UINT64>(slot) *
		kFoamVerticesPerSlot * sizeof(FoamVertex);
	vertexView.SizeInBytes = vertexCount * sizeof(FoamVertex);

	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(foamRootSignature_.Get());
	commandList->SetPipelineState(foamPipelineState_.Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &vertexView);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		foamCameraResource_->GetGPUVirtualAddress() +
			static_cast<UINT64>(slot) * kFoamCameraSlotSize
	);
	commandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void WaterSurfaceRenderer::CreateRootSignature() {
	D3D12_ROOT_PARAMETER rootParameter{};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rootParameter.Descriptor.ShaderRegister = 0;

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	rootSignatureDesc.pParameters = &rootParameter;
	rootSignatureDesc.NumParameters = 1;

	Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
	const HRESULT serializeResult = D3D12SerializeRootSignature(
		&rootSignatureDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		&signatureBlob,
		&errorBlob
	);
	if (FAILED(serializeResult)) {
		if (errorBlob) {
			Logger::Log(static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		assert(false);
	}

	const HRESULT createResult = dxCommon_->GetDevice()->CreateRootSignature(
		0,
		signatureBlob->GetBufferPointer(),
		signatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature_)
	);
	assert(SUCCEEDED(createResult));
}

void WaterSurfaceRenderer::CreateGraphicsPipeline() {
	D3D12_INPUT_ELEMENT_DESC inputElements[2]{};
	inputElements[0].SemanticName = "POSITION";
	inputElements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElements[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElements[1].SemanticName = "TEXCOORD";
	inputElements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElements[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	const D3D12_INPUT_LAYOUT_DESC inputLayout{
		inputElements,
		_countof(inputElements)
	};

	const auto vertexShader = dxCommon_->CompileShader(
		L"resources/shaders/WaterSurface.VS.hlsl",
		L"vs_6_0"
	);
	const auto pixelShader = dxCommon_->CompileShader(
		L"resources/shaders/WaterSurface.PS.hlsl",
		L"ps_6_0"
	);
	assert(vertexShader);
	assert(pixelShader);

	D3D12_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

	D3D12_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;

	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	depthStencilDesc.DepthEnable = TRUE;
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = rootSignature_.Get();
	pipelineDesc.InputLayout = inputLayout;
	pipelineDesc.VS = {
		vertexShader->GetBufferPointer(),
		vertexShader->GetBufferSize()
	};
	pipelineDesc.PS = {
		pixelShader->GetBufferPointer(),
		pixelShader->GetBufferSize()
	};
	pipelineDesc.BlendState = blendDesc;
	pipelineDesc.RasterizerState = rasterizerDesc;
	pipelineDesc.DepthStencilState = depthStencilDesc;
	pipelineDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	pipelineDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	pipelineDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pipelineDesc.SampleDesc.Count = 1;

	const HRESULT result =
		dxCommon_->GetDevice()->CreateGraphicsPipelineState(
			&pipelineDesc,
			IID_PPV_ARGS(&pipelineState_)
		);
	assert(SUCCEEDED(result));
}

void WaterSurfaceRenderer::CreateFoamPipeline() {
	D3D12_ROOT_PARAMETER parameter{};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	parameter.Descriptor.ShaderRegister = 0;
	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	rootSignatureDesc.pParameters = &parameter;
	rootSignatureDesc.NumParameters = 1;

	Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
	const HRESULT serializeResult = D3D12SerializeRootSignature(
		&rootSignatureDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		&signatureBlob,
		&errorBlob
	);
	if (FAILED(serializeResult) || !signatureBlob) {
		return;
	}
	if (FAILED(dxCommon_->GetDevice()->CreateRootSignature(
		0,
		signatureBlob->GetBufferPointer(),
		signatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&foamRootSignature_)
	))) {
		return;
	}

	const D3D12_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
			D3D12_APPEND_ALIGNED_ELEMENT,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
			D3D12_APPEND_ALIGNED_ELEMENT,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	const auto vertexShader = dxCommon_->CompileShader(
		L"resources/shaders/Lightning.VS.hlsl", L"vs_6_0"
	);
	const auto pixelShader = dxCommon_->CompileShader(
		L"resources/shaders/Lightning.PS.hlsl", L"ps_6_0"
	);
	if (!vertexShader || !pixelShader) {
		return;
	}

	D3D12_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	D3D12_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;
	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	depthStencilDesc.DepthEnable = TRUE;
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = foamRootSignature_.Get();
	pipelineDesc.InputLayout = { inputElements, _countof(inputElements) };
	pipelineDesc.VS = {
		vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()
	};
	pipelineDesc.PS = {
		pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()
	};
	pipelineDesc.BlendState = blendDesc;
	pipelineDesc.RasterizerState = rasterizerDesc;
	pipelineDesc.DepthStencilState = depthStencilDesc;
	pipelineDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	pipelineDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	pipelineDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pipelineDesc.SampleDesc.Count = 1;
	dxCommon_->GetDevice()->CreateGraphicsPipelineState(
		&pipelineDesc,
		IID_PPV_ARGS(&foamPipelineState_)
	);
}

void WaterSurfaceRenderer::CreateResources(uint32_t gridResolution) {
	std::vector<SurfaceVertex> vertices;
	vertices.reserve(
		static_cast<size_t>(gridResolution) *
		static_cast<size_t>(gridResolution) *
		6u
	);

	auto makeVertex = [](float u, float v) {
		return SurfaceVertex{
			{ u * 2.0f - 1.0f, 0.0f, v * 2.0f - 1.0f },
			{ u, v }
		};
	};

	for (uint32_t z = 0; z < gridResolution; ++z) {
		const float v0 = static_cast<float>(z) /
			static_cast<float>(gridResolution);
		const float v1 = static_cast<float>(z + 1) /
			static_cast<float>(gridResolution);
		for (uint32_t x = 0; x < gridResolution; ++x) {
			const float u0 = static_cast<float>(x) /
				static_cast<float>(gridResolution);
			const float u1 = static_cast<float>(x + 1) /
				static_cast<float>(gridResolution);

			vertices.push_back(makeVertex(u0, v0));
			vertices.push_back(makeVertex(u1, v0));
			vertices.push_back(makeVertex(u0, v1));

			vertices.push_back(makeVertex(u0, v1));
			vertices.push_back(makeVertex(u1, v0));
			vertices.push_back(makeVertex(u1, v1));
		}
	}

	vertexCount_ = static_cast<uint32_t>(vertices.size());
	vertexResource_ = dxCommon_->CreateBufferResource(
		sizeof(SurfaceVertex) * vertices.size()
	);
	SurfaceVertex* mappedVertices = nullptr;
	vertexResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&mappedVertices)
	);
	std::memcpy(
		mappedVertices,
		vertices.data(),
		sizeof(SurfaceVertex) * vertices.size()
	);
	vertexResource_->Unmap(0, nullptr);

	vertexBufferView_.BufferLocation =
		vertexResource_->GetGPUVirtualAddress();
	vertexBufferView_.SizeInBytes =
		static_cast<UINT>(sizeof(SurfaceVertex) * vertices.size());
	vertexBufferView_.StrideInBytes = sizeof(SurfaceVertex);

	surfaceResource_ = dxCommon_->CreateBufferResource(sizeof(SurfaceData));
	surfaceResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&surfaceData_)
	);
	surfaceData_->viewProjection = MakeIdentity4x4();
}

void WaterSurfaceRenderer::CreateFoamResources() {
	foamVertexResource_ = dxCommon_->CreateBufferResource(
		sizeof(FoamVertex) * kFoamVerticesPerSlot * kFoamDrawSlotCount
	);
	if (!foamVertexResource_ || FAILED(foamVertexResource_->Map(
		0, nullptr, reinterpret_cast<void**>(&foamVertices_)
	))) {
		foamVertices_ = nullptr;
		return;
	}
	foamVertexBufferView_.BufferLocation =
		foamVertexResource_->GetGPUVirtualAddress();
	foamVertexBufferView_.SizeInBytes =
		sizeof(FoamVertex) * kFoamVerticesPerSlot;
	foamVertexBufferView_.StrideInBytes = sizeof(FoamVertex);

	foamCameraResource_ = dxCommon_->CreateBufferResource(
		static_cast<size_t>(kFoamCameraSlotSize) * kFoamDrawSlotCount
	);
	if (!foamCameraResource_ || FAILED(foamCameraResource_->Map(
		0, nullptr, reinterpret_cast<void**>(&foamCameraData_)
	))) {
		foamCameraData_ = nullptr;
		return;
	}
	for (uint32_t slot = 0; slot < kFoamDrawSlotCount; ++slot) {
		FoamCameraData* data = reinterpret_cast<FoamCameraData*>(
			foamCameraData_ + static_cast<size_t>(slot) * kFoamCameraSlotSize
		);
		data->viewProjection = MakeIdentity4x4();
	}
}
