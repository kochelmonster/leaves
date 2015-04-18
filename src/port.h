
#ifdef __GNUC__

#if defined(__LP64__) || defined(__x86_64__)
#define popcount __builtin_popcountll
#define ffs __builtin_ffsll
#define clz __builtin_clzll
#else
#define popcount32 __builtin_popcountl
#define ffs32 __builtin_ffsl
#define clz32 __builtin_clzl
#define _32BIT
#endif

#endif
#ifdef _MSC_VER
#include <intrin.h>

#ifdef _M_X64
#pragma intrinsic(__popcnt64, _BitScanForward64, _BitScanReverse64)
#define popcount __popcnt64

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
      return 63-index;
    else
      return 64;
 }

#else 

#pragma intrinsic(__popcnt, _BitScanForward, _BitScanReverse)
#define popcount32 __popcnt

inline int ffs32(boost::uint32_t v) {
    unsigned long index;
    if (_BitScanForward(&index, v)) 
      return index+1;
    else
      return 0;
 }

inline int clz32(boost::uint32_t v) {
    unsigned long index;
    if (_BitScanReverse(&index, v)) 
      return 31 - index;
    else
      return 32;
 }

#define _32BIT
#endif //_M_X64
#endif // _MSC_VER


#ifdef _32BIT
size_t popcount(boost::uint64_t v) {
    boost::uint32_t *v32 = (boost::uint32_t*)&v;
    return popcount32(v32[0]) + popcount32(v32[1])
  }
  
inline int ffs(boost::uint64_t v) {
    boost::uint32_t *v32 = (boost::uint32_t*)&v;
    if (v32[0])
      return ffs32(v32[0]);
     
    return ffs32(v32[1]) + 32;
  }

inline int ffs(boost::uint64_t v) {
    boost::uint32_t *v32 = (boost::uint32_t*)&v;
    if (v32[1])
      return clz32(v32[1]) + 32;
      
    return clz32(v32[0])
  }
 
#endif
