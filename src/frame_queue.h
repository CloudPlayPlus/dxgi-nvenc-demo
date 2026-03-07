#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

// Thread-safe bounded queue with timestamps
template<typename T>
class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 4) : max_size_(max_size) {}

    // Push item (blocks if full). Returns false if closed.
    bool Push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_push_.wait(lock, [this] { return queue_.size() < max_size_ || closed_; });
        if (closed_) return false;
        queue_.push(std::move(item));
        cv_pop_.notify_one();
        return true;
    }

    // Try push without blocking. Returns false if full or closed.
    bool TryPush(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= max_size_) return false;
        queue_.push(std::move(item));
        cv_pop_.notify_one();
        return true;
    }

    // Pop item (blocks until available). Returns false if closed and empty.
    bool Pop(T& item, uint32_t timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ok = cv_pop_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return !queue_.empty() || closed_; });
        if (!ok || queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        cv_push_.notify_one();
        return true;
    }

    void Close() {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_ = true;
        cv_push_.notify_all();
        cv_pop_.notify_all();
    }

    size_t Size() {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool Closed() {
        std::unique_lock<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    std::queue<T>           queue_;
    std::mutex              mutex_;
    std::condition_variable cv_push_;
    std::condition_variable cv_pop_;
    size_t                  max_size_;
    bool                    closed_ = false;
};
