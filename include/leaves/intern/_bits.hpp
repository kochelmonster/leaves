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
  return (v && index < sizeof(bits) * 8 - 1) ? countr_zero(v) : -1;
}

template <typename bits_t = uint64_t>
int prev(bits_t bits, int index) {
  bits_t mask = ((bits_t)1 << index) - 1;
  bits_t v = bits & mask;
  return v ? (sizeof(bits_t) * 8 - 1) - countl_zero(v) : -1;
}
}  // namespace bits

template <size_t N = 256>
struct _BitField {
  const static size_t FIELD_COUNT = N / 64;

  uint64_t bits[FIELD_COUNT];  // 256 bits for compression

  static int idx(int val) { return val >> 6; }
  static int bit(int val) { return val & 63; }

  void init() {
    for (int i = 0; i < FIELD_COUNT; i++) bits[i] = 0;
  }

  template <typename T>
  void unify(const T& src) {
    assert(T::FIELD_COUNT == FIELD_COUNT);
    for (int i = 0; i < FIELD_COUNT; i++) bits[i] |= src.bits[i];
  }

  void set(int val) { bits[idx(val)] |= ((uint64_t)1) << bit(val); }

  void clear(int val) { bits[idx(val)] &= ~((uint64_t)1 << bit(val)); }

  int count() const {
    int count = 0;
    for (int i = 0; i < FIELD_COUNT; i++) count += popcount(bits[i]);
    return count;
  }

  // check if the index exists
  bool get(int idx_) const {
    return (bits[idx(idx_)] & ((uint64_t)1 << bit(idx_)));
  }

  // calculate the sparse index of val
  int index(int val) const {
    uint8_t idx_ = idx(val);
    uint64_t mask = (((uint64_t)1) << bit(val)) - 1;
    int ones = 0;
    for (int i = 0; i < idx_; i++) ones += popcount(bits[i]);
    ones += popcount(bits[idx_] & mask);
    return ones;
  }

  int first() const {
    for (char i = 0; i < FIELD_COUNT; i++) {
      if (bits[i]) return i * 64 + countr_zero(bits[i]);
    }
    return -1;
  }

  int last() const {
    for (int i = FIELD_COUNT - 1; i >= 0; i--) {
      if (bits[i]) return i * 64 + (63 - countl_zero(bits[i]));
    }
    return -1;
  }

  int next(int index) const {
    char i = idx(index);
    uint8_t bit_ = bit(index);
    uint64_t mask = ~((1ul << (bit_ + 1)) - 1);
    uint64_t v = bits[i] & mask;
    if (!v || bit_ == 63) {
      for (i++; i < FIELD_COUNT; i++) {
        if (bits[i]) return i * 64 + countr_zero(bits[i]);
      }
      return -1;
    } else
      return i * 64 + countr_zero(v);
  }

  int prev(int index) const {
    char i = idx(index);
    uint8_t bit_ = bit(index);
    uint64_t mask = (1ul << bit_) - 1;
    uint64_t v = bits[i] & mask;
    if (!v) {
      for (i--; i >= 0; i--) {
        if (bits[i]) return i * 64 + (63 - countl_zero(bits[i]));
      }
      return -1;
    } else
      return i * 64 + 63 - countl_zero(v);
  }
};

template <typename T, size_t N = 256>
struct _SparseArray {
  typedef _BitField<N> BitField;

  struct Iterator {
    const _SparseArray* array;
    int index;

    Iterator(const _SparseArray* array_, int index_)
        : array(array_), index(index_) {}

    const T& operator*() { return array->values[array->bits.index(index)]; }

    Iterator& operator++() {
      index = array->bits.next(index);
      return *this;
    }

    bool operator!=(const Iterator& other) { return index != other.index; }
    bool operator==(const Iterator& other) { return index == other.index; }
  };

  Iterator begin() const { return Iterator(this, bits.first()); }
  Iterator end() const { return Iterator(this, -1); }

  void init() { bits.init(); }

  bool get(int val) const { return bits.get(val); }

  void set(int val, const T& value) {
    if (!bits.get(val)) {
      insert(val, value);
    } else {
      values[bits.index(val)] = value;
    }
  }

  int insert(int idx, const T& value) {
    assert(!bits.get(idx));
    int rindex = bits.index(idx);
    memmove(values + rindex + 1, values + rindex,
            sizeof(T) * (bits.count() - rindex));
    bits.set(idx);
    values[rindex] = value;
    return rindex;
  }

  void remove(int idx) {
    assert(bits.get(idx));
    memmove(values + bits.index(idx), values + bits.index(idx) + 1,
            sizeof(T) * (bits.count() - bits.index(idx) - 1));
    bits.clear(idx);
  }

  const T& operator[](uint8_t val) const { return values[bits.index(val)]; }
  T& operator[](uint8_t val) { return values[bits.index(val)]; }

  int count() const { return bits.count(); }

  // the space needed for the array
  static constexpr size_t space(size_t size) {
    return sizeof(BitField) + size * sizeof(T);
  }

  size_t space() const { return space(count()); }

  BitField bits;
  T values[0];
};

}  // namespace leaves

#endif  // _LEAVES__BIT_HPP