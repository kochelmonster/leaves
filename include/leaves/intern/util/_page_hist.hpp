/*
Page history tracking and lightweight metrics for allocator behavior.
*/
#ifndef _LEAVES__PAGE_HIST_HPP
#define _LEAVES__PAGE_HIST_HPP

// Optional raw-size histogram for the page allocator.
//
// Records the requested allocation size (including PageHeader) BEFORE it is
// rounded up to a bucket in PAGE_SIZES. Used by tools/page_sizes_solver.py to
// compute an optimized PAGE_SIZES set.
//
// Enabled at compile time by defining LEAVES_PAGE_HIST. When disabled, all
// functions are no-ops with zero runtime cost.

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace leaves {

// Upper bound on the histogram size. The current MemoryMapTraits caps page
// sizes at PAGE_CONTAINER_SIZE = 4 KiB, so 4097 bins covers every possible
// raw request including the largest 4096-byte page.
static constexpr size_t _PAGE_HIST_MAX = 4097;

#ifdef LEAVES_PAGE_HIST

inline std::atomic<uint64_t> _page_hist_bins[_PAGE_HIST_MAX] = {};

inline void _page_hist_record(uint32_t size) {
  if (size < _PAGE_HIST_MAX) {
    _page_hist_bins[size].fetch_add(1, std::memory_order_relaxed);
  }
}

inline void _page_hist_reset() {
  for (size_t i = 0; i < _PAGE_HIST_MAX; i++)
    _page_hist_bins[i].store(0, std::memory_order_relaxed);
}

// Append rows "tag,size,count" for every non-zero bin. Truncates if !append.
inline bool _page_hist_dump(const char* path, const char* tag,
                            bool append = true) {
  std::FILE* f = std::fopen(path, append ? "ab" : "wb");
  if (!f) return false;
  for (size_t i = 0; i < _PAGE_HIST_MAX; i++) {
    uint64_t c = _page_hist_bins[i].load(std::memory_order_relaxed);
    if (c)
      std::fprintf(f, "%s,%zu,%llu\n", tag, i, (unsigned long long)c);
  }
  std::fclose(f);
  return true;
}

#else  // LEAVES_PAGE_HIST not defined: all no-ops

inline void _page_hist_record(uint32_t) {}
inline void _page_hist_reset() {}
inline bool _page_hist_dump(const char*, const char*, bool = true) {
  return false;
}

#endif  // LEAVES_PAGE_HIST

}  // namespace leaves

#endif  // _LEAVES__PAGE_HIST_HPP
