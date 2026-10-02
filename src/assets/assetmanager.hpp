#pragma once
// Orchestrates the multithreaded asset pipeline:
//   [job threads]     GLTF parse, image decode, HTTP download
//   [uploader thread] staging -> device-local transfer + mipmaps
//   [main thread]     registration, GPU handle ownership, entity spawning
// Because materials are serviced by Vulkan 1.4 push descriptors, no
// descriptor state has to be touched when a texture becomes resident.

#include "assets/assettypes.hpp"
#include "core/threadqueue.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

class VulkanContext;
class JobSystem;
class UploadQueue;
class Scene;

class AssetManager {
public:
    void init(VulkanContext* ctx, JobSystem* jobs, UploadQueue* uploads);
    void setScene(Scene* scene) { scene_ = scene; }
    void shutdown();

    void createDefaultTextures();
    void createDefaultScene();
    void loadModelFromFile(const std::string& path);
    void downloadModel(const std::string& url);
    void update(); // drain queues; must be called on the main thread

    std::vector<std::unique_ptr<TextureAsset>> textures;
    std::vector<std::unique_ptr<MaterialAsset>> materials;
    std::vector<std::unique_ptr<ModelAsset>> models;
    std::vector<std::unique_ptr<LoadRequest>> requests;
    std::vector<std::unique_ptr<DownloadState>> downloads;
    MaterialAsset fallbackMaterial;

    // Defaults matching set-1 bindings: baseColor, MR, normal, occlusion, emissive.
    TextureAsset* defaultTexture(size_t slot) const { return textures[defaultTexIds_[slot]].get(); }

private:
    void registerImport(GltfParsedResult&& result);
    void applyUploadResult(const UploadResult& r);
    void finishRequest(LoadRequest* req);
    uint32_t registerModel(const std::string& name, std::vector<Vertex>&& vertices,
                           std::vector<uint32_t>&& indices, const AABB& bounds);

    VulkanContext* ctx_ = nullptr;
    JobSystem* jobs_ = nullptr;
    UploadQueue* uploads_ = nullptr;
    Scene* scene_ = nullptr;

    ThreadQueue<GltfParsedResult> parsed_;
    std::array<uint32_t, 5> defaultTexIds_{};
};