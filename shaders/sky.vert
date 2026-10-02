#version 450

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 view; mat4 proj; mat4 lightViewProj; mat4 invViewProj;
    vec4 camPos; vec4 sunDir; vec4 sunColorIntensity; vec4 ambientColor;
    vec4 screenParams; vec4 skyZenith; vec4 skyHorizon; vec4 miscParams;
    vec4 pointPosRadius[8]; vec4 pointColorIntensity[8];
} scene;

layout(location = 0) out vec3 vWorldPos;

void main() {
    // Fullscreen triangle without a vertex buffer, placed on the far plane.
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 world = scene.invViewProj * vec4(ndc, 1.0, 1.0);
    vWorldPos = world.xyz / world.w;
    gl_Position = vec4(ndc, 1.0, 1.0);
}