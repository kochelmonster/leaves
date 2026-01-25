#ifndef _LEAVES__BIT_HPP
#define _LEAVES__BIT_HPP

#include <boost/core/bit.hpp>
#include <cstdint>

namespace leaves {

using boost::core::countl_zero;
using boost::core::countr_zero;
using boost::core::popcount;

namespace bits {
template <typename bits_t = uint64_t>
int count(bits_t bits) {
  return popcount(bits);
}

template <typename bits_t = uint64_t>
bool isset(bits_t bits, int val) {
  return bits & (((bits_t)1) << val);
}

template <typename bits_t = uint64_t>
void set(bits_t& bits, int val) {
  bits |= (((bits_t)1) << val);
}

template <typename bits_t = uint64_t>
int index(bits_t bits, int val) {
  bits_t v = bits & ((((bits_t)1) << val) - 1);
  return popcount(v);
}

template <typename bits_t = uint64_t>
int first(bits_t bits) {
  return bits ? countr_zero(bits) : -1;
}

template <typename bits_t = uint64_t>
int last(bits_t bits) {
  return bits ? (sizeof(bits_t) * 8 - 1) - countl_zero(bits) : -1;
}

template <typename bits_t = uint64_t>
int next(bits_t bits, int index) {
  bits_t mask = ~(((bits_t)1 << (index + 1)) - 1);
  bits_t v = bits & mask;
  return (v && index < static_cast<int>(sizeof(bits_t) * 8 - 1)) ? countr_zero(v) : -1;
}

template <typename bits_t = uint64_t>
int prev(bits_t bits, int index) {
  bits_t mask = ((bits_t)1 << index) - 1;
  bits_t v = bits & mask;
  return v ? (sizeof(bits_t) * 8 - 1) - countl_zero(v) : -1;
}
}  // namespace bits

}  // namespace leaves

#endif  // _LEAVES__BIT_HPP