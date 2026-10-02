#include "assets/textureloader.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace texload{
    bool decode(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba) {
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
        if(!pixels) return false;
        rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }

} // namespace texload