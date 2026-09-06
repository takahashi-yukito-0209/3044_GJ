// 役割: 水面を通した屈折による画面歪みを合成するピクセルシェーダー。
#include "Fullscreen.hlsli"

Texture2D<float4> gTexture : register(t0);
Texture2D<float> gDepthTexture : register(t1);
SamplerState gSampler : register(s0);
SamplerState gPointSampler : register(s1);

struct PostProcessParameters
{
    float vignetteScale;
    float vignettePower;
    float vignetteIntensity;
    float blurStrength;
    uint blurRadius;
    float gaussianSigma;
    float2 padding;
    float outlineLuminanceWeight;
    float outlineDepthWeight;
    float outlineThreshold;
    float outlineSoftness;
    float outlineThickness;
    float cameraNear;
    float cameraFar;
    uint outlineFlags;
    float4 outlineColor;
    float2 radialBlurCenter;
    float radialBlurWidth;
    uint radialBlurSamples;
    float dissolveThreshold;
    float dissolveEdgeWidth;
    float2 dissolvePadding;
    float4 dissolveEdgeColor;
    float noiseTime;
    float noiseAmount;
    float noiseScale;
    float noiseSeed;
    float dofFocusDistance;
    float dofFocusRange;
    float dofNearStrength;
    float dofFarStrength;
    float dofMaxRadius;
    float3 dofPadding;
    float4 underwaterTintColor;
    float4 underwaterParams;
    float4 cameraUpTime;
    float4 cameraPositionFovY;
    float4 cameraRightAspect;
    float4 cameraForwardActive;
    float4 waterVolumeCenterActive;
    float4 waterVolumeHalfSizeEdge;
    float4 waterRefractionTintColor;
    float4 waterRefractionParams;
};
ConstantBuffer<PostProcessParameters> gParameters : register(b0);

float ViewDepth(float ndcDepth)
{
    const float nearClip = max(gParameters.cameraNear, 0.0001f);
    const float farClip = max(gParameters.cameraFar, nearClip + 0.0001f);
    return nearClip * farClip /
        max(farClip - ndcDepth * (farClip - nearClip), 0.0001f);
}

float3 SafeInverseDirection(float3 direction)
{
    return float3(
        direction.x < 0.0f ? -1.0f : 1.0f,
        direction.y < 0.0f ? -1.0f : 1.0f,
        direction.z < 0.0f ? -1.0f : 1.0f
    ) / max(abs(direction), 0.0001f);
}

bool IntersectAabb(
    float3 origin,
    float3 direction,
    float3 boxMin,
    float3 boxMax,
    out float enter,
    out float exit
)
{
    const float3 invDirection = SafeInverseDirection(direction);
    const float3 t0 = (boxMin - origin) * invDirection;
    const float3 t1 = (boxMax - origin) * invDirection;
    const float3 tNear = min(t0, t1);
    const float3 tFar = max(t0, t1);
    enter = max(max(tNear.x, tNear.y), tNear.z);
    exit = min(min(tFar.x, tFar.y), tFar.z);
    return exit >= max(enter, 0.0f);
}

float3 RayDirectionFromUv(
    float2 uv,
    float3 cameraForward,
    float3 cameraRight,
    float3 cameraUp,
    float aspect,
    float tanHalfFovY
)
{
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f
    );
    return normalize(
        cameraForward +
        cameraRight * ndc.x * aspect * tanHalfFovY +
        cameraUp * ndc.y * tanHalfFovY
    );
}

float WaterThicknessAtUv(
    float2 uv,
    float3 cameraPosition,
    float3 cameraForward,
    float3 cameraRight,
    float3 cameraUp,
    float aspect,
    float tanHalfFovY,
    float3 boxMin,
    float3 boxMax
)
{
    const float3 rayDirection = RayDirectionFromUv(
        uv,
        cameraForward,
        cameraRight,
        cameraUp,
        aspect,
        tanHalfFovY
    );

    float enter;
    float exit;
    if (!IntersectAabb(cameraPosition, rayDirection, boxMin, boxMax, enter, exit))
    {
        return 0.0f;
    }

    const float rawDepth = gDepthTexture.SampleLevel(gPointSampler, uv, 0.0f);
    const float viewDepth = ViewDepth(rawDepth);
    const float forwardAmount = max(dot(rayDirection, cameraForward), 0.0001f);
    const float sceneRayDistance = min(viewDepth / forwardAmount, gParameters.cameraFar);

    enter = max(enter, gParameters.cameraNear);
    exit = min(exit, sceneRayDistance);
    return max(exit - enter, 0.0f);
}

float CoverageFromThickness(float thickness, float edgeSoftness)
{
    if (edgeSoftness <= 0.0001f)
    {
        return thickness > 0.0f ? 1.0f : 0.0f;
    }
    return smoothstep(0.0f, edgeSoftness, thickness);
}

float2 WaveOffset(float2 uv, float time)
{
    const float waveX =
        sin((uv.x * 19.0f + uv.y * 11.0f) + time * 1.7f) +
        sin((uv.x * -13.0f + uv.y * 23.0f) - time * 2.1f);
    const float waveY =
        sin((uv.x * 17.0f - uv.y * 9.0f) + time * 1.3f) +
        sin((uv.x * -29.0f + uv.y * 7.0f) + time * 1.9f);
    return float2(waveX, waveY) * 0.5f;
}

float4 main(VertexShaderOutput input) : SV_TARGET0
{
    const float4 sourceColor = gTexture.Sample(gSampler, input.texcoord);
    if (gParameters.waterVolumeCenterActive.w <= 0.5f)
    {
        return sourceColor;
    }

    const float3 cameraPosition = gParameters.cameraPositionFovY.xyz;
    const float3 cameraRight = normalize(gParameters.cameraRightAspect.xyz);
    const float3 cameraUp = normalize(gParameters.cameraUpTime.xyz);
    const float3 cameraForward = normalize(gParameters.cameraForwardActive.xyz);
    const float fovY = max(gParameters.cameraPositionFovY.w, 0.0001f);
    const float aspect = max(gParameters.cameraRightAspect.w, 0.0001f);
    const float tanHalfFovY = tan(fovY * 0.5f);
    const float3 center = gParameters.waterVolumeCenterActive.xyz;
    const float3 halfSize = max(gParameters.waterVolumeHalfSizeEdge.xyz, 0.001f);
    const float3 boxMin = center - halfSize;
    const float3 boxMax = center + halfSize;
    const float edgeSoftness = max(gParameters.waterVolumeHalfSizeEdge.w, 0.0f);

    const float thickness = WaterThicknessAtUv(
        input.texcoord,
        cameraPosition,
        cameraForward,
        cameraRight,
        cameraUp,
        aspect,
        tanHalfFovY,
        boxMin,
        boxMax
    );
    const float coverage = CoverageFromThickness(thickness, edgeSoftness);
    if (coverage <= 0.0001f)
    {
        return sourceColor;
    }

    const float time = gParameters.cameraUpTime.w;
    const float refractionStrength = max(gParameters.waterRefractionParams.x, 0.0f);
    // 水の中を通る距離に応じて色を吸収する。これにより浅瀬は
    // 見通せるまま、深い場所だけが自然な青緑へ沈む。
    const float tintDensity = max(gParameters.waterRefractionParams.y, 0.0f);
    const float2 requestedOffset =
        WaveOffset(input.texcoord, time) * refractionStrength;

    float2 refractedUv = saturate(input.texcoord + requestedOffset);
    float refractedCoverage = CoverageFromThickness(
        WaterThicknessAtUv(
            refractedUv,
            cameraPosition,
            cameraForward,
            cameraRight,
            cameraUp,
            aspect,
            tanHalfFovY,
            boxMin,
            boxMax
        ),
        edgeSoftness
    );

    if (refractedCoverage < 0.35f)
    {
        refractedUv = saturate(input.texcoord + requestedOffset * 0.5f);
        refractedCoverage = CoverageFromThickness(
            WaterThicknessAtUv(
                refractedUv,
                cameraPosition,
                cameraForward,
                cameraRight,
                cameraUp,
                aspect,
                tanHalfFovY,
                boxMin,
                boxMax
            ),
            edgeSoftness
        );
    }
    if (refractedCoverage < 0.35f)
    {
        refractedUv = saturate(input.texcoord + requestedOffset * 0.25f);
        refractedCoverage = CoverageFromThickness(
            WaterThicknessAtUv(
                refractedUv,
                cameraPosition,
                cameraForward,
                cameraRight,
                cameraUp,
                aspect,
                tanHalfFovY,
                boxMin,
                boxMax
            ),
            edgeSoftness
        );
    }
    if (refractedCoverage < 0.35f)
    {
        refractedUv = input.texcoord;
    }

    const float4 refractedColor = gTexture.Sample(gSampler, refractedUv);
    const float tintAmount = 1.0f - exp(-thickness * tintDensity * 0.035f);
    const float3 tintedColor = lerp(
        refractedColor.rgb,
        gParameters.waterRefractionTintColor.rgb,
        saturate(tintAmount) * coverage
    );

    return float4(
        lerp(sourceColor.rgb, tintedColor, coverage),
        sourceColor.a
    );
}
