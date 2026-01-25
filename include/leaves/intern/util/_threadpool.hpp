#ifndef _LEAVES_THREADPOOL_HPP
#define _LEAVES_THREADPOOL_HPP

#include <atomic>
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
 * like LSM merges, compaction, etc.
 *
 * Usage:
 *   struct MyStorage : _ThreadPoolMixin<MyStorage> {
 *     MyStorage() : _ThreadPoolMixin(4) {}  // 4 worker threads
 *   };
 *
 *   // Submit a task:
 *   storage.submit_task([&]() { do_background_work(); });
 */
template <typename Derived>
struct _ThreadPoolMixin {
  using Task = std::function<void()>;

  std::vector<std::thread> _workers;
  std::queue<Task> _task_queue;
  std::mutex _queue_mutex;
  std::condition_variable _queue_cv;
  std::atomic<bool> _pool_shutdown{false};
  std::atomic<uint32_t> _active_tasks{0};

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
      _workers.emplace_back([this]() { worker_loop(); });
    }
  }

  void stop_pool() {
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      _pool_shutdown.store(true);
    }
    _queue_cv.notify_all();

    for (auto& worker : _workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    _workers.clear();
  }

  /**
   * @brief Submit a task to the thread pool
   *
   * @param task The task to execute
   */
  void submit_task(Task task) {
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) {
        return;  // Don't accept new tasks during shutdown
      }
      _task_queue.push(std::move(task));
    }
    _queue_cv.notify_one();
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
   */
  void wait_all() {
    std::unique_lock<std::mutex> lock(_queue_mutex);
    _queue_cv.wait(lock, [this]() {
      return _task_queue.empty() && _active_tasks.load() == 0;
    });
  }

 private:
  void worker_loop() {
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        _queue_cv.wait(lock, [this]() {
          return _pool_shutdown.load() || !_task_queue.empty();
        });

        if (_pool_shutdown.load() && _task_queue.empty()) {
          return;
        }

        task = std::move(_task_queue.front());
        _task_queue.pop();
        _active_tasks.fetch_add(1);
      }

      // Execute task outside the lock
      task();

      _active_tasks.fetch_sub(1);
      _queue_cv.notify_all();  // Notify wait_all()
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_THREADPOOL_HPP
