//@+leo-ver=5-thin
//@+node:michael.20141215222649.45: * @file leaves.cpp
//@@language cplusplus
//@@tabwidth -2
//@+<< includes >>
//@+node:michael.20141217010530.10: ** << includes >>
#include "larch/leaves.h"
#include "node.h"
#include "leaf.h"

//@-<< includes >>

namespace larch_leaves {
void encode(const Slice& input, std::string& output);
void decode(const std::string& input, std::string& output);
  
Slice main_name_space("::main", 6);

//@+others
//@+node:michael.20141215222649.104: ** class NoWhereCursor
// A Cursor to a non exising namespace
class NoWhereCursor : public Cursor {
 public:
  bool is_valid() const { return false; }
  void set(const Slice& key, int read_forward=100) { }
  void first(int read_forward=100) { }
  void last(int read_forward=-100) { }
  void next(int read_forward=100) { }
  void prev(int read_forward=-100) { }
  
  Slice key() { throw NoValidPosition(); }
  Slice value() { throw NoValidPosition(); }
  
  void set_value(const Slice& value) { throw NotImplemented(); }
  void remove() { throw NotImplemented(); }
};


//@+node:michael.20141230111914.24: ** MemoryDatabase
struct PrivateMemoryDatabase : public MemoryDatabase {
  NodeStorageInHeap nodes;
  Trace trace;
  std::string coding_buffer;
  size_t _count;

  PrivateMemoryDatabase() : trace(nodes, nodes), _count(0) {
      reinit();
    }

  PrivateMemoryDatabase(const std::string& data) : trace(nodes, nodes), _count(0) {
      //_nodes.load(data);
    }

  void reinit() {
      assert(nodes._free_pages == 0);
      assert(nodes._pages.empty());
      assert(trace.size() == 0);
      
      NodeRef rroot(nodes.new_page(), 0);
      TempTrie root;
      copy_node(rroot.page.new_node(root.size()), root.noderef);
      trace.push_root(rroot);
    }
     
  bool is_valid() const {
      return trace.is_valid();
    }
    
  size_t count() const {
      return _count;
    }
  
  void find(const Slice& key) {
      if (key.size() > MAX_KEY_SIZE)
        throw WrongValue(KEY_EXEEDS);
      
      encode(key, coding_buffer);
      trace.find(coding_buffer);
    }
    
  void first() { 
      trace.first();
    }
    
  void last() { 
      trace.last();
    }
    
  void next() { 
      trace.next();
    }
    
  void prev() {
      trace.prev();
    }
  
  Slice key() { 
      decode(trace.key, coding_buffer);
      return coding_buffer;
    }
  
  Slice value() { 
      trace.check_valid();
      return Slice((char*)trace.current().node(), trace.current().len());
    }

  void set_value(const Slice& value) {
      if (value.size() > 2048)
        throw WrongValue("value may not exceed 2048 bytes");

      if (trace.set_leaf(TempLeaf(value)))
        _count++;
    }
  
  void remove() { 
      if (trace.remove())
        reinit();
      _count--;
    }
  
  void get_data(std::string& buffer) const {
      
    }
};
    
MemoryDatabase* MemoryDatabase::create() {
  return new PrivateMemoryDatabase();
}
  
MemoryDatabase* MemoryDatabase::load(const std::string& data) {
  return new PrivateMemoryDatabase(data);
}

//@-others
} // namespace larch_leaves 
//@-leo
