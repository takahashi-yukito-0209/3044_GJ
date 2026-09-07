// 役割: 水面の反射、屈折、深度色を合成するピクセルシェーダー。
struct PixelShaderInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float wave : TEXCOORD3;
};

cbuffer SurfaceCB : register(b0)
{
    float4x4 gViewProjection;
    float4 gCenterTime;
    float4 gHalfSizeAlpha;
    float4 gCameraPositionFresnel;
    float4 gWaveA;
    float4 gWaveB;
    float4 gWaveC;
    float4 gBaseColor;
    float4 gHighlightColorNormal;
    float4 gFoamEmitters[8];
    float4 gFoamOrientations[8];
    float4 gFoamInfo;
};

float Hash11(float value)
{
    return frac(sin(value * 91.317f) * 43758.5453f);
}

// 頂点変形では表現し切れない短い波を法線へ重ね、広い水面にも
// 太陽光の細かな反射を作る。位置はワールド空間なので画面移動で泳がない。
float2 MicroWaveSlope(float2 worldXZ, float time)
{
    const float2 directionA = normalize(float2(0.82f, 0.57f));
    const float2 directionB = normalize(float2(-0.36f, 0.93f));
    const float2 directionC = normalize(float2(0.96f, -0.28f));
    const float phaseA = dot(worldXZ, directionA) * 2.45f + time * 1.72f;
    const float phaseB = dot(worldXZ, directionB) * 3.35f - time * 2.18f;
    const float phaseC = dot(worldXZ, directionC) * 1.38f + time * 0.91f;
    return
        directionA * cos(phaseA) * 0.115f +
        directionB * cos(phaseB) * 0.062f +
        directionC * cos(phaseC) * 0.082f;
}

float4 main(PixelShaderInput input) : SV_TARGET0
{
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(gCameraPositionFresnel.xyz - input.worldPosition);
    const float2 microSlope = MicroWaveSlope(input.worldPosition.xz, gCenterTime.w);
    const float3 microNormal = normalize(float3(-microSlope.x, 1.0f, -microSlope.y));
    const float microNormalWeight = saturate(gHighlightColorNormal.w) * 0.62f;
    normal = normalize(lerp(normal, microNormal, microNormalWeight));
    float facing = saturate(abs(dot(normal, viewDirection)));
    float fresnel = pow(1.0f - facing, gCameraPositionFresnel.w);
    float waveHighlight = saturate(input.wave * 2.5f + 0.35f);

    float3 lightDirection = normalize(float3(-0.35f, 0.85f, -0.25f));
    float softLight = saturate(dot(normal, lightDirection)) * 0.16f;
    const float reflectedLight = saturate(dot(reflect(-lightDirection, normal), viewDirection));
    // 広い反射の上に鋭いきらめきを重ねる。高周波の法線でハイライトが分裂する。
    float glint =
        pow(reflectedLight, 54.0f) * 0.12f +
        pow(reflectedLight, 180.0f) * 0.58f;
    glint *= 0.45f + 0.55f * saturate(dot(normal, float3(0.0f, 1.0f, 0.0f)));

    float3 waterColor = lerp(
        gBaseColor.rgb,
        gHighlightColorNormal.rgb,
        saturate(fresnel * 0.8f + waveHighlight * 0.22f)
    );
    waterColor += softLight + glint * (0.45f + 0.55f * fresnel);

    // 水面に投影した角丸矩形の外周を、途切れた白い泡として合成する。
    // 水面そのものがDepth Testされるため、物体に隠れる箇所は自動的に欠ける。
    float foam = 0.0f;
    [loop]
    for (uint index = 0; index < 8; ++index) {
        if (index >= (uint)gFoamInfo.x) {
            break;
        }
        float4 emitter = gFoamEmitters[index];
        float2 orientation = gFoamOrientations[index].xy;
        float2 offset = input.worldPosition.xz - emitter.xy;
        // World空間から物体のローカルXZ平面へ戻し、回転した輪郭へ追従する。
        float2 local = float2(
            offset.x * orientation.y - offset.y * orientation.x,
            offset.x * orientation.x + offset.y * orientation.y
        );
        float2 boxHalfSize = emitter.zw;
        float2 boxDistance = abs(local) - boxHalfSize;
        float signedDistance = length(max(boxDistance, 0.0f)) +
            min(max(boxDistance.x, boxDistance.y), 0.0f);
        float angle = atan2(local.y, local.x);
        // 一様な輪を避けるため、外周を少し揺らし、短い泡の塊へ分割する。
        float edgeWobble =
            sin(angle * 5.0f + emitter.x * 0.43f - gCenterTime.w * 1.15f) * 0.055f +
            sin(angle * 11.0f - emitter.y * 0.31f + gCenterTime.w * 1.75f) * 0.024f;
        float ringDistance = abs(signedDistance - edgeWobble * min(boxHalfSize.x, boxHalfSize.y));
        float foamBand = 1.0f - smoothstep(0.16f, 1.05f, ringDistance);
        float arcIndex = floor((angle + 3.14159265f) * 4.8f);
        float arcRandom = Hash11(arcIndex + emitter.x * 0.19f + emitter.y * 0.27f);
        float arcMask = smoothstep(0.06f, 0.32f, arcRandom);
        float movingBreakup = smoothstep(
            -0.90f,
            -0.08f,
            sin(angle * 15.0f - gCenterTime.w * 3.1f + emitter.x)
        );
        float flecks = 1.0f - smoothstep(
            0.14f,
            0.86f,
            abs(signedDistance - (0.42f + edgeWobble * min(boxHalfSize.x, boxHalfSize.y)))
        );
        foam = max(foam, max(
            foamBand * arcMask * movingBreakup,
            flecks * (1.0f - arcMask * 0.22f) * 0.88f
        ));
    }
    waterColor = lerp(waterColor, float3(1.65f, 1.78f, 1.90f), foam);

    float alpha = saturate(gHalfSizeAlpha.w + fresnel * 0.24f + waveHighlight * 0.04f + foam * 0.55f);
    return float4(waterColor, alpha);
}
