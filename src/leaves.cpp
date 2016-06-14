#include "node.hpp"
//#include "leaf.h"

namespace larch_leaves {
void encode(const Slice &input, std::string &output);
void decode(const std::string &input, std::string &output);

Slice main_name_space("::main", 6);

// A Cursor to a non exising namespace
class NoWhereCursor : public Cursor {
public:
  bool is_valid() const { return false; }
  void set(const Slice &key, int read_forward = 100) {}
  void first(int read_forward = 100) {}
  void last(int read_forward = -100) {}
  void next(int read_forward = 100) {}
  void prev(int read_forward = -100) {}

  Slice key() { throw NoValidPosition(); }
  Slice value() { throw NoValidPosition(); }

  void set_value(const Slice &value) { throw NotImplemented(); }
  void remove() { throw NotImplemented(); }
};

struct PrivateMemoryDatabase : public MemoryDatabase {
  NodeStorageInHeap nodes;
  Trace trace;
  std::string coding_buffer;
  size_t _count;

  PrivateMemoryDatabase() : trace(nodes, nodes), _count(0) {
    reinit();
    coding_buffer.reserve(1024);
  }

  PrivateMemoryDatabase(const std::string &data)
      : trace(nodes, nodes), _count(0) {
    //_nodes.load(data);
  }

  void reinit() {
    assert(nodes._free_pages.size() == 0);
    assert(nodes._pages.empty());
    assert(trace.size() == 0);
    PageRef page(nodes.free_page(1));
    TempTrie root;
    NodeRef rroot(page, page.new_node(root.size()));
    memcpy(rroot.node, root.node(), rroot.size());
    page.page->entry_points[0] = 1;
    trace.push_root(rroot);
  }

  bool is_valid() const { return trace.is_valid(); }

  size_t count() const { return _count; }

  size_t pages() const { return nodes._pages.size(); }

  void find(const Slice &key) {
    encode(key, coding_buffer);
    trace.find(coding_buffer);
  }

  void first() { trace.first(); }

  void last() { trace.last(); }

  void next() { trace.next(); }

  void prev() { trace.prev(); }

  Slice key() {
    decode(trace.key, coding_buffer);
    return coding_buffer;
  }

  Slice value() {
    trace.check_valid();
    return trace.current().data();
  }

  void set_value(const Slice &value) {
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

  void get_statistics(Statistics *output) {
    typedef NodeStorageInHeap::_page_container_t::iterator iter_t;
    iter_t i = nodes._pages.begin();
    size_t free_size = 0;
    for (int j = 0; i != nodes._pages.end(); i++, j++) {
      if (*i)
        free_size += PageRef(i->get(), j, j).free_size();
    }
    output->page_free_size = free_size;
  }

#ifdef DEBUG
  void check() {
    typedef NodeStorageInHeap::_page_container_t::iterator iter_t;
    iter_t i = nodes._pages.begin();
    for (size_t j = 0; i != nodes._pages.end(); i++, j++) {
      if (*i)
        (*i)->check();
    }

    trace.check();
  }

  void dump(std::ostream &out) {
    out << "state:" << std::endl;
    typedef NodeStorageInHeap::_page_container_t::iterator iter_t;
    iter_t i = nodes._pages.begin();
    for (int j = 0; i != nodes._pages.end(); i++, j++) {
      if (*i) {
        PageRef(i->get(), j, j).dump(out);
      }
    }
    out << "---" << std::endl;
  }
#endif
};

MemoryDatabase *MemoryDatabase::create() { return new PrivateMemoryDatabase(); }
} // namespace larch_leaves
