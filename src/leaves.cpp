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


//@+node:michael.20141230111914.3: ** MemoryDatabase
//@+others
//@+node:michael.20141230111914.23: *3* Cursor
struct ReadMemoryCursor : public Cursor {
  Trace trace;
  NodeRef root;
  std::string encoding_buffer;
  std::string decoding_buffer;
  
  ReadMemoryCursor(NodeStorageInHeap& nodes, NodeRef root_) 
    : trace(nodes, nodes), root(root_) { 
      trace.push(root);
    }
    
  bool is_valid() const {
      return trace.complete;
    }
  
  void set(const Slice& key, int read_forward=100) { 
      encode(key, encoding_buffer);
      Slice ekey(encoding_buffer);
      trace.find(ekey);
    }
    
  void first(int read_forward=100) { 
      root.first(trace);
    }
  void last(int read_forward=-100) { 
      root.last(trace);
    }
  void next(int read_forward=100) { 
      trace.next();
    }
  void prev(int read_forward=-100) {
      trace.prev();
    }
  
  Slice key() { 
    decode(trace.key, decoding_buffer);
    return Slice(decoding_buffer);
  }
  
  Slice value() { 
    trace.check_complete();
    return Slice((char*)trace.current().node(), *trace.current().extra());
  }
  
  void set_value(const Slice& value) { throw NotImplemented(); }
  void remove() { throw NotImplemented(); }
};

struct WriteMemoryCursor : public ReadMemoryCursor {
  WriteMemoryCursor(NodeStorageInHeap& nodes, NodeRef root_)
    : ReadMemoryCursor(nodes, root_) { }
  
  void set_value(const Slice& value) {
    if (value.size() > 255)
      throw WrongValue("value may not exceed 255 bytes");
  
    trace.set_leaf(encoding_buffer, TempLeaf(value));
  }
  
  void remove() { 
    trace.remove();
  }
};
//@+node:michael.20141230111914.24: *3* PrivateMemoryDatabase
struct PrivateMemoryDatabase : public MemoryDatabase {
  NodeStorageInHeap _nodes;

  PrivateMemoryDatabase() {
      Trace trace(_nodes, _nodes);
      NodeRef root_ = root();
      trace.push(root_);
      trace.add_node(TempTrie(), Slice());
    }
    
  PrivateMemoryDatabase(const std::string& data) {
      //_nodes.load(data);
    }

  NodeRef root() {
    return NodeRef(_nodes.get_page(0), 0);
  }

  cursor_ptr reader(const Slice& namespace_=main_name_space) {
      std::string encoded;
      encode(namespace_, encoded);
      Trace trace(_nodes, _nodes);
      root().find(Slice(encoded), trace);
      if (!trace.complete) {
        // namespace does not exist
        return cursor_ptr(new NoWhereCursor);
      }
      
      return cursor_ptr(new ReadMemoryCursor(_nodes, trace.current()));
    }
  
  cursor_ptr writer(const Slice& namespace_=main_name_space) {
      std::string encoded;
      encode(namespace_, encoded);
      Slice ekey(encoded);
      Trace trace(_nodes, _nodes);
      NodeRef root_(root());
      trace.push(root_);
      root().find(ekey, trace);
      
      if (!trace.complete)
        trace.current().add(ekey.advance(trace.key.size()), TempTrie(), trace);
      
      return cursor_ptr(new WriteMemoryCursor(_nodes, trace.current()));
    }
    
    std::string data() const {
        // return _nodes.data();
        return std::string();
      }
    
#ifdef DEBUG
  void dump(std::ostream& out) {
      out << "State of Memory Storage" << std::endl
          << "=======================" << std::endl;
      typedef std::vector<NodeStorageInHeap::_page_ptr>::iterator iter_t;
      iter_t i = _nodes._pages.begin();
      for(int j = 0; i != _nodes._pages.end(); i++, j++)
        PageRef(i->get(), j, j).dump(out);
    }
#endif    
};


std::shared_ptr<MemoryDatabase> MemoryDatabase::create() {
  return std::shared_ptr<MemoryDatabase>(new PrivateMemoryDatabase());
}
  
std::shared_ptr<MemoryDatabase> MemoryDatabase::load(const std::string& data) {
  return std::shared_ptr<MemoryDatabase>(new PrivateMemoryDatabase(data));
}

//@-others

//@-others
} // namespace larch_leaves 
//@-leo
