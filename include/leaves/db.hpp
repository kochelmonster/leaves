#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include <cassert>
#include <memory>
#include <utility>

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

  TDB(storage_ptr storage, const char* name)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(name)) {}

  template <typename... Args>
  TDB(storage_ptr storage, const char* name, Args&&... args)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(
            name, std::forward<Args>(args)...)) {}

  operator bool() const {
    return _db != nullptr;
  }

  Cursor cursor() {
    _assert_initialized();
    return Cursor(_storage, _db);
  }

  Slice name() const {
    _assert_initialized();
    return _db->name();
  }

  db_type* _internal() const {
    _assert_initialized();
    return _db;
  }

  storage_ptr storage() const {
    _assert_initialized();
    return _storage;
  }

  auto& aspect() {
    _assert_initialized();
    return _db->aspect();
  }
  const auto& aspect() const {
    _assert_initialized();
    return _db->aspect();
  }

  auto txn() const {
    _assert_initialized();
    return _db->txn();
  }

  // Transaction management methods for crash recovery
  tid_t transaction_active() const {
    _assert_initialized();
    return _db->transaction_active();
  }
  bool commit(bool sync = true) {
    _assert_initialized();
    return _db->commit(0, sync);
  }
  bool rollback() {
    _assert_initialized();
    return _db->rollback(0);
  }
  void defrag() {
    _assert_initialized();
    _db->defrag();
  }

  // Replication configuration (only available on replicating storages)
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