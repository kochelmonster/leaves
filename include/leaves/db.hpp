#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include <memory>

#include "cursor.hpp"
#include "intern/db/_db.hpp"

namespace leaves {

template <typename Storage, template <typename> class DBClass = _DB>
class TDB {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  typedef DBClass<StorageImpl> DBImpl;
  typedef DBImpl db_type;
  typedef TCursor<Storage, DBClass> Cursor;

  TDB(storage_ptr storage, const char* name)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(name)) {}

  template <typename... Args>
  TDB(storage_ptr storage, const char* name, Args&&... args)
      : _storage(storage),
        _db(storage->_storage->template open<DBClass>(
            name, std::forward<Args>(args)...)) {}

  Cursor cursor() { return Cursor(_storage, _db); }

  Slice name() const { return _db->name(); }

  db_type* _internal() const { return _db; }

  storage_ptr storage() const { return _storage; }

  auto& aspect() { return _db->aspect(); }
  const auto& aspect() const { return _db->aspect(); }

  auto txn() const { return _db->txn(); }

  // Transaction management methods for crash recovery
  tid_t transaction_active() const { return _db->transaction_active(); }
  bool commit(bool sync = true) { return _db->commit(0, sync); }
  bool rollback() { return _db->rollback(0); }
  void defrag() { _db->defrag(); }

  // Replication configuration (only available on replicating storages)
  void set_retention(uint64_t seconds) { _db->set_retention(seconds); }

 private:
  storage_ptr _storage;
  DBImpl* _db;
};

}  // namespace leaves
#endif  // _LEAVES_DB_HPP