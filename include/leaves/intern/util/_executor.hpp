#ifndef _LEAVES_EXECUTOR_HPP
#define _LEAVES_EXECUTOR_HPP

#include <cstddef>
#include <functional>

// Platform / threading detection
// Native threading (pthreads / win32 threads) is assumed available unless:
//  - Emscripten without pthread support
//  - Explicitly disabled by LEAVES_SINGLE_THREADED
#if defined(__EMSCRIPTEN__)
  #if defined(__EMSCRIPTEN_PTHREADS__)
    #define LEAVES_HAS_THREADS 1
  #else
    #define LEAVES_HAS_THREADS 0
  #endif
#elif defined(LEAVES_SINGLE_THREADED)
  #define LEAVES_HAS_THREADS 0
#else
  #define LEAVES_HAS_THREADS 1
#endif

namespace leaves {

// =========================================================================
// _InlineExecutor — runs all work synchronously on the calling thread
// =========================================================================

struct _InlineExecutor {
  static constexpr size_t concurrency() noexcept { return 1; }
  static constexpr bool is_single_threaded() noexcept { return true; }

  template <typename Fn>
  void post(Fn&& fn) { fn(); }
};

// =========================================================================
// _PoolExecutor — non-owning adapter wrapping any _ThreadPoolMixin
// =========================================================================

#if LEAVES_HAS_THREADS

struct _PoolExecutor {
  using Task = std::function<void()>;

  template <typename Pool>
  explicit _PoolExecutor(Pool& pool, size_t max_concurrency = 0)
      : _submit([&pool](Task task) { pool.submit_task(std::move(task)); }),
        _concurrency(max_concurrency > 0
                         ? std::min(pool.pool_size(), max_concurrency)
                         : pool.pool_size()) {}

  _PoolExecutor(const _PoolExecutor&) = default;
  _PoolExecutor& operator=(const _PoolExecutor&) = default;

  size_t concurrency() const noexcept { return _concurrency; }
  static constexpr bool is_single_threaded() noexcept { return false; }

  template <typename Fn>
  void post(Fn&& fn) { _submit(std::forward<Fn>(fn)); }

  std::function<void(Task)> _submit;
  size_t _concurrency;
};

#endif  // LEAVES_HAS_THREADS

// =========================================================================
// default_executor_t — alias for the platform-appropriate executor
// =========================================================================

#if LEAVES_HAS_THREADS
  using default_executor_t = _PoolExecutor;
#else
  using default_executor_t = _InlineExecutor;
#endif

}  // namespace leaves

#endif  // _LEAVES_EXECUTOR_HPP
