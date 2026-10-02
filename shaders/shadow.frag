#version 450

layout(set = 1, binding = 0) uniform sampler2D uBaseColor; // pushed descriptor

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 params0;
    vec4 params1;
} push;

layout(location = 0) in vec2 vTexCoord;

void main() {
    uint flags = floatBitsToUint(push.params0.w);
    if ((flags & 64u) != 0u) { // alpha-masked materials cast cutout shadows
        float a = push.baseColorFactor.a;
        if ((flags & 1u) != 0u) a *= texture(uBaseColor, vTexCoord).a;
        if (a < push.params0.z) discard;
    }
}