#ifndef _LEAVES_MMAP_HPP
#define _LEAVES_MMAP_HPP

#include "intern/_mmap.hpp"

namespace leaves {

template <typename CursorImpl>
class TCursor {
 public:
  template <typename DB>
  TCursor(DB db) : _cursor(db) {}

  tid_t txn_id() const { return _cursor.txn_id(); }

  // return true if the cursor is on a valid position
  bool is_valid() const { return _cursor.is_valid(); }

  void find(const Slice& key) { _cursor.find(key); }

  void first() { _cursor.first(); }

  void last() { _cursor.last(); }

  void next() { _cursor.next(); }

  void prev() { _cursor.prev(); }

  void value(const Slice& value) { _cursor.value(value); }

  Slice value() const { return _cursor.value(); }

  Slice key() const { return _cursor.key(); }

  void remove() { _cursor.remove(); }

  void prepare_commit() { _cursor.prepare_commit(); }

  void commit() { _cursor.commit(); }

  void rollback() { _cursor.rollback(); }

 private:
  CursorImpl _cursor;
};

class MapStorage {
 private:
  typedef _MemoryMapFile<_MemoryMapTraits> IMapStorage;
  
 public:
  typedef TCursor<IMapStorage::DB::Cursor> Cursor;
  class DB {
   public:
    typedef IMapStorage::DB db_type;

    DB(IMapStorage::db_ptr db) : _db(db) {}

    Cursor cursor() { return Cursor(_db); }

    bool start_transaction(bool wait = false) {
      return _db->start_transaction(wait);
    }
    void prepare_commit() { _db->prepare_commit(); }
    void commit() { _db->commit(); }
    Slice name() const { return _db->name(); }

   private:
    IMapStorage::db_ptr _db;

    const db_type& dump_storage() const { return *_db; }

    template <typename T>
    friend class _Dumper;
  };

  MapStorage(const char* path, size_t map_size = 2 * G, uint16_t db_count = 48)
      : _storage(path, map_size, db_count) {}

  DB operator[](const char* name) { return DB(_storage.make(name)); }
  void remove_db(const char* name) { _storage.remove_db(name); }
  void list_dbs(std::vector<std::string>& result) {
    return _storage.list_dbs(result);
  }
  Slice filename() const { return Slice(_storage.filename()); }
  size_t file_size() const { return _storage._memory->file_size; }

 private:
  IMapStorage _storage;
};


}  // namespace leaves

#endif  // _LEAVES_MMAP_HPP