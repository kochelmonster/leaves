#ifdef __GNUC__

#define popcount __builtin_popcount
#define ffs __builtin_ffs
#define clz __builtin_clz

#endif

#ifdef _MSC_VER
#include <intrin.h>

#pragma intrinsic(_BitScanForward)
#pragma intrinsic(__popcnt, _BitScanForward, _BitScanReverse)
#define popcount __popcnt

inline int ffs(boost::uint32_t v) {
    unsigned long index;
    if (_BitScanForward(&index, v))
      return index+1;
    else
      return 0;
 }

inline int clz(boost::uint32_t v) {
    unsigned long index;
    if (_BitScanReverse(&index, v))
      return 31 - index;
    else
      return 32;
 }

#endif // _MSC_VER
