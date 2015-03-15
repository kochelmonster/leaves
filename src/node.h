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
#include <vector>
#include <unordered_map>
#include <list>
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
/* 
  the maximum key key size
  
  it is choosen arcording to MAX_BUCKET_SIZE (see below)
  
  MAX_KEY_SIZE = 3/4 * MAX_KEY_SIZE_64
  
  
  360 byte is not very much, but for a key usually quite enought.
  
  Here is a method to store bigger keys:
     cut the key and add a hash of the key so it 
     does not exceed 360 bytes, eg:
       key[:296] + sha-512
       
     and store the complete key in the value (if it is needed)
     so the key are sorted and distinct.
    
  if this method is not sufficient use another db.  
*/
#define MAX_KEY_SIZE 360
#define MAX_KEY_SIZE_64 480
#define KEY_EXEEDS "key may not exceed 360 bytes"

#define MAX_PAGE_VALUE_SIZE 32
#define VALUE_EXEEDS "value may not exceed 32 bytes"

#ifndef PAGE_SIZE 
#define PAGE_SIZE 8192
#endif
#ifndef ALIGN
#define ALIGN 4
#endif 
//#define PAGE_SPLIT_SIZE (3*PAGE_SIZE/4)
#define PAGE_SPLIT_SIZE (PAGE_SIZE/2)

#define MAX_NODE_COUNT (PAGE_SIZE/ALIGN)
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
typedef boost::uint16_t nodeid_t;
typedef boost::uint8_t nodetype_t;

// a value size: either a value below 127
// or 0x88 which is a (8byte) pointer to leaf storage
// (the byte size can be determined with v & 7f)
typedef boost::uint8_t vsize_t; 


typedef size_t leaf_ptr;
//@+node:michael.20141230111914.74: ** Utils
inline size_t common_prefix(const char* s1, const char *s2, size_t size) {
  size_t i;
  size = std::min(size, (size_t)0xff);
  for(i = 0; i < size && *s1 == *s2; i++, s1++, s2++);
  return i;
}

template<size_t align> size_t pad(size_t size) {
  return size + (align-1) - ((size-1)&(align-1));
}

inline size_t page_pad(size_t size) {
  return pad<ALIGN>(size);
}


//@+node:michael.20141215222649.99: ** NodeHandler
struct NodeRef;
struct Trace;
struct TempNode;
struct Node;
struct NodePtr;
struct Page;


struct NodeHandler {
  // returns the count of character the node consumes of the key
  // tries==1, compressed>1, leaves==0
  virtual size_t get_len(const NodeRef& rnode) = 0;

  // returns true if the last rnode points to a valid key position
  virtual bool is_valid(const NodeRef& rnode, const Trace& trace) const {
      return false;
    }

  virtual Slice get_value(const NodeRef& rnode, Node* data, Trace& trace) {
      assert(0); // only leafs, bucket or hash nodes;
      return Slice();
    }
  
  // returns the nodes children
  virtual size_t get_children(NodePtr* ptr, Node* data, 
                              nodeid_t children[65]) = 0;
  // replace the nodes children with the nodeids of children
  virtual void replace_children(NodePtr* ptr, Node* data, 
                                nodeid_t children[65]) = 0;
  
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(NodeRef& rnode, Node* data, Trace& trace) = 0;
  virtual void next(NodeRef& rnode, Node* data, Trace& trace) = 0;
  virtual void prev(NodeRef& rnode, Node* data, Trace& trace) = 0;
  virtual void first(NodeRef& rnode, Node* data, Trace& trace) = 0;
  virtual void last(NodeRef& rnode, Node* data, Trace& trace) = 0;
  
  // appends the key for the transition to child
  virtual Slice append(NodeRef& rnode, Node* data, nodeid_t child, Slice& key) {
      return key;
    }
  
  // returns true if a node is inserted, false if just the value was changed
  virtual bool add(const TempNode& leaf, NodeRef& rnode, 
                   Node* data, Trace& trace) = 0;
  // remove the node
  virtual bool remove_child(NodeRef& rnode, Node* data, Trace& trace) {
      return false;
    }
    
  // rnode tries to merge the child with itself or to a compressed
  virtual bool eat_child(NodeRef& rnode, Node* data) = 0;
    
#ifdef DEBUG
  virtual void dump(Page* page, nodeid_t nodeid, std::ostream& out) = 0;
#endif

  static NodeHandler* handlers[7];
};

enum NodeTypes {
  kLeaf = 0, kLink, kCompressed, kTrie, kBitTrie, kRemoved
};
//@+node:michael.20141215222649.67: ** Page
enum PageTypes {
  kTriePage = 0
};

// A pointer inside a page
typedef boost::uint16_t inpage_ptr;
typedef boost::uint16_t psize_t; // size of an object within a page

struct NodePtr {
  inpage_ptr offset;   // position inside page
  struct {
    boost::uint16_t type:4;    // node type
    boost::uint16_t extra:12;  // is used by several nodes
  };
};


/*
  A Page has the following layout:
  
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

#define PAGE_HEADER_SIZE (2*sizeof(boost::uint16_t))

struct Page {
  union {
    char data[PAGE_SIZE];
    struct {
      boost::uint16_t type;
      boost::uint16_t count; // node count
      NodePtr node_ptr[1];
    };
  };      
      
  Page() : type(kTriePage), count(0) { }
  
  nodeid_t new_node(size_t size_);
  
  size_t get_node_size(nodeid_t id) const {
    const NodePtr* ptr = node_ptr + id;
    return (id ? (ptr-1)->offset : sizeof(Page)) - ptr->offset;
  }
  
  size_t free_size() const {
      size_t nc = count;
      if (nc)
        return node_ptr[nc-1].offset - nc*sizeof(NodePtr) - PAGE_HEADER_SIZE;
      
      return sizeof(Page) - PAGE_HEADER_SIZE;
    }
  
  // returns only the node occupied size (without pageheader)
  size_t size() const {
      return sizeof(Page) - free_size() - PAGE_HEADER_SIZE;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&data[node_ptr[id].offset];
    }
    
  void init_sizes(size_t sizes[MAX_NODE_COUNT]) {
      size_t offset = sizeof(Page), count_ = count, *s = sizes;
      NodePtr *ptr = node_ptr;
      
      for(size_t i = 0; i < count_; i++, ptr++, s++) {
        *s = offset - ptr->offset + sizeof(NodePtr);
        offset = ptr->offset;
      }
    }
};

//@+node:michael.20141215222649.73: ** PageRef
// the access class to Page
struct PageRef {
  Page *page;
  pageid_t id;
  pageoffset_t offset;

  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
    page(page_), id(id_), offset(offset_) { }

  PageRef() : PageRef(NULL, 0, 0) { }
 
  //@+others
  //@+node:michael.20150112164548.4: *3* declarations
  // returns true f the trace was refresh
  bool defragment(Trace& trace) const;
  // grows or shrinks a node by size. size must be a multiple of 16
  void grow_node_by(nodeid_t node_id, int size) const;
  void change_to_link(nodeid_t node_id, pageid_t page_id) const;

  #ifdef DEBUG
    void dump(std::ostream& out);
  #endif

  void check(const char *msg, Trace& trace);
  //@+node:michael.20150112164548.5: *3* count
  size_t count() const {
      return page->count;
    }
  //@+node:michael.20141230111914.39: *3* size
  size_t size() const {
      return page->size();
    }
  //@+node:michael.20141230111914.38: *3* free_size
  size_t free_size() const {
      return page->free_size();
    }
  //@+node:michael.20141230111914.37: *3* get_node_size
  size_t get_node_size(nodeid_t id) const {
      return page->get_node_size(id);
    }
  //@+node:michael.20141230111914.43: *3* free_node
  void free_node(nodeid_t id_) const {
      page->node_ptr[id_].type = kRemoved;
    }
  //@+node:michael.20150112164548.7: *3* new_node
  nodeid_t new_node(size_t size_) const {
    return page->new_node(size_);
  }
  //@-others
};
//@+node:michael.20141219202729.2: ** NodeRef
// The Access class to Node
struct NodeRef {
  PageRef page;
  nodeid_t id;
  NodePtr* ptr;
  NodeHandler* handler;
    
  NodeRef(PageRef page_, nodeid_t id_)
    : page(page_), id(id_), ptr(NULL), handler(NULL) {
      if (page_.page) {
        ptr = page_.page->node_ptr+id;
        handler = NodeHandler::handlers[type()];
      }
    }

  nodetype_t type() const {
      return ptr->type;
    }
  
  void set_type(nodetype_t type) const {
      ptr->type = type;
      ((NodeRef*)this)->handler = NodeHandler::handlers[type];
    }
  
  Node* node() const {
      return (Node*)&page.page->data[ptr->offset];
    }
    
  nodeid_t extra() const {
      return ptr->extra;
    }
    
  void set_extra(nodeid_t extra) const {
      ptr->extra = extra;
    }

  size_t size() const {
      return page.get_node_size(id);
    }

  bool operator==(const NodeRef& other) const {
      return id == other.id && page.id == other.page.id;
    }
  
  bool is_leaf() const {
      return type() == kLeaf;
    }
      
  // for leaf and compress nodes
  size_t len() const {
      return extra();
    }
    
  void set_len(size_t len) const {
      set_extra((nodeid_t)len);
    }
    
  //@+others
  //@+node:michael.20141230111914.76: *3* Convenient Delegators
  size_t get_len() const {
      return handler->get_len(*this);
    }

  bool is_valid(const Trace& trace) const {
      return handler->is_valid(*this, trace);
    }

  Slice get_value(Trace& trace) {
      return handler->get_value(*this, node(), trace);
    }

  void find(Trace& trace) {
      handler->find(*this, node(), trace);
    }
    
  void next(Trace& trace) {
      handler->next(*this, node(), trace);
    }
    
  void prev(Trace& trace) {
      handler->prev(*this, node(), trace);
    }
    
  void first(Trace& trace) {
      handler->first(*this, node(), trace);
    }
    
  void last(Trace& trace) {
      handler->last(*this, node(), trace);
    }
   
  Slice append(nodeid_t child, Slice& key) {
      return handler->append(*this, node(), child, key);
    }

  bool add(const TempNode& leaf, Trace& trace) {
      return handler->add(leaf, *this, node(), trace);
    }

  bool remove_child(Trace& trace) {
      return handler->remove_child(*this, node(), trace);
    }
   
  size_t get_children(nodeid_t children[65]) const {
      return handler->get_children(ptr, node(), children);
    }

  void replace_children(nodeid_t children[65]) {
      handler->replace_children(ptr, node(), children);
    }
    
  bool eat_child() {
      return handler->eat_child(*this, node());
    }
    
  void child_find(nodeid_t child_id, Trace& trace);
  void child_first(nodeid_t child_id, Trace& trace);
  void child_last(nodeid_t child_id, Trace& trace);


  //@-others
};

template<typename src_t> void copy_node(const NodeRef& dst, const src_t& src) {
    memcpy(dst.node(), src.node(), src.size());
    dst.set_extra(src.extra());
    dst.set_type(src.type());
  }

//@+node:michael.20141220220750.4: ** TempNode
struct TempNode {
  size_t _size;
  struct {
    boost::uint16_t _type:4;
    boost::uint16_t _extra:12;
  };
  Node* _node;
     
  size_t size() const {
      return page_pad(_size);
    }
    
  Node* node() const {
      return _node;
    }
    
  nodeid_t extra() const {
      return _extra;
    }
    
  void set_extra(nodeid_t extra) {
      _extra = extra;
    }    

  nodetype_t type() const {
      return _type;
    }

  size_t len() const {
      return extra();
    }
    
  void set_len(size_t len) {
      set_extra((nodeid_t)len);
    }    
};


struct TempLeaf : public TempNode {
  TempLeaf(const Slice& value);
  TempLeaf(leaf_ptr* ptr);
};

struct TempTrie : public TempNode {
  nodeid_t _children[64];
  TempTrie(size_t child_count=1);
};

struct TempCompressed : public TempNode {
  char _data[MAX_KEY_SIZE_64 + sizeof(nodeid_t)];
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
  virtual void check(const char *msg, Trace& trace) { }
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
        : node(PageRef(), 0), start(0), end(0) { }
  };

  typedef std::vector<Transition> stack_t;

  stack_t stack;
  std::string key;       // the key the trace points to
  PageMap& map;          // needed for some operations
  NodeStorage& storage;  // needed for some operations
  Transition *back;
    
  size_t stat_find_trace;
  size_t stat_reuse_trace;
  size_t stat_find_count;
  size_t stat_compressed;
  size_t stat_tries;
  size_t stat_link;
  size_t stat_short_find;
  size_t stat_wrong_prefix;
  size_t min_page_key;
  size_t max_page_key;
    
  void reset_statistics() {
      stat_find_count = 0;
      stat_reuse_trace = 0;
      stat_find_trace = 0;
      stat_tries = 0;
      stat_compressed = 0;
      stat_link = 0;
      stat_short_find = 0;
      stat_wrong_prefix = 0;
    }
  
  Trace(PageMap& map_, NodeStorage& storage_) :
      map(map_), storage(storage_) { 
        key.reserve(MAX_KEY_SIZE_64);
        reset_statistics();
        min_page_key = MAX_KEY_SIZE_64;
        max_page_key = 0;
      }

  void check(const char *msg) { 
      storage.check(msg, *this);
    }
  
  //@+others
  //@+node:michael.20150111191610.5: *3* declarations
  void find(const Slice& key_);
  // moves the node to new page returning the id on the new_page
  // precondition: there must be enough space in new_page  
  nodeid_t move_node(const PageRef& new_page, const NodeRef& node);
  // merges the current page with the parent page if possible 
  void merge_pages();
  // ensures that the page of current node has a free_space >  size
  // place current node to a new page if nessary
  // if exclude is set this node will not move from page
  void reserve_space(size_t size_);
  //@+node:michael.20141230111914.112: *3* size
  size_t size() const {
      return stack.size();
    }
  //@+node:michael.20150111191610.2: *3* stack helpers
  void _push(const Transition& t) {
    stack.push_back(t);
    back = &stack.back();
  }

  void _pop() {
    stack.pop_back();
    back = &stack.back();
  }
  //@+node:michael.20150303095026.5: *3* new_page_in_hash
  // returns true if a new page should be hashed
  bool new_page_in_hash() const {
      size_t link_count = 0;
      stack_t::const_iterator i;
      for(i = stack.cbegin(); i != stack.cend(); i++) {
        if (i->node.type() == kLink && ++link_count > 2)
          return true;
      }
      return false;
   }
  //@+node:michael.20150303095026.6: *3* add_page_to_hash
  struct PageHashVal {
    pageid_t pageid;
    size_t root_start;

    PageHashVal(pageid_t id=0, size_t start=0)
      : pageid(id), root_start(start) { 
      }
  };

  std::unordered_map<std::string, PageHashVal> page_hash;

    
  void add_page_to_hash(Slice& key, const PageRef& page) {
    size_t reduced_len = key.size(); // / 4;
    std::string ks = key.string();
    page_hash[ks] = PageHashVal(page.id, ks.size());
    min_page_key = std::min(min_page_key, reduced_len);
    max_page_key = std::max(max_page_key, reduced_len);
    //std::cerr << "--add page: " << page.id << ", " << trace_len << ", " 
    //          << reduced_len << ", " << key.size() << std::endl;
  }
  //@+node:michael.20150106224503.48: *3* is_valid
  // returns true if it points to a valid position
  bool is_valid() const {
      return current().is_valid(*this);
    }
  //@+node:michael.20150101205559.11: *3* check_valid
  void check_valid() const {
      if (!is_valid())
        throw NoValidPosition();
    }
  //@+node:michael.20141230111914.120: *3* current
  NodeRef& current() {
      return back->node;
    }

  const NodeRef& current() const {
      return back->node;
    }
  //@+node:michael.20150106224503.56: *3* current_key
  Slice current_key() const {
      int size = std::max((int)key.size()-(int)back->start, (int)0);
      return Slice(key.data()+back->start, size);
    }
  //@+node:michael.20150106224503.55: *3* cut_key
  // cuts the key to the current end
  void cut_key() {
      key.resize(back->start);
    }
  //@+node:michael.20141230111914.119: *3* parent
  NodeRef& parent() {
      NodeRef& parent = stack[stack.size()-2].node;
      return parent.type() == kLink ? stack[stack.size()-3].node : parent;
    }
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
  //@+node:michael.20150118002311.8: *3* resize
  void resize(size_t size) {
      stack.resize(size); 
      back = &stack.back();
    }
  //@+node:michael.20141230111914.116: *3* reset
  void reset() {
      resize(1); // keep the root
      key.clear();
    }
  //@+node:michael.20141230111914.117: *3* push
  NodeRef& push(const NodeRef& node) {
      size_t start = back->end;
      _push(Transition(node, start, start+node.get_len()));
      return current();
    }
      
  void push_root(const NodeRef& node) {
      _push(Transition(node, 0, node.get_len()));
    }
  //@+node:michael.20141230111914.118: *3* pop
  // returns the node id of the skipped link  or 0
  nodeid_t pop() {
      _pop();
      if (size() && current().type() == kLink) {
        nodeid_t skipped_id = current().id;
        _pop();
        return skipped_id;
      }
      return 0;
    }
  //@+node:michael.20141230111914.123: *3* remove
  bool _eat_child(size_t ancestor) {
    int index = (int)(size()-1-ancestor);
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

        if (size() == 1) {
          stack.clear();
          free_node(me.page, me.id);
          break;
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
        *(pageid_t*)link_or_parent.node() = current().page.id;
        return link_or_parent.id;
      }
      return current().id;
    }
  //@+node:michael.20141230111914.126: *3* add_node
  void add_node(const TempNode& src) {
      // transistion_key causes transitions from parent to node
      size_t size_ = src.size();
      reserve_space(size_+sizeof(NodePtr));
      nodeid_t newid = current().page.new_node(size_);
      NodeRef dst(current().page, newid);
      copy_node(dst, src);
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
      size_t size_ = current().size(), new_size = new_node.size();
      grow_node_by((int)new_size-(int)size_);
      copy_node(current(), new_node);
      back->end = back->start + current().get_len();
    }
  //@+node:michael.20141230111914.115: *3* set_leaf
  // sets a new leaf, 
  bool set_leaf(const TempLeaf& leaf) {
      return current().add(leaf, *this);
    }

  //@+node:michael.20150118002311.14: *3* get_value
  Slice get_value() {
      check_valid();
      return current().get_value(*this);
    }
  //@+node:michael.20150101205559.69: *3* refresh_trace
  // refresh the trace remapping the nodeids
  void refresh_trace() {
      stack.resize(1);
      back = &stack.back();
      current().find(*this);
    }
  //@-others
};

//@+node:michael.20150106224503.61: ** NodeRef-inlines
inline void NodeRef::child_find(nodeid_t child_id, Trace& trace) {
  trace.push(NodeRef(page, child_id)).find(trace);
}

inline void NodeRef::child_first(nodeid_t child_id, Trace& trace) {
  trace.push(NodeRef(page, child_id)).first(trace);
}

inline void NodeRef::child_last(nodeid_t child_id, Trace& trace) {
  trace.push(NodeRef(page, child_id)).last(trace);
}
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
//@-leo
