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
//@-<< includes >>

namespace larch_leaves {

//@+others
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

// enhance the size to be a multiple of 16
inline size_t pad16(size_t size) {
  size_t rest = size & 0xf;
  if (rest)
    size += 16-rest;
    
  return size;
}
//@+node:michael.20141215222649.99: ** NodeHandler
struct NodeRef;
struct Trace;
struct TempNode;

struct NodeHandler {
  // returns the nodes children
  virtual size_t get_children(NodeRef& rnode, nodeid_t children[65]) = 0;
  // replace the nodes children with the nodeids of children
  virtual void replace_children(NodeRef& rnode, nodeid_t children[65]) = 0;
  
  // replaces a single child
  virtual void replace_child(NodeRef& rnode, int index, nodeid_t child) = 0;
  
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(const Slice& key, NodeRef& rnode, Trace& trace) = 0;
  virtual void next(NodeRef& rnode, Trace& trace) = 0;
  virtual void prev(NodeRef& rnode, Trace& trace) = 0;
  virtual void first(NodeRef& rnode, Trace& trace) = 0;
  virtual void last(NodeRef& rnode, Trace& trace) = 0;
  virtual void add(const Slice& key, const TempNode& end, 
                   NodeRef& rnode, Trace& trace) = 0;
  // remove the traces last_index
  virtual bool remove_last_index(NodeRef& rnode, Trace& trace) {
      return false;
    }
    
  static NodeHandler* handlers[6];
};

enum NodeTypes {
  kLeaf = 0, kPageLeaf, kLink, kCompressed, kTrie, kBitTrie
};
//@+node:michael.20141215222649.67: ** Page
// A pointer inside a page with a 16 byte alignment (256*16 = 4096)
typedef boost::uint8_t inpage_ptr;

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
struct Page {
  union {
    char data[4096];
    struct {
      union {
        struct {
          bsize_t node_count;  // node count
          bsize_t link_count;   // count of link nodes
          inpage_ptr free_start; 
        };
        char header[4];
      };
      pageid_t links[];
    };
  };      
      
  Page() : node_count(0), link_count(0), free_start(0) { }
};

//@+node:michael.20141215222649.73: ** PageRef
struct Node { };

#define REMOVE_BIT 0x80

// the access class to Page
struct PageRef {
  Page *page;
  pageid_t id;
  pageoffset_t offset;
  NodePtr* node_ptr;

  //@+others
  //@+node:michael.20141230111914.41: *3* update_node_ptr
  void update_node_ptr() const {
      ((PageRef*)this)->node_ptr = (NodePtr*)
  &page->data[sizeof(page->header)+page->link_count*sizeof(pageid_t)];
    }
  //@+node:michael.20141230111914.47: *3* PageRef
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
      page(page_), id(id_), offset(offset_) {
      update_node_ptr();
     }
  //@+node:michael.20141230111914.38: *3* free_size
  size_t free_size() const {
      return sizeof(page->data) - ((size_t)(page->free_start))*16 
        - page->link_count * sizeof(pageid_t) 
        - page->node_count * sizeof(NodePtr)
        - sizeof(page->header);
    }
  //@+node:michael.20141230111914.39: *3* size
  // returns only the node occupied size (without pageheader)
  // but with nodeptrs and links
  size_t size() const {
      return page->link_count * sizeof(pageid_t) // links
          + page->node_count * sizeof(NodePtr)    // node_ptr
          + ((size_t)page->free_start)*16;
    }
  //@+node:michael.20141230111914.40: *3* count
  size_t count() const {
      return page->node_count;
    }
  //@+node:michael.20141230111914.32: *3* get_node_ptr
  NodePtr* get_node_ptr(nodeid_t id) {
      return &node_ptr[id];
    }
  //@+node:michael.20141230111914.33: *3* get_node_type
  nodetype_t get_node_type(nodeid_t id) const {
      return node_ptr[id].type;
    }
  //@+node:michael.20141230111914.34: *3* get_node
  Node* get_node(nodeid_t id) const {
      return (Node*)&page->data[node_ptr[id].ptr*16];
    }
  //@+node:michael.20141230111914.35: *3* get_extra
  bsize_t* get_extra(nodeid_t id) const {
      return &node_ptr[id].extra;
    }
  //@+node:michael.20141230111914.36: *3* get_link
  pageid_t* get_link(nodeid_t id) const {
      bsize_t linkid = node_ptr[id].ptr;
      return &page->links[linkid];
    }
  //@+node:michael.20141230111914.37: *3* get_node_size
  size_t get_node_size(nodeid_t id) const {
      NodePtr* ptr = node_ptr;
      
      if (ptr[id].type == kLink)
        return sizeof(pageid_t);
      
      if (id == 0)
        return (256 - ptr[id].ptr) * 16;
        
      return (ptr[id-1].ptr - ptr[id].ptr)*16;
    }
  //@+node:michael.20141230111914.44: *3* new_node
  // creates a new node(size must be a multiple of 16)
  NodeRef new_node(size_t size) const;
  //@+node:michael.20141230111914.43: *3* free_node
  void free_node(nodeid_t id) const {
      node_ptr[id].type |= REMOVE_BIT;
    }
  //@+node:michael.20141230111914.42: *3* defragment
  void defragment();
  //@+node:michael.20141230111914.45: *3* grow_node
  // grows or shrinks a node by size. size must be a multiple of 16
  void grow_node(nodeid_t id, int size);
  //@+node:michael.20141230111914.46: *3* create_link
  void create_link(nodeid_t node_id, pageid_t page_id) const;
  //@-others
};

//@+node:michael.20141230111914.111: ** Nodes Structs
struct PageLeaf {
  char data[];
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
  
  void set_type(nodetype_t type) {
      page.node_ptr[id].type = type;
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

  //@+others
  //@+node:michael.20141230111914.76: *3* Convenient Delegators
  void find(const Slice& key, Trace& trace) {
      NodeHandler::handlers[type()]->find(key, *this, trace);
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
   
  void add(const Slice& key, const TempNode& end, Trace& trace) {
      NodeHandler::handlers[type()]->add(key, end, *this, trace);
    }

  bool remove_last_index(Trace& trace) {
      return NodeHandler::handlers[type()]->remove_last_index(*this, trace);
    }
   
  size_t get_children(nodeid_t children[65]) const {
      NodeHandler* handler = NodeHandler::handlers[type()];
      return handler->get_children((NodeRef&)*this, children);
    }

  void replace_children(nodeid_t children[65]) {
      NodeHandler::handlers[type()]->replace_children(*this, children);
    }
    
  void replace_child(int index, nodeid_t child) {
      NodeHandler::handlers[type()]->replace_child(*this, index, child);
    }
  //@-others
};



//@+node:michael.20141220220750.4: ** TempNode
struct TempNode {
  Page page;
  PageRef pageref;
  NodeRef noderef;
  
  TempNode()
    : pageref(&page, 0, 0), noderef(pageref, 0) { 
      *noderef.extra() = 0;
    }
    
  size_t size() const {
      return noderef.size();
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
  
  NodeStorageInHeap()
    : _free_pages(0) {
    new_page(); // create root page
  }

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
// nodes[0] is the first node after root
// nodes[K] is the current node (almost always a leaf)
// key[0] causes the transition from root to nodes[1]
struct Trace {
  struct Transition {
    NodeRef node;
    size_t index; // the index inside Trace.key identifying
                  // the part causing the transition to node
    Transition(NodeRef& node_, size_t index_)
        : node(node_), index(index_) { }
    Transition()
    : node(PageRef(NULL, 0, 0), 0), index(0) {}
  };

  std::vector<Transition> stack;
  std::string key;
  int last_index; // the key index before the last pop (-1 if complete)
  bool complete;  // true if key is completely
  PageMap& map;   // needed for some operations
  NodeStorage& storage;  // needed for some operations
  
  Trace(PageMap& map_, NodeStorage& storage_) :
      complete(false), map(map_), storage(storage_) { }
  
  //@+others
  //@+node:michael.20141230111914.112: *3* size
  size_t size() const {
      return stack.size();
    }
  //@+node:michael.20141230111914.120: *3* current
  NodeRef& current() {
      return stack.back().node;
    }
  //@+node:michael.20141230111914.113: *3* find
  // tries to find key inside the trace returns true if found
  bool find(const Slice& key_) {
      size_t s = common_prefix(key_.data(), key.data(), 
                               std::min(key_.size(), key.size()));
      if (!s)
        return false;
        
      for(size_t i = 0; i < stack.size(); i++) {
        if (stack[i].index > s) {
          complete = false;
          stack.resize(i);
          key.resize(stack.back().index);
          current().find(key_.advance(key.size()), *this);
          return true;
        }
      }

      assert(0);  
    }
  //@+node:michael.20141230111914.119: *3* parent
  NodeRef& parent() {
      NodeRef parent = stack[stack.size()-2].node;
      return parent.id == kLink ? stack[stack.size()-3].node : parent;
    }
  //@+node:michael.20141230111914.114: *3* value
  Slice value() {
      if (!complete)
        throw NoValidPosition();
     
      NodeRef& c(current());
      if (c.type() == kPageLeaf) {
        PageLeaf* n = (PageLeaf*)c.node();
        return Slice(n->data, *c.extra());
      }
      return Slice();
    }
  //@+node:michael.20141230111914.116: *3* reset
  void reset() {
      stack.clear();
      key.clear();
      complete = false;
    }
  //@+node:michael.20141230111914.117: *3* push
  void push(NodeRef& node) {
      size_t index = key.size();
      stack.push_back(Transition(node, index));
    }
  //@+node:michael.20141230111914.118: *3* pop
  void pop(bool skiplink=true) {
      if (!size())
        return;

      Transition& back(stack.back());
      last_index = complete ? -1 : key[back.index];
      complete = false;
      key.resize(back.index);
      stack.pop_back();
      if (skiplink && current().id == kLink)
        stack.pop_back();
    }
  //@+node:michael.20141230111914.121: *3* parent_next
  void parent_next() {
      pop();
      if (size())
        current().next(*this);
    }
  //@+node:michael.20141230111914.122: *3* parent_prev
  void parent_prev() {
      pop();
      if (size())
        current().prev(*this);
    }
  //@+node:michael.20141230111914.123: *3* remove
  void remove() {
      if (!complete)
        throw NoValidPosition();

      while(stack.size()) {
        NodeRef& me(current()); 
        if (me.remove_last_index(*this)) {
          me.page.defragment();
          merge_with_parent();
          return;
        }
        
        free_node(me.page, me.id);
        pop(false);
       }
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
  void add_node(const TempNode& new_node, const Slice& transition_key) {
      // transistion_key causes transitions from parent to node
      size_t size = new_node.size();
      reserve_space(size + sizeof(NodePtr));
      NodeRef new_ref(current().page.new_node(size));
      memcpy(new_ref.node(), new_node.node(), size);
      *new_ref.extra() = *new_node.extra();
      new_ref.set_type(new_node.type());
      push(new_ref);
      key.append(transition_key.data(), transition_key.size());
   }
  //@+node:michael.20141230111914.128: *3* grow_node_by
  // grows or shrink the current node
  void grow_node_by(int delta) {
      if (delta > 0)
        reserve_space(delta);
      
      current().page.grow_node(current().id, delta);  
    }
  //@+node:michael.20141230111914.127: *3* change_node
  // change current node to type
  void change_node(const TempNode& new_node) {
      size_t size = current().size(), new_size = new_node.size();
      grow_node_by((int)new_size-(int)size);
      memcpy(current().node(), new_node.node(), new_size);
      *current().extra() = *new_node.extra();
      current().set_type(new_node.type());
    }
  //@+node:michael.20141230111914.115: *3* set_value
  void set_value(const Slice& value) {
      TempLeaf leaf(value);
      if (complete)
        change_node(leaf);
      else 
        current().add(Slice(), leaf, *this);
    }
  //@+node:michael.20141230111914.133: *3* move_node
  // moves the node to new page returning the id on the new_page
  // precondition: there must be enough space in new_page
  nodeid_t move_node(const PageRef& new_page, const NodeRef& node) {
      if (new_page.id == node.page.id)
        return node.id;
        
      if (node.type() == kLink) {
        NodeRef new_node(new_page.new_node(0));
        new_page.create_link(new_node.id, *node.link());
        return new_node.id;
      }

      nodeid_t children[65];
      size_t count = node.get_children(children);
      
      size_t size = node.size();
      NodeRef new_node(new_page.new_node(size));
      memcpy(new_node.node(), node.node(), size);
      node.page.free_node(node.id);
      
      for(size_t i = 0; i < count; i++) {
  NodeRef child(node.page, children[i]);
        children[i] = move_node(new_page, child);
      }
      
      new_node.replace_children(children);
      return new_node.id;
    }
  //@+node:michael.20141230111914.129: *3* merge_with_parent
  // merges the current page with the parent page if possible 
  void merge_with_parent() {
      PageRef& page(current().page);
      for(int i = stack.size()-2; i >= 0; i--) {
        PageRef parent(stack[i].node.page);
        if (parent.id != page.id) {
          if (parent.free_size() >= page.size()) {
            NodeRef page_root(page, 0);
            nodeid_t child = move_node(parent, page_root);
            int old_last_index = last_index;
            pop();
            current().replace_child(last_index, child);
            current().find(Slice(last_index), *this);
            last_index = old_last_index;
            map.free_page(page);
          }
          break;
        }
      }
    }
  //@+node:michael.20141230111914.130: *3* refresh_trace_if_contains
  // refresh the hash for all nodes belonging to the same page as current()
  // if node is in trace
  void refresh_trace_if_contains(NodeRef& node) {
      pageid_t pageid = node.page.id;
      size_t key_index = key.size();
      bool refresh = false;
      int i;
      for(i = stack.size()-1; i >= 0; i--) {
        NodeRef& item(stack[i].node);
        if (item.page.id == pageid) {
          key_index = stack[i].index;
          if (item.id == node.id)
            refresh = true;
        }
        else {
          // the first item in stack not containing to the same page
          if (refresh) {
      std::string suffix(key, key_index);
            stack.resize(i+1);
            current().find(suffix, *this);
          }
          return;
        }
      }
    }
  //@+node:michael.20141230111914.131: *3* calc_sizes
  size_t calc_sizes(const NodeRef& node, size_t sizes[256]) {
      nodeid_t children[65];
      size_t size = node.size();
      size_t count = node.get_children(children);
      
      for(size_t i = 0; i < count; i++)
        size += calc_sizes(NodeRef(node.page, children[i]), sizes);
        
      size += sizeof(NodeRef);
      if (sizes[node.id])
        size = sizes[node.id];
      else
        sizes[node.id] = size;
        
      return size;
    }
  //@+node:michael.20141230111914.132: *3* reserve_space
  // enusers that the page of current node has a free_space >  size
  // place current node to a new page if nessary
  // if exclude is set this node will not move from page
  void reserve_space(size_t size, nodeid_t exclude=0) {
      // during this loop current() can change its page!
      while (current().page.free_size() < size) {
        size_t sizes[256];
        NodeRef root(current().page, 0);
        memset(sizes, 0, sizeof(sizes));
        sizes[exclude] = sizeof(Page);
        calc_sizes(root, sizes);
        int best = sizeof(Page);
        nodeid_t best_id = 1;
          
        // i = 1: it makes no sense to move the root node
        for(size_t i = 1; i < root.page.count(); i++) {
          int delta = abs(sizes[i]-sizeof(Page)/2);
          if (delta < best) {
            best = delta;
            best_id = i;
          }
        }
        PageRef newpage = storage.new_page();
        NodeRef to_move(NodeRef(newpage, best_id));
        move_node(newpage, to_move);
        
        refresh_trace_if_contains(to_move);
        root.page.create_link(best_id, newpage.id);
        root.page.defragment();
      }
    }
  //@-others
};

//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
//@-leo
