#ifndef _LEAVES_SMALL_FUNCTION_HPP
#define _LEAVES_SMALL_FUNCTION_HPP

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace leaves {

// =========================================================================
// _SmallFunction — move-only type-erased callable with inline storage
// =========================================================================
//
// Fixed-size inline buffer, no heap allocation. static_assert if the
// callable exceeds BufferSize. Move-only (no copy).

template <typename Signature, size_t BufferSize = 96>
struct _SmallFunction;

template <size_t BufferSize, typename R, typename... Args>
struct _SmallFunction<R(Args...), BufferSize> {
  using _invoke_fn = R (*)(void*, Args...);
  using _destroy_fn = void (*)(void*);
  using _move_fn = void (*)(void*, void*);

  alignas(8) char _buf[BufferSize];
  _invoke_fn _invoke;
  _destroy_fn _destroy;
  _move_fn _move;

  _SmallFunction() noexcept : _invoke(nullptr), _destroy(nullptr), _move(nullptr) {}

  template <typename Fn,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, _SmallFunction>>>
  _SmallFunction(Fn&& fn) noexcept(std::is_nothrow_move_constructible_v<std::decay_t<Fn>>) {
    using F = std::decay_t<Fn>;
    static_assert(sizeof(F) <= BufferSize,
                  "_SmallFunction: callable exceeds inline buffer size");
    static_assert(alignof(F) <= 8,
                  "_SmallFunction: callable alignment exceeds buffer alignment");
    new (_buf) F(std::forward<Fn>(fn));
    _invoke = [](void* buf, Args... args) -> R {
      return (*static_cast<F*>(buf))(std::forward<Args>(args)...);
    };
    _destroy = [](void* buf) { static_cast<F*>(buf)->~F(); };
    _move = [](void* dst, void* src) {
      new (dst) F(std::move(*static_cast<F*>(src)));
      static_cast<F*>(src)->~F();
    };
  }

  ~_SmallFunction() {
    if (_destroy) _destroy(_buf);
  }

  // Move-only
  _SmallFunction(const _SmallFunction&) = delete;
  _SmallFunction& operator=(const _SmallFunction&) = delete;

  _SmallFunction(_SmallFunction&& other) noexcept
      : _invoke(other._invoke), _destroy(other._destroy), _move(other._move) {
    if (_move) {
      _move(_buf, other._buf);
      other._invoke = nullptr;
      other._destroy = nullptr;
      other._move = nullptr;
    }
  }

  _SmallFunction& operator=(_SmallFunction&& other) noexcept {
    if (this != &other) {
      if (_destroy) _destroy(_buf);
      _invoke = other._invoke;
      _destroy = other._destroy;
      _move = other._move;
      if (_move) {
        _move(_buf, other._buf);
        other._invoke = nullptr;
        other._destroy = nullptr;
        other._move = nullptr;
      }
    }
    return *this;
  }

  R operator()(Args... args) {
    return _invoke(_buf, std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept { return _invoke != nullptr; }
};

}  // namespace leaves

#endif  // _LEAVES_SMALL_FUNCTION_HPP
