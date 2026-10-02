#include "core/jobsystem.hpp"

#include "core/log.hpp"

JobSystem::JobSystem(uint32_t threadCount) {
    if(threadCount == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threadCount = hw > 2 ? hw - 2 : 1;
    }
    for(uint32_t i = 0; i < threadCount; ++i)
        workers_.emplace_back([this] { workerLoop(); });
    LOG_INFO("job system started with {} worker threads", workers_.size());
}

JobSystem::~JobSystem() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    cvWork_.notify_all();
    for(auto& t : workers_)
        if(t.joinable()) t.join();
}

void JobSystem::dispatch(std::function<void()> job) {
    if(stopping_) return;
    {
        std::scoped_lock lock(mutex_);
        jobs_.push_back(std::move(job));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void JobSystem::workerLoop() {
    for(;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cvWork_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if(jobs_.empty()) break; // stopping and drained
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        pending_.fetch_sub(1, std::memory_order_relaxed);
        active_.fetch_add(1, std::memory_order_relaxed);
        job();
        active_.fetch_sub(1, std::memory_order_relaxed);
    }
}