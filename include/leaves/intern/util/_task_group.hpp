#ifndef _LEAVES_TASK_GROUP_HPP
#define _LEAVES_TASK_GROUP_HPP

#include <exception>
#include <functional>
#include <utility>
#include <vector>

#include "_executor.hpp"
#include "_small_function.hpp"

#if LEAVES_HAS_THREADS
  #include <atomic>
  #include <condition_variable>
  #include <mutex>
#endif

namespace leaves {

// =========================================================================
// _TaskGroup — structured concurrency: spawn tasks + wait for all
// =========================================================================
//
// Reentrant work-stealing BFS expansion:
//
// When Executor == _InlineExecutor, spawn() runs the callable inline and
// wait() just re-throws any captured exception.
//
// When Executor == _PoolExecutor, spawn() collects callables into _current.
// wait() is reentrant and acts as a FSM:
//   - Move _current → _batch, steal and run tasks from _batch inline.
//   - Each task may spawn() children and call wait() reentrantly.
//   - Reentrant wait() steals remaining siblings from the same _batch.
//   - When _batch is exhausted, check _current (next generation):
//     - Empty → return (leaf level)
//     - >= concurrency → dispatch to pool, block until done, return
//     - < concurrency → expand: move _current → _batch, loop again
//   - When dispatch completes or batch is exhausted, wait() returns.
//     The caller's stack frame (with offsets_buf etc.) is preserved.
//
// On worker threads (detected via thread_local _in_worker), spawn()
// runs immediately — workers run single-threaded depth-first.
//
// Usage (inline — single threaded):
//   _TaskGroup<_InlineExecutor> tg(executor);
//   tg.spawn([&]{ do_work_a(); });
//   tg.spawn([&]{ do_work_b(); });
//   tg.wait();  // no-op, work already done
//
// Usage (pool — multithreaded, reentrant BFS):
//   _TaskGroup<_PoolExecutor> tg(executor);
//   tg.spawn([&]{ process_branch_a(); });  // collected, not run
//   tg.spawn([&]{ process_branch_b(); });  // collected, not run
//   tg.wait();                              // BFS expand + dispatch

#if LEAVES_HAS_THREADS
inline thread_local bool _in_worker = false;
#endif

template <typename Executor>
struct _TaskGroup;

// ── Specialisation: inline executor (single-threaded) ────────────────────

template <>
struct _TaskGroup<_InlineExecutor> {
  _InlineExecutor& _executor;
  std::exception_ptr _exception;

  explicit _TaskGroup(_InlineExecutor& exec) : _executor(exec) {}

  _TaskGroup(const _TaskGroup&) = delete;
  _TaskGroup& operator=(const _TaskGroup&) = delete;

  template <typename Fn>
  void spawn(Fn&& fn) {
    if (_exception) return;
    try {
      fn();
    } catch (...) {
      _exception = std::current_exception();
    }
  }

  void wait() {
    _rethrow();
  }

  template <typename Dispatcher>
  void wait(Dispatcher&&) {
    _rethrow();
  }

  static constexpr size_t concurrency() noexcept { return 1; }

  void _rethrow() {
    if (_exception) {
      auto ex = _exception;
      _exception = nullptr;
      std::rethrow_exception(ex);
    }
  }
};

// ── Specialisation: pool executor (reentrant work-stealing BFS) ──────────

#if LEAVES_HAS_THREADS

template <>
struct _TaskGroup<_PoolExecutor> {
  using Task = _SmallFunction<void()>;

  _PoolExecutor& _executor;
  std::vector<Task> _current;  // tasks collected by spawn()
  std::vector<Task> _batch;    // current level being expanded
  size_t _batch_idx{0};        // next unprocessed task in _batch
  size_t _concurrency{0};      // dispatch threshold; 0 = executor.concurrency()
  std::atomic<int> _outstanding{0};
  std::mutex _mutex;
  std::condition_variable _cv;
  std::exception_ptr _exception;

  explicit _TaskGroup(_PoolExecutor& exec) : _executor(exec) {}

  _TaskGroup(const _TaskGroup&) = delete;
  _TaskGroup& operator=(const _TaskGroup&) = delete;

  ~_TaskGroup() {
    try { wait(); } catch (...) {}
  }

  // Collect a callable. On worker threads, runs immediately.
  template <typename Fn>
  void spawn(Fn&& fn) {
    if (_in_worker) {
      fn();
    } else {
      _current.emplace_back(std::forward<Fn>(fn));
    }
  }

  void wait() {
    if (_in_worker) return;  // all work already done inline by spawn()
    _wait_impl();
  }

  template <typename Dispatcher>
  void wait(Dispatcher&&) {
    if (_in_worker) return;
    _wait_impl();
  }

  size_t concurrency() const noexcept {
    return _concurrency ? _concurrency : _executor.concurrency();
  }

  void _wait_impl() {
    // If _batch is active, we're reentrant — steal remaining siblings.
    // Then fall through to BFS expansion for children spawned by stolen tasks.
    if (!_batch.empty()) {
      _steal_loop();
    }

    // BFS expansion — works at any nesting level.
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

      // After expansion, _current has the next generation (if any).
      // Loop to check size again.
    }

    if (_exception) {
      auto ex = _exception;
      _exception = nullptr;
      std::rethrow_exception(ex);
    }
  }

  // Run remaining tasks from _batch starting at _batch_idx.
  // Each task may call spawn() (populating _current) and wait() (reentrant).
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

  // Dispatch _current to pool threads and block.
  // Posts index-based lambdas (16 bytes) that fit std::function SBO —
  // zero heap allocs. Workers call _current[i]() directly; _current
  // stays alive until all workers finish.
  void _dispatch() {
    _outstanding.store(static_cast<int>(_current.size()),
                       std::memory_order_release);
    for (size_t i = 0; i < _current.size(); ++i) {
      _executor.post([this, i]() {
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

#endif  // _LEAVES_TASK_GROUP_HPP
