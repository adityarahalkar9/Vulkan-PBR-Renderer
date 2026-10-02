#pragma once
// HTTPS/HTTP downloader built on WinHTTP (validates certificates, follows
// redirects). Runs inside JobSystem jobs so downloads never block the frame.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace http {

    using ProgressFn = std::function<void(size_t done, size_t total)>;

    bool downloadToMemory(const std::string& url, std::vector<uint8_t>& out,
                          const ProgressFn& progress, std::string& error);
    std::string fileNameFromUrl(const std::string& url);

} // namespace http