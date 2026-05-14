#ifndef _LEAVES_CONFLUENCE_HPP
#define _LEAVES_CONFLUENCE_HPP

#include <memory>

#include "mmap.hpp"
#include "intern/multi/_confluence_db.hpp"

namespace leaves {

// ============================================================================
// ConfluenceCursor — public read/write cursor over a ConfluenceDB
// ============================================================================
//
// Write path:
//   cursor.start_transaction();
//   cursor.write_find(key);
//   cursor.value(v);       // insert/update
//   cursor.remove();       // delete
//   cursor.commit();
//
// Point read:
//   Slice v;
//   if (cursor.find(key, v)) { ... }
//
// Ordered iteration:
//   for (cursor.first(); cursor.is_valid(); cursor.next()) {
//     use cursor.key(), cursor.value();
//   }

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
class ConfluenceCursor {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using CursorImpl  = _ConfluenceCursor<StorageImpl, ConflictPolicy_>;
  using cursor_ptr  = std::shared_ptr<CursorImpl>;

  ConfluenceCursor() = default;
  ConfluenceCursor(storage_ptr storage, cursor_ptr cursor)
      : _storage(std::move(storage)), _cursor(std::move(cursor)) {}

  // --- Write path ---

  bool start_transaction(bool non_blocking = false) {
    return _cursor->start_transaction(non_blocking);
  }
  bool commit(bool sync = false) { return _cursor->commit(sync); }
  bool rollback()                { return _cursor->rollback(); }
  bool is_transaction_active() const { return _cursor->is_transaction_active(); }

  // Position the write cursor on key before calling value() or remove().
  void write_find(const Slice& key)   { _cursor->write_find(key); }
  bool write_is_valid() const         { return _cursor->write_is_valid(); }
  void value(const Slice& v)          { _cursor->value(v); }
  void remove()                       { _cursor->remove(); }

  // --- Point reads ---

  // Returns true and fills value_out if key exists (not deleted).
  bool find(const Slice& key, Slice& value_out) {
    return _cursor->find(key, value_out);
  }
  bool contains(const Slice& key) { return _cursor->contains(key); }

  // --- Ordered iteration ---

  bool  first()    { return _cursor->first(); }
  bool  next()     { return _cursor->next();  }
  bool  last()     { return _cursor->last();  }
  bool  prev()     { return _cursor->prev();  }

  bool  is_valid() const { return _cursor->is_valid(); }
  Slice key()      const { return _cursor->key();      }
  Slice value()    const { return _cursor->value();    }

 private:
  storage_ptr _storage;
  cursor_ptr  _cursor;
};

// ============================================================================
// ConfluenceDB — public wrapper over _ConfluenceDB
// ============================================================================
//
// Usage (MapStorage / default conflict policy):
//
//   auto storage = MapStorage::create("path.lvs");
//   ConfluenceDB<MapStorage> db(storage, "events");
//
//   auto cursor = db.cursor();
//   cursor.start_transaction();
//   cursor.write_find(Slice("key"));
//   cursor.value(Slice("value"));
//   cursor.commit();

template <typename Storage_, typename ConflictPolicy_ = _DefaultConflictPolicy>
class ConfluenceDB {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using DBImpl      = _ConfluenceDB<StorageImpl, ConflictPolicy_>;
  using Cursor      = ConfluenceCursor<Storage_, ConflictPolicy_>;

  // Opens (or creates) the named confluence database on the given storage.
  // auto_monitor: start the background merge thread immediately (default true).
  ConfluenceDB(storage_ptr storage, const char* name, bool auto_monitor = true)
      : _storage(storage), _tdb(storage, name, auto_monitor) {}

  // Create a cursor for reading and writing.
  Cursor cursor() {
    return Cursor(_storage, _tdb._internal()->create_confluence_cursor());
  }

  // --- Configuration (can be called at any time) ---

  // Merge a tributary into the main DB once this many writes have accumulated.
  void set_merge_write_threshold(uint32_t n) {
    _tdb._internal()->set_merge_write_threshold(n);
  }
  // Merge a tributary after it has been idle for this many seconds.
  void set_idle_timeout_seconds(uint64_t s) {
    _tdb._internal()->set_idle_timeout_seconds(s);
  }

  // --- Lifecycle ---

  // Crash recovery: merge all unclaimed tributaries left by a previous crash.
  void sanitize()                   { _tdb._internal()->sanitize(); }
  void start_monitor()              { _tdb._internal()->start_monitor(); }
  void cancel_monitor()             { _tdb._internal()->cancel_monitor(); }
  void merge_eligible_tributaries() { _tdb._internal()->merge_eligible_tributaries(); }
  // Merge all threshold/idle-eligible tributaries synchronously.
  void merge_now()                  { _tdb._internal()->merge_now(); }
  // Merge ALL free tributaries regardless of threshold/idle — use after a
  // write phase completes so reads see a clean single-source state.
  void merge_all_now()              { _tdb._internal()->merge_all_now(); }
  // Access the underlying implementation (for advanced / test use).
  DBImpl* _internal() const { return _tdb._internal(); }

 private:
  template <typename S>
  using _Impl = _ConfluenceDB<S, ConflictPolicy_>;

  storage_ptr          _storage;
  TDB<Storage_, _Impl> _tdb;
};

// Convenience aliases for the common mmap case with the default conflict policy.
using MapConfluenceDB     = ConfluenceDB<MapStorage>;
using MapConfluenceCursor = ConfluenceCursor<MapStorage>;

}  // namespace leaves

#endif  // _LEAVES_CONFLUENCE_HPP
