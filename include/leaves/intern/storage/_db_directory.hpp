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

// Type-erased DB slot for caching opened DB instances.
// Stores function pointers for type-specific operations without
// requiring virtual methods or knowledge of the concrete DB type.
struct _DBSlot {
  void* db = nullptr;
  uint16_t type_id = 0;
  void (*deleter)(void*) = nullptr;
  void (*return_areas_fn)(void*) = nullptr;
  bool (*is_active_fn)(const void*) = nullptr;

  _DBSlot() = default;

  template <typename DB>
  static _DBSlot make(DB* ptr) {
    _DBSlot slot;
    slot.db = ptr;
    slot.type_id = DB::DB_TYPE_ID;
    slot.deleter = [](void* p) { delete static_cast<DB*>(p); };
    slot.return_areas_fn = [](void* p) { static_cast<DB*>(p)->return_areas(); };
    slot.is_active_fn = [](const void* p) {
      return static_cast<const DB*>(p)->is_active();
    };
    return slot;
  }

  ~_DBSlot() { reset(); }

  void reset() {
    if (deleter && db) {
      deleter(db);
      db = nullptr;
    }
  }

  _DBSlot(_DBSlot&& o) noexcept
      : db(o.db),
        type_id(o.type_id),
        deleter(o.deleter),
        return_areas_fn(o.return_areas_fn),
        is_active_fn(o.is_active_fn) {
    o.db = nullptr;
  }

  _DBSlot& operator=(_DBSlot&& o) noexcept {
    if (this != &o) {
      reset();
      db = o.db;
      type_id = o.type_id;
      deleter = o.deleter;
      return_areas_fn = o.return_areas_fn;
      is_active_fn = o.is_active_fn;
      o.db = nullptr;
    }
    return *this;
  }

  _DBSlot(const _DBSlot&) = delete;
  _DBSlot& operator=(const _DBSlot&) = delete;
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
