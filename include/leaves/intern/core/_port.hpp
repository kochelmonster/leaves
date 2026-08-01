/*
Platform portability macros and compiler-specific compatibility helpers.
*/
#ifndef _LEAVES___PORT_HPP
#define _LEAVES___PORT_HPP

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "_exception.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

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

#if defined(_MSC_VER)
#include <intrin.h>
#include <xmmintrin.h>  // For _mm_prefetch and _MM_HINT_T0 on MSVC
#define FORCE_INLINE __forceinline
#define NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define LEAVES_HAS_BUILTIN_MEMCPY 1
#else
#define FORCE_INLINE inline
#define NOINLINE
#endif

// Platform detection for SIMD and relaxed spin-wait helpers.
#if defined(__x86_64__) || defined(_M_X64)
#define LEAVES_X86_64 1
#if defined(__AVX512BW__)
#include <immintrin.h>
#define LEAVES_HAS_AVX512 1
#define LEAVES_HAS_AVX2 1
#define LEAVES_HAS_SSE2 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define LEAVES_HAS_AVX2 1
#define LEAVES_HAS_SSE2 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && _M_X64)
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

#ifndef __EMSCRIPTEN__
#include <boost/endian/arithmetic.hpp>
#endif

// Windows headers may define min/max as macros and break std::min/std::max.
#if defined(min)
#undef min
#endif
#if defined(max)
#undef max
#endif

namespace leaves {

typedef enum { READ = 0, WRITE = 1 } Access;

static constexpr int LEAVES_INVALID_FD = -1;

FORCE_INLINE bool fd_valid(int fd) { return fd != LEAVES_INVALID_FD; }

FORCE_INLINE uint32_t get_process_id() {
#ifdef _WIN32
  return static_cast<uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

FORCE_INLINE bool process_is_alive(uint32_t pid) {
  if (pid == 0) return false;
#ifdef _WIN32
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
  if (process) {
    ::CloseHandle(process);
    return true;
  }
  // Access denied still means the process exists but we cannot query it.
  return ::GetLastError() == ERROR_ACCESS_DENIED;
#else
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#endif
}

FORCE_INLINE bool file_is_readable(const char* path) {
#ifdef _WIN32
  return _access(path, 4) == 0;
#else
  return ::access(path, R_OK) == 0;
#endif
}

FORCE_INLINE std::string errno_message(int err) {
#ifdef _WIN32
  char buffer[128] = {};
  if (::strerror_s(buffer, sizeof(buffer), err) == 0) {
    return std::string(buffer);
  }
  return std::string("errno ") + std::to_string(err);
#else
  const char* msg = std::strerror(err);
  if (msg) return std::string(msg);
  return std::string("errno ") + std::to_string(err);
#endif
}

FORCE_INLINE int open_rw_fd(const char* path, bool create = false) {
#ifdef _WIN32
  int flags = _O_RDWR | _O_BINARY;
  if (create) flags |= _O_CREAT;
  int fd = LEAVES_INVALID_FD;
  errno_t rc = _sopen_s(&fd, path, flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
  if (rc != 0) {
    errno = static_cast<int>(rc);
    return LEAVES_INVALID_FD;
  }
  return fd;
#else
  int flags = O_RDWR;
  if (create) flags |= O_CREAT;
  return ::open(path, flags, 0644);
#endif
}

FORCE_INLINE void close_fd(int fd) {
  if (!fd_valid(fd)) return;
#ifdef _WIN32
  _close(fd);
#else
  ::close(fd);
#endif
}

template <typename NativeHandle>
FORCE_INLINE int duplicate_fd_from_mapping_native_handle(
    NativeHandle native_handle) {
#ifdef _WIN32
  HANDLE source = static_cast<HANDLE>(native_handle);
  if (!source || source == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return LEAVES_INVALID_FD;
  }

  HANDLE duplicate = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), source,
                         ::GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
    errno = EIO;
    return LEAVES_INVALID_FD;
  }

  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate),
                           _O_RDWR | _O_BINARY);
  if (!fd_valid(fd)) {
    ::CloseHandle(duplicate);
    return LEAVES_INVALID_FD;
  }
  return fd;
#else
  int source = static_cast<int>(native_handle);
  if (source < 0) {
    errno = EBADF;
    return LEAVES_INVALID_FD;
  }
  return ::dup(source);
#endif
}

FORCE_INLINE uint64_t fd_size(int fd) {
  if (!fd_valid(fd)) return 0;
#ifdef _WIN32
  __int64 end = _lseeki64(fd, 0, SEEK_END);
  return end < 0 ? 0 : static_cast<uint64_t>(end);
#else
  off_t end = ::lseek(fd, 0, SEEK_END);
  return end < 0 ? 0 : static_cast<uint64_t>(end);
#endif
}

FORCE_INLINE bool write_fd_all_at(int fd, uint64_t offset, const void* src,
                                  size_t n) {
  if (!fd_valid(fd)) return false;
  const char* p = static_cast<const char*>(src);
  size_t remaining = n;

#ifdef _WIN32
  intptr_t os_handle = _get_osfhandle(fd);
  if (os_handle == -1) return false;
  HANDLE handle = reinterpret_cast<HANDLE>(os_handle);

  while (remaining > 0) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD chunk =
        static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(MAXDWORD)));
    DWORD written = 0;
    if (!WriteFile(handle, p, chunk, &written, &ov) || written == 0) return false;

    p += written;
    remaining -= static_cast<size_t>(written);
    offset += static_cast<uint64_t>(written);
  }
  return true;
#else
  uint64_t off = offset;
  while (remaining > 0) {
    ssize_t written = ::pwrite(fd, p, remaining, static_cast<off_t>(off));
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) return false;

    p += written;
    remaining -= static_cast<size_t>(written);
    off += static_cast<uint64_t>(written);
  }
  return true;
#endif
}

FORCE_INLINE bool read_fd_all_at(int fd, uint64_t offset, void* dst, size_t n) {
  if (!fd_valid(fd)) return false;
  char* p = static_cast<char*>(dst);
  size_t remaining = n;

#ifdef _WIN32
  intptr_t os_handle = _get_osfhandle(fd);
  if (os_handle == -1) return false;
  HANDLE handle = reinterpret_cast<HANDLE>(os_handle);

  while (remaining > 0) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD chunk =
        static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(MAXDWORD)));
    DWORD got = 0;
    if (!ReadFile(handle, p, chunk, &got, &ov) || got == 0) return false;

    p += got;
    remaining -= static_cast<size_t>(got);
    offset += static_cast<uint64_t>(got);
  }
  return true;
#else
  uint64_t off = offset;
  while (remaining > 0) {
    ssize_t got = ::pread(fd, p, remaining, static_cast<off_t>(off));
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (got == 0) return false;

    p += got;
    remaining -= static_cast<size_t>(got);
    off += static_cast<uint64_t>(got);
  }
  return true;
#endif
}

FORCE_INLINE void resize_fd(int fd, uint64_t new_size);

FORCE_INLINE void ensure_file_size_at_least(const char* path, uint64_t min_size,
                                           bool create = false) {
  int fd = open_rw_fd(path, create);
  if (!fd_valid(fd)) {
    int err = errno ? errno : EIO;
    throw FileError("Failed to open file '" + std::string(path) + "': " +
                        errno_message(err),
                    err);
  }

  uint64_t current_size = fd_size(fd);
  if (current_size < min_size) {
    try {
      resize_fd(fd, min_size);
    } catch (...) {
      close_fd(fd);
      throw;
    }
  }

  close_fd(fd);
}

namespace detail {

FORCE_INLINE unsigned count_trailing_zeros_32(uint32_t x) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward(&index, x);
  return index;
#elif defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctz(x));
#else
  static const unsigned debruijn32[32] = {
      0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
      31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9};
  return debruijn32[((x & -x) * 0x077CB531U) >> 27];
#endif
}

FORCE_INLINE unsigned count_trailing_zeros_64(uint64_t x) {
#if defined(_MSC_VER)
  #if defined(_M_X64) || defined(_M_ARM64)
  unsigned long index;
  _BitScanForward64(&index, x);
  return index;
  #else
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
  if (static_cast<uint32_t>(x) != 0) {
    return count_trailing_zeros_32(static_cast<uint32_t>(x));
  }
  return 32 + count_trailing_zeros_32(static_cast<uint32_t>(x >> 32));
#endif
}

FORCE_INLINE unsigned count_leading_zeros_64(uint64_t x) {
#if defined(_MSC_VER)
  #if defined(_M_X64) || defined(_M_ARM64)
  unsigned long index;
  _BitScanReverse64(&index, x);
  return 63 - index;
  #else
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
  unsigned n = 0;
  if (x <= 0x00000000FFFFFFFFULL) {
    n += 32;
    x <<= 32;
  }
  if (x <= 0x0000FFFFFFFFFFFFULL) {
    n += 16;
    x <<= 16;
  }
  if (x <= 0x00FFFFFFFFFFFFFFULL) {
    n += 8;
    x <<= 8;
  }
  if (x <= 0x0FFFFFFFFFFFFFFFULL) {
    n += 4;
    x <<= 4;
  }
  if (x <= 0x3FFFFFFFFFFFFFFFULL) {
    n += 2;
    x <<= 2;
  }
  if (x <= 0x7FFFFFFFFFFFFFFFULL) {
    n += 1;
  }
  return n;
#endif
}

FORCE_INLINE bool is_little_endian() {
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#elif defined(_WIN32) || defined(__EMSCRIPTEN__) || defined(__x86_64__) || \
    defined(_M_X64) || defined(__i386__) || defined(_M_IX86) || \
    defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || \
    defined(_M_ARM)
  return true;
#else
  const uint16_t test = 1;
  return *reinterpret_cast<const uint8_t*>(&test) == 1;
#endif
}

}  // namespace detail

FORCE_INLINE void resize_fd(int fd, uint64_t new_size) {
  assert(fd_valid(fd));
  
#ifdef _WIN32
  errno_t rc = _chsize_s(fd, new_size);
  if (rc != 0) {
    int err = static_cast<int>(rc);
    throw FileError(
        "Failed to resize file descriptor " + std::to_string(fd) + " to " +
        std::to_string(new_size) + " bytes: " + errno_message(err),
        err);
  }
#else
  if (::ftruncate(fd, static_cast<off_t>(new_size)) != 0) {
    int err = errno ? errno : EIO;
    throw FileError(
        "Failed to resize file descriptor " + std::to_string(fd) + " to " +
        std::to_string(new_size) + " bytes: " + errno_message(err),
        err);
  }
#endif
}

FORCE_INLINE void sync_fd_data(int fd) {
  if (!fd_valid(fd)) {
    int err = EBADF;
    throw FileError("Failed to sync file descriptor " + std::to_string(fd) +
                        ": " + errno_message(err),
                    err);
  }
#ifdef _WIN32
  if (_commit(fd) != 0) {
    int err = errno ? errno : EIO;
    throw FileError("Failed to sync file descriptor " + std::to_string(fd) +
                        ": " + errno_message(err),
                    err);
  }
#elif defined(__APPLE__)
  if (::fsync(fd) != 0) {
    int err = errno ? errno : EIO;
    throw FileError("Failed to sync file descriptor " + std::to_string(fd) +
                        ": " + errno_message(err),
                    err);
  }
#else
  if (::fdatasync(fd) != 0) {
    int err = errno ? errno : EIO;
    throw FileError("Failed to sync file descriptor " + std::to_string(fd) +
                        ": " + errno_message(err),
                    err);
  }
#endif
}

FORCE_INLINE void cpu_relax() {
#if defined(LEAVES_X86_64)
  _mm_pause();
#elif defined(LEAVES_ARM64) && defined(_MSC_VER)
  __yield();
#elif defined(LEAVES_ARM64)
  __asm__ __volatile__("yield");
#endif
}

// Cross-platform prefetch function
FORCE_INLINE void prefetch(const void* ptr, Access access = READ) {
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
  if (access == READ)
    __builtin_prefetch(ptr, 0);
  else
    __builtin_prefetch(ptr, 1);
#elif defined(_MSC_VER)
  // On MSVC, use the equivalent prefetch intrinsic
  _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0);
#elif defined(__SUNPRO_CC)
  // For Oracle Developer Studio (formerly Sun Studio)
  if (access == READ)
    __builtin_prefetch(ptr, 0);
  else
    __builtin_prefetch(ptr, 1);
#else
  // Fallback: do nothing if no prefetch support
  (void)ptr;
#endif
}

// Cross-platform optimized memcpy used by internal storage helpers.
FORCE_INLINE void* optimized_memcpy(void* dest, const void* src, size_t n) {
  if (n <= 32) {
    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);

#ifdef LEAVES_HAS_BUILTIN_MEMCPY
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
  if (n >= 32768) {
    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);

    size_t align_bytes = (16 - ((uintptr_t)d & 15)) & 15;
    if (align_bytes > 0 && align_bytes < n) {
      memcpy(d, s, align_bytes);
      d += align_bytes;
      s += align_bytes;
      n -= align_bytes;
    }

    size_t chunks = n / 16;
    for (size_t i = 0; i < chunks; i++) {
      __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
      _mm_stream_si128(reinterpret_cast<__m128i*>(d), data);
      d += 16;
      s += 16;
    }
    _mm_sfence();

    size_t remainder = n & 15;
    if (remainder > 0) {
      memcpy(d, s, remainder);
    }
    return dest;
  }
#elif defined(LEAVES_HAS_NEON)
  if (n >= 32768) {
    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);

    size_t align_bytes = (16 - ((uintptr_t)d & 15)) & 15;
    if (align_bytes > 0 && align_bytes < n) {
      memcpy(d, s, align_bytes);
      d += align_bytes;
      s += align_bytes;
      n -= align_bytes;
    }

    size_t chunks = n / 16;
    for (size_t i = 0; i < chunks; i++) {
      uint8x16_t data = vld1q_u8(reinterpret_cast<const uint8_t*>(s));
      vst1q_u8(reinterpret_cast<uint8_t*>(d), data);
      d += 16;
      s += 16;
    }

    size_t remainder = n & 15;
    if (remainder > 0) {
      memcpy(d, s, remainder);
    }
    return dest;
  }
#endif

  return memcpy(dest, src, n);
}

// Endian types — boost on native, lightweight wrappers on Emscripten (WASM).
// WASM is always little-endian, so little-endian types are identity wrappers.
#ifdef __EMSCRIPTEN__

struct _big_uint64_t {
  uint64_t _be;
  constexpr _big_uint64_t() = default;
  constexpr _big_uint64_t(uint64_t v) : _be(__builtin_bswap64(v)) {}
  operator uint64_t() const { return __builtin_bswap64(_be); }
  _big_uint64_t& operator=(uint64_t v) { _be = __builtin_bswap64(v); return *this; }
};

using _little_uint64_t = uint64_t;
using _little_uint32_t = uint32_t;
using _little_uint16_t = uint16_t;

#else

using _big_uint64_t = boost::endian::big_uint64_t;
using _little_uint64_t = boost::endian::little_uint64_t;
using _little_uint32_t = boost::endian::little_uint32_t;
using _little_uint16_t = boost::endian::little_uint16_t;

#endif

}  // namespace leaves

#endif  // _LEAVES___PORT_HPP