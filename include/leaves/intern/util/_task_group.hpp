#ifndef _LEAVES_TASK_GROUP_HPP
#define _LEAVES_TASK_GROUP_HPP

#include <exception>
#include <functional>
#include <utility>
#include <vector>

#include "_executor.hpp"

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
// Collect-expand-dispatch model:
//
// When Executor == _InlineExecutor, spawn() runs the callable inline and
// wait() just re-throws any captured exception.
//
// When Executor == _PoolExecutor, spawn() collects callables into a pending
// list. wait() expands the frontier BFS-style until >= concurrency items
// exist, then dispatches them to threads via a user-supplied dispatcher
// callback.
//
// Usage (inline — single threaded):
//   _TaskGroup<_InlineExecutor> tg(executor);
//   tg.spawn([&]{ do_work_a(); });
//   tg.spawn([&]{ do_work_b(); });
//   tg.wait();  // no-op, work already done
//
// Usage (pool — multithreaded, collect-expand-dispatch):
//   _TaskGroup<_PoolExecutor> tg(executor);
//   tg.spawn([&]{ process_branch_a(); });  // collected, not run
//   tg.spawn([&]{ process_branch_b(); });  // collected, not run
//   tg.wait([&](auto&& task) {             // expand + dispatch
//     executor.post(std::move(task));
//   });

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

// ── Specialisation: pool executor (collect-expand-dispatch) ──────────────

#if LEAVES_HAS_THREADS

template <>
struct _TaskGroup<_PoolExecutor> {
  using Task = std::function<void()>;

  _PoolExecutor& _executor;
  std::vector<Task> _pending;
  int _depth{0};
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

  // Collect a callable into the pending list (not executed yet).
  template <typename Fn>
  void spawn(Fn&& fn) {
    _pending.emplace_back(std::forward<Fn>(fn));
  }

  // Expand + run inline (no dispatcher — all work stays on calling thread).
  void wait() {
    _wait_impl(static_cast<std::function<void(Task&&)>*>(nullptr));
  }

  // Expand + dispatch to threads via the provided callback.
  template <typename Dispatcher>
  void wait(Dispatcher&& dispatcher) {
    std::function<void(Task&&)> d = std::forward<Dispatcher>(dispatcher);
    _wait_impl(&d);
  }

  size_t concurrency() const noexcept { return _executor.concurrency(); }

  void _wait_impl(std::function<void(Task&&)>* dispatcher) {
    _depth++;
    if (_depth > 1) {
      // Nested wait during BFS expansion — noop, let outer wait handle it.
      _depth--;
      return;
    }

    // BFS expand: run pending tasks inline; they may call spawn() to add
    // children, growing the frontier until >= concurrency.
    while (_pending.size() < concurrency()) {
      auto current = std::move(_pending);
      _pending.clear();
      if (current.empty()) break;
      for (auto& task : current) {
        task();
      }
      if (_pending.empty()) break;  // leaf work only, nothing spawned
    }

    if (dispatcher && _pending.size() > 1) {
      // Dispatch to real threads
      _outstanding.store(static_cast<int>(_pending.size()),
                         std::memory_order_release);
      for (auto& task : _pending) {
        (*dispatcher)([this, t = std::move(task)]() mutable {
          try {
            t();
          } catch (...) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_exception) _exception = std::current_exception();
          }
          if (_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            _cv.notify_all();
          }
        });
      }
      // Block until all dispatched tasks complete
      std::unique_lock<std::mutex> lock(_mutex);
      _cv.wait(lock, [this]() {
        return _outstanding.load(std::memory_order_acquire) == 0;
      });
    } else {
      // Run inline (single task or no dispatcher)
      for (auto& task : _pending) {
        try {
          task();
        } catch (...) {
          if (!_exception) _exception = std::current_exception();
        }
      }
    }

    _pending.clear();
    _depth--;

    if (_exception) {
      auto ex = _exception;
      _exception = nullptr;
      std::rethrow_exception(ex);
    }
  }
};

#endif  // LEAVES_HAS_THREADS

}  // namespace leaves

#endif  // _LEAVES_TASK_GROUP_HPP
