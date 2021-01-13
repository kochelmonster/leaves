#ifdef DEBUG
#include <iostream>
#endif

#include "trace.hpp"
#ifdef BURST
#include "table.hpp"
#endif


namespace leaves {

Slice version() {
  return Slice("0.0.1");
}


struct CursorImpl : public Cursor {
  CursorImpl(Storage& storage, DB::ptr pdb) : trace(storage), pdb(pdb) {}
  bool valid() const { return trace.valid(); }
  void find(const Slice& key) { return trace.find(key); }
  void first() { trace.first(); }
  void last() { trace.last(); }
  void next() { trace.next(); }
  void prev() { trace.prev(); }
  Slice key() const { return trace.current_key.slice(); }
  Slice value() const {
    if (trace.valid()) return trace.get_value();
    throw NoValidPosition();
  }
  void set_value(const Slice& value) { trace.set_value(value); }
  void remove() { trace.remove(); }

  Trace trace;
  DB::ptr pdb;
};


DB::~DB() { }


struct SingleDB : public DB {
    SingleDB(const char *path, const Options& options)
      : storage(path, options) { }

    cursor_ptr create_cursor() {
      return cursor_ptr(new CursorImpl(storage, me.lock()));
    }

    void flush(bool async) {
      storage.flush(async);
    }

    void get_stats(Stats& stats) {
      storage.get_stats(stats);
    }

    Storage storage;
    std::weak_ptr<DB> me;
};


DB::ptr DB::open(const char *path, const Options& options) {
  ptr result(new SingleDB(path, options));
  ((SingleDB*)result.get())->me = result;
  return result;
}

#ifdef DEBUG

void dump_node(std::ostream& out, any_ptr ptr, Storage* storage);

void dump_db(std::ostream& out, DB::ptr db) {
  SingleDB *sdb(((SingleDB*)db.get()));
  dump_node(out, sdb->storage.header->root, &sdb->storage);
}

#endif


} // namespace leaves
