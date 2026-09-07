// 役割: Scene標準設定とProfile設定で共有するPostEffect編集UIを実装する。
#include "PostProcessSettingsEditor.h"

#include "../../externals/imgui/imgui.h"
#include "../scene/SceneSettings.h"

bool DrawPostProcessSettingsEditor(ScenePostProcessSettings& settings) {
	bool changed = false;
	ImGui::BeginChild("EffectStack", ImVec2(0.0f, 0.0f), true);

	ImGui::PushID("HDRBloom");
	if (ImGui::TreeNodeEx("HDR / Bloom / ToneMap", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::Checkbox("Bloom", &settings.bloomEnabled);
		changed |= ImGui::SliderFloat("Base Exposure", &settings.baseExposure, 0.01f, 5.0f);
		const char* toneMapNames[] = { "ACES", "Reinhard" };
		changed |= ImGui::Combo("Tone Map", &settings.toneMapMode, toneMapNames, IM_ARRAYSIZE(toneMapNames));
		changed |= ImGui::SliderFloat("Threshold", &settings.bloomThreshold, 0.0f, 10.0f);
		changed |= ImGui::SliderFloat("Soft Knee", &settings.bloomSoftKnee, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Intensity", &settings.bloomIntensity, 0.0f, 5.0f);
		changed |= ImGui::SliderInt("Blur Iterations", &settings.bloomBlurIterations, 0, 12);
		changed |= ImGui::SliderInt("Downsample", &settings.bloomDownsampleScale, 1, 8);
		changed |= ImGui::SliderFloat("Blur Radius", &settings.bloomBlurRadius, 0.0f, 8.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Grayscale");
	changed |= ImGui::Checkbox("##Enabled", &settings.grayscaleEnabled);
	ImGui::SameLine();
	ImGui::TextUnformatted("Grayscale");
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Vignette");
	changed |= ImGui::Checkbox("##Enabled", &settings.vignetteEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Vignette", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::SliderFloat("Scale", &settings.vignetteScale, 0.0f, 32.0f);
		changed |= ImGui::SliderFloat("Power", &settings.vignettePower, 0.05f, 4.0f);
		changed |= ImGui::SliderFloat("Intensity", &settings.vignetteIntensity, 0.0f, 1.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("BoxBlur");
	changed |= ImGui::Checkbox("##Enabled", &settings.boxBlurEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Box Blur", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		const char* kernelNames[] = { "3 x 3", "5 x 5" };
		int kernelIndex = settings.boxBlurKernelSize == 5 ? 1 : 0;
		if (ImGui::Combo("Kernel", &kernelIndex, kernelNames, IM_ARRAYSIZE(kernelNames))) {
			settings.boxBlurKernelSize = kernelIndex == 1 ? 5 : 3;
			changed = true;
		}
		changed |= ImGui::SliderFloat("Strength", &settings.boxBlurStrength, 0.0f, 1.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("GaussianBlur");
	changed |= ImGui::Checkbox("##Enabled", &settings.gaussianBlurEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Gaussian Blur", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		const char* kernelNames[] = { "3 x 3", "5 x 5" };
		int kernelIndex = settings.gaussianBlurKernelSize == 5 ? 1 : 0;
		if (ImGui::Combo("Kernel", &kernelIndex, kernelNames, IM_ARRAYSIZE(kernelNames))) {
			settings.gaussianBlurKernelSize = kernelIndex == 1 ? 5 : 3;
			changed = true;
		}
		changed |= ImGui::SliderFloat("Sigma", &settings.gaussianBlurSigma, 0.1f, 5.0f);
		changed |= ImGui::SliderFloat("Strength", &settings.gaussianBlurStrength, 0.0f, 1.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("DepthOfField");
	changed |= ImGui::Checkbox("##Enabled", &settings.depthOfFieldEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Depth Of Field", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		ImGui::TextDisabled("Depth based focus blur");
		changed |= ImGui::SliderFloat("Focus Distance", &settings.depthOfFieldFocusDistance, 0.1f, 200.0f);
		changed |= ImGui::SliderFloat("Focus Range", &settings.depthOfFieldFocusRange, 0.1f, 100.0f);
		changed |= ImGui::SliderFloat("Blur Strength", &settings.depthOfFieldBlurStrength, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Max Radius", &settings.depthOfFieldMaxRadius, 0.0f, 8.0f);
		changed |= ImGui::SliderFloat("Near Strength", &settings.depthOfFieldNearStrength, 0.0f, 2.0f);
		changed |= ImGui::SliderFloat("Far Strength", &settings.depthOfFieldFarStrength, 0.0f, 2.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("MotionBlur");
	changed |= ImGui::Checkbox("##Enabled", &settings.motionBlurEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Motion Blur", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		ImGui::TextDisabled("Camera motion only");
		changed |= ImGui::SliderFloat("Strength", &settings.motionBlurStrength, 0.0f, 1.0f);
		changed |= ImGui::SliderInt("Samples", &settings.motionBlurSamples, 2, 32);
		changed |= ImGui::SliderFloat("Max Radius", &settings.motionBlurMaxRadius, 0.0f, 64.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("RadialBlur");
	changed |= ImGui::Checkbox("##Enabled", &settings.radialBlurEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Radial Blur", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::DragFloat2("Center", &settings.radialBlurCenter.x, 0.005f, 0.0f, 1.0f, "%.3f");
		changed |= ImGui::SliderFloat("Blur Width", &settings.radialBlurWidth, 0.0f, 0.1f, "%.4f");
		changed |= ImGui::SliderInt("Samples", &settings.radialBlurSamples, 2, 32);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Noise");
	changed |= ImGui::Checkbox("##Enabled", &settings.noiseEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Noise", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::Checkbox("Animate", &settings.noiseAnimate);
		changed |= ImGui::SliderFloat("Amount", &settings.noiseAmount, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Scale", &settings.noiseScale, 0.25f, 8.0f);
		if (settings.noiseAnimate) {
			changed |= ImGui::SliderFloat("Speed", &settings.noiseSpeed, 0.0f, 10.0f);
		}
		changed |= ImGui::DragFloat("Seed", &settings.noiseSeed, 0.01f);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Dissolve");
	changed |= ImGui::Checkbox("##Enabled", &settings.dissolveEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Dissolve", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		const char* maskNames[] = { "Noise 0", "Noise 1" };
		changed |= ImGui::Combo("Mask", &settings.dissolveMaskIndex, maskNames, IM_ARRAYSIZE(maskNames));
		changed |= ImGui::SliderFloat("Threshold", &settings.dissolveThreshold, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Edge Width", &settings.dissolveEdgeWidth, 0.001f, 0.25f);
		changed |= ImGui::ColorEdit4("Edge Color", &settings.dissolveEdgeColor.x);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Outline");
	changed |= ImGui::Checkbox("##Enabled", &settings.outlineEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Outline", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		ImGui::TextUnformatted("Sources");
		changed |= ImGui::Checkbox("Luminance", &settings.outlineLuminanceEnabled);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Depth", &settings.outlineDepthEnabled);
		if (!settings.outlineLuminanceEnabled && !settings.outlineDepthEnabled) {
			ImGui::TextDisabled("Enable at least one source");
		}
		ImGui::SeparatorText("Detection");
		if (settings.outlineLuminanceEnabled) {
			changed |= ImGui::SliderFloat("Luminance Weight", &settings.outlineLuminanceWeight, 0.0f, 10.0f);
		}
		if (settings.outlineDepthEnabled) {
			changed |= ImGui::SliderFloat("Depth Weight", &settings.outlineDepthWeight, 0.0f, 10.0f);
		}
		changed |= ImGui::SliderFloat("Threshold", &settings.outlineThreshold, 0.0f, 2.0f);
		changed |= ImGui::SliderFloat("Softness", &settings.outlineSoftness, 0.001f, 1.0f);
		changed |= ImGui::SliderFloat("Thickness", &settings.outlineThickness, 1.0f, 5.0f);
		changed |= ImGui::ColorEdit4("Color", &settings.outlineColor.x);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("Underwater");
	changed |= ImGui::Checkbox("##Enabled", &settings.underwaterEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Underwater", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::ColorEdit4("Tint", &settings.underwaterTintColor.x);
		changed |= ImGui::SliderFloat("Intensity", &settings.underwaterIntensity, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Fog Density", &settings.underwaterFogDensity, 0.0f, 0.25f, "%.4f");
		changed |= ImGui::SliderFloat("Distortion", &settings.underwaterDistortion, 0.0f, 0.08f, "%.4f");
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();

	ImGui::PushID("WaterRefraction");
	changed |= ImGui::Checkbox("##Enabled", &settings.waterRefractionEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Water Refraction", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::ColorEdit4("Tint", &settings.waterRefractionTintColor.x);
		changed |= ImGui::SliderFloat("Strength", &settings.waterRefractionStrength, 0.0f, 0.08f, "%.4f");
		changed |= ImGui::SliderFloat("Edge Softness", &settings.waterRefractionEdgeSoftness, 0.0f, 1.0f, "%.4f");
		changed |= ImGui::SliderFloat("Tint Density", &settings.waterRefractionTintStrength, 0.0f, 1.0f, "%.4f");
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();
	ImGui::PushID("Pixelation");
	changed |= ImGui::Checkbox("##Enabled", &settings.pixelationEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Pixelation", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::DragInt("Block Size", &settings.pixelationBlockSize, 1.0f, 1, 64);
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::Separator();
	ImGui::PushID("ChromaticAberration");
	changed |= ImGui::Checkbox("##Enabled", &settings.chromaticAberrationEnabled);
	ImGui::SameLine();
	if (ImGui::TreeNodeEx("Chromatic Aberration", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		changed |= ImGui::DragFloat2("Center", &settings.chromaticAberrationCenter.x, 0.005f, 0.0f, 1.0f, "%.3f");
		changed |= ImGui::DragFloat("Intensity", &settings.chromaticAberrationIntensity, 0.0001f, 0.0f, 0.05f, "%.4f");
		changed |= ImGui::DragFloat("Falloff", &settings.chromaticAberrationFalloff, 0.01f, 0.01f, 8.0f, "%.2f");
		ImGui::TreePop();
	}
	ImGui::PopID();

	ImGui::EndChild();
	return changed;
}
