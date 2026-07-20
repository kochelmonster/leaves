/*
Shared internal utility helpers, constants, and small support primitives.
*/
#ifndef _LEAVES__UTIL_HPP
#define _LEAVES__UTIL_HPP

#include "_port.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "_serial.hpp"

namespace leaves {

enum class LogSeverity : uint8_t {
  info,
  debug,
  warn,
  error,
  critical,
};

inline const char* log_severity_name(LogSeverity severity) {
  switch (severity) {
    case LogSeverity::info:
      return "info";
    case LogSeverity::debug:
      return "debug";
    case LogSeverity::warn:
      return "warn";
    case LogSeverity::error:
      return "error";
    case LogSeverity::critical:
      return "critical";
    default:
      return "unknown";
  }
}

inline int default_log_call(LogSeverity severity, const char* fmt, ...) {
  int written = std::fprintf(stderr, "[%s] ", log_severity_name(severity));
  va_list args;
  va_start(args, fmt);
  written += std::vfprintf(stderr, fmt, args);
  va_end(args);
  return written;
}

// Internal logging for leaves intern headers.
// Enable with -DLEAVES_LOG and optionally override LEAVES_LOG_CALL.
#if defined(LEAVES_LOG)
  #ifndef LEAVES_LOG_CALL
    #define LEAVES_LOG_CALL(...) ::leaves::default_log_call(__VA_ARGS__)
  #endif
  #define LEAVES_LOG_INFO ::leaves::LogSeverity::info
  #define LEAVES_LOG_DEBUG ::leaves::LogSeverity::debug
  #define LEAVES_LOG_WARN ::leaves::LogSeverity::warn
  #define LEAVES_LOG_ERROR ::leaves::LogSeverity::error
  #define LEAVES_LOG_CRITICAL ::leaves::LogSeverity::critical
  #define LEAVES_INTERNAL_LOG(severity, ...) LEAVES_LOG_CALL(severity, __VA_ARGS__)
#else
  #define LEAVES_INTERNAL_LOG(...) ((void)0)
#endif

static constexpr uint32_t DEFAULT_COPY_WRITE_PIVOT_BYTES = 64 * 1024;
static constexpr uint32_t COPY_WRITE_PIVOT_DISABLED =
    std::numeric_limits<uint32_t>::max();

inline uint32_t calibrate_copy_write_pivot_file(const char* calibration_file) {
  static constexpr size_t CHUNKS[] = {512,  1024, 2048, 4096, 8192,
                                      16384, 32768, 65536, 131072};
  static constexpr size_t TARGET_BYTES = 32 * 1024 * 1024;
  static constexpr size_t WINDOW_BYTES = 32 * 1024 * 1024;
  static constexpr size_t WARMUP_ITERS = 8;
  static constexpr size_t TRIALS = 5;

  const size_t max_chunk = CHUNKS[sizeof(CHUNKS) / sizeof(CHUNKS[0]) - 1];
  const size_t bench_file_size = WINDOW_BYTES + max_chunk;

  auto median_ns = [](std::vector<int64_t>& samples) -> int64_t {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
  };

  std::error_code ec;
  auto cleanup = [&]() {
    std::filesystem::remove(calibration_file, ec);
  };

  try {
    {
      std::ofstream f(calibration_file,
                      std::ios::out | std::ios::binary | std::ios::trunc);
      f.put('\0');
    }
    std::filesystem::resize_file(calibration_file, bench_file_size);

    int fd = open_rw_fd(calibration_file, false);
    if (fd < 0) {
      cleanup();
      return DEFAULT_COPY_WRITE_PIVOT_BYTES;
    }

    std::vector<char> src(max_chunk, 'p');
    std::vector<char> dst(bench_file_size, 0);

    uint32_t pivot = DEFAULT_COPY_WRITE_PIVOT_BYTES;
    bool found = false;

    for (size_t chunk : CHUNKS) {
      const size_t loops = (std::max)(size_t(64), TARGET_BYTES / chunk);
      const size_t span = WINDOW_BYTES - chunk;

      size_t off = 0;
      for (size_t i = 0; i < WARMUP_ITERS; i++) {
        optimized_memcpy(dst.data() + off, src.data(), chunk);
        off += chunk;
        if (off > span) off = 0;
      }

      std::vector<int64_t> memcpy_samples;
      std::vector<int64_t> write_samples;
      memcpy_samples.reserve(TRIALS);
      write_samples.reserve(TRIALS);

      for (size_t t = 0; t < TRIALS; t++) {
        off = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < loops; i++) {
          optimized_memcpy(dst.data() + off, src.data(), chunk);
          off += chunk;
          if (off > span) off = 0;
        }
        auto t1 = std::chrono::steady_clock::now();
        memcpy_samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());

        off = 0;
        t0 = std::chrono::steady_clock::now();
        bool write_ok = true;
        for (size_t i = 0; i < loops; i++) {
          if (!write_fd_all_at(fd, off, src.data(), chunk)) {
            write_ok = false;
            break;
          }
          off += chunk;
          if (off > span) off = 0;
        }
        t1 = std::chrono::steady_clock::now();
        if (!write_ok) {
          memcpy_samples.clear();
          write_samples.clear();
          break;
        }
        write_samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
      }

      if (memcpy_samples.empty() || write_samples.empty()) continue;

      const int64_t memcpy_median = median_ns(memcpy_samples);
      const int64_t write_median = median_ns(write_samples);
      // Require 7% win to avoid noisy threshold flips.
      if (write_median * 100 <= memcpy_median * 93) {
        pivot = static_cast<uint32_t>(chunk);
        found = true;
        break;
      }
    }

  close_fd(fd);
    cleanup();

    if (!found) {
      return DEFAULT_COPY_WRITE_PIVOT_BYTES;
    }
    return pivot;
  } catch (...) {
    cleanup();
    return DEFAULT_COPY_WRITE_PIVOT_BYTES;
  }
}

typedef tid_serial tid_t;
typedef enum { TRIE = 0, LEAF = 1 } NodeTypes;

// CAS spinlock using TTAS (test-and-test-and-set) pattern.
// Safe in shared memory (mmap) — uses only hardware atomics, no kernel state.
struct SpinLock {
  std::atomic<uint32_t> _flag{0};

  void lock() {
    while (_flag.exchange(1, std::memory_order_acquire)) {
      // Spin on load (shared cache line) to reduce bus traffic
      while (_flag.load(std::memory_order_relaxed)) {
        cpu_relax();
      }
    }
  }

  bool try_lock() {
    uint32_t expected = 0;
    return _flag.compare_exchange_weak(expected, 1,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed);
  }

  void unlock() { _flag.store(0, std::memory_order_release); }
};

class Slice {
 private:
  size_t _size;
  const char* _data;

 public:
  // Constructs a view over an arbitrary byte range.
  Slice(const void* data, size_t size)
      : _size(size), _data((const char*)data) {}

  // Constructs a view over a null-terminated string.
  Slice(const char* str) : _size(std::strlen(str)), _data(str) {}

  // Constructs a non-owning view over src's internal buffer.
  Slice(const std::string& src) : _size(src.size()), _data(src.data()) {}

  // Constructs an empty view.
  Slice() : _size(0), _data(NULL) {}

  Slice(const Slice& other) : _size(other._size), _data(other._data) {}

  Slice slice(size_t len) const { return Slice(data(), len); }

  // Returns a copied std::string from the view.
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
    int cmp = 0;
    size_t size_ = size() < other.size() ? size() : other.size();
    (cmp = memcmp(data(), other.data(), size_)) ||
        (cmp = (size() > other.size()) - (size() < other.size()));
    return (0 < cmp) - (cmp < 0);
  }

  unsigned char operator[](size_t index) const { return data()[index]; }

  // Returns a pointer to the first byte.
  const char* data() const { return _data; }

  // Returns the byte length of the view.
  size_t size() const { return _size; }

  Slice advance(size_t size) const {
    assert(size <= _size);
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

  // Get the raw offset value including metadata bits (for memory-store pointer round-tripping)
  uint64_t raw() const { return _offset; }
  
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

inline size_t get_prefix(const char* str1, const char* str2, size_t size1,
                         size_t size2, int& cmp) {
  size_t i = 0;
  const size_t min_size = std::min(size1, size2);

  // Fast path for short prefixes (< 8 bytes): covers compressed trie nodes
  if (min_size < 8) {
    if (min_size >= 4) {
      uint32_t w1, w2;
      memcpy(&w1, str1, 4);
      memcpy(&w2, str2, 4);
      if (w1 != w2) {
        i = detail::count_trailing_zeros_32(w1 ^ w2) / 8;
        cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
        return i;
      }
      i = 4;
    }
    while (i < min_size && str1[i] == str2[i]) i++;
    if (i < min_size)
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
    else if (size1 > size2) cmp = 1;
    else if (size2 > size1) cmp = -1;
    else cmp = 0;
    return i;
  }

  // Fast path: compare first 16 bytes without loops (covers 99.9% of cases)
  {
    uint64_t w1, w2;
    memcpy(&w1, str1, 8);
    memcpy(&w2, str2, 8);
    if (w1 != w2) {
      uint64_t diff = w1 ^ w2;
      i = detail::count_trailing_zeros_64(diff) / 8;
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i = 8;
    if (min_size >= 16) {
      memcpy(&w1, str1 + 8, 8);
      memcpy(&w2, str2 + 8, 8);
      if (w1 != w2) {
        uint64_t diff = w1 ^ w2;
        i = 8 + detail::count_trailing_zeros_64(diff) / 8;
        cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
        return i;
      }
      i = 16;
    }
  }

#if defined(LEAVES_HAS_AVX512) && !defined(__EMSCRIPTEN__)
  // AVX-512: Compare 64 bytes at a time
  while (i + 64 <= min_size) {
    __m512i a = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(str1 + i));
    __m512i b = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(str2 + i));
    __mmask64 mask = _mm512_cmpneq_epi8_mask(a, b);
    if (mask != 0) {
      i += detail::count_trailing_zeros_64(mask);
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i += 64;
  }
#endif

#if defined(LEAVES_HAS_AVX2) && !defined(__EMSCRIPTEN__)
  // AVX2: Compare 32 bytes at a time
  while (i + 32 <= min_size) {
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(str1 + i));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(str2 + i));
    __m256i eq = _mm256_cmpeq_epi8(a, b);
    int mask = _mm256_movemask_epi8(eq);
    if (mask != -1) {  // -1 = 0xFFFFFFFF = all 32 bytes equal
      i += detail::count_trailing_zeros_32(static_cast<uint32_t>(~mask));
      cmp = (uint8_t)str1[i] > (uint8_t)str2[i] ? 1 : -1;
      return i;
    }
    i += 32;
  }
#endif

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

const static uint32_t ALIGN = sizeof(offset_t);
constexpr uint32_t align(uint32_t s) { return (s + ALIGN - 1) & ~(ALIGN - 1); }

template <typename DstBlock, typename SrcBlock>
void copy(DstBlock& dst, const SrcBlock& src) {
  uint16_t src_size = src.size();
  memcpy((char*)&dst, (char*)&src, src_size);
}

}  // namespace leaves

#endif  // _LEAVES__UTIL_HPP
