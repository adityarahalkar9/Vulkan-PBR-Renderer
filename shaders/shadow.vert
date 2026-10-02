#version 450

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 view; mat4 proj; mat4 lightViewProj; mat4 invViewProj;
    vec4 camPos; vec4 sunDir; vec4 sunColorIntensity; vec4 ambientColor;
    vec4 screenParams; vec4 skyZenith; vec4 skyHorizon; vec4 miscParams;
    vec4 pointPosRadius[8]; vec4 pointColorIntensity[8];
} scene;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 params0;
    vec4 params1;
} push;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = scene.lightViewProj * push.model * vec4(aPosition, 1.0);
}