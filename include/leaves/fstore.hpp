#ifndef _LEAVES_FSTORE_HPP
#define _LEAVES_FSTORE_HPP

#include "cursor.hpp"
#include "intern/_fstore.hpp"

namespace leaves {

class FileStorage {
 public:
  typedef TCursor<_FileStore::DB::Cursor> Cursor;
  class DB {
   public:
    typedef _FileStore::DB db_type;

    DB(_FileStore::db_ptr db) : _db(db) {}

    Cursor cursor() { return Cursor(_db); }
    Slice name() const { return _db->name(); }

   private:
    _FileStore::db_ptr _db;

    const db_type& dump_db() const { return *_db; }

    template <typename T>
    friend class _Dumper;
  };

  FileStorage(const char* path, uint16_t db_count = 48, size_t cache_capacity = 500 * M)
      : _storage(path, db_count, cache_capacity) {}

  DB operator[](const char* name) { return DB(_storage.make(name)); }
  void remove_db(const char* name) { _storage.remove_db(name); }
  void list_dbs(std::vector<std::string>& result) {
    return _storage.list_dbs(result);
  }
  Slice filename() const { return Slice(_storage.filename()); }
  size_t file_size() const { return _storage.file_size(); }

  void debug_reset() { _storage.debug_reset(); }
  void debug_check_cache() const { _storage.debug_check_cache(); }

 private:
  _FileStore _storage;
};


}  // namespace leaves

#endif  // _LEAVES_FSTORE_HPP