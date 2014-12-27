// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_H
#define _LARCH_LEAVES_NODE_H

#include "boost/cstdint.h"

namespace larch_leaves {

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
  | header (4 byte)      |
  +----------------------+
  | node_ptr[0] (3 byte) |
  +----------------------+
  | node_ptr[1] (3 byte) |
  +----------------------+
  | ...                  |
  +----------------------+ 
  | node_ptr[n] (3 byte) | n = node_count -1
  +----------------------+ <-- link_start poinst to start of links
  | link_node[0] (4byte) |
  +----------------------+
  | link_node[1] (4byte) |
  +----------------------+
  | ...                  |
  +----------------------+
  | link_node[l] (4byte) | l = link_count -1
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
struct Page {
  union {
    char data[4094];
    struct {
      union {
        bsize_t node_count;  // node count
        bize_t link_count;   // count of link nodes
        inpage_ptr free_start; 
        inpage_ptr link_start;
      };
      NodePtr node_ptr[];
    };
  };      
      
  Page() : node_count(0), link_count(0), free_start(0) { }
};

struct Node;

struct PageRef {
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
    page(page_), id(id_), offset(offset_) { }

  NodePtr* get_node_ptr(nodeid_t, id) {
      return &page->node_ptr[id];
    }
 
  nodetype_t type(nodeid_t id) const {
      return page->node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&_page->data[page->node_ptr[id].ptr*16];
    }
    
  nodeid_t* extra(nodeid_t id) const {
      return page->node_ptr[id].extra;
    }
    
  pageid_t* link(nodeid_t id) const {
      bsize_t linkid = page->node_ptr[id].ptr;
      pageid_t* links = (pageid_t*)&page.data[page.link_start];
      return links+linkid;
    }

  size_t free_size() const {
      return 4096 - page.free_start*16 - PAGE_HEADER_SIZE 
        - (page->last_node+1)*sizeof(NodePtr)
    }

  Page *_page;
  pageid_t _id;
  pageoffset_t _offset;
};

struct Node { };
struct Trace;

// A reference to a Node
struct NodeRef {
  NodeRef(const PageRef& page_, nodeid_t id_)
    : page(page_), id(id_) { }

  nodetype_t type() const {
      return page.nodetype(id);
    }
  
  Node* node() const {
      return page.get_node(id);
    }
    
  pageid_t* link() const {
      return page.get_link(id);
    }
    
  nodeid_t* extra() const {
      return page.get_extra(id);
    }
    
  // the following methods walks the trie, and fills the trace
  void find(const Slice& key, Trace& trace) {
      trie_handlers[type()]->find(key, *this, trace);
    }
    
  void next(Trace& trace) {
      trie_handlers[type()]->next(*this, trace);
    }
    
  void prev(Trace& trace) {
      trie_handlers[type()]->prev(*this, trace);
    }
    
  void first(Trace& trace) {
      trie_handlers[type()]->first(*this, trace);
    }
    
  void last(Trace& trace) {
      trie_handlers[type()]->last(*this, trace);
    }
   
  void add(const Slice& key, const Slice& value, Trace& trace) {
      trie_handlers[type()]->add(key, value, *this, trace);
    }

  void remove_last(Trace& trace) {
      trie_handlers[type()]->remove_last(*this, trace);
    }

  PageRef page;
  nodeid_t id;
};



// A stack trace inside a trie
// nodes[0] is the first node after root
// nodes[K] is the current node (almost always a leaf)
// key[0] causes the transition from root to nodes[1]
struct Trace {
  struct Transition {
    NodeRef node;
    size_t index; // the index inside Trace.key identifiying
                  // the part causing the transition to node
    Transition(NodeRef& node_, size_t index_)
        : node(node_), index(index_) { }
  };

  std::vector<Transition> stack;
  std::string key;
  int last_index; // the key index before the last pop (-1 if complete)
  bool complete;  // true if key is completely
  Pagemap& map;   // needed for some operations
  NodeStorage& storage;  // needed for some operations
  
  Trace(Pagemap& map_, NodeStorage& storage_) : 
      map(map_), storage(storage_), complete(false) { }
  
  void reset() {
      nodes.clear(0);
      key.clear();
      complete(false);
    }
    
  void push(NodeRef& node) {
      size_t index = key.size();
      stack.push_back(Transition(node, index));
    }
    
  void pop(bool skiplink=true) {
      Transitions& back(stack.back());
      last_index = complete ? -1 : key[back.index];
      complete = false;
      key.resize(back.index);
      stack.pop_back();
      if (skiplink && current().id == kLink)
        stack.pop_back();
    }
    
  NodeRef& parent() {
      NodeRef parent = stack[stack.size()-2];
      return parent.node.id == kLink ? stack[stack.size()-3] : parent;
    }
    
  NodeRef& current() {
    return stack.back();
  }
    
  void parent_next() {
      pop();
      current().next(*this);
    };
      
  void parent_prev() {
      pop();
      current().prev(*this);
    };

  void remove() {
      while(stack.size()) {
        NodeRef& me(current()); 
        if (me.remove_last_index()) {
          me.page.normalize();
          merge_with_parent();
          return;
        }

        me.page.free_node(me.id, map);
        me.pop(false);
       }
    }
  
  // returns the nodeid that connects parent with current
  // if current is on another page it conntects the link
  // with current's page and retuns the nodeid of the link
  nodeid_t child_of_parent() {
      Transistion link_or_parent(stack[stack.size()-2);
      if (link_or_parent.node.type() == kLink) {
        *link_or_parent.link() = current().page.id;
        return link_or_parent.id;
      }
      return current().id;
    }
  
 void add_node(TempNode& new_node, const Slice& transition_key) {
      // transistion_key causes transitions from parent to node
      size_t size = new_node.size();
      if (current().page.free_size() < size + sizeof(NodePtr))
        split_page();
        
      NodeRef& new_ref(current().page.new_node(new_size));
      memcpy(new_ref.node(), new_node.node, size);
      push(new_ref);
      key.append(transition_key.data(), transition_key.size());
   }
  
  // change current node to type
  void change_node(TempNode& new_node) {
      NodeRef &node(current());
      size_t size = node.size(), new_size = new_node.size();
      nodeid_t nodeid = node.id;
      if (node.page.free_size() + size < new_size) 
        split_page();
      
      page.grow(nodeid, new_size-size);
      memcpy(current().node(), new_node.node(), new_size);
    }
   
  // merges the current page with the parent page if possible 
  void merge_with_parent() {
          
  
    }


  // splits the page in to with about have size on each
  void split_page() {
      
      
  
  
  
    }

  nodeid_t move_node(PageRef& page, nodeid_t id) {
      NodeRef& node(current());
      if (node.page.id == page.id)
        return id;
  
      // move the node an its children to current.page
            
  
  
    }
};
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
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
