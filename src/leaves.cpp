#include <iostream>
#include <leaves.hpp>
#include "storage.hpp"
#include "trace.hpp"

namespace leaves {

struct CursorImpl : public Cursor {
  CursorImpl(Storage& storage, DB::db_ptr pdb) : trace(storage), pdb(pdb) {}
  bool valid() const { return trace.valid(); }
  void find(const Slice& key) { return trace.find(key); }
  void first() { trace.first(); }
  void last() { trace.last(); }
  void next() { trace.next(); }
  void prev() { trace.prev(); }
  Slice key() const { return trace.current_key; }
  Slice value() const {
    if (trace.valid()) return trace.get_value();
    throw NoValidPosition();
  }
  void set_value(const Slice& value) { trace.set_value(value); }
  void remove() { trace.remove(); }

  Trace trace;
  DB::db_ptr pdb;
};


DB::~DB() { }


struct DBImpl : public DB {
    DBImpl(const char *path, size_t size, size_t delta) : storage(path, size, delta) { }

    cursor_ptr create_cursor() {
      return cursor_ptr(new CursorImpl(storage, me.lock()));
    }

    void flush() {
      storage.output.flush();
    }

    Storage storage;
    std::weak_ptr<DB> me;
};


DB::db_ptr DB::open(const char *path, size_t size, size_t delta) {
  db_ptr result(new DBImpl(path, size, delta));
  ((DBImpl*)result.get())->me = result;
  return result;
}

#ifdef DEBUG

void dump_node(std::ostream& out, location_p loc, Storage* storage);

void dump_db(std::ostream& out, DB::db_ptr db) {
  DBImpl *sdb(((DBImpl*)db.get()));
  dump_node(out, sdb->storage->start.header.root, &sdb->storage);
}

#endif


} // namespace leaves
