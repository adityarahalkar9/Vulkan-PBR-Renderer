#version 450

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 view; mat4 proj; mat4 lightViewProj; mat4 invViewProj;
    vec4 camPos; vec4 sunDir; vec4 sunColorIntensity; vec4 ambientColor;
    vec4 screenParams; vec4 skyZenith; vec4 skyHorizon; vec4 miscParams;
    vec4 pointPosRadius[8]; vec4 pointColorIntensity[8];
} scene;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

// Set 1 is pushed per draw via vkCmdPushDescriptorSet (core Vulkan 1.4).
layout(set = 1, binding = 0) uniform sampler2D uBaseColor;
layout(set = 1, binding = 1) uniform sampler2D uMetallicRoughness;
layout(set = 1, binding = 2) uniform sampler2D uNormal;
layout(set = 1, binding = 3) uniform sampler2D uOcclusion;
layout(set = 1, binding = 4) uniform sampler2D uEmissive;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 params0;
    vec4 params1;
} mat;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

const uint FLAG_BASE_COLOR   = 1u;
const uint FLAG_MR           = 2u;
const uint FLAG_NORMAL       = 4u;
const uint FLAG_OCCLUSION    = 8u;
const uint FLAG_EMISSIVE     = 16u;
const uint FLAG_DOUBLE_SIDED = 32u;
const uint FLAG_ALPHA_MASK   = 64u;
const uint FLAG_ALPHA_BLEND  = 128u;

const float PI = 3.14159265359;

// GGX / Trowbridge-Reitz normal distribution.
float distributionGGX(float NdotH, float roughness) {
    float a2 = roughness * roughness;
    a2 *= a2;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Height-correlated Smith visibility.
float visibilitySmith(float NdotV, float NdotL, float roughness) {
    float a2 = roughness * roughness;
    a2 *= a2;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnelSchlick(float u, vec3 f0) {
    return f0 + (vec3(1.0) - f0) * pow(1.0 - u, 5.0);
}

vec3 fresnelSchlickRoughness(float u, vec3 f0, float roughness) {
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - u, 5.0);
}

// Cheap environment term: sky gradient projected onto the normal (IBL-lite).
vec3 hemisphereAmbient(vec3 n) {
    float t = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(scene.skyHorizon.rgb * 0.6, scene.skyZenith.rgb, t);
}

// 3x3 PCF with normal-offset receiving position.
float sampleShadow(vec3 n) {
    vec3 world = vWorldPos + n * scene.miscParams.z;
    vec4 lp = scene.lightViewProj * vec4(world, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float depth = ndc.z; // zero-to-one depth
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0) return 1.0;
    float texel = 1.0 / scene.miscParams.w;
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            shadow += texture(uShadowMap, vec3(uv + vec2(float(x), float(y)) * texel, depth));
    return shadow / 9.0;
}

vec3 aces(vec3 x) { // Narkowicz 2015 fit
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// Screen-space cotangent frame (normal mapping without precomputed tangents).
mat3 cotangentFrame(vec3 n, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-8) return mat3(1.0);
    float inv = 1.0 / det;
    vec3 t = normalize(dp1 * duv2.y - dp2 * duv1.y);
    vec3 b = dp2 * duv1.x - dp1 * duv2.x;
    return mat3(t, b, n);
}

void main() {
    uint flags = floatBitsToUint(mat.params0.w);
    float metallic = mat.params0.x;
    float roughness = clamp(mat.params0.y, 0.045, 1.0);

    vec4 baseColor = mat.baseColorFactor;
    if ((flags & FLAG_BASE_COLOR) != 0u) baseColor *= texture(uBaseColor, vTexCoord);
    if ((flags & FLAG_ALPHA_MASK) != 0u && baseColor.a < mat.params0.z) discard;

    if ((flags & FLAG_MR) != 0u) {
        vec4 mr = texture(uMetallicRoughness, vTexCoord); // G = metallic, A = roughness
        metallic = clamp(mr.g * metallic, 0.0, 1.0);
        roughness = clamp(mr.a * roughness, 0.045, 1.0);
    }

    vec3 albedo = baseColor.rgb;

    vec3 n = normalize(vNormal);
    if ((flags & FLAG_DOUBLE_SIDED) != 0u && !gl_FrontFacing) n = -n;
    if ((flags & FLAG_NORMAL) != 0u) {
        vec3 nt = texture(uNormal, vTexCoord).xyz * 2.0 - 1.0;
        nt.xy *= mat.params1.x;
        n = normalize(cotangentFrame(n, vWorldPos, vTexCoord) * nt);
    }

    vec3 v = normalize(scene.camPos.xyz - vWorldPos);
    float NdotV = clamp(dot(n, v), 1e-4, 1.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);

    // ---- directional sun --------------------------------------------------
    {
        vec3 l = normalize(scene.sunDir.xyz);
        float NdotL = dot(n, l);
        if (NdotL > 0.0 && scene.sunColorIntensity.a > 0.0) {
            float shadow = scene.miscParams.y > 0.5 ? sampleShadow(n) : 1.0;
            vec3 h = normalize(v + l);
            float NdotH = clamp(dot(n, h), 0.0, 1.0);
            float VdotH = clamp(dot(v, h), 0.0, 1.0);
            vec3 f = fresnelSchlick(VdotH, f0);
            vec3 specular = distributionGGX(NdotH, roughness) * visibilitySmith(NdotV, NdotL, roughness) * f;
            vec3 diffuse = albedo / PI * (1.0 - f) * (1.0 - metallic);
            vec3 radiance = scene.sunColorIntensity.rgb * scene.sunColorIntensity.a;
            Lo += (diffuse + specular) * radiance * NdotL * shadow;
        }
    }

    // ---- point lights ------------------------------------------------------
    int count = int(scene.miscParams.x);
    for (int i = 0; i < count; ++i) {
        vec3 toLight = scene.pointPosRadius[i].xyz - vWorldPos;
        float dist2 = dot(toLight, toLight);
        float dist = sqrt(dist2);
        vec3 l = toLight / max(dist, 1e-4);
        float NdotL = dot(n, l);
        if (NdotL <= 0.0) continue;
        float window = clamp(1.0 - dist / max(scene.pointPosRadius[i].w, 1e-3), 0.0, 1.0);
        float attenuation = window * window / (dist2 + 1.0);
        vec3 radiance = scene.pointColorIntensity[i].rgb * scene.pointColorIntensity[i].a * attenuation;
        vec3 h = normalize(v + l);
        float NdotH = clamp(dot(n, h), 0.0, 1.0);
        float VdotH = clamp(dot(v, h), 0.0, 1.0);
        vec3 f = fresnelSchlick(VdotH, f0);
        vec3 specular = distributionGGX(NdotH, roughness) * visibilitySmith(NdotV, NdotL, roughness) * f;
        vec3 diffuse = albedo / PI * (1.0 - f) * (1.0 - metallic);
        Lo += (diffuse + specular) * radiance * NdotL;
    }

    // ---- ambient / occlusion -----------------------------------------------
    float occlusion = 1.0;
    if ((flags & FLAG_OCCLUSION) != 0u)
        occlusion = mix(1.0, texture(uOcclusion, vTexCoord).r, mat.params1.y);
    vec3 env = hemisphereAmbient(n) * scene.ambientColor.rgb * scene.ambientColor.a;
    vec3 ambientF = fresnelSchlickRoughness(NdotV, f0, roughness);
    vec3 ambient = (albedo * (1.0 - metallic) * (vec3(1.0) - ambientF) + ambientF * 0.5) * env * occlusion;

    vec3 emissive = mat.emissiveFactor.rgb;
    if ((flags & FLAG_EMISSIVE) != 0u) emissive *= texture(uEmissive, vTexCoord).rgb;

    vec3 color = (Lo + ambient + emissive) * scene.screenParams.z;
    color = aces(color);
    if (scene.screenParams.w > 0.5) color = pow(color, vec3(1.0 / 2.2)); // non-sRGB swapchain fallback
    outColor = vec4(color, baseColor.a);
}