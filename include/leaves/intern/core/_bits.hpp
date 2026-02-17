#ifndef _LEAVES__BIT_HPP
#define _LEAVES__BIT_HPP

#include <boost/core/bit.hpp>
#include <cstdint>
#include <type_traits>

namespace leaves {

using boost::core::countl_zero;
using boost::core::countr_zero;
using boost::core::popcount;

namespace bits {

// Helper to convert endian types to native format
// For native types, returns the value unchanged
// For boost endian types, extracts the native value via implicit conversion
template <typename T>
auto to_native(T val) -> typename std::enable_if<std::is_integral<T>::value, T>::type {
  return val;
}

template <typename T>
auto to_native(T val) -> typename std::enable_if<!std::is_integral<T>::value, 
                                                  typename T::value_type>::type {
  return static_cast<typename T::value_type>(val);
}

// Helper to get the unsigned native type, promoting small types to at least uint32_t
// to avoid integer promotion issues with boost bit functions
template <typename T>
struct native_unsigned {
  using base = typename std::make_unsigned<T>::type;
  using type = typename std::conditional<(sizeof(base) < sizeof(uint32_t)), uint32_t, base>::type;
};

template <typename bits_t = uint64_t>
int count(bits_t bits) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  return popcount(static_cast<native_t>(to_native(bits)));
}

template <typename bits_t = uint64_t>
bool isset(bits_t bits, int val) {
  auto native = to_native(bits);
  return native & (((decltype(native))1) << val);
}

template <typename bits_t = uint64_t>
void set(bits_t& bits, int val) {
  bits |= (((bits_t)1) << val);
}

template <typename bits_t = uint64_t>
int index(bits_t bits, int val) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  native_t native = static_cast<native_t>(to_native(bits));
  native_t v = native & (((native_t)1 << val) - 1);
  return popcount(v);
}

template <typename bits_t = uint64_t>
int first(bits_t bits) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  native_t native = static_cast<native_t>(to_native(bits));
  return native ? countr_zero(native) : -1;
}

template <typename bits_t = uint64_t>
int last(bits_t bits) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  native_t native = static_cast<native_t>(to_native(bits));
  return native ? (sizeof(native_t) * 8 - 1) - countl_zero(native) : -1;
}

template <typename bits_t = uint64_t>
int next(bits_t bits, int index) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  constexpr int WIDTH = sizeof(native_t) * 8;
  if (index >= WIDTH - 1) return -1;
  native_t native = static_cast<native_t>(to_native(bits));
  native_t mask = ~(((native_t)1 << (index + 1)) - 1);
  native_t v = native & mask;
  return v ? countr_zero(v) : -1;
}

template <typename bits_t = uint64_t>
int prev(bits_t bits, int index) {
  using native_t = typename native_unsigned<decltype(to_native(bits))>::type;
  native_t native = static_cast<native_t>(to_native(bits));
  native_t mask = ((native_t)1 << index) - 1;
  native_t v = native & mask;
  return v ? (sizeof(native_t) * 8 - 1) - countl_zero(v) : -1;
}
}  // namespace bits

}  // namespace leaves

#endif  // _LEAVES__BIT_HPP