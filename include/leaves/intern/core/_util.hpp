#ifndef _LEAVES__UTIL_HPP
#define _LEAVES__UTIL_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

// Compiler detection for builtin memcpy
#if defined(__GNUC__) || defined(__clang__)
  #define LEAVES_HAS_BUILTIN_MEMCPY 1
#endif

// MSVC intrinsics for bit operations
#if defined(_MSC_VER)
  #include <intrin.h>
#endif

// Platform detection for SIMD optimizations
#if defined(__x86_64__) || defined(_M_X64)
  #define LEAVES_X86_64 1
  #if defined(__SSE2__) || (defined(_MSC_VER) && _M_X64)
    #include <emmintrin.h>  // SSE2 for non-temporal stores
    #define LEAVES_HAS_SSE2 1
  #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define LEAVES_ARM64 1
  #if defined(__ARM_NEON) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define LEAVES_HAS_NEON 1
  #endif
#endif

#include "_serial.hpp"

namespace leaves {

// Optimized memcpy for database values
inline void* optimized_memcpy(void* dest, const void* src, size_t n) {
  // Fast path for small sizes (<= 32 bytes) - common for keys and small values
  if (n <= 32) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    
#ifdef LEAVES_HAS_BUILTIN_MEMCPY
    // Use compiler builtins when available (GCC/Clang)
    if (n >= 16) {
      __builtin_memcpy(d, s, 16);
      __builtin_memcpy(d + n - 16, s + n - 16, 16);
    } else if (n >= 8) {
      __builtin_memcpy(d, s, 8);
      __builtin_memcpy(d + n - 8, s + n - 8, 8);
    } else if (n >= 4) {
      __builtin_memcpy(d, s, 4);
      __builtin_memcpy(d + n - 4, s + n - 4, 4);
    } else {
      for (size_t i = 0; i < n; i++) d[i] = s[i];
    }
#else
    // Fallback for other compilers (MSVC, etc.)
    if (n >= 16) {
      memcpy(d, s, 16);
      memcpy(d + n - 16, s + n - 16, 16);
    } else if (n >= 8) {
      memcpy(d, s, 8);
      memcpy(d + n - 8, s + n - 8, 8);
    } else {
      memcpy(d, s, n);
    }
#endif
    return dest;
  }

#if defined(LEAVES_HAS_SSE2)
  // x86-64: Use non-temporal stores for large copies to avoid cache pollution
  if (n >= 32768) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    
    // Align destination to 16-byte boundary
    size_t align_bytes = (16 - ((uintptr_t)d & 15)) & 15;
    if (align_bytes > 0 && align_bytes < n) {
      memcpy(d, s, align_bytes);
      d += align_bytes;
      s += align_bytes;
      n -= align_bytes;
    }
    
    // Non-temporal stores for aligned 16-byte chunks
    size_t chunks = n / 16;
    for (size_t i = 0; i < chunks; i++) {
      __m128i data = _mm_loadu_si128((const __m128i*)s);
      _mm_stream_si128((__m128i*)d, data);
      d += 16;
      s += 16;
    }
    _mm_sfence();  // Ensure stores are visible
    
    // Copy remainder
    size_t remainder = n & 15;
    if (remainder > 0) {
      memcpy(d, s, remainder);
    }
    return dest;
  }
#elif defined(LEAVES_HAS_NEON)
  // ARM64: Use NEON non-temporal stores for large copies
  if (n >= 32768) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    
    // Align destination to 16-byte boundary
    size_t align_bytes = (16 - ((uintptr_t)d & 15)) & 15;
    if (align_bytes > 0 && align_bytes < n) {
      memcpy(d, s, align_bytes);
      d += align_bytes;
      s += align_bytes;
      n -= align_bytes;
    }
    
    // NEON stores for 16-byte chunks
    size_t chunks = n / 16;
    for (size_t i = 0; i < chunks; i++) {
      uint8x16_t data = vld1q_u8((const uint8_t*)s);
      vst1q_u8((uint8_t*)d, data);
      d += 16;
      s += 16;
    }
    
    // Copy remainder
    size_t remainder = n & 15;
    if (remainder > 0) {
      memcpy(d, s, remainder);
    }
    return dest;
  }
#endif
  
  // Default path for medium sizes and unsupported platforms
  return memcpy(dest, src, n);
}

typedef tid_serial tid_t;
typedef enum { TRIE = 0, LEAF = 1 } NodeTypes;
typedef enum { READ = 0, WRITE = 1 } Access;

class Slice {
 private:
  size_t _size;
  const char* _data;

 public:
  Slice(const void* data, size_t size)
      : _size(size), _data((const char*)data) {}

  Slice(const char* str) : _size(std::strlen(str)), _data(str) {}

  Slice(const std::string& src) : _size(src.size()), _data(src.data()) {}

  Slice() : _size(0), _data(NULL) {}

  Slice(const Slice& other) : _size(other._size), _data(other._data) {}

  Slice slice(size_t len) const { return Slice(data(), len); }

  std::string string() const { return std::string(data(), _size); }

  const Slice& operator=(const Slice& src) {
    _size = src.size();
    _data = src.data();
    return *this;
  }

  template <typename ot>
  bool operator==(const ot& other) const {
    return size() == other.size() && memcmp(data(), other.data(), size()) == 0;
  }

  template <typename ot>
  int compare(const ot& other) const {
    int cmp = 0, size_ = size() < other.size() ? size() : other.size();
    (cmp = memcmp(data(), other.data(), size_)) ||
        (cmp = (size() > other.size()) - (size() < other.size()));
    return (0 < cmp) - (cmp < 0);
  }

  char operator[](size_t index) const { return data()[index]; }

  const char* data() const { return _data; }

  size_t size() const { return _size; }

  Slice advance(size_t size) const {
    return Slice(data() + size, _size - size);
  }

  void iadvance(size_t size) {
    assert(size <= _size);
    _data += size;
    _size -= size;
  }

  bool empty() const { return _size == 0; }

  operator bool() const { return _size != 0; }
  void reset() {
    _size = 0;
    _data = nullptr;
  }
};

template <typename BaseType>
struct _Offset {
  typedef _Offset<BaseType> OffsetType;
  BaseType _offset;

  static const uint64_t TYPE_MASK = 0x3;
  static const uint64_t RELATIVE_FLAG = 0x4;  // Bit 2: relative offset flag (offsets are 8-byte aligned)

  constexpr _Offset(uint64_t src = 0) : _offset(src) {}

  constexpr _Offset(const _Offset& other) : _offset(other._offset) {}

  template <typename T>
  bool operator==(T other) const {
    return _offset == (BaseType)other;
  }
  template <typename T>
  bool operator!=(T other) const {
    return _offset != (BaseType)other;
  }

  template <typename T>
  bool operator<=(T other) const {
    return _offset <= other;
  }

  template <typename T>
  bool operator<(T other) const {
    return _offset < other;
  }

  template <typename T>
  bool operator>=(T other) const {
    return _offset >= other;
  }

  bool operator==(const _Offset& src) const { return _offset == src._offset; }
  bool operator!=(const _Offset& src) const { return _offset != src._offset; }
  bool operator<(const _Offset& src) const { return _offset < src._offset; }
  bool operator>(const _Offset& src) const { return _offset > src._offset; }
  bool operator<=(const _Offset& src) const { return _offset <= src._offset; }
  bool operator>=(const _Offset& src) const { return _offset >= src._offset; }

  const _Offset& operator=(const _Offset& src) {
    _offset = src._offset;
    return *this;
  }

  template <typename T>
  const _Offset& operator=(T src) {
    _offset = src;
    return *this;
  }

  // Arithmetic on _Offset operates on the raw value including metadata bits.
  // This is safe because offsets are 8-byte aligned, so the low 3 metadata
  // bits (TYPE_MASK | RELATIVE_FLAG) are not affected by aligned operands.
  template <typename T>
  const _Offset& operator+=(T add) {
    static_assert(std::is_convertible<T, uint64_t>::value, "T must be convertible to uint64_t");
    _offset += add;
    return *this;
  }

  template <typename T>
  _Offset operator+(T src) {
    static_assert(std::is_convertible<T, uint64_t>::value, "T must be convertible to uint64_t");
    return _offset + src;
  }

  template <typename T>
  _Offset operator-(T src) {
    static_assert(std::is_convertible<T, uint64_t>::value, "T must be convertible to uint64_t");
    return _offset - src;
  }

  // Get absolute offset value (mask out TYPE_MASK and RELATIVE_FLAG)
  operator uint64_t() const { return _offset & ~(TYPE_MASK | RELATIVE_FLAG); }
  
  NodeTypes type() const { return (NodeTypes)(_offset & TYPE_MASK); }
  const _Offset& type(NodeTypes type) {
    _offset &= ~TYPE_MASK;
    _offset |= type;
    return *this;
  }

  // Relative offset flag accessors
  bool is_relative() const { return (_offset & RELATIVE_FLAG) != 0; }
  
  // Convert to relative offset given the resolved destination pointer
  // dest_ptr: the actual memory address of the target (resolved from absolute offset)
  // this: the offset_t field that will hold the relative offset
  // Result: relative = dest_ptr - this
  const _Offset& set_relative(const void* dest_ptr) {
    NodeTypes saved_type = type();
    _offset = (BaseType)(int64_t)dest_ptr - (int64_t)this;
    assert((_offset & 7) == 0);
    _offset |= RELATIVE_FLAG;
    type(saved_type);
    return *this;
  }

  // Get the raw offset as signed integer (for relative addressing)
  int64_t as_signed() const {
    // Sign-extend the offset value (excluding TYPE_MASK and RELATIVE_FLAG bits)
    uint64_t raw = _offset & ~(TYPE_MASK | RELATIVE_FLAG);
    // Check if the sign bit is set (bit 63 - 3 = bit 60 after masking)
    constexpr int shift = 3;  // TYPE_MASK (2 bits) + RELATIVE_FLAG (1 bit)
    int64_t signed_val = (int64_t)(raw << shift) >> shift;
    return signed_val;
  }

  template <typename T>
  T* resolve() const {
    return reinterpret_cast<T*>((char*)this + as_signed());
  }

};

typedef _Offset<uint64_t> offset_t;

struct AreaSlice {
  std::atomic<uint64_t> _offset;
  uint32_t _size;
  std::atomic<uint32_t> _ref;

  // Copy assignment operator needed because std::atomic isn't copyable by
  // default
  AreaSlice& operator=(const AreaSlice& other) {
    _offset.store(other._offset.load(std::memory_order_acquire),
                  std::memory_order_release);
    _size = other._size;
    _ref.store(other._ref.load(std::memory_order_acquire),
               std::memory_order_release);
    return *this;
  }

  AreaSlice(uint64_t offset_ = 0, uint32_t size_ = 0, uint32_t ref_ = 0)
      : _offset(offset_), _size(size_), _ref(ref_) {}
  AreaSlice(const AreaSlice& other)
      : _offset(other._offset.load(std::memory_order_acquire)),
        _size(other._size),
        _ref(other._ref.load(std::memory_order_acquire)) {}

  uint64_t offset() const { return _offset.load(std::memory_order_acquire); }

  void offset(uint64_t new_offset) {
    _offset.store(new_offset, std::memory_order_release);
  }

  uint32_t size() const { return _size; }

  void size(uint32_t new_size) { _size = new_size; }

  // Reference counting methods - atomic
  // inc_ref uses relaxed: just incrementing, no synchronization needed
  // dec_ref uses acq_rel: ensures all prior writes are visible before potential delete
  uint32_t inc_ref() {
    return _ref.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  uint32_t dec_ref() {
    return _ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
  }

  uint32_t get_ref() const { return _ref.load(std::memory_order_acquire); }

  operator bool() const { return size(); }
  uint64_t end() const { return offset() + size(); }
};

// Portable bit manipulation helpers
namespace detail {

inline unsigned count_trailing_zeros_32(uint32_t x) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward(&index, x);
  return index;
#elif defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctz(x));
#else
  // Portable fallback using de Bruijn sequence
  static const unsigned debruijn32[32] = {
    0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
    31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
  };
  return debruijn32[((x & -x) * 0x077CB531U) >> 27];
#endif
}

inline unsigned count_trailing_zeros_64(uint64_t x) {
#if defined(_MSC_VER)
  #if defined(_M_X64) || defined(_M_ARM64)
  unsigned long index;
  _BitScanForward64(&index, x);
  return index;
  #else
  // 32-bit MSVC fallback
  unsigned long index;
  if (static_cast<uint32_t>(x) != 0) {
    _BitScanForward(&index, static_cast<uint32_t>(x));
    return index;
  }
  _BitScanForward(&index, static_cast<uint32_t>(x >> 32));
  return index + 32;
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(x));
#else
  // Portable fallback
  if (static_cast<uint32_t>(x) != 0) {
    return count_trailing_zeros_32(static_cast<uint32_t>(x));
  }
  return 32 + count_trailing_zeros_32(static_cast<uint32_t>(x >> 32));
#endif
}

inline unsigned count_leading_zeros_64(uint64_t x) {
#if defined(_MSC_VER)
  #if defined(_M_X64) || defined(_M_ARM64)
  unsigned long index;
  _BitScanReverse64(&index, x);
  return 63 - index;
  #else
  // 32-bit MSVC fallback
  unsigned long index;
  if (static_cast<uint32_t>(x >> 32) != 0) {
    _BitScanReverse(&index, static_cast<uint32_t>(x >> 32));
    return 31 - index;
  }
  _BitScanReverse(&index, static_cast<uint32_t>(x));
  return 63 - index;
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_clzll(x));
#else
  // Portable fallback
  unsigned n = 0;
  if (x <= 0x00000000FFFFFFFFULL) { n += 32; x <<= 32; }
  if (x <= 0x0000FFFFFFFFFFFFULL) { n += 16; x <<= 16; }
  if (x <= 0x00FFFFFFFFFFFFFFULL) { n += 8;  x <<= 8; }
  if (x <= 0x0FFFFFFFFFFFFFFFULL) { n += 4;  x <<= 4; }
  if (x <= 0x3FFFFFFFFFFFFFFFULL) { n += 2;  x <<= 2; }
  if (x <= 0x7FFFFFFFFFFFFFFFULL) { n += 1; }
  return n;
#endif
}

// Detect endianness at compile time
inline bool is_little_endian() {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#elif defined(_WIN32) || defined(__EMSCRIPTEN__) || defined(__x86_64__) || \
      defined(_M_X64) || defined(__i386__) || defined(_M_IX86) || \
      defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
  // Most common platforms are little-endian
  return true;
#else
  // Runtime check as last resort
  const uint16_t test = 1;
  return *reinterpret_cast<const uint8_t*>(&test) == 1;
#endif
}

}  // namespace detail

inline size_t get_prefix(const char* str1, const char* str2, size_t size1,
                         size_t size2, int& cmp) {
  size_t i = 0;
  const size_t min_size = std::min(size1, size2);

#if defined(LEAVES_HAS_SSE2) && !defined(__EMSCRIPTEN__)
  // SSE2: Compare 16 bytes at a time (x86-64 only, not Emscripten)
  while (i + 16 <= min_size) {
    __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(str1 + i));
    __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(str2 + i));
    __m128i eq = _mm_cmpeq_epi8(a, b);
    int mask = _mm_movemask_epi8(eq);
    if (mask != 0xFFFF) {
      i += detail::count_trailing_zeros_32(static_cast<uint32_t>(~mask) & 0xFFFF);
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i += 16;
  }
#elif defined(LEAVES_HAS_NEON) && !defined(__EMSCRIPTEN__)
  // NEON: Compare 16 bytes at a time (ARM64 only)
  while (i + 16 <= min_size) {
    uint8x16_t a = vld1q_u8(reinterpret_cast<const uint8_t*>(str1 + i));
    uint8x16_t b = vld1q_u8(reinterpret_cast<const uint8_t*>(str2 + i));
    uint8x16_t eq = vceqq_u8(a, b);
    // Invert: 0xFF at mismatch positions, 0x00 at matches
    uint8x16_t neq = vmvnq_u8(eq);
    // Narrow 16 bytes to 8 nibbles: take the high nibble of each pair,
    // producing a non-zero nibble for each differing byte
    uint8x8_t narrow = vshrn_n_u16(vreinterpretq_u16_u8(neq), 4);
    uint64_t mask64 = vget_lane_u64(vreinterpret_u64_u8(narrow), 0);
    if (mask64 != 0) {
      // Each byte maps to a 4-bit nibble; ctz/4 gives the byte index
      unsigned j = detail::count_trailing_zeros_64(mask64) / 4;
      i += j;
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i += 16;
  }
#endif

  // 64-bit word comparison for remaining bytes
  // Using memcpy for safe unaligned access (compiler optimizes this)
  while (i + sizeof(uint64_t) <= min_size) {
    uint64_t w1, w2;
    memcpy(&w1, str1 + i, sizeof(uint64_t));
    memcpy(&w2, str2 + i, sizeof(uint64_t));
    if (w1 != w2) {
      uint64_t diff = w1 ^ w2;
      size_t byte_offset;
      if (detail::is_little_endian()) {
        byte_offset = detail::count_trailing_zeros_64(diff) / 8;
      } else {
        byte_offset = detail::count_leading_zeros_64(diff) / 8;
      }
      i += byte_offset;
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i += sizeof(uint64_t);
  }

  // Byte-by-byte for remainder (0-7 bytes)
  while (i < min_size && str1[i] == str2[i]) {
    i++;
  }

  if (i < min_size) {
    cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
  } else if (size1 > size2) {
    cmp = 1;
  } else if (size2 > size1) {
    cmp = -1;
  } else {
    cmp = 0;
  }
  return i;
}

constexpr size_t padding(size_t a, size_t b) {
  return ((a + b - 1) / b) * b;  // Round up to the next multiple of b
}

const static uint32_t ALIGN = sizeof(void*);
constexpr uint32_t align(uint32_t s) { return (s + ALIGN - 1) & ~(ALIGN - 1); }

template <typename DstBlock, typename SrcBlock>
void copy(DstBlock& dst, const SrcBlock& src) {
  uint16_t src_size = src.size();
  memcpy((char*)&dst, (char*)&src, src_size);
}

}  // namespace leaves

#endif  // _LEAVES__UTIL_HPP
