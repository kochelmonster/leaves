#ifndef _LEAVES_THREADPOOL_HPP
#define _LEAVES_THREADPOOL_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace leaves {

/**
 * @brief Thread pool mixin for storage classes
 *
 * Provides a shared thread pool that databases can use for background tasks
 * like LSM merges, compaction, etc.  Also includes a job scheduler for
 * delayed/periodic tasks via schedule_after() / cancel_job().
 *
 * Scheduled jobs are handled by the worker threads themselves — no
 * dedicated scheduler thread.  Workers sleep until the next scheduled
 * job is due (or until an immediate task arrives).
 *
 * Usage:
 *   struct MyStorage : _ThreadPoolMixin<MyStorage> {
 *     MyStorage() : _ThreadPoolMixin(4) {}  // 4 worker threads
 *   };
 *
 *   // Submit a task immediately:
 *   storage.submit_task([&]() { do_background_work(); });
 *
 *   // Schedule a task to run after a delay:
 *   auto id = storage.schedule_after(std::chrono::seconds(60), [&]() { ... });
 *   storage.cancel_job(id);  // cancel before it fires
 */
template <typename Derived>
struct _ThreadPoolMixin {
  using Task = std::function<void()>;

  // --- Scheduled job (min-heap by time) ---
  struct _ScheduledJob {
    uint64_t id;
    std::chrono::steady_clock::time_point when;
    Task task;
    bool operator>(const _ScheduledJob& o) const { return when > o.when; }
  };

  // Everything protected by _queue_mutex
  std::vector<std::thread> _workers;
  std::queue<Task> _task_queue;
  std::priority_queue<_ScheduledJob, std::vector<_ScheduledJob>,
                      std::greater<_ScheduledJob>> _sched_queue;
  std::mutex _queue_mutex;
  std::condition_variable _queue_cv;
  std::atomic<bool> _pool_shutdown{false};
  std::atomic<uint32_t> _active_tasks{0};
  std::atomic<uint64_t> _next_job_id{1};

  explicit _ThreadPoolMixin(size_t num_threads = 0) {
    if (num_threads == 0) {
      num_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    }
    start_pool(num_threads);
  }

  ~_ThreadPoolMixin() { stop_pool(); }

  // Non-copyable, non-movable
  _ThreadPoolMixin(const _ThreadPoolMixin&) = delete;
  _ThreadPoolMixin& operator=(const _ThreadPoolMixin&) = delete;
  _ThreadPoolMixin(_ThreadPoolMixin&&) = delete;
  _ThreadPoolMixin& operator=(_ThreadPoolMixin&&) = delete;

  void start_pool(size_t num_threads) {
    _pool_shutdown.store(false);
    _workers.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
      _workers.emplace_back([this]() { _worker_loop(); });
    }
  }

  void stop_pool() {
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      _pool_shutdown.store(true);
    }
    _queue_cv.notify_all();

    for (auto& worker : _workers) {
      if (worker.joinable()) worker.join();
    }
    _workers.clear();
  }

  /**
   * @brief Submit a task to the thread pool for immediate execution
   */
  void submit_task(Task task) {
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) return;
      _task_queue.push(std::move(task));
    }
    _queue_cv.notify_one();
  }

  /**
   * @brief Schedule a task to execute after a delay
   *
   * The task is picked up by a worker thread once the delay elapses.
   * Returns a job ID that can be passed to cancel_job().
   */
  template <typename Rep, typename Period>
  uint64_t schedule_after(std::chrono::duration<Rep, Period> delay, Task task) {
    uint64_t id = _next_job_id.fetch_add(1, std::memory_order_relaxed);
    auto when = std::chrono::steady_clock::now() + delay;
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) return id;
      _sched_queue.push({id, when, std::move(task)});
    }
    _queue_cv.notify_one();  // wake a worker to recalculate its deadline
    return id;
  }

  /**
   * @brief Cancel a scheduled job that has not yet fired
   *
   * If the job has already been picked up by a worker, this has no effect.
   * Rebuilds the queue without the cancelled job (O(n) but n < 10 typically).
   */
  void cancel_job(uint64_t id) {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    std::vector<_ScheduledJob> remaining;
    while (!_sched_queue.empty()) {
      auto job = std::move(const_cast<_ScheduledJob&>(_sched_queue.top()));
      _sched_queue.pop();
      if (job.id != id) {
        remaining.push_back(std::move(job));
      }
    }
    for (auto& job : remaining) {
      _sched_queue.push(std::move(job));
    }
  }

  /**
   * @brief Get the number of worker threads
   */
  size_t pool_size() const { return _workers.size(); }

  /**
   * @brief Get the number of pending tasks
   */
  size_t pending_tasks() const {
    std::lock_guard<std::mutex> lock(
        const_cast<std::mutex&>(_queue_mutex));
    return _task_queue.size();
  }

  /**
   * @brief Get the number of currently executing tasks
   */
  uint32_t active_tasks() const { return _active_tasks.load(); }

  /**
   * @brief Wait for all pending and active tasks to complete
   *
   * Waits until the immediate task queue is drained and no tasks are
   * executing.  Scheduled (delayed) jobs are NOT waited on — use
   * cancel_job() to cancel them first if needed.
   */
  void wait_all() {
    std::unique_lock<std::mutex> lock(_queue_mutex);
    _queue_cv.wait(lock, [this]() {
      _promote_scheduled_jobs();
      return _task_queue.empty() && _active_tasks.load() == 0;
    });
  }

  // Move due scheduled jobs into the immediate task queue.
  // Must be called with _queue_mutex held.
  void _promote_scheduled_jobs() {
    auto now = std::chrono::steady_clock::now();
    bool promoted = false;
    while (!_sched_queue.empty() && _sched_queue.top().when <= now) {
      auto& top = const_cast<_ScheduledJob&>(_sched_queue.top());
      _task_queue.push(std::move(top.task));
      _sched_queue.pop();
      promoted = true;
    }
    if (promoted) {
      _queue_cv.notify_all();
    }
  }

  void _worker_loop() {
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(_queue_mutex);

        while (true) {
          _promote_scheduled_jobs();

          if (!_task_queue.empty()) {
            task = std::move(_task_queue.front());
            _task_queue.pop();
            _active_tasks.fetch_add(1);
            break;
          }

          if (_pool_shutdown.load()) return;

          if (!_sched_queue.empty()) {
            // Sleep until the next scheduled job or a new event
            _queue_cv.wait_until(lock, _sched_queue.top().when);
          } else {
            // Nothing scheduled — sleep until a task arrives
            _queue_cv.wait(lock, [this]() {
              return _pool_shutdown.load() || !_task_queue.empty() ||
                     !_sched_queue.empty();
            });
          }

          if (_pool_shutdown.load() && _task_queue.empty()) return;
        }
      }

      // Execute task outside the lock
      task();

      {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _active_tasks.fetch_sub(1);
        if (!_task_queue.empty()) {
          _queue_cv.notify_one();  // Wake one worker for next queued task
        } else if (_active_tasks.load() == 0) {
          _queue_cv.notify_all();  // Pool idle — unblock wait_all()
        }
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_THREADPOOL_HPP
