#version 450

// Set 0: per-frame scene data (shared by all pipelines; bound normally).
layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj;
    mat4 invViewProj;
    vec4 camPos;              // xyz = camera position
    vec4 sunDir;              // xyz = direction from surface to the sun
    vec4 sunColorIntensity;   // rgb = color, a = intensity
    vec4 ambientColor;        // rgb = tint, a = intensity
    vec4 screenParams;        // x = width, y = height, z = exposure, w = manual gamma flag
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 miscParams;          // x = point light count, y = shadows, z = normal offset, w = shadow map size
    vec4 pointPosRadius[8];   // xyz = position, w = radius
    vec4 pointColorIntensity[8];
} scene;

// Pushed per draw (128 bytes).
layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 params0;             // x = metallic, y = roughness, z = alpha cutoff, w = material flags (bits)
    vec4 params1;             // x = normal scale, y = occlusion strength
} push;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;

void main() {
    vec4 world = push.model * vec4(aPosition, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(transpose(inverse(push.model))) * aNormal; // handles non-uniform scale
    vTexCoord = aTexCoord;
    gl_Position = scene.proj * scene.view * world;
}