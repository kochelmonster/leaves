#ifndef _LEAVES_CONFLUENCE_HPP
#define _LEAVES_CONFLUENCE_HPP

#include <cassert>
#include <memory>
#include <utility>
#include <string_view>

#include "intern/multi/_confluence_db.hpp"
#include "mmap.hpp"

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

template <typename Storage_, template <typename> class DBClass_ = _DB>
class ConfluenceCursor {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using MainDBImpl = DBClass_<StorageImpl>;
  using DBImpl = _ConfluenceDB<MainDBImpl>;
  using CursorImpl = _ConfluenceCursor<DBImpl>;
  using cursor_ptr = std::shared_ptr<CursorImpl>;

  ConfluenceCursor() = default;
  ConfluenceCursor(storage_ptr storage, cursor_ptr cursor)
      : _storage(std::move(storage)), _cursor(std::move(cursor)) {}

  // --- Write path ---

  // Starts a write transaction on this cursor's tributary.
  // Returns false if the slot cannot be claimed, another transaction is
  // already active, or the tributary cannot open a write transaction.
  bool start_transaction(bool non_blocking = false) {
    return _cursor->start_transaction(non_blocking);
  }

  // Commits the active transaction.
  // Returns false if no transaction is active or commit fails.
  bool commit(bool sync = false) { return _cursor->commit(sync); }

  // Rolls back the active transaction.
  // Returns false if no transaction is active or rollback fails.
  bool rollback() { return _cursor->rollback(); }

  // Returns true while a transaction is active.
  bool is_transaction_active() const {
    return _cursor->is_transaction_active();
  }

  // Inserts or updates value at the current position.
  void value(const Slice& v) { _cursor->value(v); }

  // Deletes the current record.
  void remove() { _cursor->remove(); }

  // --- Point reads ---

  // Seeks to key.
  void find(const Slice& key) { _cursor->find(key); }

  // --- Ordered iteration ---

  // Moves to first key and returns validity.
  bool first() {
    _cursor->first();
    return _cursor->is_valid();
  }

  // Moves to next key and returns validity.
  bool next() {
    _cursor->next();
    return _cursor->is_valid();
  }

  // Moves to last key and returns validity.
  bool last() {
    _cursor->last();
    return _cursor->is_valid();
  }

  // Moves to previous key and returns validity.
  bool prev() {
    _cursor->prev();
    return _cursor->is_valid();
  }

  // Returns current cursor validity.
  bool is_valid() const { return _cursor->is_valid(); }
  // Returns current key.
  Slice key() const { return _cursor->key(); }
  // Returns current value.
  Slice value() const { return _cursor->value(); }

 private:
  storage_ptr _storage;
  cursor_ptr _cursor;
};

// ============================================================================
// ConfluenceDB — public wrapper over _ConfluenceDB
// ============================================================================
//
// Usage (MapStorage / default conflict policy):
//
//   auto storage = MapStorage::create("path.lvs");
//   auto db = storage->open<MapStorage::ConfluenceDB>("events");
//
//   auto cursor = db.cursor();
//   cursor.start_transaction();
//   cursor.find(Slice("key"));
//   cursor.value(Slice("value"));
//   cursor.commit();

template <typename Storage_, template <typename> class DBClass_>
class ConfluenceDB {
 public:
  using storage_ptr = typename Storage_::storage_ptr;
  using StorageImpl = typename Storage_::StorageImpl;
  using MainDBImpl = DBClass_<StorageImpl>;
  using MainTDB = TDB<Storage_, DBClass_>;
  using ConcreteDBImpl = _ConfluenceDB<MainDBImpl>;
  using Cursor = ConfluenceCursor<Storage_, DBClass_>;
  template <typename AnyStorage>
  using DBWrapper = ConfluenceDB<AnyStorage, DBClass_>;
  template <typename AnyStorageImpl>
  using DBImpl = DBClass_<AnyStorageImpl>;

  ConfluenceDB() = default;

  ConfluenceDB(const ConfluenceDB&) = delete;
  ConfluenceDB& operator=(const ConfluenceDB&) = delete;

  ConfluenceDB(ConfluenceDB&& other)
      : _storage(std::move(other._storage)),
        _main_tdb(std::move(other._main_tdb)),
        _impl(other._impl) {
    other._impl = nullptr;
  }

  ConfluenceDB& operator=(ConfluenceDB&& other) {
    if (this == &other) {
      return *this;
    }

    delete _impl;
    _storage = std::move(other._storage);
    _main_tdb = std::move(other._main_tdb);
    _impl = other._impl;
    other._impl = nullptr;
    return *this;
  }

  // Opens or creates the named confluence database and manages main plus
  // tributary databases.
  ConfluenceDB(storage_ptr storage, std::string_view name)
      : _storage(std::move(storage)),
        _main_tdb(_storage, name),
        _impl(new ConcreteDBImpl(*_main_tdb._internal())) {}

  ~ConfluenceDB() { delete _impl; }

  operator bool() const {
    return _impl != nullptr;
  }

  // Returns a ConfluenceCursor bound to this database.
  Cursor cursor() {
    _assert_initialized();
    return Cursor(_storage, _impl->create_cursor());
  }

  // --- Configuration (can be called at any time) ---

  // Sets the tributary write-count threshold for merge eligibility.
  void set_merge_write_threshold(uint32_t n) {
    _assert_initialized();
    _impl->set_merge_write_threshold(n);
  }
  // Sets the max attach age before a tributary becomes merge-eligible.
  void set_max_attached_age_ms(uint64_t ms) {
    _assert_initialized();
    _impl->set_max_attached_age_ms(ms);
  }

  // --- Lifecycle ---

  // Performs synchronous merge of threshold- or idle-eligible tributaries.
  void merge_now() {
    _assert_initialized();
    _impl->merge_now();
  }
  // Forces merge of all free tributaries.
  void merge_all_now() {
    _assert_initialized();
    _impl->merge_all_now();
  }
  // Returns and clears the last asynchronous merge error.
  std::exception_ptr get_merge_error() {
    _assert_initialized();
    return _impl->get_merge_error();
  }

  ConcreteDBImpl* _internal() const {
    _assert_initialized();
    return _impl;
  }
  MainTDB& _internal_main() {
    _assert_initialized();
    return _main_tdb;
  }

 private:
  void _assert_initialized() const {
    assert(_storage);
    assert(_impl != nullptr);
  }

  storage_ptr _storage;
  MainTDB _main_tdb;  // keeps DB<StorageImpl>* alive via storage cache
  ConcreteDBImpl* _impl = nullptr;  // owned by this ConfluenceDB
};

template <typename Traits>
class MapStorage_<Traits>::ConfluenceDB
    : public ::leaves::ConfluenceDB<MapStorage_<Traits>, ::leaves::_DB> {
 public:
  using Base = ::leaves::ConfluenceDB<MapStorage_<Traits>, ::leaves::_DB>;
  template <typename AnyStorage>
  using DBWrapper = typename AnyStorage::ConfluenceDB;
  template <typename AnyStorageImpl>
  using DBImpl = ::leaves::_DB<AnyStorageImpl>;

  ConfluenceDB() = default;
  using Base::Base;
};

}  // namespace leaves

#endif  // _LEAVES_CONFLUENCE_HPP
