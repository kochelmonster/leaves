#include "larch/leaves.h"

namespace larch_leaves {

static Slice main_name_space("::main", 6);

// find prefix common to s1 and s2
inline size_t prefix(const char* s1, const *char s2, size_t size) {
  size_t i;
  for(i = 0; i < size; i++, s1++, s2++) {
    if (*s1 != *s2)
      break;
  }
  return i;
}

// Cursor implementation
// ---------------------
bool Cursor::is_valid() const {
  if (_search_trace.empty())
    return false;
    
  return is_leaf(_search_trace.back());
}

void set(const Slice& key, int read_forward=100) {
  if (! is_valid())
    return;

  // to do less search we try to start from a common
  // prefix    
  size_t prefix_len = prefix(key.data(), _key.data(), 
                             std::min(key.size(), _key.size()));
  _search_trace.resize(prefix_len+1);
  _key.assign(key.data(), key.size());  
  
  NodeRef top = _search_trace.back();
  Slice rest_key(_key.data()+prefix_len, key.size()-prefix_len);
  
  for(size_t i = prefix_len; i < _key.size(); i++) {
    byte_t index = trie_index(*rest_key.data());
    top = find_next(_storage, top, index);
    
    if (!top.is_valid()) {
      top = _search_trace.back();
      break;
    }
    
    eat(top, rest_key);
    _search_trace.push_back(top);
  }
      
  if (is_leaf(top) && ! key_equal(top, rest_key)) {
    _search_trace.pop_back();
  }
}


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

struct MemoryCursor : public Cursor {
  Trace trace;
  NodeRef root;

};

MemoryStorage::MemoryStorage() {
  // create the main namespace
  std::string encoded = to_base64(main_name_space);
  Trace trace(_nodes, _nodes);
  trace.push(root());
  trace.add(TempPageLeaf(encoded, Slice()));
}


std::shared_ptr<Cursor> read_cursor(const Slice& namespace_) {
  std::string encoded = to_base64(namespace_);
  MemoryCursor cursor(_nodes);
  root().find(Slice(endcoded), cursor.trace);
  
}


std::shared_ptr<Cursor> write_cursor(const Slice& namespace_) {
  return 
}


} // namespace larch_leaves 
