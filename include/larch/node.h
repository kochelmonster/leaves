// declarations for the node stroage
#ifndef _LARCH_LEAVES_NODE_H
#define _LARCH_LEAVES_NODE_H

#include <boost/cstdint.h>
#include <stdlib.h> 

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

struct Node;

#define REMOVE_BIT 0x80

struct PageRef {
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
      page(page_), id(id_), offset(offset_) {
      update_node_ptr();
     }

  NodePtr* get_node_ptr(nodeid_t, id) {
      return &node_ptr[id];
    }
 
  nodetype_t get_type(nodeid_t id) const {
      return node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&_page->data[node_ptr[id].ptr*16];
    }
    
  bsize_t* get_extra(nodeid_t id) const {
      return node_ptr[id].extra;
    }
    
  pageid_t* get_link(nodeid_t id) const {
      bsize_t linkid = node_ptr[id].ptr;
      return &page.links[linkid];
    }

  size_t get_node_size(nodeid_t id) const {
      NodePtr* ptr = node_ptr();
      
      if (ptr[id].type == kLink)
        return sizeod(pageid_t);
      
      if (id == 0)
        return (256 - ptr[id].ptr) * 16;
        
      return (ptr[id-1].ptr - ptr[id].ptr)*16;
    }

  size_t free_size() const {
      return sizeof(page->data) - ((size_t)(page->free_start))*16 
        - page->link_count * sizeof(pageptr_t) 
        - page->node_count * sizeof(NodePtr)
        - sizeof(page->header);
    }
    
  // returns only the node occupied size (without pageheader)
  // but with nodeptrs and links
  size_t size() const {
      return page->link_count * sizeof(pageptr_t) // links
          + page->node_count * sizeof(NodePtr)    // node_ptr
          + ((size_t)page->free_start)*16);
    }
    
  size_t count() const {
      return page->node_count;
    }
    
  void update_node_ptr() {
      node_ptr = (NodePtr*)&page->data[sizeof(page->header)
                                       +page->link_count*sizeof(pageptr_t));
    }
    
  void defragment() {
      NodePtr new_ptrs[256]; 
      bsize_t node_count = 0;
      nodeid_t map[256]; // maps old nodeid_s to new node_ids
      size_t size[256]; // is needed later

      // create node map 
      NodePtr *ptrs = node_ptr;
      for(nodeid_t i = 0; i < page->count; i++) {
        map[i] = node_count;
        if (nodes[i].type & REMOVE_BIT)
          continue;
          
        size[node_count] = get_node_size(i);
        memcpy(&new_ptrs[node_count++], &nodes[i], sizeof(NodePtr));
      }
      
      // move nodes
      pageid_t *links = page->links;
      pageid_t new_links[256];
      bsize_t link_count = 0;
      size_t node_start = new_ptrs[0].ptr*16;

      for(nodeid_t id = 1; i < node_count; i++) {
        switch(new_ptrs[i].type) {
          case kLink:
            new_links[link_count] = links[new_ptr[i].ptr];
            new_ptr[i].ptr = link_count++;
            break;
            
          default:
            node_start -= size[i];
            memmove(new_ptrs[i].ptr*16, node_start, size[i]);
        }
      }
        
      // update page administration
      page->free_start = (sizeof(page->data)-node_start)/16;
      page->link_count = link_count;
      page->node_count = node_count;
      update_node_ptr();
      memcpy(links, new_links, sizeof(pageid_t)*link_count);
      memcpy(node_ptr, new_ptrs, sizeof(NodePtr)*node_count);
        
      // map the child ids for each node
      for(nodeid_t i = 0; i < count; i++) {
        NodeRef node(*this, i);
        nodeid_t children[65];
        size_t child_count;
        child_count = node.get_children(children);
        for(size_t j = 0; j < count; j++) {
          children[j] = map[children[j]];
        node.replace_children(children);
      }
    }

  void free_node(nodeid_t id) {
      node_ptr[id].type |= REMOVE_BIT:
    }
    
  // creates a new node(size must be a multiple of 16)
  NodeRef new_node(size_t size) {
      size_t free_start = sizeof(page->data)-((size_t)page->free_start)*16;
      free_start -= size;
      page->free_start = (sizeof(page->data)-free_start)/16;
      nodeid_t new_id = page->node_count++;
      NodePtr *node = node_ptr+new_id;
      node->ptr = free_start/16;
      return NodeRef(*this, new_id);
    }
    
  // grows or shrinks a node by size. size must be a multiple of 16
  void grow_node(nodeid_t id, int size) {
      NodePtr* ptrs = node_ptr;
      size_t free_start = sizeof(page->data)-((size_t)page->free_start)*16;
      size_t node_start = ((size_t)ptrs[id].ptr) * 16;
      size_t node_size = get_node_size(id);
      
      if (size < 0)
        node_size += size;
        
      memove(free_start, free_start+size, node_start-free_start+node_size);
      
      free_start += size;
      page->free_start = (sizeof(page->data)-free_start)/16;
      
      size /= 16;
      for(node_id i = id; i < page->node_count; i++)
        ptrs[i].ptr += size;
    }
    
  void create_link(node_id node_id, pageid_t page_id) {
      NodePtr* nodes = node_ptr;
      node[node_id].ptr = page->linkcount;
      node[node_id].type = kLink;
      
      char* p = (char*)nodes;
      memove((p, p+sizeof(pageid_t), sizeof(NodePtr)*page->node_count));
      page->links[page->linkcount++] = page_id;
      
      update_node_ptr();
    }

  Page *page;
  pageid_t id;
  pageoffset_t offset;
  NodePtr* node_ptr;
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
    
  size_t size() {
      return page.get_node_size(id);
    }
    
  size_t get_children(nodeid_t children[65]) {
      return trie_handlers[type()]->get_children(*this, children);
    }

  void replace_children(nodeid_t children[65]) {
      trie_handlers[type()]->replace_children(*this, children);
    }
    
  void replace_child(int index, nodeid_t child) {
      trie_handlers[type()]->replace_child(*this, index, child);
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
          me.page.defragment();
          merge_with_parent();
          return;
        }
        
        free_node(me.page, me.id);
        pop(false);
       }
    }
    
  void free_node(PageRef& page, nodeid_t id) {
      if (id == 0)
        map.free_page(page);
      else 
        page.free_node(id);
        // no defragment needed (it is done in remove())
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
      reserve_space(size + sizeof(NodePtr));
      NodeRef& new_ref(current().page.new_node(size));
      memcpy(new_ref.node(), new_node.node(), size);
      *new_ref.extra() = *new_node.extra();
      new_ref.set_type(new_node.type());
      push(new_ref);
      key.append(transition_key.data(), transition_key.size());
   }
  
  // change current node to type
  void change_node(TempNode& new_node) {
      size_t size = current.size(), new_size = new_node.size();
      grow_node(new_size-size);
      memcpy(current().node(), new_node.node(), new_size);
      *current().extra() = *new_node.extra();
      *current().set_type(new_node.type());
    }
   
  // grows or shrink the current node
  void grow_node_by(int delta) {
      if (delta > 0)
        reserve_space(delta);
      
      current().page.grow_node(current().id, delta);  
    }
   
  // merges the current page with the parent page if possible 
  void merge_with_parent() {
      PageRef& page(current().page)
      size_t page_size = page.size();
      
      for(int i = stack.size()-2; i >= 0; i--) {
        PageRef parent(stack[i].node.page);
        if (parent.id != page.id) {
          if (parent.free_size() >= page.size()) {
            nodeid_t child = move_node(parent, NodeRef(page, 0));
            int old_last_index = last_index;
            pop()
            current().replace_child(last_index, child);
            current().find(Slice(last_index), *this);
            last_index = old_last_index;
            map.free_page(page);
          }
          break;
        }
      }
    }
    
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
            string suffix(key, key_index);
            stack.resize(i+1);
            current().find(suffix);
          }
          return;
        }
      }
    }
 
  size_t calc_sizes(NodeRef& node, size_t sizes[256]) {
      nodeid_t children[65];
      size_t size = node.size();
      size_t count = node.get_children(chidlren);
      for(size_t i = 0; i < count; i++)
        size += calc_sizes(NodeRef(node.page, children[i]);
        
      size += sizeof(NodeRef);
      sizes[node.id] = size;
      return size;
    }

  // enusers that the page of current node has a free_space >  size
  // place current node to a new page if nessary
  void reserve_space(size_t size) {
      // during this loop current() can change its page!
      while (current().page.free_size() < size) {
        size_t sizes[256];
        NodeRef &root(current().page, 0);
        calc_size(root, sizes);
        int best = sizeof(Page);
        nodeid_t best_id = 1;
          
        // i = 1: it makes no sense to move the root node
        for(int i = 1; i < root.page.count(); i++) {
          int delta = abs(sizes[i]-sizeof(Page)/2);
          if (delta < nearest) {
            best = delta;
            best_id = i;
          }
        }
        PageRef newpage = storage.new_page();
        NodeRef to_move(NodeRef(page, best_id))
        move_node(newpage, to_move);
        
        refresh_trace_if_contains(to_move);
        root.page.defragment();
        root.page.create_link(best_id, newpage.id);
      }
    }

  // moves the node to new page returning the id on the new_page
  // precondition: there must be enough space in new_page
  nodeid_t move_node(PageRef& new_page, NodeRef& node) {
      if (new_page.id == node.page.id)
        return id;
  
      nodeid_t children[65];
      size_t count = node.get_children(children);
      
      size_t size = node.size()
      NodeRef new_node(new_page.new_node(size));
      memcpy(new_node.node(), node.node(), size);
      node.page.free_node(node.id);
      
      for(size_t i = 0; i < count; i++)
        children[i] = move_node(new_page, NodeRef(node.page, children[i]));
      
      new_node.replace_children(children);
      return new_node.id;
    }
};

class NodeStorage;

// Translates a pageid to page offset
struct PageMap {
  virtual PageRef get_page(pageid_t id) = 0;
  virtual void free_page(const PageRef& page) = 0;
};

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
  
  void free_page(const PageRef& page) {
    if (page.id == _pages.size()-1) {
      if (page.id == 0) return;
      _pages.pop_back();
    }
    else {
      _pages[page.id].release();
      _free_pages++;
    }
  }
   
  PageRef new_page() {
      if (_free_pages) {
        std::vector<_page_ptr>::iterator i = _pages.begin();
        for(pageid_t j = 0; i != _pages.end(); i++, j++) {
          if (! i->.get()) {
            i->reset(new Page);
            _free_pages--;
            return PageRef(i->get(), j, j);
          }
        }
      }
      pageid_t id(_pages.size());
      Page* page = new Page;
      _pages.push_back(page);
      return PageRef(page, id, id);
    }
};


struct PersistentNodeStorage : public NodeStorage {
  PagePositionMap Page;
  boost::interprocess::file_mapping _file_mapping;
  
  PersistentNodeStorage(const char* path);
  std::shared_ptr<PageMap> get_pagemap(version_t version) = 0;
  virtual PageRef new_page();
 
  void grow_file(size_t size);
  void shrink_file(size_t size);
};


class MultiProcessNodeStorage : public PersistentNodeStorage {
 public:
  MultiProcessNodeStorage(const char* path);
};
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_MEMORY_H
