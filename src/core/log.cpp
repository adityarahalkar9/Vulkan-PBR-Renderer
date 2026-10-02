#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace logr {

    namespace {
        std::mutex g_mutex;
        FILE* g_file = nullptr;

        const char* levelTag(Level level) {
            switch(level) {
                case Level::Trace: return "trace";
                case Level::Info:  return "info ";
                case Level::Warn:  return "warn ";
                default:           return "error";
            }
        }
    } // namespace

    void init(const std::filesystem::path& file) {
        std::scoped_lock lock(g_mutex);
        if(!g_file) g_file = _wfopen(file.wstring().c_str(), L"w");
    }

    void shutdown() {
        std::scoped_lock lock(g_mutex);
        if(g_file) { fclose(g_file); g_file = nullptr; }
    }

    void write(Level level, const std::string& message) {
        using clock = std::chrono::system_clock;
        auto now = clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        auto t = clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) % 10000;

        std::scoped_lock lock(g_mutex);
        std::printf("[%02d:%02d:%02d.%03d] [%s] [t%zu] %s\n",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<unsigned>(ms),
                    levelTag(level), tid, message.c_str());
        if(g_file) {
            std::fprintf(g_file, "[%02d:%02d:%02d.%03d] [%s] [t%zu] %s\n",
                         tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<unsigned>(ms),
                         levelTag(level), tid, message.c_str());
            std::fflush(g_file);
        }
    }

} // namespace logr