#pragma once
#include <deque>
#include <mutex>
#include <utility>

// Minimal mutex-protected queue for handing results between threads.
template <typename T>
class ThreadQueue {
public:
    void push(T&& value) {
        std::scoped_lock lock(mutex_);
        items_.push_back(std::move(value));
    }

    bool tryPop(T& out) {
        std::scoped_lock lock(mutex_);
        if(items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    size_t size() const {
        std::scoped_lock lock(mutex_);
        return items_.size();
    }

private:
    std::deque<T> items_;
    mutable std::mutex mutex_;
};