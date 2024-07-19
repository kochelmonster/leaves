#include <iostream>
#include <leaves.hpp>
#include "storage.hpp"
#include "trace.hpp"

namespace leaves {

struct CursorImpl : public Trace {
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
  void commit() { trace.commit(); }

  Trace trace;
  DB::db_ptr pdb;
};


DB::~DB() { }


struct DBImpl : public DB {
    DBImpl(const char *path) : storage(path) { }

    cursor_ptr create_cursor() {
      return cursor_ptr(new CursorImpl(storage, me.lock()));
    }

    Storage storage;
    std::weak_ptr<DB> me;
};


DB::db_ptr DB::open(const char *path) {
  db_ptr result(new DBImpl(path));
  ((DBImpl*)result.get())->me = result;
  return result;
}

#ifdef DEBUG

void dump_node(std::ostream& out, Page* page, node_t nid, Storage* storage);

void dump_db(std::ostream& out, DB::db_ptr db) {
  DBImpl *sdb(((DBImpl*)db.get()));
  uint64_t transaction_id = sdb->storage.transaction_id();
  int idx = transaction_id % TRANSACTION_COUNT;
  stored_ptr root = sdb->storage.view->get_header()->txn[idx].root;
  dump_node(out, root.get<Page>(sdb->storage.view.get()), 0, &sdb->storage);
}

uint64_t dump_info(std::ostream& out, DB::db_ptr db) {
  DBImpl *sdb(((DBImpl*)db.get()));
  out << "Transaction Id: " << sdb->storage.transaction_id() << std::endl;
  return sdb->storage.transaction_id();
}

#endif


} // namespace leaves
