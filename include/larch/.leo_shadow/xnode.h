//@+leo-ver=4-thin
//@+node:michael.20141215222649.65:@shadow node.h
//@@language cplusplus
//@@tabwidth -2
// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_H
#define _LARCH_LEAVES_NODE_H

//@<< includes >>
//@+node:michael.20141217010530.12:<< includes >>
#include "boost/cstdint.h"
//@nonl
//@-node:michael.20141217010530.12:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.72:Page
//@+others
//@+node:michael.20141215222649.67:struct Page
// Nodes are identfied by a node id.
// the node id is the index of the node pointer table.

typedef boost::uint8_t nodetype_t;
typedef boost::uint8_t nodeid_t;
typedef boost::uint8_t bsize_t;  // byte size

// A pointer inside a page with a 16 byte alignment (256*16 = 4096)
typedef boost::uint8_t inpage_ptr;

struct NodePtr {
  inpage_ptr ptr;      // position inside page
  nodetype_t type;     // node type
  nodeid_t extra;      // is used by several nodes and
                       // ensures all nodes are good aligned
};


/*
  A Page has a size of 4096 bytes: it has the following layout:
  
  +----------------------+
  | header (2 byte)      |
  +----------------------+
  | node_ptr[0] (3 byte) |
  +----------------------+
  | node_ptr[1] (3 byte) |
  +----------------------+
  | ...                  |
  +----------------------+
  |                      |
  |                      |
  |                      |
  +----------------------+ <-- free_start points to the end all nodes
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
struct Page {
  union {
    char data[4094];
    struct {
      bsize_t count;  // node count
      inpage_ptr free_start;
      NodePtr node_ptr[];
    };
  };      
      
  Page() : last_node(0), holes(0), first_hole(0), free_start(0) { }

};

//@-node:michael.20141215222649.67:struct Page
//@+node:michael.20141215222649.73:class PageRef
// a page is identified by a page id
// because of "copy on write" for concurrent storages
// there can be multiple pages with the same id
// but each page has a unique pageoffset_t
typedef boost::uint32_t pageid_t;     // page id 
typedef boost::uint32_t pageoffset_t; // points to page inside file

struct Node;

class PageRef {
 public:
  PageRef(Page *page, pageid_t id, pageoffset_t offset) :
    _page(page), _id(id), _offset(offset) { }

  NodePtr* get_node_ptr(nodeid_t, id) {
      return &_page->node_ptr[id];
    }
 
  nodetype_t type(nodeid_t id) const {
      return _page->node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&_page->data[_page->node_ptr[id].ptr*16];
    }
    
  nodeid_t get_extra(nodeid_t id) const {
      return _page->node_ptr[id].extra;
    }
    
  void get_extra(nodeid_t id, nodeid_t v) const {
      _page->node_ptr[id].extra = v;
    }

  size_t free_size() const {
      return 4096 - _page.free_start*16 - PAGE_HEADER_SIZE 
        - (_page->last_node+1)*sizeof(NodePtr)
    }

 private:
  Page *_page;
  pageid_t _id;
  pageoffset_t _offset;
};

//@-node:michael.20141215222649.73:class PageRef
//@-others
//@nonl
//@-node:michael.20141215222649.72:Page
//@+node:michael.20141219202729.2:class NodeRef
struct Node { };
struct Trace;

// A reference to a Node
class NodeRef {
 public:
  NodeRef(const PageRef& page_, nodeid_t id_)
    : page(page_), id(id_) { }

  nodetype_t type() const {
      return page.nodetype(id);
    }
  
  Node* get_node() const {
      return page.get_node(id);
    }
    
  nodeid_t extra() const {
      return page.get_extra(id);
    }
    
  void extra(nodeid_t v) const {
      return page.set_extra(id, v);
    }
 
  // the following methods walks the trie, and fills the trace
  void find(const Slice& key, HandlerContext& context) {
      trie_handlers[type()]->find(key, *this, context);
    }
    
  void next(HandlerContext& context) {
      trie_handlers[type()]->next(*this, context);
    }
    
  void prev(HandlerContext& context) {
      trie_handlers[type()]->prev(*this, context);
    }
    
  void first(HandlerContext& context) {
      trie_handlers[type()]->first(*this, context);
    }
    
  void last(HandlerContext& context) {
      trie_handlers[type()]->last(*this, context);
    }
  
  void pop(HandlerContext& context) {
      trie_handlers[type()]->pop(*this, context);
    }
    
  void add(const Slice& key, const Slice& value, HandlerContext& context) {
      trie_handlers[type()]->add(key, value, *this, context);
    }

  PageRef page;
  nodeid_t id;
};


// A stack trace inside a trie
// nodes[0] is the first node after root
// nodes[K] is the current node (almost always a leaf)
// key[0] causes the transition from root to nodes[1]
struct Trace {
  std::vector<NodeRef> nodes; 
  std::string key;
  bool last_was_empty;  // true if the last trie index was empty
  
  Trace() : last_was_empty(false) { }
  
  void reset() {
      nodes.clear(0);
      key.clear();
      last_was_empty(false);
    }
};


void NodeRef::find(const Slice& key, NodeStorage& storage, 
                   PageMap& map, Trace& trace) {
}

struct HandlerContext {
  Trace trace;
  Pagemap& map;
  NodeStorage& storage;
};
//@nonl
//@-node:michael.20141219202729.2:class NodeRef
//@+node:michael.20141219202729.3:class PageMap
class NodeStorage;

// Translates a pageid to page offset
class PageMap {
 public:
  PageMap(NodeStorage& storage) : _storage(storage) { }
 
  virtual pageoffset_t get_offset(pageid_t id) = 0;
  
  // delegators to _storage
  
  PageRef get_page(pageid_t id) {
      return _storage.get_page(get_offset(id), id);
    }
  PageRef new_page() {
      return _storage.new_page();
    }
  void free_page(pageid_t id) {
      _storage.free_page(id);
    }

 protected:
  NodeStorage& _storage;
};

//@-node:michael.20141219202729.3:class PageMap
//@+node:michael.20141215222649.135:class NodeStorage (Declaration)
class NodeStorage {
 public:
  virtual std::shared_ptr<PageMap> get_pagemap(version_t version) = 0;
  virtual PageRef get_page(pageoffset_t offset, pageid_t id) = 0;
  virtual PageRef new_page() = 0;
  virtual void free_page(pageid_t id) = 0;
  virtual void flush(async=true) { }
  virtual void start_write() {}
  virtual void end_write() {}
};


class NodeStorageInHeap : public NodeStorage {
 public:
 
  class HeapPageMap : public PageMap {
   virtual pageoffset_t get_offset(pageid_t id) { 
      return (pageoffset_t*)id; }
  };

 
  NodeStorageInHeap() 
    : _free_pages(0), _pagemap(new HeapPageMap(*this)) {}
    
  virtual std::shared_ptr<PageMap> get_pagemap(version_t version) {
      return _pagemap;
    }
    
  virtual PageRef get_page(pageoffset_t offset, pageid_t id) {
       return PageRef(_pages[offset].get(), offset, id);
    }
    
  virtual PageRef new_page();
  virtual void free_page(pageid_t id);
    
 private:
  HeapPageMap _pagemap;
  typedef std::unique_ptr<Page> _page_ptr;
  std::vector<_page_ptr> _pages;
  size_t _free_pages;  // count of _free_pages inside the vector
};


class PersistentNodeStorage : public PersistentNodeStorage {
 public:
  PersistentNodeStorage(const char* path);
  virtual std::shared_ptr<PageMap> get_pagemap(version_t version) = 0;
  virtual PageRef new_page();
  virtual void free_page(pageid_t pageid);
  virtual void flush(async=true)
  
 private:
  void grow_file(size_t size);
  void shrink_file(size_t size);
 
  PagePositionMap Page;
  boost::interprocess::file_mapping _file_mapping;
}


class MultiProcessNodeStorage : public PersistentNodeStorage {
 public:
  MultiProcessNodeStorage(const char* path);
}
//@-node:michael.20141215222649.135:class NodeStorage (Declaration)
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
//@nonl
//@-node:michael.20141215222649.65:@shadow node.h
//@-leo
