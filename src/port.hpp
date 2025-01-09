#ifdef __GNUC__

#define popcount __builtin_popcountll
#define clz __builtin_clzll
#define ctz __builtin_ctzll

#endif

#ifdef _MSC_VER
#include <intrin.h>

#pragma intrinsic(_BitScanForward)
#pragma intrinsic(__popcnt, _BitScanForward, _BitScanReverse)
#define popcount __popcnt

inline int ffs(boost::uint64_t v) {
    unsigned long index;
    if (_BitScanForward64(&index, v))
      return index+1;
    else
      return 0;
 }

inline int clz(boost::uint64_t v) {
    unsigned long index;
    if (_BitScanReverse64(&index, v))
      return 64 - index;
    else
      return 64;
 }

#endif // _MSC_VER
