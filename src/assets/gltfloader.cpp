#include "assets/gltfloader.hpp"

#include "core/log.hpp"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cmath>
#include <functional>
#include <map>

namespace gltf {

    namespace {

        // Keeps compressed image bytes instead of decoding them; decode happens later
        // in parallel jobs. Missing images are tolerated (defaults are used instead).
        bool onImageLoad(tinygltf::Image* image, int imageIndex, std::string* err, std::string* warn,
                         int, int, const unsigned char* bytes, int size, void* userData) {
            (void)warn;
            auto* raw = static_cast<std::vector<std::vector<uint8_t>>*>(userData);
            if(imageIndex >= static_cast<int>(raw->size())) raw->resize(static_cast<size_t>(imageIndex) + 1);
            if(bytes && size > 0) {
                (*raw)[static_cast<size_t>(imageIndex)].assign(bytes, bytes + size);
                return true;
            }
            if(err) *err = "no image data for '" + image->uri + "'";
            return true; // non-fatal
        }

        size_t componentSize(int componentType) {
            switch(componentType) {
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
                case TINYGLTF_COMPONENT_TYPE_SHORT:
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
                default: return 4;
            }
        }

        size_t componentsPerType(int type) {
            switch(type) {
                case TINYGLTF_TYPE_SCALAR: return 1;
                case TINYGLTF_TYPE_VEC2: return 2;
                case TINYGLTF_TYPE_VEC3: return 3;
                case TINYGLTF_TYPE_VEC4: return 4;
                default: return 16; // MAT4
            }
        }

        struct AccessorView {
            const uint8_t* data = nullptr;
            size_t stride = 0;
            size_t count = 0;
            int componentType = 0;
            bool valid = false;
        };

        AccessorView view(const tinygltf::Model& model, int accessorIndex) {
            AccessorView v;
            if(accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) return v;
            const auto& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
            if(accessor.bufferView < 0) return v;
            const auto& bv = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
            if(bv.buffer < 0) return v;
            const auto& buffer = model.buffers[static_cast<size_t>(bv.buffer)];

            size_t element = componentSize(accessor.componentType) * componentsPerType(accessor.type);
            size_t stride = bv.byteStride != 0 ? bv.byteStride : element;
            size_t offset = bv.byteOffset + accessor.byteOffset;
            if(offset + stride * accessor.count > buffer.data.size()) return v;

            v.data = buffer.data.data() + offset;
            v.stride = stride;
            v.count = accessor.count;
            v.componentType = accessor.componentType;
            v.valid = accessor.sparse.empty;
            return v;
        }

        glm::vec3 readVec3(const AccessorView& a, size_t i) {
            const float* p = reinterpret_cast<const float*>(a.data + i * a.stride);
            return {p[0], p[1], p[2]};
        }

        glm::vec2 readVec2(const AccessorView& a, size_t i) {
            const uint8_t* p = a.data + i * a.stride;
            auto norm = [&](const uint8_t* q) {
                switch(a.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return static_cast<float>(q[0]) / 255.0f;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return static_cast<float>(*reinterpret_cast<const uint16_t*>(q)) / 65535.0f;
                    case TINYGLTF_COMPONENT_TYPE_FLOAT:          return *reinterpret_cast<const float*>(q);
                    default:                                     return 0.0f;
                }
                };
            return {norm(p), norm(p + componentSize(a.componentType))};
        }

        uint32_t readIndex(const AccessorView& a, size_t i) {
            const uint8_t* p = a.data + i * a.stride;
            switch(a.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return p[0];
                default:                                     return 0;
            }
        }

        glm::mat4 nodeMatrix(const tinygltf::Node& node) {
            if(!node.matrix.empty()) return glm::make_mat4(node.matrix.data()); // glTF is column-major
            glm::mat4 t{1.0f};
            if(node.translation.size() == 3)
                t = glm::translate(t, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
            if(node.rotation.size() == 4)
                t *= glm::mat4_cast(glm::quat(static_cast<float>(node.rotation[3]),
                                              static_cast<float>(node.rotation[0]),
                                              static_cast<float>(node.rotation[1]),
                                              static_cast<float>(node.rotation[2])));
            if(node.scale.size() == 3)
                t = glm::scale(t, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
            return t;
        }

        bool convertModel(const tinygltf::Model& model,
                          const std::vector<std::vector<uint8_t>>& rawImages,
                          GltfImportData& out, std::string& error) {
            for(const auto& ext : model.extensionsRequired) {
                if(ext == "KHR_draco_mesh_compression" || ext == "KHR_texture_basisu" ||
                   ext == "EXT_meshopt_compression") {
                    error = "unsupported required extension: " + ext;
                    return false;
                }
            }

            // ---- textures (deduplicated by image + sampler + color space) ----------
            std::map<std::tuple<int, int, bool>, int> textureMap;
            auto addTexture = [&](int gltfTextureIndex, bool srgb) -> int {
                if(gltfTextureIndex < 0) return -1;
                const auto& tex = model.textures[static_cast<size_t>(gltfTextureIndex)];
                int image = tex.image;
                int sampler = tex.sampler;
                auto key = std::make_tuple(image, sampler, srgb);
                auto it = textureMap.find(key);
                if(it != textureMap.end()) return it->second;

                // Skip images whose data never arrived; the renderer substitutes defaults.
                if(image < 0 || image >= static_cast<int>(rawImages.size()) ||
                   rawImages[static_cast<size_t>(image)].empty())
                    return -1;

                TextureDesc desc;
                desc.srgb = srgb;
                desc.bytes = rawImages[static_cast<size_t>(image)];
                desc.name = image < static_cast<int>(model.images.size())
                    ? model.images[static_cast<size_t>(image)].name
                    : "texture";
                if(sampler >= 0 && sampler < static_cast<int>(model.samplers.size())) {
                    const auto& s = model.samplers[static_cast<size_t>(sampler)];
                    desc.sampler.mag = (s.magFilter == 9728) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                    bool minNearest = (s.minFilter == 9728 || s.minFilter == 9984);
                    desc.sampler.min = minNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                    desc.sampler.mip = minNearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    if(s.wrapS == 33071)      desc.sampler.wrapS = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    else if(s.wrapS == 33648) desc.sampler.wrapS = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                    if(s.wrapT == 33071)      desc.sampler.wrapT = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    else if(s.wrapT == 33648) desc.sampler.wrapT = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                }
                out.textures.push_back(std::move(desc));
                int id = static_cast<int>(out.textures.size()) - 1;
                textureMap.emplace(key, id);
                return id;
                };

            // ---- materials -----------------------------------------------------------
            for(const auto& m : model.materials) {
                MaterialDesc mat;
                const auto& pbr = m.pbrMetallicRoughness;
                for(int i = 0; i < 4; ++i)
                    mat.baseColorFactor[i] = static_cast<float>(pbr.baseColorFactor[static_cast<size_t>(i)]);
                for(int i = 0; i < 3; ++i)
                    mat.emissiveFactor[i] = static_cast<float>(m.emissiveFactor[static_cast<size_t>(i)]);
                mat.metallic = static_cast<float>(pbr.metallicFactor);
                mat.roughness = static_cast<float>(pbr.roughnessFactor);
                mat.name = m.name;

                mat.texBaseColor = addTexture(pbr.baseColorTexture.index, true);
                mat.texMetallicRoughness = addTexture(pbr.metallicRoughnessTexture.index, false);
                mat.texNormal = addTexture(m.normalTexture.index, false);
                mat.texOcclusion = addTexture(m.occlusionTexture.index, false);
                mat.texEmissive = addTexture(m.emissiveTexture.index, true);
                mat.normalScale = static_cast<float>(m.normalTexture.scale);
                mat.occlusionStrength = static_cast<float>(m.occlusionTexture.strength);
                mat.alphaCutoff = static_cast<float>(m.alphaCutoff);

                uint32_t flags = 0;
                if(mat.texBaseColor >= 0) flags |= matflags::HasBaseColor;
                if(mat.texMetallicRoughness >= 0) flags |= matflags::HasMetallicRoughness;
                if(mat.texNormal >= 0) flags |= matflags::HasNormal;
                if(mat.texOcclusion >= 0) flags |= matflags::HasOcclusion;
                if(mat.texEmissive >= 0) flags |= matflags::HasEmissive;
                if(m.doubleSided) flags |= matflags::DoubleSided;
                if(m.alphaMode == "MASK") flags |= matflags::AlphaMask;
                if(m.alphaMode == "BLEND") flags |= matflags::AlphaBlend;
                mat.flags = flags;
                out.materials.push_back(std::move(mat));
            }

            // ---- meshes (all primitives merged into one vertex/index buffer) ---------
            size_t vertexBase = 0;
            for(const auto& mesh : model.meshes) {
                for(const auto& prim : mesh.primitives) {
                    if(prim.mode != TINYGLTF_MODE_TRIANGLES) continue;

                    auto itPos = prim.attributes.find("POSITION");
                    if(itPos == prim.attributes.end()) continue;
                    AccessorView pos = view(model, itPos->second);
                    if(!pos.valid || pos.type != TINYGLTF_TYPE_VEC3 ||
                       pos.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        LOG_WARN("gltf '{}': primitive without valid POSITION skipped", out.name);
                        continue;
                    }
                    AccessorView nrm;
                    if(auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end())
                        nrm = view(model, it->second);
                    AccessorView uv;
                    if(auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end())
                        uv = view(model, it->second);

                    size_t firstVertex = out.vertices.size();
                    for(size_t i = 0; i < pos.count; ++i) {
                        Vertex v;
                        v.position = readVec3(pos, i);
                        v.normal = nrm.valid ? readVec3(nrm, i) : glm::vec3(0.0f);
                        v.uv = uv.valid ? readVec2(uv, i) : glm::vec2(0.0f);
                        out.vertices.push_back(v);
                    }

                    // Smooth normals when the file does not provide them.
                    if(!nrm.valid && prim.indices >= 0) {
                        AccessorView idx = view(model, prim.indices);
                        for(size_t i = 0; i + 2 < idx.count; i += 3) {
                            uint32_t a = readIndex(idx, i) + static_cast<uint32_t>(vertexBase);
                            uint32_t b = readIndex(idx, i + 1) + static_cast<uint32_t>(vertexBase);
                            uint32_t c = readIndex(idx, i + 2) + static_cast<uint32_t>(vertexBase);
                            if(a >= out.vertices.size() || b >= out.vertices.size() || c >= out.vertices.size())
                                continue;
                            glm::vec3 n = glm::cross(out.vertices[b].position - out.vertices[a].position,
                                                     out.vertices[c].position - out.vertices[a].position);
                            out.vertices[a].normal += n;
                            out.vertices[b].normal += n;
                            out.vertices[c].normal += n;
                        }
                        for(size_t i = firstVertex; i < out.vertices.size(); ++i) {
                            auto& n = out.vertices[i].normal;
                            n = glm::length(n) > 1e-8f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
                        }
                    }

                    Primitive enginePrim;
                    enginePrim.firstIndex = static_cast<uint32_t>(out.indices.size());
                    if(prim.indices >= 0) {
                        AccessorView idx = view(model, prim.indices);
                        for(size_t i = 0; i < idx.count; ++i)
                            out.indices.push_back(readIndex(idx, i) + static_cast<uint32_t>(vertexBase));
                    }
                    else {
                        for(size_t i = 0; i < pos.count; ++i)
                            out.indices.push_back(static_cast<uint32_t>(vertexBase + i));
                    }
                    enginePrim.indexCount = static_cast<uint32_t>(out.indices.size()) - enginePrim.firstIndex;
                    enginePrim.material = prim.material;

                    // glTF guarantees POSITION accessor min/max.
                    const auto& pa = model.accessors[static_cast<size_t>(itPos->second)];
                    if(pa.min.size() == 3 && pa.max.size() == 3) {
                        AABB p;
                        p.min = glm::vec3(pa.min[0], pa.min[1], pa.min[2]);
                        p.max = glm::vec3(pa.max[0], pa.max[1], pa.max[2]);
                        out.bounds.expand(p);
                    }
                    else {
                        for(size_t i = firstVertex; i < out.vertices.size(); ++i)
                            out.bounds.expand(out.vertices[i].position);
                    }
                    if(prim.material >= 0 && prim.material < static_cast<int>(out.materials.size()))
                        enginePrim.flags = out.materials[static_cast<size_t>(prim.material)].flags;

                    vertexBase = out.vertices.size();
                    out.primitives.push_back(enginePrim);
                }
            }

            if(out.primitives.empty()) {
                error = "model contains no renderable primitives";
                return false;
            }

            // ---- node hierarchy --------------------------------------------------------
            std::function<void(const tinygltf::Node&, const glm::mat4&)> walk =
                [&](const tinygltf::Node& node, const glm::mat4& parent) {
                glm::mat4 world = parent * nodeMatrix(node);
                if(node.mesh >= 0) {
                    GltfEntitySpawn spawn;
                    spawn.name = node.name.empty() ? "node" : node.name;
                    spawn.matrix = world;
                    out.entities.push_back(std::move(spawn));
                }
                for(int child : node.children)
                    if(child >= 0 && child < static_cast<int>(model.nodes.size()))
                        walk(model.nodes[static_cast<size_t>(child)], world);
                };

            if(!model.scenes.empty()) {
                int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
                for(int node : model.scenes[static_cast<size_t>(sceneIndex)].nodes)
                    if(node >= 0 && node < static_cast<int>(model.nodes.size()))
                        walk(model.nodes[static_cast<size_t>(node)], glm::mat4(1.0f));
            }
            if(out.entities.empty()) out.entities.push_back({"root", glm::mat4(1.0f)});
            return true;
        }

    } // namespace

    bool parseFile(const std::string& path, GltfImportData& out, std::string& error) {
        out.name = path.substr(path.find_last_of("/\\") + 1);

        std::vector<std::vector<uint8_t>> raw;
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(onImageLoad, &raw);

        tinygltf::Model model;
        std::string err, warn;

        bool isBinary = path.size() > 4 &&
            (path.compare(path.size() - 4, 4, ".glb") == 0 || path.compare(path.size() - 4, 4, ".GLB") == 0);

        bool ok = isBinary ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
            : loader.LoadASCIIFromFile(&model, &err, &warn, path);
        if(!warn.empty()) LOG_WARN("gltf '{}': {}", out.name, warn);
        if(!ok) {
            error = err.empty() ? "failed to parse GLTF file" : err;
            return false;
        }
        return convertModel(model, raw, out, error);
    }

    bool parseMemory(const uint8_t* data, size_t size, const std::string& name,
                     GltfImportData& out, std::string& error) {
        out.name = name;
        if(size < 4 || data[0] != 'g' || data[1] != 'l' || data[2] != 'T' || data[3] != 'F') {
            error = "only GLB (binary GLTF) is supported for downloads";
            return false;
        }

        std::vector<std::vector<uint8_t>> raw;
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(onImageLoad, &raw);

        tinygltf::Model model;
        std::string err, warn;
        if(!loader.LoadBinaryFromMemory(&model, &err, &warn, data, static_cast<unsigned int>(size), "")) {
            error = err.empty() ? "failed to parse GLB" : err;
            return false;
        }
        return convertModel(model, raw, out, error);
    }

} // namespace gltf