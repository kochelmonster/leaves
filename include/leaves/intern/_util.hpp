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
        (cmp = size() - other.size());
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

  template <typename T>
  const _Offset& operator+=(T add) {
    _offset += add;
    return *this;
  }

  template <typename T>
  _Offset operator+(T src) {
    return _Offset(_offset + src);
  }

  template <typename T>
  _Offset operator-(T src) {
    return _Offset(_offset - src);
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
  uint32_t inc_ref() {
    return _ref.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  uint32_t dec_ref() {
    return _ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
  }

  uint32_t get_ref() const { return _ref.load(std::memory_order_acquire); }

  operator bool() const { return size(); }
  uint64_t end() const { return offset() + size(); }
};

inline size_t get_prefix(const char* str1, const char* str2, size_t size1,
                         size_t size2, int& cmp) {
  size_t i = 0;
  size_t limit = std::min(size1, size2) / sizeof(uint64_t);
  const uint64_t* wstr1 = reinterpret_cast<const uint64_t*>(str1);
  const uint64_t* wstr2 = reinterpret_cast<const uint64_t*>(str2);

  while (i < limit && wstr1[i] == wstr2[i]) {
    i++;
  }
  i *= sizeof(uint64_t);

  limit = std::min(size1, size2);
  while (i < limit && str1[i] == str2[i]) {
    i++;
  }

  if (i < limit) {
    cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
  } else if (size1 > limit)
    cmp = 1;
  else if (size2 > limit)
    cmp = -1;
  else
    cmp = 0;
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
