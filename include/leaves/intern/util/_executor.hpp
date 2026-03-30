#ifndef _LEAVES_EXECUTOR_HPP
#define _LEAVES_EXECUTOR_HPP

#include <cstddef>
#include <functional>

#include "../core/_port.hpp"

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
