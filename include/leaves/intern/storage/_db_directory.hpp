#ifndef _LEAVES__DB_DIRECTORY_HPP
#define _LEAVES__DB_DIRECTORY_HPP

#include <cstdint>
#include <cstring>
#include "../core/_traits.hpp"  // for offset_t, K

namespace leaves {

struct _DBDirectoryEntry {
  char name[21];
  offset_t offset;
};

// Overflow directory page for linked DB entries.
// The first page is embedded in FileHeader; overflow pages are full 4K.
struct _DBDirectoryPage {
  uint16_t count;               // entries used in this page (high-water mark)
  offset_t next;                // link to next overflow page (0 = none)
  _DBDirectoryEntry entries[];  // flexible array fills rest of page

  static constexpr uint16_t capacity_for(size_t available_bytes) {
    return static_cast<uint16_t>(available_bytes / sizeof(_DBDirectoryEntry));
  }

  void init(size_t available_bytes) {
    count = 0;
    next = 0;
    uint16_t cap = capacity_for(available_bytes);
    std::memset(entries, 0, sizeof(_DBDirectoryEntry) * cap);
  }
};

}  // namespace leaves

#endif  // _LEAVES__DB_DIRECTORY_HPP
