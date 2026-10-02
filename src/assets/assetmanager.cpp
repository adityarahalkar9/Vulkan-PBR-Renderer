#include "assets/assetmanager.hpp"

#include "assets/gltfloader.hpp"
#include "assets/textureloader.hpp"
#include "core/jobsystem.hpp"
#include "core/log.hpp"
#include "platform/httpdownloader.hpp"
#include "render/uploadqueue.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <fstream>

namespace {

    void makeSphere(std::vector<Vertex>& v, std::vector<uint32_t>& i, float radius, int seg, int rings) {
        const float pi = 3.14159265359f;
        for(int r = 0; r <= rings; ++r) {
            float phi = pi * static_cast<float>(r) / rings;
            float y = std::cos(phi), s = std::sin(phi);
            for(int g = 0; g <= seg; ++g) {
                float theta = 2.0f * pi * static_cast<float>(g) / seg;
                glm::vec3 n(s * std::cos(theta), y, s * std::sin(theta));
                v.push_back({n * radius, n, {static_cast<float>(g) / seg, static_cast<float>(r) / rings}});
            }
        }
        uint32_t stride = static_cast<uint32_t>(seg) + 1;
        for(int r = 0; r < rings; ++r)
            for(int g = 0; g < seg; ++g) {
                uint32_t a = static_cast<uint32_t>(r) * stride + g;
                uint32_t b = a + 1, c = a + stride, d = c + 1;
                i.insert(i.end(), {a, b, c, b, d, c});
            }
    }

    void makePlane(std::vector<Vertex>& v, std::vector<uint32_t>& i, float size) {
        float h = size * 0.5f;
        v = {
            {{-h, 0, -h}, {0, 1, 0}, {0, 0}},
            {{ h, 0, -h}, {0, 1, 0}, {1, 0}},
            {{ h, 0,  h}, {0, 1, 0}, {1, 1}},
            {{-h, 0,  h}, {0, 1, 0}, {0, 1}},
        };
        i = {0, 2, 1, 0, 3, 2};
    }

} // namespace

void AssetManager::init(VulkanContext* ctx, JobSystem* jobs, UploadQueue* uploads) {
    ctx_ = ctx;
    jobs_ = jobs;
    uploads_ = uploads;
    fallbackMaterial.name = "fallback";
}

void AssetManager::shutdown() {
    if(ctx_) vkDeviceWaitIdle(ctx_->device);
    for(auto& t : textures)
        if(t->image) {
            vkDestroySampler(ctx_->device, t->sampler, nullptr);
            vkDestroyImageView(ctx_->device, t->view, nullptr);
            vmaDestroyImage(ctx_->allocator, t->image, t->allocation);
        }
    for(auto& m : models)
        if(m->vertexBuffer) {
            vmaDestroyBuffer(ctx_->allocator, m->vertexBuffer, m->vertexAllocation);
            vmaDestroyBuffer(ctx_->allocator, m->indexBuffer, m->indexAllocation);
        }
}

void AssetManager::createDefaultTextures() {
    struct Def { const char* name; uint8_t rgba[4]; bool srgb; };
    const Def defs[] = {
        {"white(srgb)",  {255, 255, 255, 255}, true },  // base color
        {"mr default",   {  0,   0,   0, 255}, false},  // metallic 0, roughness 1
        {"normal flat",  {128, 128, 255, 255}, false},  // +Z
        {"white(linear)",{255, 255, 255, 255}, false},  // occlusion
        {"black",        {  0,   0,   0, 255}, false},  // emissive
    };
    for(auto& d : defs) {
        TextureAsset asset;
        asset.name = d.name;
        uint32_t id = static_cast<uint32_t>(textures.size());
        textures.push_back(std::make_unique<TextureAsset>(std::move(asset)));

        UploadTask task;
        task.kind = UploadTask::Kind::Texture;
        task.texture = {id, {d.rgba[0], d.rgba[1], d.rgba[2], d.rgba[3]}, 1, 1, d.srgb, {}, d.name};
        applyUploadResult(uploads_->processSync(std::move(task)));
    }
    defaultTexIds_ = {0, 1, 2, 3, 4};
}

uint32_t AssetManager::registerModel(const std::string& name, std::vector<Vertex>&& vertices,
                                     std::vector<uint32_t>&& indices, const AABB& bounds) {
    auto asset = std::make_unique<ModelAsset>();
    asset->name = name;
    asset->state = AssetState::Uploading;
    asset->bounds = bounds;
    asset->vertexCount = static_cast<uint32_t>(vertices.size());
    asset->indexCount = static_cast<uint32_t>(indices.size());

    uint32_t id = static_cast<uint32_t>(models.size());
    models.push_back(std::move(asset));

    UploadTask task;
    task.kind = UploadTask::Kind::Mesh;
    task.mesh = {id, std::move(vertices), std::move(indices), bounds};
    uploads_->enqueue(std::move(task));
    return id;
}

void AssetManager::createDefaultScene() {
    // Sphere study: metallic varies along rows, roughness along columns.
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    makeSphere(verts, idx, 0.5f, 48, 24);
    AABB sphereBounds;
    for(auto& v : verts) sphereBounds.expand(v.position);
    uint32_t sphereModel = registerModel("sphere", std::move(verts), std::move(idx), sphereBounds);

    verts.clear();
    idx.clear();
    makePlane(verts, idx, 40.0f);
    AABB planeBounds{{-20, 0, -20}, {20, 0, 20}};
    uint32_t planeModel = registerModel("ground", std::move(verts), std::move(idx), planeBounds);

    uint32_t groundMat = static_cast<uint32_t>(materials.size());
    {
        MaterialAsset m;
        m.name = "ground";
        m.baseColorFactor = {0.32f, 0.33f, 0.35f, 1.0f};
        m.metallic = 0.0f;
        m.roughness = 0.95f;
        materials.push_back(std::make_unique<MaterialAsset>(std::move(m)));
    }

    const glm::vec3 palette[5] = {
        {0.90f, 0.15f, 0.12f}, {0.95f, 0.64f, 0.20f}, {0.20f, 0.75f, 0.35f},
        {0.25f, 0.40f, 0.90f}, {0.92f, 0.92f, 0.95f},
    };
    for(int i = 0; i < 5; ++i) {
        for(int j = 0; j < 5; ++j) {
            MaterialAsset m;
            m.name = "study_m" + std::to_string(i) + "_r" + std::to_string(j);
            m.baseColorFactor = glm::vec4(palette[i], 1.0f);
            m.metallic = static_cast<float>(i) / 4.0f;
            m.roughness = std::max(static_cast<float>(j) / 4.0f, 0.05f);
            uint32_t matId = static_cast<uint32_t>(materials.size());
            materials.push_back(std::make_unique<MaterialAsset>(std::move(m)));

            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                             glm::vec3((i - 2) * 2.3f, 0.55f, (j - 2) * 2.3f));
            if(scene_) scene_->addEntity(sphereModel, "sphere m" + std::to_string(i) + " r" + std::to_string(j),
                                         model, static_cast<int32_t>(matId), models[sphereModel]->bounds);
        }
    }
    if(scene_) scene_->addEntity(planeModel, "ground", glm::mat4(1.0f),
                                 static_cast<int32_t>(groundMat), models[planeModel]->bounds);
    LOG_INFO("default scene queued (26 entities)");
}

void AssetManager::loadModelFromFile(const std::string& path) {
    auto req = std::make_unique<LoadRequest>();
    req->name = path.substr(path.find_last_of("/\\") + 1);
    req->source = path;
    LoadRequest* raw = req.get();
    requests.push_back(std::move(req));

    jobs_->dispatch([this, raw, path] {
        GltfImportData data;
        std::string error;
        if(gltf::parseFile(path, data, error)) {
            raw->state.store(static_cast<int>(AssetState::Uploading));
            parsed_.push({raw, std::move(data)});
        }
        else {
            raw->fail(error);
        }
                    });
}

void AssetManager::downloadModel(const std::string& url) {
    std::string file = http::fileNameFromUrl(url);
    std::filesystem::path cache = std::filesystem::path(RENDERER_ASSET_DIR) / "downloads" / file;

    // Cached copy?
    if(std::filesystem::exists(cache)) {
        LOG_INFO("using cached '{}'", file);
        loadModelFromFile(cache.string());
        return;
    }

    auto st = std::make_unique<DownloadState>();
    st->url = url;
    DownloadState* rawSt = st.get();
    downloads.push_back(std::move(st));

    auto req = std::make_unique<LoadRequest>();
    req->name = file;
    req->source = url;
    LoadRequest* rawReq = req.get();
    requests.push_back(std::move(req));

    jobs_->dispatch([this, rawSt, rawReq, url, cache] {
        rawSt->phase.store(1);
        std::vector<uint8_t> bytes;
        std::string error;
        auto progress = [rawSt](size_t done, size_t total) {
            rawSt->done.store(static_cast<uint64_t>(done));
            rawSt->total.store(static_cast<uint64_t>(total));
            };
        if(!http::downloadToMemory(url, bytes, progress, error)) {
            rawSt->phase.store(4);
            rawReq->fail("download failed: " + error);
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(cache.parent_path(), ec);
        if(std::ofstream out(cache, std::ios::binary); out)
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));

        rawSt->phase.store(2);
        GltfImportData data;
        if(gltf::parseMemory(bytes.data(), bytes.size(), rawReq->name, data, error)) {
            rawReq->state.store(static_cast<int>(AssetState::Uploading));
            parsed_.push({rawReq, std::move(data)});
            rawSt->phase.store(3);
        }
        else {
            rawSt->phase.store(4);
            rawReq->fail("parse failed: " + error);
        }
                    });
}

void AssetManager::update() {
    GltfParsedResult parsed;
    while(parsed_.tryPop(parsed))
        registerImport(std::move(parsed));

    UploadResult result;
    while(uploads_->results().tryPop(result))
        applyUploadResult(result);
}

void AssetManager::registerImport(GltfParsedResult&& result) {
    LoadRequest* req = static_cast<LoadRequest*>(result.request);
    GltfImportData& data = result.data;
    LOG_INFO("registering '{}' ({} verts, {} prims, {} textures, {} materials)",
             data.name, data.vertices.size(), data.primitives.size(),
             data.textures.size(), data.materials.size());

    // Textures: register, then dispatch parallel decode jobs.
    std::vector<int32_t> texIds(data.textures.size(), -1);
    for(size_t i = 0; i < data.textures.size(); ++i) {
        TextureDesc& desc = data.textures[i];
        if(desc.bytes.empty()) continue; // loader skips missing images

        auto asset = std::make_unique<TextureAsset>();
        asset->name = desc.name;
        asset->srgb = desc.srgb;
        asset->state = static_cast<int>(AssetState::Decoding);
        uint32_t id = static_cast<uint32_t>(textures.size());
        textures.push_back(std::make_unique<TextureAsset>(std::move(asset)));
        texIds[i] = static_cast<int32_t>(id);

        req->pendingUploads += 1; // incremented on the main thread before dispatch
        auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(desc.bytes));
        bool srgb = desc.srgb;
        SamplerParams sampler = desc.sampler;
        std::string name = desc.name;
        jobs_->dispatch([this, id, bytes, srgb, sampler, name, req] {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if(texload::decode(bytes->data(), bytes->size(), w, h, rgba)) {
                UploadTask task;
                task.kind = UploadTask::Kind::Texture;
                task.request = req;
                task.texture = {id, std::move(rgba), static_cast<uint32_t>(w),
                                static_cast<uint32_t>(h), srgb, sampler, name};
                uploads_->enqueue(std::move(task));
            }
            else {
                // Route the failure through the main-thread result queue.
                UploadResult r;
                r.kind = UploadResult::Kind::Texture;
                r.id = id;
                r.ok = false;
                r.error = "decode failed for '" + name + "'";
                r.request = req;
                LOG_WARN("{}", r.error);
                uploads_->results().push(std::move(r));
            }
                        });
    }

    // Materials: remap texture indices.
    for(auto& md : data.materials) {
        MaterialAsset m;
        m.name = md.name;
        m.baseColorFactor = md.baseColorFactor;
        m.emissiveFactor = md.emissiveFactor;
        m.metallic = md.metallic;
        m.roughness = md.roughness;
        m.alphaCutoff = md.alphaCutoff;
        m.normalScale = md.normalScale;
        m.occlusionStrength = md.occlusionStrength;
        m.flags = md.flags;
        m.texBaseColor = md.texBaseColor >= 0 ? texIds[static_cast<size_t>(md.texBaseColor)] : -1;
        m.texMetallicRoughness = md.texMetallicRoughness >= 0 ? texIds[static_cast<size_t>(md.texMetallicRoughness)] : -1;
        m.texNormal = md.texNormal >= 0 ? texIds[static_cast<size_t>(md.texNormal)] : -1;
        m.texOcclusion = md.texOcclusion >= 0 ? texIds[static_cast<size_t>(md.texOcclusion)] : -1;
        m.texEmissive = md.texEmissive >= 0 ? texIds[static_cast<size_t>(md.texEmissive)] : -1;
        materials.push_back(std::make_unique<MaterialAsset>(std::move(m)));
    }

    // One merged model per file.
    uint32_t modelId = registerModel(data.name, std::move(data.vertices),
                                     std::move(data.indices), data.bounds);
    req->modelIds.push_back(modelId);
    req->pendingUploads += 1;
    req->entities = std::move(data.entities);
}

void AssetManager::applyUploadResult(const UploadResult& r) {
    if(r.kind == UploadResult::Kind::Mesh) {
        auto& m = *models[r.id];
        if(r.ok) {
            m.vertexBuffer = r.vertexBuffer;
            m.indexBuffer = r.indexBuffer;
            m.vertexAllocation = r.vertexAllocation;
            m.indexAllocation = r.indexAllocation;
            m.state = AssetState::Ready;
            LOG_INFO("model '{}' resident ({} indices)", m.name, m.indexCount);
        }
        else {
            m.state = AssetState::Failed;
        }
    }
    else {
        auto& t = *textures[r.id];
        if(r.ok) {
            t.image = r.image;
            t.view = r.view;
            t.allocation = r.imageAllocation;
            t.sampler = r.sampler;
            t.mipLevels = r.mipLevels;
            t.state = static_cast<int>(AssetState::Ready);
        }
        else {
            t.state = static_cast<int>(AssetState::Failed);
        }
    }

    if(r.request) {
        LoadRequest* req = static_cast<LoadRequest*>(r.request);
        if(!r.ok && r.kind == UploadResult::Kind::Mesh) req->meshFailed = true;
        if(--req->pendingUploads == 0) finishRequest(req);
    }
}

void AssetManager::finishRequest(LoadRequest* req) {
    if(req->meshFailed) {
        req->fail("mesh upload failed");
        return;
    }
    if(!req->modelIds.empty() && scene_) {
        uint32_t modelId = req->modelIds.front();
        const AABB& bounds = models[modelId]->bounds;
        for(auto& spawn : req->entities)
            scene_->addEntity(modelId, spawn.name, spawn.matrix, -1, bounds);
    }
    req->state.store(static_cast<int>(AssetState::Ready));
    LOG_INFO("asset '{}' ready", req->name);
}