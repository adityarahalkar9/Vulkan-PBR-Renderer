#pragma once
// Shared engine types: math helpers, settings, frame statistics rings.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

inline float uintToFloatBits(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t floatToUintBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

// ------------------------------------------------------------------- AABB
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    bool valid() const { return min.x <= max.x; }
    void expand(const glm::vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
    void expand(const AABB& o) { min = glm::min(min, o.min); max = glm::max(max, o.max); }
    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extents() const { return (max - min) * 0.5f; }
    float radius() const { return glm::length(max - min) * 0.5f; }

    AABB transformed(const glm::mat4& m) const {
        AABB out;
        for(int i = 0; i < 8; ++i) {
            glm::vec3 c(i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z);
            out.expand(glm::vec3(m * glm::vec4(c, 1.0f)));
        }
        return out;
    }
};

// ----------------------------------------------------------------- Settings
struct Settings {
    bool shadows = true;
    bool sky = true;
    bool wireframe = false;
    bool frustumCulling = true;
    bool animateLights = true;
    bool showUi = true;

    uint32_t shadowResolution = 2048;
    float shadowBiasConstant = 2.0f;
    float shadowBiasSlope = 1.75f;
    float shadowNormalOffset = 1.5f; // in shadow-map texels

    float exposure = 1.0f;
    float ambientIntensity = 0.55f;

    float sunAzimuth = 2.2f;
    float sunElevation = 0.9f;
    glm::vec3 sunColor{1.0f, 0.96f, 0.9f};
    float sunIntensity = 3.5f;

    int presentMode = 0;        // index into Swapchain::availablePresentModes()
    uint32_t msaaRequest = 4;   // clamped to device support
};

// ------------------------------------------------------------ ring buffers
template <size_t N>
struct Ring {
    std::array<float, N> data{};
    size_t head = 0;
    size_t count = 0;

    void push(float v) { data[head] = v; head = (head + 1) % N; count = std::min(count + 1, N); }
    float latest() const { return count ? data[(head + N - 1) % N] : 0.0f; }
    float at(size_t i) const { return data[(head + N - count + i) % N]; } // i = 0 is oldest
    size_t size() const { return count; }
    static constexpr size_t capacity() { return N; }

    void copyTo(float* out) const { for(size_t i = 0; i < count; ++i) out[i] = at(i); }
};

struct FrameStats {
    Ring<240> frameMs;
    Ring<240> cpuUpdateMs, cpuImGuiMs, cpuRenderMs;
    Ring<240> gpuShadowMs, gpuOpaqueMs, gpuSkyMs, gpuUiMs, gpuTotalMs;

    float fps = 0.0f;
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t entitiesTotal = 0;
    uint32_t entitiesVisible = 0;
    uint32_t shadowCasters = 0;
    uint64_t frameIndex = 0;
};