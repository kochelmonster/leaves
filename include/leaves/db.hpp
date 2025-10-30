#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include <memory>

#include "cursor.hpp"
#include "intern/_db.hpp"

namespace leaves {

template <typename StorageImpl>
class TDB {
 public:
  typedef typename StorageImpl::DB db_type;
  typedef typename StorageImpl::db_ptr db_ptr;
  typedef std::shared_ptr<StorageImpl> storage_ptr;
  typedef TCursor<typename StorageImpl::DB::Cursor, storage_ptr> Cursor;

  TDB(storage_ptr storage, const char* name)
      : _db(storage->make(name)), _storage(storage) {}

  Cursor cursor() { return Cursor(_storage, _db); }

  Slice name() const { return _db->name(); }

  db_ptr _internal() const { return _db; }

  // Transaction management methods for crash recovery
  tid_t transaction_active() const { return _db->transaction_active(); }
  bool commit(bool sync = true) { return _db->commit(0, sync); }
  bool rollback() { return _db->rollback(0); }

 private:
  db_ptr _db;
  storage_ptr _storage;
};

}  // namespace leaves
#endif  // _LEAVES_DB_HPP