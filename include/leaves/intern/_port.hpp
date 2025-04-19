#ifndef _LEAVES___PORT_HPP
#define _LEAVES___PORT_HPP

#if defined(_MSC_VER)
#include <xmmintrin.h> // For _mm_prefetch and _MM_HINT_T0 on MSVC
#endif

namespace leaves {

// Cross-platform prefetch function
inline void prefetch(const void* ptr) {
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
    __builtin_prefetch(ptr);
#elif defined(_MSC_VER)
    // On MSVC, use the equivalent prefetch intrinsic
    _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0);
#elif defined(__SUNPRO_CC)
    // For Oracle Developer Studio (formerly Sun Studio)
    __builtin_prefetch(ptr);
#else
    // Fallback: do nothing if no prefetch support
    (void)ptr;
#endif
}

} // namespace leaves

#endif // _LEAVES__PORT_HPP