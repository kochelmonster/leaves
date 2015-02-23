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
      coding_buffer.reserve(MAX_KEY_SIZE_64);
    }

  PrivateMemoryDatabase(const std::string& data) : trace(nodes, nodes), _count(0) {
      //_nodes.load(data);
    }

  void reinit() {
      assert(nodes._free_pages == 0);
      assert(nodes._pages.empty());
      assert(trace.size() == 0);
      
      PageRef page(nodes.new_page());
      TempTrie root;
      copy_node(NodeRef(page, page.new_node(root.size())), root);
      trace.push_root(NodeRef(page, 0));
    }
     
  bool is_valid() const {
      return trace.is_valid();
    }
    
  size_t count() const {
      return _count;
    }
    
  size_t pages() const {
      return nodes._pages.size() - nodes._free_pages;
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
      return trace.get_value();
    }

  void set_value(const Slice& value) {
      if (value.size() > MAX_PAGE_VALUE_SIZE)
        throw WrongValue(VALUE_EXEEDS);

      if (trace.set_leaf(TempLeaf(value), false))
        _count++;
    }
  
  void remove() { 
      if (trace.remove())
        reinit();
      _count--;
    }
    
  void reset_statistics() {
      trace.reset_statistics();
    }
    
  void print_statistics() {
      double avg_trace = (double)trace.stat_find_trace/(double)trace.stat_find_count;
      double avg_reuse = (double)trace.stat_reuse_trace/(double)trace.stat_find_count;
      double avg_tries = (double)trace.stat_tries/(double)trace.stat_find_count;
      double avg_compressed = (double)trace.stat_compressed/(double)trace.stat_find_count;
      double avg_link = (double)trace.stat_link/(double)trace.stat_find_count;
      std::cerr << "avg trace: " << avg_trace << std::endl;
      std::cerr << "avg reuse: " << avg_reuse << std::endl;
      std::cerr << "avg tries: " << avg_tries << std::endl;
      std::cerr << "avg compressed: " << avg_compressed << std::endl;
      std::cerr << "avg links: " << avg_link << std::endl;
      std::cerr << "pages: " << pages() << std::endl;
      std::cerr << "free pages: " << nodes._free_pages << std::endl;
    }
    
#ifdef DEBUG
  void dump(std::ostream& out) {
      out << "state:" << std::endl;
      typedef std::vector<NodeStorageInHeap::_page_ptr>::iterator iter_t;
      iter_t i = nodes._pages.begin();
      for(int j = 0; i != nodes._pages.end(); i++, j++) {
        if (*i) {
          PageRef(i->get(), j, j).dump(out);
        }
      }
      out << "---" << std::endl;
    }
#endif    
};
    
MemoryDatabase* MemoryDatabase::create() {
  return new PrivateMemoryDatabase();
}
//@-others
} // namespace larch_leaves 
//@-leo
