#ifndef _LEAVES_DB_HPP
#define _LEAVES_DB_HPP

#include "cursor.hpp"
#include "intern/_db.hpp"

namespace leaves {

template <typename StorageImpl>
class TDB {
 public:
  typedef typename StorageImpl::DB db_type;
  typedef typename StorageImpl::db_ptr db_ptr;
  typedef TCursor<typename StorageImpl::DB::Cursor> Cursor;

  TDB(db_ptr db) : _db(db) {}

  Cursor cursor() { return Cursor(_db); }

  Slice name() const { return _db->name(); }

  db_ptr _internal() const { return _db; }

 private:
  db_ptr _db;
};

}  // namespace leaves
#endif  // _LEAVES_DB_HPP