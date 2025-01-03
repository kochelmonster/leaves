#include <iostream>
#include <leaves.hpp>

#include "memory.hpp"
#include "trace.hpp"

namespace leaves {

struct CursorImpl : public Cursor {
  CursorImpl(DBMemory& storage, DB::db_ptr pdb) : trace(storage), pdb(pdb) {}
  bool is_valid() const { return trace.is_valid(); }
  void find(const Slice& key) { return trace.find(key); }
  void first() { trace.first(); }
  void last() { trace.last(); }
  void next() { trace.next(); }
  void prev() { trace.prev(); }
  Slice get_key() const { return trace.current_key; }
  Slice get_value() {
    if (trace.is_valid()) return trace.get_value();
    throw NoValidPosition();
  }
  void set_value(const Slice& value) { trace.set_value(value); }
  void remove() { trace.remove(); }
  void commit() { trace.commit(); }

  Trace trace;
  DB::db_ptr pdb;
};

DB::~DB() {}

struct DBImpl : public DB {
  DBImpl(const char* path) : storage(path) {}

  cursor_ptr create_cursor() {
    return cursor_ptr(new CursorImpl(storage, me.lock()));
  }

  DBMemory storage;
  std::weak_ptr<DB> me;
};

DB::db_ptr DB::open(const char* path) {
  db_ptr result(new DBImpl(path));
  ((DBImpl*)result.get())->me = result;
  return result;
}

#ifdef DEBUG

void dump_branch(std::ostream& out, offset_ptr offset, DBMemory* storage);


void dump_db(std::ostream& out, DB::db_ptr db) {
  DBImpl* sdb(((DBImpl*)db.get()));
  offset_ptr root = sdb->storage.active_txn()->root;

  if (sdb->storage.transaction_active()) {
    root = sdb->storage.txn.root;
  }
  return dump_branch(out, root, &sdb->storage);
}

uint64_t dump_info(std::ostream& out, DB::db_ptr db) {
  DBImpl* sdb(((DBImpl*)db.get()));
  tid_t txn_id = sdb->storage.active_txn()->txn_id;
  out << "Transaction Id: " << txn_id << std::endl;
  return txn_id;
}

#endif

}  // namespace leaves
