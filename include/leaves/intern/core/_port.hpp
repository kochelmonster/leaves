#ifndef _LEAVES___PORT_HPP
#define _LEAVES___PORT_HPP

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

#include "_util.hpp"

#if defined(_MSC_VER)
#include <xmmintrin.h>  // For _mm_prefetch and _MM_HINT_T0 on MSVC
#define FORCE_INLINE __forceinline
#define NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#else
#define FORCE_INLINE inline
#define NOINLINE
#endif

#ifndef __EMSCRIPTEN__
#include <boost/endian/arithmetic.hpp>
#endif

namespace leaves {

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