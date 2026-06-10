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
//   cursor.find(key);      // position cursor before insert/update/delete
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

template <typename Storage_,
          typename ConflictPolicy_ = _DefaultConflictPolicy,
          template <typename> class DBClass_ = _DB>
class ConfluenceCursor {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using MainDBImpl  = DBClass_<StorageImpl>;
  using DBImpl      = _ConfluenceDB<MainDBImpl, ConflictPolicy_>;
  using CursorImpl  = _ConfluenceCursor<DBImpl>;
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

  // Position the cursor on key before calling value() or remove().
  // The same find()/is_valid() are used for reads and to position writes —
  // during a write transaction, find() searches the snapshot taken at
  // start_transaction() including the writer's own uncommitted state.
  void value(const Slice& v)          { _cursor->value(v); }
  void remove()                       { _cursor->remove(); }

  // --- Point reads ---

  // Position the cursor on key. Use is_valid() / value() after this call.
  // During a write txn, also positions the write cursor for value()/remove().
  void find(const Slice& key) { _cursor->find(key); }

  // --- Ordered iteration ---

  bool  first()    { _cursor->first(); return _cursor->is_valid(); }
  bool  next()     { _cursor->next();  return _cursor->is_valid(); }
  bool  last()     { _cursor->last();  return _cursor->is_valid(); }
  bool  prev()     { _cursor->prev();  return _cursor->is_valid(); }

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
//   cursor.find(Slice("key"));
//   cursor.value(Slice("value"));
//   cursor.commit();

template <typename Storage_,
          typename ConflictPolicy_ = _DefaultConflictPolicy,
          template <typename> class DBClass_ = _DB>
class ConfluenceDB {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using MainDBImpl  = DBClass_<StorageImpl>;
  using MainTDB     = TDB<Storage_, DBClass_>;
  using DBImpl      = _ConfluenceDB<MainDBImpl, ConflictPolicy_>;
  using Cursor      = ConfluenceCursor<Storage_, ConflictPolicy_, DBClass_>;

  ConfluenceDB(const ConfluenceDB&) = delete;
  ConfluenceDB& operator=(const ConfluenceDB&) = delete;

  // Opens (or creates) the named confluence database on the given storage.
  // Background merging runs automatically on the storage thread pool.
  ConfluenceDB(storage_ptr storage, const char* name)
      : _storage(storage),
        _main_tdb(storage, name),
        _impl(new DBImpl(*_main_tdb._internal())) {}

  ~ConfluenceDB() { delete _impl; }

  // Create a cursor for reading and writing.
  Cursor cursor() {
    return Cursor(_storage, _impl->create_cursor());
  }

  // --- Configuration (can be called at any time) ---

  // Merge a tributary into the main DB once this many writes have accumulated.
  void set_merge_write_threshold(uint32_t n) { _impl->set_merge_write_threshold(n); }
  // Merge a tributary once it has been attached this many milliseconds since
  // its last write.
  void set_max_attached_age_ms(uint64_t ms)  { _impl->set_max_attached_age_ms(ms); }

  // --- Lifecycle ---

  // Crash recovery: merge all unclaimed tributaries left by a previous crash.
  void sanitize()                   { _impl->sanitize(); }
  void merge_eligible_tributaries() { _impl->merge_eligible_tributaries(); }
  // Merge all threshold/idle-eligible tributaries synchronously.
  void merge_now()                  { _impl->merge_now(); }
  // Merge ALL free tributaries regardless of threshold/idle — use after a
  // write phase completes so reads see a clean single-source state.
  void merge_all_now()              { _impl->merge_all_now(); }
  // Access the underlying implementation (for advanced / test use).
  // Returns the stored async merge error (if any) and clears it.
  // One-shot: first-error-wins (subsequent errors are silently dropped).
  // Returns nullptr if no error.  Safe to call from any thread.
  std::exception_ptr get_merge_error() { return _impl->get_merge_error(); }

  DBImpl* _internal() const { return _impl; }

 private:
  storage_ptr  _storage;
  MainTDB      _main_tdb;  // keeps _DB<StorageImpl>* alive via storage cache
  DBImpl*      _impl;      // owned by this ConfluenceDB
};

// Convenience aliases for the common mmap case with the default conflict policy.
using MapConfluenceDB     = ConfluenceDB<MapStorage>;
using MapConfluenceCursor = ConfluenceCursor<MapStorage>;

}  // namespace leaves

#endif  // _LEAVES_CONFLUENCE_HPP
