//@+leo-ver=5-thin
//@+node:michael.20141215222649.65: * @file node.h
//@@language cplusplus
//@@tabwidth -2
// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_H
#define _LARCH_LEAVES_NODE_H

//@+<< includes >>
//@+node:michael.20141217010530.12: ** << includes >>
#include <cassert>
#include <algorithm>
#include <boost/cstdint.hpp>
#include "larch/leaves.h"
#ifdef DEBUG
#include <iostream>
#include <iomanip>
#endif
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20150106224503.60: ** Macros
/* the maximum key key size
   could be increased but compressed node
   may not exceed a page:
     MAX_KEY_SIZE_64 < 4096 + overhead
     
  MAX_KEY_SIZE = 3/4 * MAX_KEY_SIZE_64
*/
#define MAX_KEY_SIZE 1536
#define MAX_KEY_SIZE_64 2048
#define KEY_EXEEDS "key may not exceed 1536 bytes"
//@+node:michael.20150101205559.30: ** TestCode
#ifdef TESTING
void testpoint(const char* str);
#define TESTPOINT(x) testpoint(#x)

#else
#define TESTPOINT(x)
#endif
//@+node:michael.20141230111914.146: ** types
// a page is identified by a page id
// because of "copy on write" for concurrent storages
// there can be multiple pages with the same id
// but each page has a unique pageoffset_t
typedef boost::uint32_t pageid_t;     // page id 
typedef boost::uint32_t pageoffset_t; // points to page inside file


// Nodes are identfied by a node id.
// the node id is the index of the node pointer table.

typedef boost::uint8_t nodetype_t;
typedef boost::uint8_t nodeid_t;
typedef boost::uint8_t bsize_t;  // byte size
//@+node:michael.20141230111914.74: ** Utils
inline size_t common_prefix(const char* s1, const char *s2, size_t size) {
  size_t i;
  size = std::min(size, (size_t)0xff);
  for(i = 0; i < size && *s1 == *s2; i++, s1++, s2++);
  return i;
}

inline size_t page_pad(size_t size) {
  return size + ALIGN - 1 - ((size-1)&(ALIGN-1));
}

inline size_t pad8(size_t size) {
  return size + 7 - ((size-1)&7);
}

//@+node:michael.20141215222649.99: ** NodeHandler
struct NodeRef;
struct Trace;
struct TempNode;

struct NodeHandler {
  // returns the count of character the node consumes of the key
  // tries==1, compressed>1, leaves==0
  virtual size_t get_len(const NodeRef& rnode) = 0;
  
  // returns the nodes children
  virtual size_t get_children(NodeRef& rnode, nodeid_t children[65]) = 0;
  // replace the nodes children with the nodeids of children
  virtual void replace_children(NodeRef& rnode, nodeid_t children[65]) = 0;
  
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(NodeRef& rnode, Trace& trace) = 0;
  virtual void next(NodeRef& rnode, Trace& trace) = 0;
  virtual void prev(NodeRef& rnode, Trace& trace) = 0;
  virtual void first(NodeRef& rnode, Trace& trace) = 0;
  virtual void last(NodeRef& rnode, Trace& trace) = 0;
  virtual void add(const TempNode& leaf, NodeRef& rnode, Trace& trace) = 0;
  // remove the node
  virtual bool remove_child(NodeRef& rnode, Trace& trace) {
      return false;
    }
    
  // rnode tries to merge the child with itself or to a compressed
  virtual bool eat_child(NodeRef& rnode) = 0;

    
#ifdef DEBUG
  virtual void dump(NodeRef& rnode, std::ostream& out) = 0;
#endif  
    
  static NodeHandler* handlers[6];
};

enum NodeTypes {
  kLeaf = 0, kBigLeaf, kLink, kCompressed, kTrie, kBitTrie
};
//@+node:michael.20141215222649.67: ** Page
// A pointer inside a page
typedef boost::uint16_t inpage_ptr;

struct NodePtr {
  inpage_ptr ptr;      // position inside page
  nodetype_t type;     // node type
  bsize_t extra;       // is used by several nodes and
                       // ensures all nodes are good aligned
};


/*
  A Page has a size of 4096 bytes: it has the following layout:
  
  +----------------------+
  | header (4 byte)      |
  +----------------------+
  | link[0] (4byte)      |
  +----------------------+
  | link[1] (4byte)      |
  +----------------------+
  | ...                  |
  +----------------------+
  | link[l] (4byte)      | l = link_count -1
  +----------------------+  
  | node_ptr[0] (3 byte) |
  +----------------------+
  | node_ptr[1] (3 byte) |
  +----------------------+
  | ...                  |
  +----------------------+ 
  | node_ptr[n] (3 byte) | n = node_count -1
  +----------------------+ 
  |                      |
  |                      |
  +----------------------+ <-- free_start points to the end all nodes
  | node[n] (>= 16 byte) |
  +----------------------+
  | ...                  |
  +----------------------+
  | node[1] (>= 16 byte) |
  +----------------------+
  | node[0] (>= 16 byte) |
  +----------------------+

  node_ptrs grow downwards 
  nodes grow uppwards

  node 0 is always an ancestor of all other nodes
  and is called the page root.
  
  Annotations:
  because of the nature of inpage_ptr there, free_space underflows
  before nodeid_t space overflows. ==> we never run out of nodeids
  
*/

#define PAGE_HEADER_SIZE sizeof(boost::uint16_t)




struct Page {
  union {
    char data[4096];
    struct {
      boost::uint16_t node_count; // 16bit is not needed but we pad anyway
      NodePtr node_ptr[];
      
    };
  };      
      
  Page() : node_count(0) { }
};

//@+node:michael.20141215222649.73: ** PageRef
struct Node { };

#define REMOVE_BIT 0x80

// the access class to Page
struct PageRef {
  Page *page;
  pageid_t id;
  pageoffset_t offset;

  //@+others
  //@+node:michael.20141230111914.47: *3* PageRef
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
      page(page_), id(id_), offset(offset_) { }
  //@+node:michael.20141230111914.41: *3* node_ptr
  NodePtr* node_ptr() const {
      return page->node_ptr;
    }
  //@+node:michael.20141230111914.38: *3* free_size
  size_t free_size() const {
      if (!page->node_count)
        return sizeof(Page) - PAGE_HEADER_SIZE;
      
      return node_ptr()[page->node_count-1].ptr
        - page->node_count * sizeof(NodePtr)
        - PAGE_HEADER_SIZE;
    }
  //@+node:michael.20141230111914.39: *3* size
  // returns only the node occupied size (without pageheader)
  // but with nodeptrs and links
  size_t size() const {
      return sizeof(Page) - free_size() - PAGE_HEADER_SIZE;
    }
  //@+node:michael.20141230111914.40: *3* count
  size_t count() const {
      return page->node_count;
    }
  //@+node:michael.20141230111914.33: *3* get_node_type
  nodetype_t get_node_type(nodeid_t id) const {
      return node_ptr()[id].type;
    }
  //@+node:michael.20141230111914.34: *3* get_node
  Node* get_node(nodeid_t id) const {
      return (Node*)&page->data[node_ptr()[id].ptr];
    }
  //@+node:michael.20141230111914.35: *3* get_extra
  bsize_t* get_extra(nodeid_t id) const {
      return &node_ptr()[id].extra;
    }
  //@+node:michael.20141230111914.36: *3* get_link
  pageid_t* get_link(nodeid_t id) const {
      return (pageid_t*)&page->data[node_ptr()[id].ptr];
    }
  //@+node:michael.20141230111914.37: *3* get_node_size
  size_t get_node_size(nodeid_t id) const {
      NodePtr* ptr = node_ptr();
      if (id == 0)
        return sizeof(Page) - ptr[id].ptr;
      
      return ptr[id-1].ptr - ptr[id].ptr;
    }
  //@+node:michael.20150106224503.57: *3* new_node
  NodeRef new_node(size_t size) const;
  //@+node:michael.20141230111914.43: *3* free_node
  void free_node(nodeid_t id) const {
      node_ptr()[id].type |= REMOVE_BIT;
    }
  //@+node:michael.20141230111914.42: *3* defragment
  bool defragment(Trace& trace) const;
  //@+node:michael.20141230111914.45: *3* grow_node_by
  // grows or shrinks a node by size. size must be a multiple of 16
  void grow_node_by(nodeid_t node_id, int size) const;
  //@+node:michael.20141230111914.46: *3* change_to_link
  void change_to_link(nodeid_t node_id, pageid_t page_id) const;
  //@+node:michael.20150101205559.1: *3* dump
  #ifdef DEBUG
  void dump(std::ostream& out);
  #endif
  //@-others
};

//@+node:michael.20141219202729.2: ** NodeRef
// The Access class to Node
struct NodeRef {
  PageRef page;
  nodeid_t id;
  
  NodeRef(const PageRef& page_, nodeid_t id_)
    : page(page_), id(id_) { }

  nodetype_t type() const {
      return page.get_node_type(id);
    }
  
  void set_type(nodetype_t type) const {
      page.node_ptr()[id].type = type;
    }
  
  Node* node() const {
      return page.get_node(id);
    }
    
  pageid_t* link() const {
      return page.get_link(id);
    }
    
  bsize_t* extra() const {
      return page.get_extra(id);
    }

  size_t size() const {
      return page.get_node_size(id);
    }

  bool operator==(const NodeRef& other) const {
      return id == other.id && page.id == other.page.id;
    }
  
  bool is_leaf() const {
      return type() <= kBigLeaf;
    }
    
  // for leaf and compress nodes
  size_t len() const {
      return size() - *extra();
    }
    
  void set_len(size_t len) const {
      *extra() = size() - len;
    }
    
  //@+others
  //@+node:michael.20141230111914.76: *3* Convenient Delegators
  size_t get_len() const {
      return NodeHandler::handlers[type()]->get_len(*this);
    }

  void find(Trace& trace) {
      NodeHandler::handlers[type()]->find(*this, trace);
    }
    
  void next(Trace& trace) {
      NodeHandler::handlers[type()]->next(*this, trace);
    }
    
  void prev(Trace& trace) {
      NodeHandler::handlers[type()]->prev(*this, trace);
    }
    
  void first(Trace& trace) {
      NodeHandler::handlers[type()]->first(*this, trace);
    }
    
  void last(Trace& trace) {
      NodeHandler::handlers[type()]->last(*this, trace);
    }
   
  void add(const TempNode& leaf, Trace& trace) {
      NodeHandler::handlers[type()]->add(leaf, *this, trace);
    }

  bool remove_child(Trace& trace) {
      return NodeHandler::handlers[type()]->remove_child(*this, trace);
    }
   
  size_t get_children(nodeid_t children[65]) const {
      NodeHandler* handler = NodeHandler::handlers[type()];
      return handler->get_children((NodeRef&)*this, children);
    }

  void replace_children(nodeid_t children[65]) {
      NodeHandler::handlers[type()]->replace_children(*this, children);
    }
    
  bool eat_child() {
      return NodeHandler::handlers[type()]->eat_child(*this);
    }
    
  void child_find(nodeid_t child_id, Trace& trace);
  void child_first(nodeid_t child_id, Trace& trace);
  void child_last(nodeid_t child_id, Trace& trace);
  //@-others
};

inline void copy_node(const NodeRef& dst, const NodeRef& src) {
    memcpy(dst.node(), src.node(), src.size());
    *dst.extra() = *src.extra();
    dst.set_type(src.type());
  }

//@+node:michael.20141220220750.4: ** TempNode
struct TempNode {
  Page page;
  PageRef pageref;
  NodeRef noderef;
  
  TempNode()
    : pageref(&page, 0xFFFFFFFF, 0xFFFFFFFF), noderef(pageref, 0) { 
      *noderef.extra() = 0;
    }
    
  size_t size() const {
      return noderef.size();
    }
    
  size_t len() const {
      return noderef.len();
    }
    
  Node* node() const {
      return noderef.node();
    }
    
  bsize_t* extra() const {
      return noderef.extra();
    }

  nodetype_t type() const {
      return noderef.type();
    }
    
  size_t get_len() const {
      return noderef.get_len();
    }
};


struct TempLeaf : public TempNode {
  TempLeaf(const Slice& value);
};

struct TempTrie : public TempNode {
  TempTrie(size_t child_count=1);
};

struct TempCompressed : public TempNode {
  TempCompressed(const Slice& part);
};
//@+node:michael.20141219202729.3: ** PageMap
// Translates a pageid to page offset
struct PageMap {
  virtual PageRef get_page(pageid_t id) = 0;
  virtual void free_page(const PageRef& page) = 0;
};

//@+node:michael.20141215222649.135: ** NodeStorage (Declaration)
class NodeStorage {
 public:
  virtual PageRef new_page() = 0;
};


struct NodeStorageInHeap : public NodeStorage, public PageMap {
  typedef std::unique_ptr<Page> _page_ptr;
  std::vector<_page_ptr> _pages;
  size_t _free_pages;  // count of _free_pages inside the vector
  
  NodeStorageInHeap() : _free_pages(0) { }

  PageRef get_page(pageid_t id) {
      return PageRef(_pages[id].get(), id, id);
   }
  
  void free_page(const PageRef& page);
  PageRef new_page();
};


struct PersistentNodeStorage : public NodeStorage {
  /*PagePositionMap Page;
  boost::interprocess::file_mapping _file_mapping;
  
  PersistentNodeStorage(const char* path);
  std::shared_ptr<PageMap> get_pagemap(version_t version) = 0;
  virtual PageRef new_page();
 
  void grow_file(size_t size);
  void shrink_file(size_t size);*/
};


class MultiProcessNodeStorage : public PersistentNodeStorage {
  /*public:
    MultiProcessNodeStorage(const char* path);*/
};
//@+node:michael.20141220220750.15: ** Trace
// A stack trace inside a trie
// nodes[0] is the root
// nodes[K] is the current node (almost always a leaf)
// key[nodes[1]] causes the transition from nodes[0] to nodes[1]
struct Trace {
  struct Transition {
    NodeRef node;
    size_t start; // the start index of key 
    size_t end;   // the end index of the key
    Transition(const NodeRef& node_, size_t start_, size_t end_)
        : node(node_), start(start_), end(end_) { }
    Transition()
        : node(PageRef(NULL, 0, 0), 0), start(0), end(0) { }
  };

  std::vector<Transition> stack;
  std::string key;       // the key the trace points to
  PageMap& map;          // needed for some operations
  NodeStorage& storage;  // needed for some operations
  
  Trace(PageMap& map_, NodeStorage& storage_) :
      map(map_), storage(storage_) { }
  
  //@+others
  //@+node:michael.20141230111914.112: *3* size
  size_t size() const {
      return stack.size();
    }
  //@+node:michael.20150106224503.48: *3* is_valid
  // returns true if it points to a valid position
  bool is_valid() const {
    const Transition& back = stack.back();
    return back.end >= key.size() && back.node.is_leaf();
  }
  //@+node:michael.20150101205559.11: *3* check_valid
  void check_valid() const {
      if (!is_valid())
        throw NoValidPosition();
    }
  //@+node:michael.20141230111914.120: *3* current
  NodeRef& current() {
      return stack.back().node;
    }
  //@+node:michael.20150106224503.56: *3* current_key
  Slice current_key() const {
      const Transition& back = stack.back();
      int size = std::max((int)key.size()-(int)back.start, (int)0);
      return Slice(key.data()+back.start, size);
    }
  //@+node:michael.20150106224503.55: *3* cut_key
  // cuts the key to the current end
  void cut_key() {
      key.resize(stack.back().start);
    }
  //@+node:michael.20141230111914.119: *3* parent
  NodeRef& parent() {
      NodeRef& parent = stack[stack.size()-2].node;
      return parent.type() == kLink ? stack[stack.size()-3].node : parent;
    }
  //@+node:michael.20141230111914.113: *3* find
  void find(const Slice& key_);
  //@+node:michael.20150101205559.64: *3* first
  void first() { 
      reset();
      current().first(*this);
    }
  //@+node:michael.20150101205559.65: *3* last
  void last() { 
      reset();
      current().last(*this);
    }
  //@+node:michael.20150101205559.23: *3* next
  void next() {
      current().next(*this);
    }
  //@+node:michael.20150101205559.24: *3* prev
  void prev() {
      current().prev(*this);
    }
  //@+node:michael.20141230111914.121: *3* parent_next
  void parent_next() {
      if (size() > 1) {
        pop();
        current().next(*this);
      }
    }
  //@+node:michael.20141230111914.122: *3* parent_prev
  void parent_prev() {
      if (size() > 1) {
        pop();
        current().prev(*this);
      }
    }
  //@+node:michael.20141230111914.116: *3* reset
  void reset() {
      stack.resize(1); // keep the root
      key.clear();
    }
  //@+node:michael.20141230111914.117: *3* push
  void push(const NodeRef& node) {
      Transition& back = stack.back();
      if (back.node == node)
        return;
      size_t start = back.end;
      stack.push_back(Transition(node, start, start+node.get_len()));
    }
      
  void push_root(const NodeRef& node) {
      stack.push_back(Transition(node, 0, node.get_len()));
    }
  //@+node:michael.20141230111914.118: *3* pop
  // returns the node id of the skipped link  or 0
  nodeid_t pop() {
      stack.pop_back();
      if (size() && current().type() == kLink) {
        nodeid_t skipped_id = current().id;
        stack.pop_back();
        return skipped_id;
      }
      return 0;
    }
  //@+node:michael.20141230111914.123: *3* remove
  bool _eat_child(size_t ancestor) {
    int index = size()-1-ancestor;
    return index>0 ? stack[index].node.eat_child() : false;
  }


  // returns true if the complete trie is removed
  bool remove() {
      if (!is_valid())
        throw NoValidPosition();

      while(size()) {
        NodeRef& me(current()); 
        if (me.remove_child(*this)) {
          me.page.defragment(*this);
          merge_pages();

          if (_eat_child(0)+_eat_child(1)+_eat_child(2))
            if (!current().page.defragment(*this))
              refresh_trace();
         
          return false;
        }
        nodeid_t skipped_id = pop();
        if (skipped_id)
          free_node(current().page, skipped_id);
          
        free_node(me.page, me.id);
      }
       
      return true;
    }
  //@+node:michael.20141230111914.124: *3* free_node
  bool free_node(const PageRef& page, nodeid_t id) {
      if (id == 0) {
        map.free_page(page);
        return false;
      }
      else {
        page.free_node(id);
        // no defragment needed (it is done in remove())
        return true;
      }
    }
  //@+node:michael.20141230111914.125: *3* child_of_parent
  // returns the nodeid that connects parent with current
  // if current is on another page it conntects the link
  // with current's page and retuns the nodeid of the link
  nodeid_t child_of_parent() {
      NodeRef& link_or_parent(stack[stack.size()-2].node);
      if (link_or_parent.type() == kLink) {
        *link_or_parent.link() = current().page.id;
        return link_or_parent.id;
      }
      return current().id;
    }
  //@+node:michael.20141230111914.126: *3* add_node
  void add_node(const TempNode& src) {
      // transistion_key causes transitions from parent to node
      size_t size_ = src.size();
      reserve_space(size_+sizeof(NodePtr));
      
      NodeRef dst(current().page.new_node(size_));
      copy_node(dst, src.noderef);
      
      push(dst);
   }
  //@+node:michael.20141230111914.128: *3* grow_node_by
  // grows or shrink the current node
  void grow_node_by(int delta) {
      if (delta > 0)
        reserve_space(delta);
      
      current().page.grow_node_by(current().id, delta);  
    }
  //@+node:michael.20141230111914.127: *3* change_node
  // change current node to type
  void change_node(const TempNode& new_node) {
      Transition& back = stack.back();
      size_t size_ = back.node.size(), new_size = new_node.size();
      grow_node_by((int)new_size-(int)size_);
      copy_node(back.node, new_node.noderef);
      back.end = back.start + new_node.get_len();
    }
  //@+node:michael.20141230111914.115: *3* set_leaf
  // sets a new leaf, 

  bool set_leaf(const TempLeaf& leaf) {
      if (is_valid()) {
        change_node(leaf);
        return false;
      }
      else {
        current().add(leaf, *this);
        return true;
      }
    }
  //@+node:michael.20141230111914.133: *3* move_node
  // moves the node to new page returning the id on the new_page
  // precondition: there must be enough space in new_page  
  nodeid_t move_node(const PageRef& new_page, const NodeRef& node) {
      if (new_page.id == node.page.id)
        return node.id;
        
      nodeid_t children[65];
      size_t count = node.get_children(children);
      
      NodeRef new_node(new_page.new_node(node.size()));
      copy_node(new_node, node);
          
      for(size_t i = 0; i < count; i++) {
        NodeRef child(node.page, children[i]);
        children[i] = move_node(new_page, child);
      }
      
      new_node.replace_children(children);
      node.page.free_node(node.id);
      return new_node.id;
    }
  //@+node:michael.20141230111914.129: *3* merge_pages
  // merges the current page with the parent page if possible 
  void merge_pages() {
      PageRef page(current().page);
      
      // this is never called by the trace root
      // => stack.size() >= 2
      
      for(int i = stack.size()-2; i >= 0; i--) {
        NodeRef& link(stack[i].node);
        
        if (link.page.id != page.id) {
          assert(link.type() == kLink);
          
          if (link.page.free_size() >= page.size() 
              && link.page.count() + page.count() <= 230) {
            TESTPOINT(MergePages);
            
            NodeRef& parent(stack[i-1].node); // the child's parent
            NodeRef page_root(page, 0);
            
            nodeid_t child_id = move_node(parent.page, page_root);
                      
            // replace the link with the child
            nodeid_t children[65];
            size_t count = parent.get_children(children);
            for(size_t i = 0; i < count; i++) {
              if (children[i] == link.id) {
                children[i] = child_id;
                break;
              }
            }
            parent.replace_children(children);
                      
            link.page.free_node(link.id);
            if (!link.page.defragment(*this))
              refresh_trace();
            map.free_page(page);
          }
          return;
        }
      }
    }
  //@+node:michael.20141230111914.131: *3* calc_sizes
  size_t calc_sizes(const NodeRef& node, size_t sizes[256]) {
      nodeid_t children[65];
      size_t size_ = node.size();
      size_t count = node.get_children(children);
      
      for(size_t i = 0; i < count; i++)
        size_ += calc_sizes(NodeRef(node.page, children[i]), sizes);
        
      size_ += sizeof(NodePtr);
      sizes[node.id] = size_;
        
      return size_;
    }
  //@+node:michael.20150101205559.69: *3* refresh_trace
  // refresh the trace remapping the nodeids
  void refresh_trace() {
      stack.resize(1);
      current().find(*this);
    }
  //@+node:michael.20141230111914.132: *3* reserve_space
  // ensures that the page of current node has a free_space >  size
  // place current node to a new page if nessary
  // if exclude is set this node will not move from page
  void reserve_space(size_t size_);
  //@-others
};

//@+node:michael.20150106224503.61: ** NodeRef-inlines
inline void NodeRef::child_find(nodeid_t child_id, Trace& trace) {
  NodeRef child(page, child_id);
  trace.push(child);
  child.find(trace);
}

inline void NodeRef::child_first(nodeid_t child_id, Trace& trace) {
  NodeRef child(page, child_id);
  trace.push(child);
  child.first(trace);
}

inline void NodeRef::child_last(nodeid_t child_id, Trace& trace) {
  NodeRef child(page, child_id);
  trace.push(child);
  child.last(trace);
}
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
//@-leo
