#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace texload {
	// Decodes PNG/JPEG/TGA/BMP bytes to RGBA8. Runs on JobSystem workers.
	bool decode(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);
} // namespace texload