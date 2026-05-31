#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace shortcog {

// A small fixed-size thread pool. shortcog keeps GDAL's thread-safe machinery
// (GDAL_OF_THREAD_SAFE, the block cache, MayMultiBlockReadingBeMultiThreaded),
// but GDAL's own worker pool lives behind internal headers that are not
// installed, so an out-of-tree build cannot reach it. This is the
// self-contained replacement: one process-global pool, shared by every Image.
//
// Work is grouped into batches. A Batch is the analogue of GDAL's CPLJobQueue:
// several batches share the same workers, and wait() blocks only on the jobs
// of that batch, so concurrent reads never wait on each other. Nesting is not
// supported: a job running on a worker must not create a batch and wait on it,
// which would deadlock once every worker is blocked the same way. shortcog
// never does this; a read is planned and run from the calling thread, not from
// inside a worker.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    class Batch {
    public:
        explicit Batch(ThreadPool& pool) noexcept : pool_(pool) {}

        void submit(std::function<void()> job);
        void wait();

    private:
        ThreadPool&             pool_;
        std::mutex              mutex_;
        std::condition_variable done_;
        std::size_t             pending_{0};
    };

private:
    void enqueue(std::function<void()> job);

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           ready_;
    bool                              stop_{false};
};


inline ThreadPool::ThreadPool(unsigned threads)
{
    if (threads < 1) threads = 1;
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                    if (stop_ && jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop();
                }
                job();
            }
        });
    }
}

inline ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    ready_.notify_all();
    for (std::thread& w : workers_) {
        if (w.joinable()) w.join();
    }
}

inline void ThreadPool::enqueue(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        jobs_.push(std::move(job));
    }
    ready_.notify_one();
}

inline void ThreadPool::Batch::submit(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        ++pending_;
    }
    pool_.enqueue([this, job = std::move(job)]() mutable {
        job();
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) done_.notify_all();
    });
}

inline void ThreadPool::Batch::wait()
{
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
}


// Process-global pool, sized on first use, analogous to
// GDALGetGlobalThreadPool. The thread count from the first caller that builds
// it wins; later callers share it regardless of their own request.
inline ThreadPool& global_thread_pool(unsigned threads)
{
    static ThreadPool pool(threads);
    return pool;
}

}  // namespace shortcog