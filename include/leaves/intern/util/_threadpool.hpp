/*
Internal worker-thread pool and task queue primitives.
*/
#ifndef _LEAVES_THREADPOOL_HPP
#define _LEAVES_THREADPOOL_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  include <intrin.h>
#endif

#include "../core/_port.hpp"

namespace leaves {

// _Task<N> — heap-free move-only callable wrapper.
//
// Stores up to N bytes of functor data inline; avoids heap allocation.
// Move-only — copy is deleted.  Supports any move-constructible functor,
// including lambdas that capture non-trivially-copyable types (smart
// pointers, std::string, etc.).
template <size_t N = 96>
struct _Task {
  using _invoke_fn  = void (*)(void*);
  using _destroy_fn = void (*)(void*);
  using _move_fn    = void (*)(void*, void*);  // move-construct dst from src, destroy src

  alignas(alignof(std::max_align_t)) char _buf[N]{};
  _invoke_fn  _invoke{nullptr};
  _destroy_fn _destroy{nullptr};
  _move_fn    _move_op{nullptr};

  _Task() = default;

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, _Task>>>
  _Task(F&& f) noexcept(std::is_nothrow_move_constructible_v<std::decay_t<F>>) {
    using DecF = std::decay_t<F>;
    static_assert(sizeof(DecF) <= N,
        "_Task: functor too large; increase N");
    static_assert(alignof(DecF) <= alignof(std::max_align_t),
        "_Task: functor alignment exceeds buffer alignment");
    ::new (static_cast<void*>(_buf)) DecF(std::forward<F>(f));
    _invoke  = [](void* p) { (*static_cast<DecF*>(p))(); };
    _destroy = [](void* p) { static_cast<DecF*>(p)->~DecF(); };
    _move_op = [](void* dst, void* src) {
      ::new (dst) DecF(std::move(*static_cast<DecF*>(src)));
      static_cast<DecF*>(src)->~DecF();
    };
  }

  ~_Task() { if (_destroy) _destroy(_buf); }

  _Task(const _Task&) = delete;
  _Task& operator=(const _Task&) = delete;

  _Task(_Task&& o) noexcept
      : _invoke(o._invoke), _destroy(o._destroy), _move_op(o._move_op) {
    if (_move_op) {
      _move_op(_buf, o._buf);
      o._invoke  = nullptr;
      o._destroy = nullptr;
      o._move_op = nullptr;
    }
  }

  _Task& operator=(_Task&& o) noexcept {
    if (this != &o) {
      if (_destroy) _destroy(_buf);
      _invoke  = o._invoke;
      _destroy = o._destroy;
      _move_op = o._move_op;
      if (_move_op) {
        _move_op(_buf, o._buf);
        o._invoke  = nullptr;
        o._destroy = nullptr;
        o._move_op = nullptr;
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
  using PoolTask = _Task<64>;

  // --- Scheduled job (min-heap by time) ---
  struct _ScheduledJob {
    uint64_t id;
    std::chrono::steady_clock::time_point when;
    PoolTask task;
    bool operator>(const _ScheduledJob& o) const { return when > o.when; }
  };

  // Number of spin iterations before falling back to condvar sleep.
  // At ~5 ns per pause instruction (x86) this covers ~1 µs — enough to
  // catch tasks submitted during a concurrent main-thread operation.
  static constexpr int _SPIN_ITERS = 30000;  // ~150 µs spin window

  // Everything protected by _queue_mutex
  std::vector<std::thread> _workers;
  std::queue<PoolTask> _task_queue;    // all pending tasks (immediate and promoted scheduled)
  std::priority_queue<_ScheduledJob, std::vector<_ScheduledJob>,
                      std::greater<_ScheduledJob>> _sched_queue;
  std::mutex _queue_mutex;
  std::condition_variable _queue_cv;   // workers waiting for tasks / scheduled jobs
  std::condition_variable _idle_cv;    // wait_idle() waiting for pool to go idle
  std::atomic<bool>     _pool_shutdown{false};
  std::atomic<uint32_t> _active_tasks{0};
  std::atomic<uint64_t> _next_job_id{1};
  // Mirrors _task_queue.size(); readable without the mutex so workers can
  // spin cheaply before falling back to condvar sleep.
  std::atomic<uint32_t> _task_count{0};
  // Number of workers currently blocked in condvar.wait().
  // submit_task checks this to skip the futex notify_one() when all
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
   * @brief Submit a task for immediate execution with no heap allocation.
   *
   * The functor must be trivially copyable and fit in 64 bytes
   * (captures limited to pointers, references, and small POD values).
   * Workers spin on _task_count before sleeping, so tasks are picked up
   * in ~5–50 ns when workers are active rather than ~1–10 µs condvar wake-up.
   */
  template <typename F>
  void submit_task(F&& f) {
    if (_workers.empty()) {
      f();  // execute inline when no thread pool
      return;
    }
    {
      std::lock_guard<std::mutex> lock(_queue_mutex);
      if (_pool_shutdown.load()) return;
      _task_queue.emplace(std::forward<F>(f));
    }
    _task_count.fetch_add(1, std::memory_order_release);
    // Only pay the futex syscall if there are actually sleeping workers.
    // Spinning workers discover the task via _task_count without any syscall.
    if (_sleeping_count.load(std::memory_order_relaxed) > 0)
      _queue_cv.notify_one();
  }

  bool has_workers() const { return !_workers.empty(); }
  static constexpr bool is_single_threaded() noexcept { return false; }
  size_t concurrency() const noexcept { return _workers.size(); }

  /**
   * @brief Schedule a task to execute after a delay
   *
   * The task is picked up by a worker thread once the delay elapses.
   * Returns a job ID that can be passed to cancel_job().
   * The functor must be trivially copyable and fit in 64 bytes (same
   * constraint as submit_task()).
   */
  template <typename Rep, typename Period, typename F>
  uint64_t schedule_after(std::chrono::duration<Rep, Period> delay, F&& task) {
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
      _sched_queue.push({id, when, PoolTask(std::forward<F>(task))});
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
  void wait_idle() {
    std::unique_lock<std::mutex> lock(_queue_mutex);
    _idle_cv.wait(lock, [this]() {
      _promote_scheduled_jobs();
      return _task_queue.empty() && _active_tasks.load() == 0;
    });
  }

  // Move due scheduled jobs into the task queue.
  // Must be called with _queue_mutex held.
  void _promote_scheduled_jobs() {
    auto now = std::chrono::steady_clock::now();
    bool promoted = false;
    while (!_sched_queue.empty() && _sched_queue.top().when <= now) {
      auto& top = const_cast<_ScheduledJob&>(_sched_queue.top());
      _task_queue.push(std::move(top.task));
      _task_count.fetch_add(1, std::memory_order_release);
      _sched_queue.pop();
      promoted = true;
    }
    if (promoted) {
      _queue_cv.notify_all();
    }
  }

  // Dequeue the front task and mark it active.
  // Must be called with _queue_mutex held. Returns false if queue is empty.
  bool _try_dequeue(PoolTask& out) {
    if (_task_queue.empty()) return false;
    out = std::move(_task_queue.front());
    _task_queue.pop();
    _task_count.fetch_sub(1, std::memory_order_relaxed);
    _active_tasks.fetch_add(1);
    return true;
  }

  // Mark a task as completed and notify waiters.
  // Must be called with _queue_mutex held.
  void _complete_task() {
    _active_tasks.fetch_sub(1);
    if (!_task_queue.empty())
      _queue_cv.notify_one();
    else if (_active_tasks.load() == 0)
      _idle_cv.notify_all();
  }

  void _worker_loop() {
    while (true) {
      bool got = false;

      // --- Fast path: spin on _task_count for low-latency immediate tasks. ---
      // Workers that spun recently will pick up the next task in ~5–50 ns
      // instead of the ~1–10 µs it takes to wake from pthread_cond_wait.
      // The atomic check is cheap (~1 ns); the mutex is only acquired when
      // _task_count > 0, so idle workers burn no CPU in the spin phase.
      for (int s = 0; s < _SPIN_ITERS; ++s) {
        if (_task_count.load(std::memory_order_acquire) > 0) {
          PoolTask task;
          // try_lock: only ONE spinning worker acquires the mutex at a time;
          // others back off and retry, eliminating thundering-herd contention.
          if (_queue_mutex.try_lock()) {
            got = _try_dequeue(task);
            _queue_mutex.unlock();
          }
          if (got) {
            task();
            {
              std::lock_guard<std::mutex> lock(_queue_mutex);
              _complete_task();
            }
            break;  // restart outer loop (spin again for next task)
          }
        }
        if (_pool_shutdown.load()) return;
        _pool_cpu_relax();
      }
      if (got) continue;

      // --- Slow path: condvar sleep for scheduled jobs. ---
      {
        std::unique_lock<std::mutex> lock(_queue_mutex);
        while (true) {
          _promote_scheduled_jobs();

          PoolTask task;
          if (_try_dequeue(task)) {
            lock.unlock();
            task();
            lock.lock();
            _complete_task();
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
                     !_sched_queue.empty();
            });
            _sleeping_count.fetch_sub(1, std::memory_order_relaxed);
          }

          if (_pool_shutdown.load() && _task_queue.empty()) return;
        }
      }
    }
  }
};

// _InlineExecutor — runs all work synchronously on the calling thread

struct _InlineExecutor {
  static constexpr size_t concurrency() noexcept { return 1; }
  static constexpr bool is_single_threaded() noexcept { return true; }
  template <typename Fn>
  void post(Fn&& fn) { std::forward<Fn>(fn)(); }
};

// _TaskGroup<Executor> — structured concurrency: spawn tasks + wait
//
// Two specialisations keyed on Executor::is_single_threaded():
//
//   true  → spawn() runs the callable inline; wait() just re-throws.
//   false → BFS work-stealing: spawn() collects callables into _current.
//           wait() expands batches inline until count >= concurrency(),
//           then dispatches to the pool and blocks.
//
// Usage (inline):
//   _InlineExecutor exec;
//   _TaskGroup<_InlineExecutor> tg(exec);
//   tg.spawn([&]{ do_work(); });
//   tg.wait();
//
// Usage (pool — pass storage or any _ThreadPoolMixin-derived type):
//   _TaskGroup<MyStorage> tg(storage, max_threads);
//   tg.spawn([&]{ process_branch(); });
//   tg.wait();

#if LEAVES_HAS_THREADS
inline thread_local bool _in_worker = false;
#endif

template <typename Executor, bool = Executor::is_single_threaded()>
struct _TaskGroup;

// ── Specialisation: single-threaded executor ─────────────────────────────────

template <typename Executor>
struct _TaskGroup<Executor, true> {
  Executor& _executor;
  std::exception_ptr _exception;

  explicit _TaskGroup(Executor& exec, size_t /*cap*/ = 0) : _executor(exec) {}

  _TaskGroup(const _TaskGroup&) = delete;
  _TaskGroup& operator=(const _TaskGroup&) = delete;

  template <typename Fn>
  void spawn(Fn&& fn) {
    if (_exception) return;
    try {
      std::forward<Fn>(fn)();
    } catch (...) {
      _exception = std::current_exception();
    }
  }

  void wait() { _rethrow(); }

  template <typename Dispatcher>
  void wait(Dispatcher&&) { _rethrow(); }

  static constexpr size_t concurrency() noexcept { return 1; }

  void _rethrow() {
    if (_exception) {
      auto ex = _exception;
      _exception = nullptr;
      std::rethrow_exception(ex);
    }
  }
};

// ── Specialisation: pool executor (reentrant work-stealing BFS) ──────────────

#if LEAVES_HAS_THREADS

template <typename Executor>
struct _TaskGroup<Executor, false> {
  using Task = _Task<96>;

  Executor& _executor;
  std::vector<Task> _current;   // tasks collected by spawn()
  std::vector<Task> _batch;     // current level being expanded
  size_t _batch_idx{0};         // next unprocessed task in _batch
  size_t _concurrency{0};       // dispatch threshold
  std::atomic<int> _outstanding{0};
  std::mutex _mutex;
  std::condition_variable _cv;
  std::exception_ptr _exception;

  explicit _TaskGroup(Executor& exec, size_t cap = 0)
      : _executor(exec),
        _concurrency(cap > 0
                         ? std::min(exec.pool_size(), cap)
                         : exec.pool_size()) {}

  _TaskGroup(const _TaskGroup&) = delete;
  _TaskGroup& operator=(const _TaskGroup&) = delete;

  ~_TaskGroup() {
    try { wait(); } catch (...) {}
  }

  template <typename Fn>
  void spawn(Fn&& fn) {
    if (_in_worker) {
      std::forward<Fn>(fn)();
    } else {
      _current.emplace_back(std::forward<Fn>(fn));
    }
  }

  void wait() {
    if (_in_worker) return;
    _wait_impl();
  }

  template <typename Dispatcher>
  void wait(Dispatcher&&) {
    if (_in_worker) return;
    _wait_impl();
  }

  size_t concurrency() const noexcept { return _concurrency; }

  void _wait_impl() {
    if (!_batch.empty()) {
      _steal_loop();
    }

    for (;;) {
      if (_current.empty()) break;

      if (_current.size() >= concurrency()) {
        _dispatch();
        break;
      }

      // Expand: move _current → _batch, steal and run inline.
      _batch = std::move(_current);
      _current.clear();
      _batch_idx = 0;
      _steal_loop();
      _batch.clear();
    }

    if (_exception) {
      auto ex = _exception;
      _exception = nullptr;
      std::rethrow_exception(ex);
    }
  }

  void _steal_loop() {
    while (_batch_idx < _batch.size()) {
      auto task = std::move(_batch[_batch_idx++]);
      try {
        task();
      } catch (...) {
        if (!_exception) _exception = std::current_exception();
      }
    }
  }

  void _dispatch() {
    _outstanding.store(static_cast<int>(_current.size()),
                       std::memory_order_release);
    for (size_t i = 0; i < _current.size(); ++i) {
      _executor.submit_task([this, i]() {
        _in_worker = true;
        try {
          _current[i]();
        } catch (...) {
          std::lock_guard<std::mutex> lock(_mutex);
          if (!_exception) _exception = std::current_exception();
        }
        _in_worker = false;
        if (_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          _cv.notify_all();
        }
      });
    }
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this]() {
      return _outstanding.load(std::memory_order_acquire) == 0;
    });
    _current.clear();
  }
};

#endif  // LEAVES_HAS_THREADS

}  // namespace leaves

#endif  // _LEAVES_THREADPOOL_HPP
