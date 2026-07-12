#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include <cassert>
#include <memory>
#include <utility>
#include <string_view>

#include "cursor.hpp"
#include "intern/db/_db.hpp"

namespace leaves {

template <typename Storage, template <typename> class DBClass = _DB>
class TDB {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  using db_impl_type = DBClass<StorageImpl>;
  typedef db_impl_type db_type;
  typedef TCursor<Storage, DBClass> Cursor;
  template <typename AnyStorage>
  using DBWrapper = TDB<AnyStorage, DBClass>;
  template <typename AnyStorageImpl>
  using DBImpl = DBClass<AnyStorageImpl>;

  TDB() = default;

  TDB(storage_ptr storage, std::string_view name)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(name)) {}

  template <typename... Args>
  TDB(storage_ptr storage, std::string_view name, Args&&... args)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(
            name, std::forward<Args>(args)...)) {}

  operator bool() const {
    return _db != nullptr;
  }

  // Returns a new TCursor bound to this database.
  Cursor cursor() {
    _assert_initialized();
    return Cursor(_storage, _db);
  }

  // Returns the database name.
  Slice name() const {
    _assert_initialized();
    return _db->name();
  }

  db_type* _internal() const {
    _assert_initialized();
    return _db;
  }

  // Returns the owning storage shared pointer.
  storage_ptr storage() const {
    _assert_initialized();
    return _storage;
  }

  // Returns mutable access to the database aspect object.
  auto& aspect() {
    _assert_initialized();
    return _db->aspect();
  }
  // Returns read-only access to the database aspect object.
  const auto& aspect() const {
    _assert_initialized();
    return _db->aspect();
  }

  // Returns the current transaction descriptor.
  auto txn() const {
    _assert_initialized();
    return _db->txn();
  }

  // Returns the active transaction id used for crash-recovery tracking.
  tid_t transaction_active() const {
    _assert_initialized();
    return _db->transaction_active();
  }

  // Commits current database transaction state.
  // Returns false if no transaction is active, an aspect hook vetoes commit,
  // or the underlying commit fails. Set sync = false to skip fsync.
  bool commit(bool sync = true) {
    _assert_initialized();
    return _db->commit(0, sync);
  }

  // Discards changes made since the last commit.
  // Returns false if no transaction is active or rollback is vetoed.
  bool rollback() {
    _assert_initialized();
    return _db->rollback(0);
  }

  // Performs in-place compaction to reclaim fragmented storage.
  void defrag() {
    _assert_initialized();
    _db->defrag();
  }

  // Sets replication-history retention time.
  void set_retention(uint64_t seconds) {
    _assert_initialized();
    _db->set_retention(seconds);
  }

 private:
  void _assert_initialized() const {
    assert(_storage);
    assert(_db != nullptr);
  }

  storage_ptr _storage;
  db_impl_type* _db = nullptr;
};

}  // namespace leaves
#endif  // _LEAVES_DB_HPP