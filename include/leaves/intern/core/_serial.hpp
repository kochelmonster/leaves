/*
RFC 1982-style serial number arithmetic with wraparound ordering and a
zero sentinel for uninitialized values.

Provides serial number types and aliases used for transaction and protocol
IDs, including overflow-aware comparison, wraparound addition, and explicit
handling of zero-valued sentinels.
*/
#ifndef _LEAVES_SERIAL_HPP
#define _LEAVES_SERIAL_HPP

#include <cstdint>
#include <limits>

namespace leaves {

/**
 * @brief Revolving serial number type with overflow-aware ordering (RFC 1982)
 * 
 * This implements RFC 1982-style serial number arithmetic with a reserved
 * zero sentinel for uninitialized values.
 * 
 * Properties:
 * - All valid values are != 0 (0 is reserved for "uninitialized")
 * - Wraps around: 0xFFFFFFFF + 1 = 1 (not 0)
 * - Overflow-aware comparison: 0xFFFFFFFF < 1 (because 1 is "after" 0xFFFFFFFF)
 * - Addition wraps: (a + b) wraps to valid range [1, 0xFFFFFFFF]
 * 
 * Ordering semantics:
 * - i1 < i2 if (i1 < i2 && i2 - i1 < 2^31) OR (i1 > i2 && i1 - i2 > 2^31)
 * - This creates a "sliding window" where numbers can be compared within 2^31 of each other
 * 
 * Example:
 *   serial32 s1 = 0xFFFFFFFE;
 *   serial32 s2 = s1 + 3;  // s2 == 1 (wrapped around, skipping 0)
 *   s1 < s2;               // true (0xFFFFFFFE < 1 because 1 is "after" in sequence)
 */
template<typename T = uint32_t>
struct serial_number {
  using value_type = T;
  static constexpr T MAX_VALUE = std::numeric_limits<T>::max();
  static constexpr T HALF_RANGE = T(1) << (sizeof(T) * 8 - 1);
  
  // Constructors
  constexpr serial_number() : _value(0) {}
  constexpr explicit serial_number(T value) : _value(value) {}
  
  // Get underlying value
  constexpr T value() const { return _value; }
  constexpr operator T() const { return _value; }
  
  // Check if initialized (non-zero)
  constexpr bool is_valid() const { return _value != 0; }
  constexpr explicit operator bool() const { return is_valid(); }
  
  // Addition with wrapping (skips 0)
  constexpr serial_number& operator+=(T rhs) {
    if (_value == 0) {
      _value = rhs;
    } else {
      // Add and wrap, ensuring we skip 0
      uint64_t sum = static_cast<uint64_t>(_value) + static_cast<uint64_t>(rhs);
      _value = static_cast<T>((sum - 1) % MAX_VALUE) + 1;
    }
    return *this;
  }
  
  constexpr serial_number operator+(T rhs) const {
    serial_number result = *this;
    result += rhs;
    return result;
  }
  
  // Increment (wraps from MAX_VALUE to 1, skipping 0)
  constexpr serial_number& operator++() {
    if (_value == MAX_VALUE) {
      _value = 1;
    } else {
      ++_value;
    }
    return *this;
  }
  
  constexpr serial_number operator++(int) {
    serial_number tmp = *this;
    ++(*this);
    return tmp;
  }
  
  // Decrement (wraps from 1 to MAX_VALUE, skipping 0)
  constexpr serial_number& operator--() {
    if (_value <= 1) {
      _value = MAX_VALUE;
    } else {
      --_value;
    }
    return *this;
  }
  
  constexpr serial_number operator--(int) {
    serial_number tmp = *this;
    --(*this);
    return tmp;
  }
  
  // Difference (signed, accounts for wrapping)
  constexpr int64_t operator-(const serial_number& rhs) const {
    if (_value == 0 || rhs._value == 0) {
      // Handle uninitialized values
      return static_cast<int64_t>(_value) - static_cast<int64_t>(rhs._value);
    }
    
    T diff;
    if (_value >= rhs._value) {
      diff = _value - rhs._value;
    } else {
      diff = _value + (MAX_VALUE - rhs._value) + 1;
    }
    
    // If difference is more than half the range, it wrapped the other way
    if (diff > HALF_RANGE) {
      return -static_cast<int64_t>(MAX_VALUE - diff + 1);
    }
    return static_cast<int64_t>(diff);
  }
  
  // RFC 1982 serial number ordering
  // i1 < i2 if:
  //   (i1 < i2 && i2 - i1 < 2^31) OR
  //   (i1 > i2 && i1 - i2 > 2^31)
  constexpr bool operator<(const serial_number& rhs) const {
    if (_value == 0 || rhs._value == 0) {
      return _value < rhs._value;  // Treat 0 as minimum
    }
    
    if (_value < rhs._value) {
      return (rhs._value - _value) < HALF_RANGE;
    } else if (_value > rhs._value) {
      return (_value - rhs._value) > HALF_RANGE;
    }
    return false;  // Equal
  }
  
  constexpr bool operator>(const serial_number& rhs) const {
    return rhs < *this;
  }
  
  constexpr bool operator<=(const serial_number& rhs) const {
    return !(*this > rhs);
  }
  
  constexpr bool operator>=(const serial_number& rhs) const {
    return !(*this < rhs);
  }
  
  constexpr bool operator==(const serial_number& rhs) const {
    return _value == rhs._value;
  }
  
  constexpr bool operator!=(const serial_number& rhs) const {
    return _value != rhs._value;
  }
  
  // Comparison with raw values
  constexpr bool operator<(T rhs) const {
    return *this < serial_number(rhs);
  }
  
  constexpr bool operator>(T rhs) const {
    return *this > serial_number(rhs);
  }
  
  constexpr bool operator<=(T rhs) const {
    return *this <= serial_number(rhs);
  }
  
  constexpr bool operator>=(T rhs) const {
    return *this >= serial_number(rhs);
  }
  
  constexpr bool operator==(T rhs) const {
    return _value == rhs;
  }
  
  constexpr bool operator!=(T rhs) const {
    return _value != rhs;
  }

  T _value;
};

// Type aliases
using serial32 = serial_number<uint32_t>;
using serial64 = serial_number<uint64_t>;
using tid_serial = serial32;  // For transaction IDs

} // namespace leaves

#endif // _LEAVES_SERIAL_HPP
