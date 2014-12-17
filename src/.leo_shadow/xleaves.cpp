//@+leo-ver=4-thin
//@+node:michael.20141215222649.45:@shadow leaves.cpp
//@@language cplusplus
//@@tabwidth -2
//@<< includes >>
//@+node:michael.20141217010530.10:<< includes >>
#include "larch/leaves.h"
//@nonl
//@-node:michael.20141217010530.10:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.104:class Cursor (Implementation)
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
//@+others
//@+node:michael.20141215222649.198:is_valid()
bool Cursor::is_valid() const {
  if (_search_trace.empty())
    return false;
    
  return is_leaf(_search_trace.back());
}

//@-node:michael.20141215222649.198:is_valid()
//@+node:michael.20141215222649.203:set()
void set(const Slice& key, int read_forward=100) {
  if (! is_valid())
    return;

  // to do less search we try to start from a common
  // prefix    
  size_t prefix_len = prefix(key.data(), _key.data(), 
                             std::min(key.size(), _key.size()));
  _search_trace.resize(prefix_len+1);
  _key.assign(key.data(), key.size());  
  
  NodePtr top = _search_trace.back();
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

//@-node:michael.20141215222649.203:set()
//@-others

//@-node:michael.20141215222649.104:class Cursor (Implementation)
//@+node:michael.20141215222649.44:class Storage (Implementation)
// Storage implemenation
// ---------------------
//@+others
//@+node:michael.20141215222649.189:~Storage()
Storage::~Storage() {
  _node_memory_manager->flush(false);
  _leaf_memory_manager->flush(false);
}

//@-node:michael.20141215222649.189:~Storage()
//@+node:michael.20141215222649.188:open()
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

//@-node:michael.20141215222649.188:open()
//@+node:michael.20141215222649.190:cursor()
std::shared_ptr<Cursor> Storage::cursor(const Slice& namespace_, 
                                        bool signed_compare=false) {
  if signed_compare
    return new SignedNamespaceCursor(*this, namespace_);
  
  return new UnsignedNamespaceCursor(*this, namespace_);
}

std::shared_ptr<Cursor> Storage::cursor(bool signed_compare=false) {
  if (signed_compare)
    return new SignedCursor(*this);
    
  return new UnsignedCursor(*this);
}

//@-node:michael.20141215222649.190:cursor()
//@-others
//@-node:michael.20141215222649.44:class Storage (Implementation)
//@-others
} // namespace larch_leaves 
//@-node:michael.20141215222649.45:@shadow leaves.cpp
//@-leo
