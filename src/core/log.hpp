#pragma once
#include <filesystem>
#include <format>
#include <string>

namespace logr {

	enum class Level { Trace, Info, Warn, Error };

	void init(const std::filesystem::path& file);
	void shutdown();
	void write(Level level, const std::string& message);

} // namespace logr

#define LOG_TRACE(...) ::logr::write(logr::Level::Trace, std::format(__VA_ARGS__))
#define LOG_INFO(...)  ::logr::write(logr::Level::Info,  std::format(__VA_ARGS__))
#define LOG_WARN(...)  ::logr::write(logr::Level::Warn,  std::format(__VA_ARGS__))
#define LOG_ERROR(...) ::logr::write(logr::Level::Error, std::format(__VA_ARGS__))