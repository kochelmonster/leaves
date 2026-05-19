#ifndef _LEAVES_THREADPOOL_HPP
#define _LEAVES_THREADPOOL_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _MSC_VER
#  include <intrin.h>
#endif

namespace leaves {

// ============================================================================
// _InplaceTask — heap-free callable wrapper for trivially-copyable functors.
//
// Stores up to N bytes of functor data inline; avoids the heap allocation that
// std::function incurs when the capture list exceeds its small-buffer limit
// (~16–24 bytes depending on the implementation).  Requires the functor to be
// trivially copyable — satisfied by any lambda that captures only pointers,
// references, or plain-old-data values.  Move transfers ownership via memcpy
// (safe because trivially-copyable types are trivially relocatable).
// ============================================================================
template <size_t N = 64>
struct _InplaceTask {
  alignas(alignof(std::max_align_t)) char _buf[N]{};
  void (*_invoke)(void*){nullptr};
  void (*_destroy)(void*){nullptr};

  _InplaceTask() = default;

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, _InplaceTask>>>
  _InplaceTask(F&& f) {
    using DecF = std::decay_t<F>;
    static_assert(sizeof(DecF) <= N,
        "_InplaceTask: functor too large; increase N or use std::function");
    static_assert(std::is_trivially_copyable_v<DecF>,
        "_InplaceTask: functor must be trivially copyable "
        "(captures must be pointers, references, or POD values)");
    ::new (static_cast<void*>(_buf)) DecF(std::forward<F>(f));
    _invoke  = [](void* p) { (*static_cast<DecF*>(p))(); };
    _destroy = [](void* p) {  static_cast<DecF*>(p)->~DecF(); };
  }

  ~_InplaceTask() { if (_destroy) _destroy(_buf); }

  _InplaceTask(const _InplaceTask&) = delete;
  _InplaceTask& operator=(const _InplaceTask&) = delete;

  _InplaceTask(_InplaceTask&& o) noexcept
      : _invoke(o._invoke), _destroy(o._destroy) {
    if (_invoke) {
      std::memcpy(_buf, o._buf, N);
      o._invoke  = nullptr;
      o._destroy = nullptr;
    }
  }

  _InplaceTask& operator=(_InplaceTask&& o) noexcept {
    if (this != &o) {
      if (_destroy) _destroy(_buf);
      _invoke  = o._invoke;
      _destroy = o._destroy;
      if (_invoke) {
        std::memcpy(_buf, o._buf, N);
        o._invoke  = nullptr;
        o._destroy = nullptr;
      }
    }
    return *this;
  }

  void operator()() { _invoke(_buf); }
  explicit operator bool() const noexcept { return _invoke != nullptr; }
};

// One CPU-pipeline relaxation instruction to reduce contention during spin-waits.
// Avoids saturating the memory bus and improves SMT partner throughput.
static inline void _pool_cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ volatile("yield" ::: "memory");
#endif
}

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
// Tag type for constructing _ThreadPoolMixin without starting workers.
// Workers are lazy-started on the first schedule_after() call.
struct _lazy_pool_t {};
static constexpr _lazy_pool_t _lazy_pool{};

template <typename Derived>
struct _ThreadPoolMixin {
  using Task    = std::function<void()>;  // used by schedule_after / ScheduledJob
  using ImmTask = _InplaceTask<64>;       // used by submit_task (no heap allocation)

  // --- Scheduled job (min-heap by time) ---
  struct _ScheduledJob {
    uint64_t id;
    std::chrono::steady_clock::time_point when;
    Task task;
    bool operator>(const _ScheduledJob& o) const { return when > o.when; }
  };

  // Number of spin iterations before falling back to condvar sleep.
  // At ~5 ns per pause instruction (x86) this covers ~1 µs — enough to
  // catch tasks submitted during a concurrent main-thread operation.
  static constexpr int _IMM_SPIN_ITERS = 30000;  // ~150 µs spin window

  // Everything protected by _queue_mutex
  std::vector<std::thread> _workers;
  std::queue<Task>    _task_queue;   // general tasks (std::function — scheduled jobs land here)
  std::queue<ImmTask> _imm_queue;    // immediate tasks (ImmTask — no heap allocation)
  std::priority_queue<_ScheduledJob, std::vector<_ScheduledJob>,
                      std::greater<_ScheduledJob>> _sched_queue;
  std::mutex _queue_mutex;
  std::condition_variable _queue_cv;
  std::atomic<bool>     _pool_shutdown{false};
  std::atomic<uint32_t> _active_tasks{0};
  std::atomic<uint64_t> _next_job_id{1};
  // Mirrors _imm_queue.size(); readable without the mutex so workers can
  // spin cheaply before falling back to condvar sleep.
  std::atomic<uint32_t> _imm_count{0};
  // Number of workers currently blocked in condvar.wait().
  // submit_imm checks this to skip the futex notify_one() when all
  // workers are already spinning (avoiding unnecessary syscall overhead).
  std::atomic<uint32_t> _sleeping_count{0};

  // Lazy mode: no workers started, pool auto-starts on first schedule_after()
  explicit _ThreadPoolMixin(_lazy_pool_t) {}

  explicit _ThreadPoolMixin(size_t num_threads = 0) {
    if (num_threads == 0) {
#ifndef __EMSCRIPTEN__
      num_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
#else
      return;  // no threads in WASM
#endif
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

  // Ensure the pool has at least `n` workers; starts additional threads if needed.
  void ensure_pool(size_t n) {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    if (_workers.size() < n) {
      _pool_shutdown.store(false);
      size_t to_add = n - _workers.size();
      _workers.reserve(n);
      for (size_t i = 0; i < to_add; ++i) {
        _workers.emplace_back([this]() { _worker_loop(); });
      }
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
   * @brief Submit a task to the thread pool for immediate execution (legacy path).
   *
   * Accepts any callable including std::function.  Prefer submit_imm() for
   * hot paths where the functor is a small trivially-copyable lambda.
   */
  void submit_task(Task task) {
    if (_workers.empty()) {
      task();  // execute inline when no thread pool
      return;
    }
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) return;
      _task_queue.push(std::move(task));
    }
    _queue_cv.notify_one();
  }

  /**
   * @brief Submit an immediate task with no heap allocation.
   *
   * The functor must be trivially copyable and fit in 64 bytes
   * (captures limited to pointers, references, and small POD values).
   * Workers spin on _imm_count before sleeping, so tasks are picked up
   * in ~5–50 ns when workers are active rather than ~1–10 µs condvar wake-up.
   */
  template <typename F>
  void submit_imm(F&& f) {
    if (_workers.empty()) {
      f();  // execute inline when no thread pool
      return;
    }
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) return;
      _imm_queue.emplace(std::forward<F>(f));
    }
    _imm_count.fetch_add(1, std::memory_order_release);
    // Only pay the futex syscall if there are actually sleeping workers.
    // Spinning workers discover the task via _imm_count without any syscall.
    if (_sleeping_count.load(std::memory_order_relaxed) > 0)
      _queue_cv.notify_one();
  }

  bool has_workers() const { return !_workers.empty(); }

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
      // Lazy-start: ensure at least 1 worker for scheduled jobs
      if (_workers.empty()) {
        _workers.reserve(1);
        _workers.emplace_back([this]() { _worker_loop(); });
      }
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
    return _task_queue.size() + _imm_queue.size();
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
      return _task_queue.empty() && _imm_queue.empty() && _active_tasks.load() == 0;
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
      bool got = false;

      // --- Fast path: spin on _imm_count for low-latency immediate tasks. ---
      // Workers that spun recently will pick up the next task in ~5–50 ns
      // instead of the ~1–10 µs it takes to wake from pthread_cond_wait.
      // The atomic check is cheap (~1 ns); the mutex is only acquired when
      // _imm_count > 0, so idle workers burn no CPU in the spin phase.
      for (int s = 0; s < _IMM_SPIN_ITERS && !got; ++s) {
        if (_imm_count.load(std::memory_order_acquire) > 0) {
          ImmTask imm;
          // try_lock: only ONE spinning worker acquires the mutex at a time;
          // others back off and retry, eliminating thundering-herd contention.
          if (_queue_mutex.try_lock()) {
            if (!_imm_queue.empty()) {
              imm = std::move(_imm_queue.front());
              _imm_queue.pop();
              _imm_count.fetch_sub(1, std::memory_order_relaxed);
              _active_tasks.fetch_add(1);
              got = true;
            }
            _queue_mutex.unlock();
          }
          if (got) {
            imm();
            {
              std::lock_guard<std::mutex> lock(_queue_mutex);
              _active_tasks.fetch_sub(1);
              if (!_task_queue.empty() || !_imm_queue.empty()) {
                _queue_cv.notify_one();
              } else if (_active_tasks.load() == 0) {
                _queue_cv.notify_all();  // unblock wait_all()
              }
            }
            break;  // restart outer loop (spin again for next task)
          }
        }
        if (_pool_shutdown.load()) return;
        _pool_cpu_relax();
      }
      if (got) continue;

      // --- Slow path: condvar sleep for general tasks and scheduled jobs. ---
      Task task;
      {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        while (true) {
          _promote_scheduled_jobs();

          // Check imm_queue first (may have arrived while we were falling back)
          if (!_imm_queue.empty()) {
            ImmTask imm = std::move(_imm_queue.front());
            _imm_queue.pop();
            _imm_count.fetch_sub(1, std::memory_order_relaxed);
            _active_tasks.fetch_add(1);
            lock.unlock();
            imm();
            lock.lock();
            _active_tasks.fetch_sub(1);
            if (!_task_queue.empty() || !_imm_queue.empty()) {
              _queue_cv.notify_one();
            } else if (_active_tasks.load() == 0) {
              _queue_cv.notify_all();
            }
            break;
          }

          if (!_task_queue.empty()) {
            task = std::move(_task_queue.front());
            _task_queue.pop();
            _active_tasks.fetch_add(1);
            got = true;
            break;
          }

          if (_pool_shutdown.load()) return;

          if (!_sched_queue.empty()) {
            auto next_when = _sched_queue.top().when;
            _sleeping_count.fetch_add(1, std::memory_order_relaxed);
            _queue_cv.wait_until(lock, next_when);
            _sleeping_count.fetch_sub(1, std::memory_order_relaxed);
          } else {
            _sleeping_count.fetch_add(1, std::memory_order_relaxed);
            _queue_cv.wait(lock, [this]() {
              return _pool_shutdown.load() || !_task_queue.empty() ||
                     !_sched_queue.empty() || !_imm_queue.empty();
            });
            _sleeping_count.fetch_sub(1, std::memory_order_relaxed);
          }

          if (_pool_shutdown.load() && _task_queue.empty() && _imm_queue.empty()) return;
        }
      }

      if (got) {
        task();
        {
          std::lock_guard<std::mutex> lock(_queue_mutex);
          _active_tasks.fetch_sub(1);
          if (!_task_queue.empty() || !_imm_queue.empty()) {
            _queue_cv.notify_one();
          } else if (_active_tasks.load() == 0) {
            _queue_cv.notify_all();  // Pool idle — unblock wait_all()
          }
        }
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_THREADPOOL_HPP
