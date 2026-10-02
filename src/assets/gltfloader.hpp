#pragma once
#include "assets/assettypes.hpp"

#include <cstdint>
#include <string>

namespace gltf {
    // Parses GLTF 2.0 files into CPU-side engine data. Image decoding is deferred:
    // raw compressed bytes are kept so textures can be decoded in parallel jobs.
    bool parseFile(const std::string& path, GltfImportData& out, std::string& error);
    bool parseMemory(const uint8_t* data, size_t size, const std::string& name,
                     GltfImportData& out, std::string& error);
} // namespace gltf