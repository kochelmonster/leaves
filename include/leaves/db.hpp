#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include <memory>

#include "cursor.hpp"
#include "intern/db/_db.hpp"

namespace leaves {

template <typename Storage>
class TDB {
 public:
  typedef typename Storage::storage_ptr storage_ptr;
  typedef typename Storage::StorageImpl StorageImpl;
  typedef typename StorageImpl::DB DBImpl;
  typedef DBImpl db_type;
  typedef TCursor<Storage> Cursor;

  TDB(storage_ptr storage, const char* name)
      : _storage(storage), _db(storage->_storage->make(name)) {}

  Cursor cursor() { return Cursor(_storage, _db); }

  Slice name() const { return _db->name(); }

  db_type* _internal() const { return _db; }

  auto& aspect() { return _db->aspect(); }
  const auto& aspect() const { return _db->aspect(); }

  // Transaction management methods for crash recovery
  tid_t transaction_active() const { return _db->transaction_active(); }
  bool commit(bool sync = true) { return _db->commit(0, sync); }
  bool rollback() { return _db->rollback(0); }
  void defrag() { _db->defrag(); }

  // Replication configuration (only available on replicating storages)
  void set_retention(uint64_t seconds) { _db->set_retention(seconds); }

 private:
  storage_ptr _storage;
  DBImpl *_db;
};

}  // namespace leaves
#endif  // _LEAVES_DB_HPP