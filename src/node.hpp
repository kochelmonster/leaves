// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_HPP
#define _LARCH_LEAVES_NODE_HPP

#include "../include/larch/leaves.hpp"
#include "page.hpp"
#include "storage.hpp"
#include <algorithm>
#include <boost/cstdint.hpp>
#include <cassert>

namespace larch_leaves {


#ifdef TESTING
void testpoint(const char *str);
#define TESTPOINT(x) testpoint(#x)

#else
#define TESTPOINT(x)
#endif

#define LINK_SIZE page_pad(sizeof(PageLink))
#define MAX_COMPRESSED_LEN 1024

inline size_t common_prefix(const char *s1, const char *s2, size_t size) {
  size_t i;
  size = std::min(size, (size_t)0xff);
  for (i = 0; i < size && *s1 == *s2; i++, s1++, s2++)
    ;
  return i;
}

enum NodeTypes {
  kLeaf = 0,
  kBigLeaf,
  kLink,
  kCompressed,
  kTrie,
  kBitTrie,
  kRemoved
};

struct Node;
struct NodeRef;
struct TempNode;
struct Trace;

struct NodeHandler {
  // returns the count of character the node consumes of the key
  // tries==1, compressed>1, leaves==0
  virtual size_t len(const Node *node) = 0;

  virtual void data(Node *node, const void *data) = 0; // sets the data of Node
  virtual Slice data(const Node *node) = 0; // return the data of the Node

  // returns the count of children
  virtual size_t count(const Node *node) = 0;

  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(Trace &trace) = 0;
  virtual void next(Trace &trace) = 0;
  virtual void prev(Trace &trace) = 0;
  virtual void first(Trace &trace) = 0;
  virtual void last(Trace &trace) = 0;
  virtual void add(const TempNode &leaf, Trace &trace) = 0;

  // remove the node returns true if the node is still valid
  virtual bool remove_child(Trace &trace) { return false; }

  virtual boost::uint64_t start_grow(const Node *node) { return 0; }
  virtual void end_grow(Node *node, boost::uint64_t value) {}

#ifdef DEBUG
  virtual void dump(Page *page, Node *node, std::ostream &out) = 0;
#endif

  static NodeHandler *handlers[7];
};

#pragma warning(disable : 4200)

struct Node {
  typedef boost::uint16_t header_t;
  struct {
    header_t type : 4;
    header_t size : 12; // size in bytes
  };
  Page::ptr children_[0]; // an array for the Nodes children
  // ... some type specific data

  NodeHandler *handler() const { return NodeHandler::handlers[type]; }

  void data(const void *data) { handler()->data(this, data); }

  Slice data() { return handler()->data(this); }

  size_t children(Page::ptr **children) {
    *children = children_;
    return count();
  }

  size_t len() const { return handler()->len(this); }

  size_t count() const { return handler()->count(this); }

  size_t pad_size() const { return page_pad(size); }

  Page::ptr ptr_size() const { return (Page::ptr)(pad_size() / ALIGN); }

  boost::uint64_t start_grow() { return handler()->start_grow(this); }

  void end_grow(boost::uint64_t value) { handler()->end_grow(this, value); }

  void adjust_children(Page::ptr old_ptr, int delta) {
    size_t count_ = count();
    for (size_t i = 0; i < count_; i++) {
      if (children_[i] >= old_ptr)
        children_[i] += delta;
    }
  }
};

// The Access class to Node
struct NodeRef {
  PageRef page;
  Node *node;

  NodeRef(PageRef page_, Page::ptr node_ptr)
      : page(page_), node(page_.page->node(node_ptr)) {}

  int type() const { return node->type; }

  void type(int type) { node->type = type; }

  size_t size() const { return node->size; }

  size_t pad_size() const { return node->pad_size(); }

  NodeHandler *handler() const { return NodeHandler::handlers[type()]; }

  Page::ptr ptr() const { return (Page::ptr)((char *)node - page.page->data) / ALIGN; }

  bool operator==(const NodeRef &other) const {
    return ptr() == other.ptr() && page.id == other.page.id;
  }

  bool is_leaf() const { return type() <= kBigLeaf; }

  // for leaf and compress nodes
  void data(void *data) { handler()->data(node, data); }

  Slice data() { return node->data(); }

  size_t len() const { return node->len(); }

  void find(Trace &trace) { handler()->find(trace); }

  void next(Trace &trace) { handler()->next(trace); }

  void prev(Trace &trace) { handler()->prev(trace); }

  void first(Trace &trace) { handler()->first(trace); }

  void last(Trace &trace) { handler()->last(trace); }

  void add(const TempNode &leaf, Trace &trace) { handler()->add(leaf, trace); }

  bool remove_child(Trace &trace) { return handler()->remove_child(trace); }

  size_t children(Page::ptr **children) const {
    return node->children(children);
  }

  size_t count() { return node->count(); }

  void child_find(Page::ptr child, Trace &trace);

  void child_first(Page::ptr child, Trace &trace);

  void child_last(Page::ptr child, Trace &trace);
};

struct NodeRef;
struct Trace;
struct TempNode;
struct Node;
struct Page;

struct TempNode {
  Page page;

  TempNode() {}

  Node *node() const { return page.node(0); }

  size_t size() const { return node()->size; }

  size_t pad_size() const { return node()->pad_size(); }

  int type() const { return node()->type; }

  void to_leaf(const Slice &value);

  void to_trie(size_t child_count = 0);

  void to_compressed(const Slice &part);

  void to_link(pageid_t page_id, Page::entry_t entry);

  void add_to(Trace &trace) const;
};

struct TempLeaf : public TempNode {
  TempLeaf(const Slice &value) { to_leaf(value); }
};

struct TempTrie : public TempNode {
  TempTrie(size_t child_count = 0) { to_trie(child_count); }
};

struct TempCompressed : public TempNode {
  TempCompressed(const Slice &part) { to_compressed(part); }
};


// A stack trace inside a trie
// nodes[0] is the root
// nodes[K] is the current node (almost always a leaf)
// key[nodes[1]] causes the transition from nodes[0] to nodes[1]
struct Trace {
  struct Transition {
    NodeRef node;
    size_t start; // the start index of key
    size_t end;   // the end index of the key
    Transition(const NodeRef &node_, size_t start_, size_t end_)
        : node(node_), start(start_), end(end_) {}
    Transition() : node(PageRef(), 0), start(0), end(0) {}
  };

  std::vector<Transition> stack;
  std::string key;      // the key the trace points to
  PageMap &map;         // needed for some operations
  NodeStorage &storage; // needed for some operations
  Transition *back;

  Trace(PageMap &map_, NodeStorage &storage_) : map(map_), storage(storage_) {
    key.reserve(1024);
  }

  // DEBUG
  void check() {
    std::vector<Transition>::iterator i;
    Node* node_before = NULL;
    size_t index = 0;
    for (i = stack.begin(); i != stack.end(); i++, index++) {
      assert(i->end == i->start + i->node.len());
      if (node_before) {
        if (node_before->type == kLink) {
          PageLink* link = (PageLink*)node_before->data().data();
          assert(i->node.page.page->entry_points[link->entry] == i->node.ptr() + 1);
          assert(i->node.page.id == link->page_id);
        }
        else {
          size_t j, count = node_before->count();
          Page::ptr ptr = i->node.ptr();
          for (j = 0; j < count; j++) {
            if (node_before->children_[j] == ptr)
              break;
          }
          assert(j < count);
        }
      }
      node_before = i->node.node;
    }
  }

  void find(const Slice &key_);

  // ensures that the page of current node has a free_space >  size
  // place current node to a new page if nessary
  // if exclude is set this node will not move from page
  void reserve_space(size_t size_);

  // returns true if the complete trie is removed
  bool remove();

  // condense single BitTtrie Nodes to a compress Node
  void condense_trie();

  size_t size() const { return stack.size(); }

  void _push(const Transition &t) {
    stack.push_back(t);
    back = &stack.back();
  }

  void _pop() {
    stack.pop_back();
    back = &stack.back();
  }

  // returns true if it points to a valid position
  bool is_valid() const {
    return back->end >= key.size() && back->node.is_leaf();
  }

  void check_valid() const {
    if (!is_valid())
      throw NoValidPosition();
  }

  NodeRef &current() { return back->node; }

  Slice current_key() const {
    size_t size = key.size() > back->start ? key.size() - back->start : 0;
    return Slice(key.data() + back->start, size);
  }

  // cuts the key to the current end
  void cut_key() { key.resize(back->start); }

  NodeRef &parent() {
    NodeRef &parent = stack[stack.size() - 2].node;
    return parent.type() == kLink ? stack[stack.size() - 3].node : parent;
  }

  void first() {
    reset();
    current().first(*this);
  }

  void last() {
    reset();
    current().last(*this);
  }

  void next() { current().next(*this); }

  void prev() { current().prev(*this); }

  void parent_next() {
    if (size() > 1) {
      pop();
      current().next(*this);
    }
  }

  void parent_prev() {
    if (size() > 1) {
      pop();
      current().prev(*this);
    }
  }

  void reset() {
    stack.resize(1); // keep the root
    back = &stack.back();
    key.clear();
  }

  NodeRef &push(const NodeRef &node) {
    size_t start = back->end;
    _push(Transition(node, start, start + node.len()));
    return current();
  }

  void push_root(const NodeRef &node) {
    _push(Transition(node, 0, node.len()));
  }

  Page::ptr connect_ptr() {
    assert(parent().page.id == current().page.id);
    return current().ptr();
  }

  // returns the node id of the skipped link or 0
  Page::ptr pop() {
    _pop();
    if (size() && current().type() == kLink) {
      Page::ptr skipped_id = current().ptr();
      _pop();
      return skipped_id;
    }
    return 0;
  }

  void add(const TempNode &src) { current().add(src, *this); }

  // grows or shrink the current node returns the delta of adjust_pointers
  int grow_node_by(int delta) {
   if (delta > 0) {
      NodeRef& me(current());
      size_t new_pad_size = page_pad(me.size() + delta);
      reserve_space(new_pad_size - me.pad_size());
    }

    NodeRef& me(current());
    PageRef &page(me.page);
    bool had_reserve = page.has_reserve();
    delta = page.grow_node_by(me.ptr(), delta);
    if (had_reserve && !page.has_reserve())
      storage.no_reserve(page);

    if (delta) {
      Page::ptr ptr = me.ptr();
      for (int i = (int)stack.size() - 2; i > 0; i--) {
        NodeRef& item(stack[i].node);
        if (item.page.id != page.id)
          break;
        if (item.ptr() > ptr) {
          refresh_trace(page.id);
          break;
        }
      }
    }
    return delta;
  }

  // change current node to type
  int change_node(const TempNode &new_node) {
    size_t size_ = current().size(), new_size = new_node.size();
    int delta = grow_node_by((int)new_size - (int)size_);
    memcpy(current().node, new_node.node(), page_pad(new_size));
    back->end = back->start + current().len();
    return delta;
  }

  // sets a new leaf,
  bool set_leaf(const TempLeaf &leaf) {
    if (is_valid()) {
      change_node(leaf);
      return false;
    } else {
      current().add(leaf, *this);
      return true;
    }
  }

  // refresh the trace remapping the ptrs of the current page
  void refresh_trace(pageid_t page_id) {
    for (size_t i = 1; i < size(); i++) {
      if (stack[i].node.page.id == page_id) {
        stack.erase(stack.begin()+i, stack.end());
        back = &stack.back();
        current().find(*this);
        break;
      }
    }
  }
};

inline void NodeRef::child_find(Page::ptr child, Trace &trace) {
  trace.push(NodeRef(page, child)).find(trace);
}

inline void NodeRef::child_first(Page::ptr child, Trace &trace) {
  trace.push(NodeRef(page, child)).first(trace);
}

inline void NodeRef::child_last(Page::ptr child, Trace &trace) {
  trace.push(NodeRef(page, child)).last(trace);
}

// partitionate large keys
inline void add_compressed(Slice data, Trace &trace) {
  while (data.size()) {
    TempCompressed compressed(
        Slice(data.data(), std::min(data.size(), (size_t)MAX_COMPRESSED_LEN)));
    trace.add(compressed);
    data = data.advance(compressed.node()->len());
  }
}

} // namespace larch_leaves
#endif // _LARCH_LEAVES_MEMORY_HPP
