#include <iostream>
#include <leaves.hpp>

#include "storage.hpp"
#include "trace.hpp"

namespace leaves {

struct CursorImpl : public Cursor {
  CursorImpl(Storage& storage, DB::db_ptr pdb) : trace(storage), pdb(pdb) {}
  bool isvalid() const { return trace.isvalid(); }
  void find(const Slice& key) { return trace.find(key); }
  void first() { trace.first(); }
  void last() { trace.last(); }
  void next() { trace.next(); }
  void prev() { trace.prev(); }
  Slice key() const { return trace.current_key; }
  Slice value() const {
    if (trace.isvalid()) return trace.get_value();
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

  Storage storage;
  std::weak_ptr<DB> me;
};

DB::db_ptr DB::open(const char* path) {
  db_ptr result(new DBImpl(path));
  ((DBImpl*)result.get())->me = result;
  return result;
}

#ifdef DEBUG

void dump_node(std::ostream& out, const TrieBlock* page, node_ptr nid,
               Storage* storage, int upper);

void dump_db(std::ostream& out, DB::db_ptr db) {
  DBImpl* sdb(((DBImpl*)db.get()));
  const TrieBlock* root = &sdb->storage.memory->get_root()->trie;
  if (sdb->storage.transaction_active()) {
    root = &sdb->storage.get_txn_block(sdb->storage.memory->head.root)->trie;
  }
  dump_node(out, root, *root->resolve_ptr(0), &sdb->storage, -1);
}

uint64_t dump_info(std::ostream& out, DB::db_ptr db) {
  DBImpl* sdb(((DBImpl*)db.get()));
  tid_t txn_id = sdb->storage.memory->get_active_head()->txn_id;
  out << "Transaction Id: " << txn_id << std::endl;
  return txn_id;
}

#endif

}  // namespace leaves
