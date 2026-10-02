#version 450

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 view; mat4 proj; mat4 lightViewProj; mat4 invViewProj;
    vec4 camPos; vec4 sunDir; vec4 sunColorIntensity; vec4 ambientColor;
    vec4 screenParams; vec4 skyZenith; vec4 skyHorizon; vec4 miscParams;
    vec4 pointPosRadius[8]; vec4 pointColorIntensity[8];
} scene;

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 dir = normalize(vWorldPos - scene.camPos.xyz);
    vec3 sky = mix(scene.skyHorizon.rgb, scene.skyZenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.6));
    sky = mix(sky, scene.skyHorizon.rgb * 0.35, clamp(-dir.y * 3.0, 0.0, 1.0));

    float sd = clamp(dot(dir, normalize(scene.sunDir.xyz)), 0.0, 1.0);
    vec3 sun = scene.sunColorIntensity.rgb * scene.sunColorIntensity.a;
    sky += sun * (pow(sd, 1200.0) * 60.0 + pow(sd, 32.0) * 0.12); // disc + glow

    vec3 color = sky * scene.screenParams.z;
    color = aces(color);
    if (scene.screenParams.w > 0.5) color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}