/*
Browser-oriented threadpool helpers for asynchronous task execution.
*/
#ifndef _LEAVES_BROWSER_THREADPOOL_HPP
#define _LEAVES_BROWSER_THREADPOOL_HPP

#ifdef __EMSCRIPTEN__

extern "C" {
#include <emscripten.h>
#include <emscripten/eventloop.h>
}
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include "../util/_threadpool.hpp"

namespace leaves {

template <typename Derived>
struct _BrowserThreadPoolMixin {
    using PoolTask = _Task<64>;

    struct JobData {
        _BrowserThreadPoolMixin* self;
        PoolTask task;
        uint64_t job_id;
    };

    std::map<uint64_t, int> _scheduled_jobs;
    std::mutex _mutex;
    std::atomic<uint64_t> _next_job_id{1};
    std::atomic<bool> _pool_shutdown{false};

    explicit _BrowserThreadPoolMixin(size_t num_threads = 0) {}
    ~_BrowserThreadPoolMixin() { stop_pool(); }

    _BrowserThreadPoolMixin(const _BrowserThreadPoolMixin&) = delete;
    _BrowserThreadPoolMixin& operator=(const _BrowserThreadPoolMixin&) = delete;
    _BrowserThreadPoolMixin(_BrowserThreadPoolMixin&&) = delete;
    _BrowserThreadPoolMixin& operator=(_BrowserThreadPoolMixin&&) = delete;

    void start_pool(size_t num_threads) {}

    void stop_pool() {
        std::lock_guard<std::mutex> lock(_mutex);
        _pool_shutdown.store(true);
        for (auto const& [id, em_id] : _scheduled_jobs) {
            emscripten_clear_timeout(em_id);
        }
        _scheduled_jobs.clear();
    }

    template <typename F>
    void submit_task(F&& f) {
        if (_pool_shutdown.load()) return;
        // Execute immediately
        std::forward<F>(f)();
    }

    template <typename Rep, typename Period, typename F>
    uint64_t schedule_after(std::chrono::duration<Rep, Period> delay, F&& task) {
        if (_pool_shutdown.load()) return 0;

        uint64_t id = _next_job_id.fetch_add(1, std::memory_order_relaxed);
        auto* job_data = new JobData{this, PoolTask(std::forward<F>(task)), id};

        auto callback = [](void* userData) {
            JobData* data = static_cast<JobData*>(userData);
            _BrowserThreadPoolMixin* self = data->self;
            {
                std::lock_guard<std::mutex> lock(self->_mutex);
                self->_scheduled_jobs.erase(data->job_id);
            }
            data->task();
            delete data;
        };

        int em_id = emscripten_set_timeout(callback, std::chrono::duration_cast<std::chrono::milliseconds>(delay).count(), job_data);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _scheduled_jobs[id] = em_id;
        }

        return id;
    }

    void cancel_job(uint64_t id) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _scheduled_jobs.find(id);
        if (it != _scheduled_jobs.end()) {
            emscripten_clear_timeout(it->second);
            _scheduled_jobs.erase(it);
        }
    }

    size_t pool_size() const { return 0; }
    size_t pending_tasks() const { return 0; }
    uint32_t active_tasks() const { return 0; }
    void wait_idle() {}
    bool has_workers() const { return false; }
    static constexpr bool is_single_threaded() noexcept { return true; }
    size_t concurrency() const noexcept { return 1; }
};

} 

#endif 
#endif 
