#include "trace.hpp"
#include <iostream>

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
  Slice key() const { return trace.current_key; }
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

    void flush() {
        storage.flush();
    }

    void get_stats(Stats& stats) {
      stats.value_pool_start_size = storage.value_pool_start_size;
      stats.value_pool_increment = storage.value_pool_increment;
      stats.value_pool_count = storage.value_pool_count;
      stats.area_count = storage.pools[0].pool->area_size / storage.pools[0].pool->node_size;
      stats.segment_size = storage.segment_size;
      stats.segment_count = storage.segments.size();
      for(size_t i = 0; i < POOL_COUNT; i++) {
        stats.used_nodes[i] = storage.pools[i].pool->used_nodes;
        stats.freed_nodes[i] = storage.pools[i].pool->freed_nodes;
      }
      for(size_t i = 0; i < storage.value_pool_count; i++) {
        stats.value_used_nodes[i] = storage.value_pools[i].pool->used_nodes;
        stats.value_freed_nodes[i] = storage.value_pools[i].pool->freed_nodes;
      }
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

void dump_node(std::ostream& out, segment_ptr ptr, Storage* storage);

void dump_db(std::ostream& out, DB::ptr db) {
  SingleDB *sdb(((SingleDB*)db.get()));
  dump_node(out, *sdb->storage.start, &sdb->storage);
}

#endif


} // namespace leaves
