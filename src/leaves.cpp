#include "larch/leaves.h"

namespace larch_leaves {

static Slice main_name_space("::main", 6);

// A Cursor to a non exising namespace
class NoWhereCursor : public Cursor {
 public:
  bool is_valid() const { return false; }
  void set(const Slice& key, int read_forward=100) { }
  void first(int read_forward=100) { }
  void last(int read_forward=-100) { }
  void next(int read_forward=100) { }
  void prev(int read_forward=-100) { }
  
  Slice key() const { throw NoValidPosition(); }
  Slice value() const { throw NoValidPosition(); }
  
  void set_value(const Slice& value) { throw NotImpented(); }
};


// Storage implemenation
// ---------------------
Storage::~Storage() {
  _node_memory_manager->flush(false);
  _leaf_memory_manager->flush(false);
}

std::shared_ptr<Storage> Storage::open(const char* path, const Options& options) {
  if (path) {
      if (options.multiprocess)
        _node_memory_manager = new MultiProcessNodeMemoryManager(path);
      else
        _node_memory_manager = new PersistentNodeMemoryManager(path);
        
      _leaf_memory_manager = new PersistentLeafMemoryManager(
          path, options.max_leaf_size);
  }
  else {
    _node_memory_manager = new HeapNodeMemoryManager();
    _leaf_memory_manager = new HeapLeafMemoryManager();
  }
}

struct ReadMemoryCursor : public Cursor {
  Trace trace;
  NodeRef root;
  std::string decoded_key;
  
  MemoryCursor(NodeStorageInHeap& nodes, NodeRef root_) 
    : trace(node, node), root(root_) { }
    
  bool is_valid() const { return trace.complete }
  void set(const Slice& key, int read_forward=100) { 
      std::string encoded = to_base64(key);
      Slice ekey(encoded);
      if (trace.find(ekey))
        return;
      trace.reset();
      root.find(ekey, trace);
    }
    
  void first(int read_forward=100) { 
      root.first(trace);
    }
  void last(int read_forward=-100) { 
      root.last(trace);
    }
  void next(int read_forward=100) { 
      if (trace.size())
        trace.current().next(trace);
    }
  void prev(int read_forward=-100) {
      if (trace.size())
        trace.current().prev(trace);
    }
  
  Slice key() const { 
    decoded_key = decode(trace.key());
    return Slice(decoded_key);
  }
  Slice value() const { 
    return trace.value();
  }
  
  void set_value(const Slice& value) { throw NotImpented(); }
};

struct WriteMemoryCursor : public ReadMemoryCursor {
  void set_value(const Slice& value) {
    trace.set_value(value);
  }
}



MemoryStorage::MemoryStorage() {
  Trace trace(_nodes, _nodes);
  trace.push(root());
  trace.add(TempTrie());
}


std::shared_ptr<Cursor> read_cursor(const Slice& namespace_) {
  std::string encoded = to_base64(namespace_);
  Trace trace(_nodes, _nodes);
  root().find(Slice(encoded), trace);
  if (!trace.complete) {
    // namespace does not exist
    return new NoWhereCursor();
  }
  
  return new ReadMemoryCursor(_nodes, trace.current());
}


std::shared_ptr<Cursor> write_cursor(const Slice& namespace_) {
  std::string encoded = to_base64(namespace_);
  Slice ekey(encoded);
  Trace trace(_nodes, _nodes);
  trace.push(root());
  root().find(ekey, trace);
  
  if (!trace.complete) {
    TempTrie ns;
    trace.current().add_node(ekey.advance(trace.key.size()), ns)
  }
  
  return new WriteMemoryCursor(_nodes, trace.current());
}


} // namespace larch_leaves 
