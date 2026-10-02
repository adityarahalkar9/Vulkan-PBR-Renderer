#pragma once
// Data types shared by the asset pipeline (loader -> upload queue -> scene).

#include "core/types.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

enum class AssetState { Queued, Decoding, Uploading, Ready, Failed };

inline const char* stateName(AssetState s) {
    switch(s) {
        case AssetState::Queued:    return "queued";
        case AssetState::Decoding:  return "decoding";
        case AssetState::Uploading: return "uploading";
        case AssetState::Ready:     return "ready";
        default:                    return "failed";
    }
}

namespace matflags {
    constexpr uint32_t HasBaseColor = 0x1;
    constexpr uint32_t HasMetallicRoughness = 0x2;
    constexpr uint32_t HasNormal = 0x4;
    constexpr uint32_t HasOcclusion = 0x8;
    constexpr uint32_t HasEmissive = 0x10;
    constexpr uint32_t DoubleSided = 0x20;
    constexpr uint32_t AlphaMask = 0x40;
    constexpr uint32_t AlphaBlend = 0x80;
} // namespace matflags

struct SamplerParams {
    VkFilter mag = VK_FILTER_LINEAR;
    VkFilter min = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapS = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapT = VK_SAMPLER_ADDRESS_MODE_REPEAT;
};

// ----------------------------------------------------------------- textures
struct TextureAsset {
    std::string name;
    std::atomic<int> state{static_cast<int>(AssetState::Queued)};
    // Filled in by the main thread when the upload completes:
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, mipLevels = 0;
    bool srgb = false;
};

// ---------------------------------------------------------------- materials
struct MaterialAsset {
    std::string name;
    AssetState state = AssetState::Queued;

    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveFactor{0.0f}; // rgb used
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.5f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    uint32_t flags = 0;

    int32_t texBaseColor = -1;
    int32_t texMetallicRoughness = -1;
    int32_t texNormal = -1;
    int32_t texOcclusion = -1;
    int32_t texEmissive = -1;
};

// ------------------------------------------------------------------- models
struct Primitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t material = -1;
    uint32_t flags = 0; // matflags subset used for pipeline selection
};

struct ModelAsset {
    std::string name;
    AssetState state = AssetState::Queued;
    std::vector<Primitive> primitives;
    AABB bounds;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
};

// ----------------------------------------------------------------- entities
struct Entity {
    uint32_t model = 0;
    int32_t materialOverride = -1;
    glm::mat4 modelMatrix{1.0f};
    std::string name;
    bool visible = true;
    AABB worldAABB;
};

// -------------------------------------------------- GLTF parse output (CPU side)
struct GltfEntitySpawn {
    std::string name;
    glm::mat4 matrix{1.0f};
};

struct TextureDesc {
    std::vector<uint8_t> bytes; // still compressed (PNG/JPEG); decoded in parallel jobs
    bool srgb = false;
    SamplerParams sampler;
    std::string name;
};

struct MaterialDesc {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveFactor{0.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.5f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    uint32_t flags = 0;
    int32_t texBaseColor = -1;
    int32_t texMetallicRoughness = -1;
    int32_t texNormal = -1;
    int32_t texOcclusion = -1;
    int32_t texEmissive = -1;
    std::string name;
};

struct GltfImportData {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Primitive> primitives;
    std::vector<MaterialDesc> materials;
    std::vector<TextureDesc> textures;
    std::vector<GltfEntitySpawn> entities;
    AABB bounds;
};

struct GltfParsedResult {
    void* request = nullptr; // LoadRequest*
    GltfImportData data;
};

// --------------------------------------------------------- upload queue types
struct MeshUploadTask {
    uint32_t modelId = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds;
};

struct TextureUploadTask {
    uint32_t textureId = 0;
    std::vector<uint8_t> pixels; // decoded RGBA8
    uint32_t width = 0, height = 0;
    bool srgb = false;
    SamplerParams sampler;
    std::string name;
};

struct UploadTask {
    enum class Kind { Mesh, Texture };
    Kind kind = Kind::Mesh;
    void* request = nullptr; // LoadRequest* (null for procedural assets)
    MeshUploadTask mesh;
    TextureUploadTask texture;
};

struct UploadResult {
    enum class Kind { Mesh, Texture };
    Kind kind = Kind::Mesh;
    uint32_t id = 0;
    bool ok = false;
    std::string error;
    void* request = nullptr;
    // Mesh handles
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;
    // Texture handles
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t mipLevels = 0;
};

// ------------------------------------------------------------ load tracking
struct LoadRequest {
    std::string name;
    std::string source;
    std::atomic<int> state{static_cast<int>(AssetState::Queued)};
    std::string error;
    std::mutex errorMutex;

    std::vector<GltfEntitySpawn> entities;
    std::vector<uint32_t> modelIds;
    int pendingUploads = 0; // touched only on the main thread
    bool meshFailed = false;

    void fail(const std::string& msg) {
        {
            std::scoped_lock lock(errorMutex);
            error = msg;
        }
        state.store(static_cast<int>(AssetState::Failed));
    }
    bool failed() const { return state.load() == static_cast<int>(AssetState::Failed); }
    std::string errorString() const {
        std::scoped_lock lock(errorMutex);
        return error;
    }
};

struct DownloadState {
    std::string url;
    std::atomic<int> phase{0};          // 0 idle, 1 downloading, 2 parsing, 3 done, 4 failed
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
};