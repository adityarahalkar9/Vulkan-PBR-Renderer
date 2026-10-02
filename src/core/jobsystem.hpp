#pragma once
// Generic worker pool. GLTF parsing, image decoding and HTTP downloads are
// dispatched here so none of them ever block the render thread.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class JobSystem {
public:
    explicit JobSystem(uint32_t threadCount = 0);
    ~JobSystem();

    void dispatch(std::function<void()> job);

    uint32_t threadCount() const { return static_cast<uint32_t>(workers_.size()); }
    uint32_t pending() const { return pending_.load(std::memory_order_relaxed); }
    uint32_t active() const { return active_.load(std::memory_order_relaxed); }

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cvWork_;
    bool stopping_ = false;
    std::atomic<uint32_t> pending_{0};
    std::atomic<uint32_t> active_{0};
};