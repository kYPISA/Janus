#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>
#include <exception>
#include <stdexcept>

// Minimal blocking MPMC work queue. Contention is kept low because workers
// spend most of their time inside heavy LZMA/Brotli compressors, not in the
// queue itself.
template <typename T>
class WorkQueue {
public:
    WorkQueue() = default;
    ~WorkQueue() = default;

    void push(T item) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool stop_ = false;
};

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t n) {
        if (n == 0) throw std::runtime_error("Thread pool must have workers");
        try {
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
                Task task;
                while (queue_.pop(task)) {
                    task();
                }
            });
        }
        } catch (...) {
            queue_.stop();
            for (auto& worker : workers_) if (worker.joinable()) worker.join();
            throw;
        }
    }

    ~ThreadPool() {
        queue_.stop();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(Task task) {
        queue_.push(std::move(task));
    }

private:
    WorkQueue<Task> queue_;
    std::vector<std::thread> workers_;
};

// Lock-free-ish reorder buffer: workers insert completed results by index,
// the consumer (main / writer thread) extracts them strictly in order.
template <typename T>
class ReorderBuffer {
public:
    explicit ReorderBuffer(size_t capacity) : items_(capacity) {}

    void put(size_t index, T value) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            items_[index] = std::move(value);
            ready_.insert(index);
        }
        cv_.notify_all();
    }

    T take(size_t expected_index) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, expected_index] {
            return failure_ || ready_.count(expected_index) > 0;
        });
        if (failure_) std::rethrow_exception(failure_);
        T result = std::move(*items_[expected_index]);
        items_[expected_index].reset();
        ready_.erase(expected_index);
        return result;
    }

    void fail(std::exception_ptr error) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!failure_) failure_ = error;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::optional<T>> items_;
    std::unordered_set<size_t> ready_;
    std::exception_ptr failure_;
};

#endif // THREAD_POOL_HPP
