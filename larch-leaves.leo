<?xml version="1.0" encoding="utf-8"?>
<!-- Created by Leo: http://leoeditor.com/leo_toc.html -->
<?xml-stylesheet ekr_test?>
<leo_file xmlns:leo="http://leoeditor.com/namespaces/leo-python-editor/1.1" >
<leo_header file_format="2" tnodes="0" max_tnode_index="0" clone_windows="0"/>
<globals body_outline_ratio="0.5" body_secondary_ratio="0.5">
	<global_window_position top="50" left="50" height="500" width="700"/>
	<global_log_window_position top="0" left="0" height="0" width="0"/>
</globals>
<preferences/>
<find_panel_settings/>
<vnodes>
<v t="michael.20141215222649.4"><vh>@settings</vh>
<v t="michael.20141215222649.5"><vh>@bool shadow_in_home_dir = False</vh></v>
<v t="michael.20141215222649.6"><vh>@string shadow_subdir = .leo_shadow</vh></v>
</v>
<v t="michael.20141215222649.9" a="E"><vh>Source</vh>
<v t="michael.20141215222649.13"><vh>Logical</vh>
<v t="michael.20141215222649.12"><vh>API</vh>
<v t="michael.20141215222649.38"><vh>struct Options</vh></v>
<v t="michael.20141217010530.13"><vh>struct NodePtr</vh></v>
<v t="michael.20141215222649.23"><vh>class Cursor (Declaration)</vh></v>
<v t="michael.20141215222649.104"><vh>class NoWhereCursor</vh></v>
<v t="michael.20141215222649.27"><vh>class Storage (Declaration)</vh></v>
<v t="michael.20141215222649.44"><vh>class Storage (Implementation)</vh>
<v t="michael.20141215222649.189"><vh>~Storage()</vh></v>
<v t="michael.20141215222649.188"><vh>open()</vh></v>
</v>
</v>
<v t="michael.20141215222649.10"><vh>BitTrie</vh>
<v t="michael.20141215222649.83" a="E"><vh>Nodes</vh>
<v t="michael.20141215222649.85"><vh>Leaf</vh></v>
<v t="michael.20141215222649.84"><vh>PageLeaf</vh></v>
<v t="michael.20141215222649.126"><vh>Compressed</vh></v>
<v t="michael.20141215222649.137"><vh>Link</vh></v>
<v t="michael.20141215222649.93"><vh>Trie</vh>
<v t="michael.20141220220750.2"><vh>Navigation</vh></v>
</v>
<v t="michael.20141215222649.95"><vh>BitTrie</vh></v>
</v>
</v>
<v t="michael.20141215222649.63"><vh>Memory Managment</vh>
<v t="michael.20141215222649.11"><vh>@rst memory_manager.html</vh>
<v t="michael.20141215222649.149"><vh>Links</vh>
<v t="michael.20141215222649.135"><vh>class NodeStorage (Declaration)</vh></v>
<v t="michael.20141215222649.155"><vh>class NodeStorage (Implementation)</vh>
<v t="michael.20141215222649.159"><vh>class PersistentNodeManager</vh>
<v t="michael.20141215222649.168"><vh>class PageMap</vh></v>
</v>
<v t="michael.20141215222649.156"><vh>class NodeStorageInHeap</vh></v>
<v t="michael.20141215222649.160"><vh>class MultiProcessNodeMemoryManager</vh></v>
</v>
<v t="michael.20141215222649.136"><vh>class LeafStorage</vh></v>
<v t="michael.20141215222649.72"><vh>Page</vh>
<v t="michael.20141215222649.67"><vh>struct Page</vh></v>
<v t="michael.20141215222649.73"><vh>struct PageRef</vh></v>
</v>
</v>
</v>
</v>
</v>
<v t="michael.20141215222649.14" a="E"><vh>Files</vh>
<v t="michael.20141215222649.16" a="E"><vh>@path include/larch</vh>
<v t="michael.20141215222649.17" a="E"><vh>@shadow leaves.h</vh></v>
<v t="michael.20141215222649.65" a="E"><vh>@shadow node.h</vh></v>
<v t="michael.20141219202729.4"><vh>@shadow leaf.h</vh></v>
</v>
<v t="michael.20141215222649.15" a="E"><vh>@path src</vh>
<v t="michael.20141220220750.5"><vh>@shadow port.h</vh></v>
<v t="michael.20141215222649.81"><vh>@shadow bittrie.h</vh></v>
<v t="michael.20141219202729.8" a="E"
expanded="michael.20141215222649.83,"><vh>@shadow bittrie.cpp</vh></v>
<v t="michael.20141215222649.161"><vh>@shadow node.cpp</vh></v>
<v t="michael.20141215222649.45" a="E"><vh>@shadow leaves.cpp</vh></v>
</v>
</v>
</v>
<v t="michael.20141215222649.8"><vh>Tests</vh>
<v t="michael.20141220220750.8"><vh>compressed size &gt; 256</vh></v>
<v t="michael.20141220220750.17"><vh>remove than next() prev()</vh></v>
<v t="michael.20141220220750.18"><vh>add than next() prev()</vh></v>
</v>
<v t="michael.20141219202729.5" a="E"><vh>edit</vh>
<v t="michael.20141215222649.135"></v>
<v t="michael.20141215222649.27"></v>
<v t="michael.20141230111914.3"><vh>class MemoryStorage (Implementation)</vh></v>
</v>
</vnodes>
<tnodes>
<t tx="michael.20141215222649.10"></t>
<t tx="michael.20141215222649.104">// A Cursor to a non exising namespace
class NoWhereCursor : public Cursor {
 public:
  bool is_valid() const { return false; }
  void set(const Slice&amp; key, int read_forward=100) { }
  void first(int read_forward=100) { }
  void last(int read_forward=-100) { }
  void next(int read_forward=100) { }
  void prev(int read_forward=-100) { }
  
  Slice key() const { throw NoValidPosition(); }
  Slice value() const { throw NoValidPosition(); }
  
  void set_value(const Slice&amp; value) { throw NotImpented(); }
};


</t>
<t tx="michael.20141215222649.11">###############
Memory Managers
###############


|ll| saves its data in two locations:
- node memory (file name nodes.llv) saves all trie nodes and small leaves (key
  and value may not exceed 256 bytes). :cpp:class:`NodeMemoryManager` is
  responsible for the memory.
- leaf memory (file name leaves.llv) saves all leaf data that is bigger than
  256 bytes</t>
<t tx="michael.20141215222649.12">The public interface to larch-leaves

@language cplusplus
@tabwidth -2
</t>
<t tx="michael.20141215222649.126">// A Node containing a string part equal to all descendants
// the equal part fit into a page; the data size is in Nodeptr.extra
struct Compressed {
  nodeid_t child;
  char data[]
};


struct CompressHandler : public NodeHandler, public LeafMixin {
  size_t get_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      Compressed *n = (Compressed*)rnode.node();
      if (n-&gt;child) {
        children[0] = n-&gt;child;
        return 1;
      }
      return 0;
    }
    
  void replace_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      if (n-&gt;child) {
        n-&gt;child = children[0];
      }
    }

  void add_to_trace(Compressed* c, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      trace.key.append(c-&gt;data, c-&gt;size);
    }
   
  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.node();
      bsize_t size = (bsize_t)*rnode.extra() ;
      if (key.size() &lt; size)
        return;
        
      if (memcmp(key.data(), n-&gt;data, size) == 0) {
        NodeRef child(rnode.page, n-&gt;child);
        add_to_trace(n, child, trace);
        child.find(key.advance(size), trace);
      }
    }
    
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      NodeRef child(rnode.page, n-&gt;child);
      add_to_trace(n, child, trace);
      child.first(trace);
    }
    
  void last(NodeRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      NodeRef child(rnode.page, n-&gt;child);
      add_to_trace(n, child, trace);
      child.last(trace);
    }
    
  nodeid_t move_child(NodeRef&amp; child, Trace&amp; trace) {
      // moves the child and its descendant to current page
      PageRef&amp; page(trace.current().page);
      if (page.id != child.page.id) {
        size_t sizes[256];
        memset(sizes, 0, sizeof(sizes));
        size_t size = calc_sizes(child, sizes);
        trace.reserve_space(size, current().id);
        nodeid_t r = trace.move_node(page, child);
        child.page.defragment();
        return r;
      }
      return child.id;
    }
    
  void reinsert(TempCompressed&amp; rest_me, NodeRef&amp; child, 
                trieindex_t index, Trace&amp; trace) {
      BitTrie* bn;
      nodeid_t child_id;
      
      switch(rest_me.size()) {
        case 0:
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn-&gt;add(index, child_id);
          break;
          
        case 1:
          trace.add_node(TempTrie(), Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn-&gt;add(index, trace.child_of_parent());
          
          child_id = move_child(child, trace);
          bn = (BitTrie*)trace.current().node();
          bn-&gt;add(me.data()[0], child_id);
          trace.pop()
          break;
          
        default: {
          trace.add(me, Slice(index));
          child_id = move_child(child, trace);
          Compressed *n = (Compressed*)trace.current().node()
          n-&gt;child = child_id;
          trace.pop();
          break;
      }
    }
    
  void add(const Slice&amp; key, TempnNode&amp; end, NodRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.node();
      bsize_t size = (bsize_t)*rnode.extra();
      
      if (!n-&gt;child) {
        // we insert a
        assert(size == key.size());
        trace.add_node(end, key);
        n-&gt;child = trace.child_of_parent();
        trace.complete = true;
        return;
      }
      
      size_t prefix_size = common_prefix(key.data(), n-&gt;data,
                                         std::min(size, key.size()));
      trieindex_t first = n-&gt;data[0], second = n-&gt;data[1];
      TempCompressed rest_me(n-&gt;data+prefix_size+1, size-prefix_size-1);
            
      // move my page descendants to a temporary page to insert them later
      NodeRef child(rnode.page, n-&gt;child);
      n-&gt;child = 0;
      
      switch(prefix_size) {
        case 0:
          // insert one trie
          trace.change_node(TempTrie());
          reinsert(rest_me, child, first, trace);
          break;

        case 1: {
          // insert two tries
          trace.change_node(TempTrie());
          trace.add_node(TempTrie(), Slice(first));
          BitTrie* bn = (BitTrie*)trace.parent().node();
          bn-&gt;add(first, trace.child_of_parent());
          reinsert(rest_me, child, second, trace);
          break;
          }
            
        default: {
          // another compressed
          Slice next_key(key.data(), prefix_size)
          TempCompressed compressed(next_key);
          first = n-&gt;data[prefix_size+1];
          trace.change_node(compressed);
          trace.add(trie, Slice(key.data(), compressed.size());
          n = (Compressed*)trace.current().node();
          n-&gt;child = trace.child_of_parent();
          reinsert(rest_me, child, first, trace);
      }

      trace.current().add(key.advance(prefix_size), end, context);
    }
};

static CompressedHandler compressed;


</t>
<t tx="michael.20141215222649.13"></t>
<t tx="michael.20141215222649.135">class NodeStorage {
 public:
  virtual PageRef new_page() = 0;
};


struct NodeStorageInHeap : public NodeStorage, public PageMap {
  typedef std::unique_ptr&lt;Page&gt; _page_ptr;
  std::vector&lt;_page_ptr&gt; _pages;
  size_t _free_pages;  // count of _free_pages inside the vector
  
  NodeStorageInHeap()
    : _free_pages(0) {
    new_page(); // create root page
  }

  PageRef get_page(pageid_t id) {
      return PageRef(_pages[id].get(), id, id);
  }
  
  void free_page(const PageRef&amp; page) {
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
        std::vector&lt;_page_ptr&gt;::iterator i = _pages.begin();
        for(pageid_t j = 0; i != _pages.end(); i++, j++) {
          if (! i-&gt;.get()) {
            i-&gt;reset(new Page);
            _free_pages--;
            return PageRef(i-&gt;get(), j, j);
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
  std::shared_ptr&lt;PageMap&gt; get_pagemap(version_t version) = 0;
  virtual PageRef new_page();
 
  void grow_file(size_t size);
  void shrink_file(size_t size);
};


class MultiProcessNodeStorage : public PersistentNodeStorage {
 public:
  MultiProcessNodeStorage(const char* path);
};
</t>
<t tx="michael.20141215222649.136">class LeafStorage {
 public:
  virtual char* get_data(const PageLeaf&amp; leaf) = 0;
  virtual PageLeaf allocate(size_t size) = 0;
  virtual void free(const PageLeaf&amp; leaf) = 0;
  virtual void flush(async=true) { }
};


class LeafStorageInHeap : public LeafStorage {
 public:
  virtual char* get_data(const PageLeaf&amp; leaf) {
    return (char*)leaf.pointer;
  }
  
  virtual PageLeaf allocate(size_t size) {
    return { 0, (offset_t)new char[size] };
  }
  
  virtual void free(const PageLeaf&amp; leaf) {
    delete[] (char*)leaf.pointer;
  }
};


class PersistentLeafStorage : public LeafStorage {
 public:
  PersistentLeafStorage(const char* path);
};
</t>
<t tx="michael.20141215222649.137">// links to another page

struct LinkHandler : public NodeHandler, public LeafMixin {
  NodeRef link_node(NodeRef&amp; rnode, Trace&amp; trace) {
      pageid_t *page_id = (pageid_t*)rnode.link();
      PageRef next_page(trace.map.get_page(*page_id));
      NodeRef child(next_page, 0)
      trace.push(child);
      return child;
    }
    
  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      link_node(rnode, trace).find(key, trace);
    }
    
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      link_node(rnode, trace).first(trace);
    }
    
  void last(NodeRef&amp; rnode, Trace&amp; trace) {
      link_node(rnode, trace).last(trace);
    }
};

static LinkHandler link;</t>
<t tx="michael.20141215222649.14"></t>
<t tx="michael.20141215222649.149">@language cplusplus
@tabwidth -2
</t>
<t tx="michael.20141215222649.15"></t>
<t tx="michael.20141215222649.155">@others
</t>
<t tx="michael.20141215222649.156">// NodeStorageInHeap implementation
// --------------------------------

PageRef NodeStorageInHeap::new_page() {
  if (_free_pages) {
    std::vector&lt;_page_ptr&gt;::iterator i;
    for(i = _pages.begin(); i != _pages.end(); i++) {
      if (!(*i).get()) {
        pageid_t id = i - _pages.begin();
        Page* page = *i = new Page;
        _free_pages--;
        return PageRef(page, (pageid_t)id, (pageofset_t)id);
    }
    _free_pages = 0;
  }
  
  Page* page = new Page;
  _pages.push_back(page);
  return PageRef((*i).get(), (pageid_t)page, (pageofset_t)page);
}

void NodeStorageInHeap::free_page(pageid_t id) {
  if (id == _pages.size()-1) {
    _pages.pop_back(); // remove last page
    // remove all pages at the end
    
    while(!_pages.empty() &amp;&amp; !_pages-back().get()) {
      _pages.pop_back(); 
      _free_pages--;
    }
  }
  else {
    _pages[id].reset();
    _free_pages++;
  }
}


</t>
<t tx="michael.20141215222649.159">@others</t>
<t tx="michael.20141215222649.16"></t>
<t tx="michael.20141215222649.160"></t>
<t tx="michael.20141215222649.168">class PersistantPageMap {
  virtual pageoffset_t get_offset(pageid_t pageid) { return 0 };
};


</t>
<t tx="michael.20141215222649.188">std::shared_ptr&lt;Storage&gt; Storage::open(const char* path, const Options&amp; options) {
  if (path) {
      if (options.multiprocess)
        _node_memory_manager = new MultiProcessNodeMemoryManager(path);
      else
        _node_memory_manager = new PersistentNodeMemoryManager(path);
        
      _leaf_memory_manager = new PersistentLeafMemoryManager(
          path, options.max_leaf_size);
  }
  else {
    _node_memory_manager = new HeapNodeMemoryManager();
    _leaf_memory_manager = new HeapLeafMemoryManager();
  }
}

</t>
<t tx="michael.20141215222649.189">Storage::~Storage() {
  _node_memory_manager-&gt;flush(false);
  _leaf_memory_manager-&gt;flush(false);
}

</t>
<t tx="michael.20141215222649.23">class Cursor {
 public:
  // returns true if cursor is on a valid position
  virtual bool is_valid() const = 0;
 
  // sets the cursor to key
  virtual void set(const Slice&amp; key, int read_forward=100) = 0;
  
  // sets the cursor to first
  virtual void first(int read_forward=100) = 0;
  virtual void last(int read_forward=-100) = 0;
  virtual void next(int read_forward=100) = 0;
  virtual void prev(int read_forward=-100) = 0;
  
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  
  // sets the value raise an exception if a read curor
  virtual void set_value(const Slice&amp; value) = 0;
};


</t>
<t tx="michael.20141215222649.27">extern Slice main_name_space;

class Storage {
 public:
  virtual std::shared_ptr&lt;Cursor&gt; read_cursor(const Slice&amp; namespace_=main_name_space) = 0;
  virtual std::shared_ptr&lt;Cursor&gt; write_cursor(const Slice&amp; namespace_=main_name_space) = 0;
};


class MemoryStorage : public Storage {
public:
  MemoryStorage();
  virtual std::shared_ptr&lt;Cursor&gt; read_cursor(const Slice&amp; namespace_=main_name_space);
  virtual std::shared_ptr&lt;Cursor&gt; write_cursor(const Slice&amp; namespace_=main_name_space);

  NodeRef root() const {
    return NodeRef(_nodes._get_page(0), 0);
  }
  
private:
  NodeStorageInHeap _nodes;
  LeafStorageInHeap _leafs;
};


class PersistentStorage : public Storage {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  void open(const char* path, const Options&amp; options);
};


class CopyOnWriteStorage : public PersistentStorage {
  // returns true if changes have to be commited
  virtual bool is_dirty() const = 0;

};</t>
<t tx="michael.20141215222649.38">struct Options {
  // if true Storage starts with multiprocess support
  bool multiprocess;
  
  // size of a map window for node storage must be a multiple of 4096
  size_t node_window_size;
  
  // size of a map window for leaf storage
  // value cannot be bigger than leaf_window_size
  size_t leaf_window_size;
  
  Options()
    : multiprocess(true), node_window_size(4096*4096*8), 
    node_window_size(4096*4096*8) {}
};


</t>
<t tx="michael.20141215222649.4"></t>
<t tx="michael.20141215222649.44">// Storage implemenation
// ---------------------
@others
</t>
<t tx="michael.20141215222649.5"></t>
<t tx="michael.20141215222649.6"></t>
<t tx="michael.20141215222649.63"></t>
<t tx="michael.20141215222649.67">// a page is identified by a page id
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
  +----------------------+ &lt;-- free_start points to the end all nodes
  | node[n] (&gt;= 16 byte) |
  +----------------------+
  | ...                  |
  +----------------------+
  | node[1] (&gt;= 16 byte) |
  +----------------------+
  | node[0] (&gt;= 16 byte) |
  +----------------------+

  node_ptrs grow downwards 
  nodes grow uppwards

  node 0 is always an ancestor of all other nodes
  and is called the page root.
  
  Annotations:
  because of the nature of inpage_ptr there, free_space underflows
  before nodeid_t space overflows. ==&gt; we never run out of nodeids
  
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

</t>
<t tx="michael.20141215222649.72">@others</t>
<t tx="michael.20141215222649.73">struct Node;

#define REMOVE_BIT 0x80

struct PageRef {
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
      page(page_), id(id_), offset(offset_) {
      update_node_ptr();
     }

  NodePtr* get_node_ptr(nodeid_t, id) {
      return &amp;node_ptr[id];
    }
 
  nodetype_t get_type(nodeid_t id) const {
      return node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&amp;_page-&gt;data[node_ptr[id].ptr*16];
    }
    
  bsize_t* get_extra(nodeid_t id) const {
      return node_ptr[id].extra;
    }
    
  pageid_t* get_link(nodeid_t id) const {
      bsize_t linkid = node_ptr[id].ptr;
      return &amp;page.links[linkid];
    }

  size_t get_node_size(nodeid_t id) const {
      NodePtr* ptr = node_ptr();
      
      if (ptr[id].type == kLink)
        return sizeof(pageid_t);
      
      if (id == 0)
        return (256 - ptr[id].ptr) * 16;
        
      return (ptr[id-1].ptr - ptr[id].ptr)*16;
    }

  size_t free_size() const {
      return sizeof(page-&gt;data) - ((size_t)(page-&gt;free_start))*16 
        - page-&gt;link_count * sizeof(pageptr_t) 
        - page-&gt;node_count * sizeof(NodePtr)
        - sizeof(page-&gt;header);
    }
    
  // returns only the node occupied size (without pageheader)
  // but with nodeptrs and links
  size_t size() const {
      return page-&gt;link_count * sizeof(pageptr_t) // links
          + page-&gt;node_count * sizeof(NodePtr)    // node_ptr
          + ((size_t)page-&gt;free_start)*16);
    }
    
  size_t count() const {
      return page-&gt;node_count;
    }
    
  void update_node_ptr() {
      node_ptr = (NodePtr*)&amp;page-&gt;data[sizeof(page-&gt;header)
                                       +page-&gt;link_count*sizeof(pageptr_t));
    }
    
  void defragment() {
      NodePtr new_ptrs[256]; 
      bsize_t node_count = 0;
      nodeid_t map[256]; // maps old nodeid_s to new node_ids
      size_t size[256]; // is needed later

      // create node map 
      NodePtr *ptrs = node_ptr;
      for(nodeid_t i = 0; i &lt; page-&gt;node_count; i++) {
        map[i] = node_count;
        if (nodes[i].type &amp; REMOVE_BIT)
          continue;
          
        size[node_count] = get_node_size(i);
        memcpy(&amp;new_ptrs[node_count++], &amp;nodes[i], sizeof(NodePtr));
      }
      if (node_count == page-&gt;node_count)
        return; // not defragmented
      
      // move nodes
      pageid_t *links = page-&gt;links;
      pageid_t new_links[256];
      bsize_t link_count = 0;
      size_t node_start = new_ptrs[0].ptr*16;

      for(nodeid_t id = 1; i &lt; node_count; i++) {
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
      page-&gt;free_start = (sizeof(page-&gt;data)-node_start)/16;
      page-&gt;link_count = link_count;
      page-&gt;node_count = node_count;
      update_node_ptr();
      memcpy(links, new_links, sizeof(pageid_t)*link_count);
      memcpy(node_ptr, new_ptrs, sizeof(NodePtr)*node_count);
        
      // map the child ids for each node
      for(nodeid_t i = 0; i &lt; count; i++) {
        NodeRef node(*this, i);
        nodeid_t children[65];
        size_t child_count;
        child_count = node.get_children(children);
        for(size_t j = 0; j &lt; count; j++) {
          children[j] = map[children[j]];
        node.replace_children(children);
      }
    }

  void free_node(nodeid_t id) {
      node_ptr[id].type |= REMOVE_BIT:
    }
    
  // creates a new node(size must be a multiple of 16)
  NodeRef new_node(size_t size) {
      size_t free_start = sizeof(page-&gt;data)-((size_t)page-&gt;free_start)*16;
      free_start -= size;
      page-&gt;free_start = (sizeof(page-&gt;data)-free_start)/16;
      nodeid_t new_id = page-&gt;node_count++;
      NodePtr *node = node_ptr+new_id;
      node-&gt;ptr = free_start/16;
      return NodeRef(*this, new_id);
    }
    
  // grows or shrinks a node by size. size must be a multiple of 16
  void grow_node(nodeid_t id, int size) {
      NodePtr* ptrs = node_ptr;
      size_t free_start = sizeof(page-&gt;data)-((size_t)page-&gt;free_start)*16;
      size_t node_start = ((size_t)ptrs[id].ptr) * 16;
      size_t node_size = get_node_size(id);
      
      if (size &lt; 0)
        node_size += size;
        
      memove(free_start, free_start+size, node_start-free_start+node_size);
      
      free_start += size;
      page-&gt;free_start = (sizeof(page-&gt;data)-free_start)/16;
      
      size /= 16;
      for(node_id i = id; i &lt; page-&gt;node_count; i++)
        ptrs[i].ptr += size;
    }
    
  void create_link(node_id node_id, pageid_t page_id) {
      NodePtr* nodes = node_ptr;
      node[node_id].ptr = page-&gt;linkcount;
      node[node_id].type = kLink;
      
      char* p = (char*)nodes;
      memove((p, p+sizeof(pageid_t), sizeof(NodePtr)*page-&gt;node_count));
      page-&gt;links[page-&gt;linkcount++] = page_id;
      
      update_node_ptr();
    }

  Page *page;
  pageid_t id;
  pageoffset_t offset;
  NodePtr* node_ptr;
};

</t>
<t tx="michael.20141215222649.8"></t>
<t tx="michael.20141215222649.83">/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/



struct LeafMixin {
  size_t get_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      return 0;
    }
    
  void replace_children(NodeRef&amp; rnode, nodeid_t children[65]) {
    }
    
  void replace_child(NodeRef&amp; rnode, int index, nodeid_t child) {
      // this may be only called in trie nodes
      assert(0);
    }

  void next(NodeRef&amp; rnode, Trace&amp; trace) {
      trace.parent_next();
    }
    
  void prev(Node* rnode, PageRef&amp; page, Trace&amp; trace) {
      trace.parent_prev();
    }
};


struct TrieMixin {
  virtual void add_node(const Slice&amp; key, TempNode&amp; node, Trace&amp; trace) = 0;

  void add(const Slice&amp; key, TempNode&amp; end, NodeRef&amp; rnode, Trace&amp; trace) {
      switch(key.size()) {
        case 0:
          trace.add_node(end, Slice());
          *trace.parent().extra() = trace.child_of_parent());
          trace.complete = true;
          break;
          
        case 1: 
          add_node(key, end, trace);
          trace.complete = true;
          break;
          
        case 2:
          add_node(key.slice(1), TempTrie(), trace);
          trace.current().add(key.advance(1), end);
          break;
          
        default: {
          Slice next_key(key.advance(1));
          add_node(key.slice(1), TempCompressed(next_key), trace);
          trace.current().add(next_key, end);
          break;
      }
    }
};    
  

@others

NodeHandler* NodeHandler::handlers[200] = {
  &amp;leaf;
  &amp;pageleaf,
  &amp;link,
  &amp;trie,
  &amp;bittrie,
  &amp;compressed,
};</t>
<t tx="michael.20141215222649.84">// a leaf node with key and values &lt;= 256 bytes
// the key_size is saved in Nodeptr.extra
struct PageLeaf {
  char data[];
};


struct PageLeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      if (!key.size())
        trace.complete = true;
    }
  
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      trace.complete = true;
    }
    
  void last(NodeRef&amp; rnode, Trace&amp; trace) {
      trace.complete = true;
    }
    
  void reinsert_me(TempLeaf&amp; me, Trace&amp; trace) {
      trace.add_node(me, Slice());
      // extra() is the end node
      *trace.parent().extra() = trace.child_of_parent());
      pop(); // back to parent
    }
    
  void add(const Slice&amp; key, TempNode&amp; end, NodeRef&amp; rnode, Trace&amp; trace) {
      assert(key.size() != 0);
      PageLeaf *n = (PageLeave*)rnode.node();
      TempLeaf me(Slice(n-&gt;value, *rnode.extra()));
      TempTrie trie;
      Slice rest_key;
      
      if (key.size() == 2) {
        TempTrie parent_trie;
        trace.change_node(parent_trie);
        reinsert_me(me, trace);
        trace.add_node(trie, key.slice(1));
        BitTrie* pn = (BitTrie*)trace.parent().node();
        pn-&gt;add(key.trie_index(), trace.child_of_parent());
        rest_key = key.advance(1);
      }
      else {
        trace.change.node(trie);
        reinsert_me(me, trace);
        if (key.size() &gt; 2) {
          rest_key = key.advance(1);
          TempCompressed compressed(rest_key);
          trace.add_node(compressed, key.slice(1));
          BitTrie* pn = (BitTrie*)trace.parent().node();
          pn-&gt;add(key.trie_index(), trace.child_of_parent());
        }
        else {
          rest_key = key;
        }
      }

      trace.current().add(rest_key, end, trace);
    }
};

static PageLeafHandler pageleaf;</t>
<t tx="michael.20141215222649.85">// a leaf node with data in Leaf memory
struct Leaf {
  offset_t pointer;
  size_t size;
};


struct LeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
    }
    
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void last(NodeRef&amp; rnode, Trace&amp; trace) {
      //add_to_trace((Leaf*)node, page, context);
    }
};

static LeafHandler leaf;
</t>
<t tx="michael.20141215222649.9"></t>
<t tx="michael.20141215222649.93">// Node with the complete alphabet 
// children count is &gt; 56
struct Trie {
  nodeid_t children[64];
};

struct TrieHandler : public NodeHandler, public TrieMixin {
  virtual void add_to_trace(int key, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      if (key &gt;=0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
 
  size_t get_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = (Trie*)rnode.node();
      for(int i = 0; i &lt; 64; i++) {
        if (n-&gt;children[i]) 
          children[count++] = n-&gt;children[i];
      }
      if (*rnode.extra())
        children[count++] = *rnode.extra();
        
      return count;
    }
   
  void replace_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      size_t count = 0;
      Trie *n = (Trie*)rnode.node();
      for(int i = 0; i &lt; 64; i++) {
        if (n-&gt;children[i]) 
          n-&gt;children[i] = children[count++];
      }
      if (*rnode.extra())
        *rnode.extra() = children[count];
    }

  void replace_child(NodeRef&amp; rnode, int index, nodeid_t child) {
      if (index &lt; 0) {
        *rnode.extra() = child;
      }
      else {
        Trie *n = (Trie*)rnode.node();
        n-&gt;children[index] = child;
      }
    }
    
  void add_node(const Slice&amp; key, TempNode&amp; node, Trace&amp; trace) {
      trace.add_node(node, key);
      Trie *n = (Trie*)trace.parent().node();
      n-&gt;children[key.trie_index()] = trace.child_of_parent();
    }
 
  bool remove_last_index(NodeRef&amp; rnode, Trace&amp; trace) {
      if (trace.last_index &lt; 0) {
        *rnode.exra() = 0;
        return true;
      }

      Trie *n = (Trie*)rnode.node();
      n-&gt;children[trace.last_index] = 0;
      
      nodeid_t children[64];
      size_t count = 0;
      for(int i = 0; i &lt; 64; i++) {
        if (n-&gt;children[i])
          count++;
      }
      if (count &lt; 56) {
        TempTrie trie(count);
        *trie.extra() = *rnode.extra();
        BitTrie* bt = (BitTrie*)trie.node();
        for(int i = 0; i &lt; 64; i++) {
          if (n-&gt;children[i])
            bt-&gt;add(i, n-&gt;children[i]);
        }
        trace.change_node(trie);
      }

      return true;
    }
    
    
  @others
};

static TrieHandler trie;


</t>
<t tx="michael.20141215222649.95">// Node with a range
struct BitTrie {
  boost::uint64_t bits;
  nodeid_t children[];

  size_t count() const {
      return popcount(bits);    
    }
  
  int get_child_index(trieindex_t index) const {
      if (bits &amp; (1&lt;&lt;index)) {
        boost::uint64_t mask = ~0;
        mask &lt;&lt;= index;
        return popcount(bits &amp; ~mask)
      }
      return -1;
    }
    
  int first_bit() const {
      return ffs(bits) - 1;
   }
  
  int lastbit() const {
      return 64 - clz(bits)
    }
  
  int next_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask &lt;&lt;= index;
      return ffs(bits &amp; mask) - 1;
    }
    
  int prev_bit(int index) const {
      boost::uint64_t mask = ~0;
      mask &lt;&lt;= index;
      return 64 - clz(bits &amp; ~mask);
    }
    
  void add(int index, nodeid_t node) {
      bits |= 1 &lt;&lt; index;
      int child_index = get_child_index(index); 
      memove(children+child_index, children+child_index+1,
             count()-child_index-1);
      children[child_index] = node;
    }
    
  void remove(int index) {
      int child_index = get_child_index(index);
      assert(child_index &gt;= 0);
      bits &amp;= ~(1 &lt;&lt; index);
      memmove(children+child_index+1, children+child_index,
              count()-child_index-1);
    }
};


struct BitTrieHandler : public NodeHandler, public TrieMixin  {
  void add_to_trace(int key, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      if (key &gt;= 0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
    
  size_t get_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n-&gt;count();
      for(int i = 0; i &lt; count; i++) 
          children[i] = n-&gt;children[i];

      if (*rnode.extra())
        children[count++] = *rnode.extra();

      return count;
    }
    
  void replace_children(NodeRef&amp; rnode, nodeid_t children[65]) {
      BitTrie *n = (BitTrie*)rnode.node();
      size_t count = n-&gt;count();
      for(int i = 0; i &lt; count; i++) 
          n-&gt;children[i] = children[i];

      if (*rnode.extra())
        *rnode.extra() = children[count];
    }
    
  void replace_child(NodeRef&amp; rnode, int index, nodeid_t child) {
      if (index &lt; 0) {
        *rnode.extra() = child;
      }
      else {
        BitTrie *n = (Trie*)rnode.node();
        n-&gt;children[n-&gt;get_childindex(index)] = child;
      }
    }
 
  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      if (key.empty()) {
        nodeid_t *end_child = rnode.extra();
        if (*end_child) { 
          NodeRef child(rnode.page, *end_child);
          add_to_trace(-1, child, trace);
          child.first(trace);
        }
        return;
      }
        
      trieindex_t index = key.trie_index();
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n-&gt;get_child_index(index);
      if (child_index &gt;= 0) {
        nodeid_t child_id = n-&gt;children[child_index];
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, context);
        child.find(key.advance(1), trace);
      }
    }
    
  void next(NodeRef&amp; rnode, Trace&amp; trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      int index = trace.last_index;
      
      if (index &lt; 0) {
        int first_bit = n-&gt;first_bit();
        NodeRef child(rnode.page, n-&gt;children[0]).first();
        add_to_trace(first_bit, child, trace);
        return;
      }
     
      int child_index = n-&gt;get_child_index(index) + 1;
      if (child_index &lt; n-&gt;count()) {
        index = n-&gt;next_bit(index)
        NodeRef child(rnode.page, n-&gt;children[child_index]);
        add_to_trace(index, child, trace);
        child.first();
        return;
      }
      
      trace.parent_next();
    }
    
  void prev(NodeRef&amp; rnode, Trace&amp; trace) {
      int index = trace.last_index;
      if (index &lt; 0) {
        trace.parent_prev();
        return;
      }
      
      BitTrie *n = (BitTrie*)rnode.node();
      int child_index = n-&gt;get_child_index(index) - 1;
      if (child_index &gt;= 0) {
        index = n-&gt;prev_bit(index);
        NodeRef child(rnode.page, n-&gt;children[child_index]);
        add_to_trace(index, child, trace);
        child.last(trace);
        return
      }
    
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(-1, child, trace);
        child.last(trace);
        return;
      }

      trace.parent_prev();
    }
    
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child)
        add_to_trace(-1, child, trace);
        child.first(trace);
      }
      else {
        trieindex_t index = n-&gt;first_bit();
        NodeRef child(rnode.page, n-&gt;children[0]);
        add_to_trace(index, rnode, context);
        child.first(trace);
      }
    }
    
  void last(Node* node, Trace&amp; trace) {
      trieindex_t index = n-&gt;last_bit();
      int index = n-&gt;count() - 1;
      NodeRef child(rnode.page, n-&gt;children[index])
      add_to_trace(index, child, trace);
      child.first();
    }
    
  void add_node(const Slice&amp; key, TempNode&amp; node, Trace&amp; trace) {
      BitTrie *n = (BitTrie*)trace.current().node();
      size_t count = n-&gt;count();
      switch(count) {
        case 56: {
            TempTrie trie(count);
            *trie.extra() = *trace.current().extra();
            Trie *np = (Trie*)trie.node();
            int bit = n-&gt;first_bit(), i = 0;
            while(bit &gt;= 0) {
              np-&gt;children[bit] = n-&gt;children[i++];
              bit = n-&gt;next_bit(bit);
            }
            trace.change_node(trie);
            trace.add_node(node, key);
            np = (Trie*)trace.parent().node();
            np-&gt;children[key.trie_index()] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 24:
        case 40:
          trace.grow_node_by(16);
          
        default: 
          trace.add_node(node, key);
          n = (BitTrie*)trace.parent().node();
          n-&gt;add(key.trie_index(), trace.child_of_parent());
      }
    }
    
  bool remove_last_index(NodeRef&amp; rnode, Trace&amp; trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      if (trace.last_index &lt; 0) {
        *rnode.exra() = 0;
        if (n-&gt;count() == 1)
          change_node(NodeRef(rnode.page, n-&gt;children[0]), 
                      n-&gt;first_bit(), trace);
        return true;
      }
      
      n-&gt;remove(trace.last_index);
      switch(n-&gt;count()) {
        case 0: 
          if (*rnode.exra())
            change_node(NodeRef(rnode.page, *rnode.exra()), -1, trace);
          else
            return false;
        
          break;
          
        case 1: 
          if (!*rnode.exra())
            change_node(NodeRef(rnode.page, n-&gt;children[0]), 
                        n-&gt;first_bit(), trace);
          break;
      
        case 24:
        case 40:
          trace.grow_node_by(-16);
      }
       
      return true;
    }
    
  void change_node(NodeRef&amp; child, int index, Trace&amp; trace) {
      switch(child.type()) {
        case kCompressed: {
          assert(index &gt;= 0); // is certainly no end_node
          Compressed* n = (Compressed*)child.node();
          bsize_t size = (bsize_t)*child.extra();
          NodeRef compress_child(child.page, n-&gt;child);
          
          // insert the index in compressed and replace me with compressed
          char data[256];
          data[0] = (char)index;
          memcpy(data+1, n-&gt;data, size);
          TempCompressed compressed(Slice(data, size+1));
          trace.free_node(child.page, child.id);
          trace.change_node(compressed);
          
          // ensure compress_child is on same page
          PageRef&amp; page(trace.current().page);
          if (page.id != compress_child.page.id) {
            size_t sizes[256];
            memset(sizes, 0, sizeof(sizes));
            size_t size = calc_sizes(child, sizes);
            trace.reserve_space(size, current().id);
            compress_child.id = trace.move_node(page, compress_child);
            compress_child.page.defragment();
          }
          
          n = (Compressed*)current().node()
          n-&gt;child = compress_child.id;
          break;
          }
                
        case kPageLeaf: 
        case kLeaf: 
          if (index &lt; 0) {
            // make the trie to leaf
            TempNode leaf;
            trace.move_node(leaf.page_ref, child);
            trace.free_node(child.page, child.id);
            trace.change_node(leaf);
          }
      }
    }
};


static BitTrieHandler bittrie;


</t>
<t tx="michael.20141217010530.13">struct NodePtr {
  PagePtr page;
  nodeid_t node;
};</t>
<t tx="michael.20141219202729.5">@language cplusplus
@tabwidth -2
</t>
<t tx="michael.20141220220750.17"></t>
<t tx="michael.20141220220750.18"></t>
<t tx="michael.20141220220750.2">void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
    if (key.empty()) {
      nodeid_t *end_child = rnode.extra();
      if (*end_child) {
        NodeRef child(rnode.page, *end_child);
        add_to_trace(-1, child, trace);
        child.first(trace);
      }
      return;
    }

    Trie *n = (Trie*)rnode.node();
    trieindex_t index = key.trie_index();
    nodeid_t child_id= n-&gt;children[index];
    if (child_id) {
      NodeRef child(rnode.page, child_id)
      add_to_trace(index, child, trace);
      child.find(key.advance(1), context);
    }
  }

void next(NodeRef&amp; rnode, Trace&amp; trace) {
    int index = trace.last_index &lt; 0 ? 0 : (trace.last_index + 1);
    Trie *n = (Trie*)rnode.node();
    for(; index &lt; 64; index++) {
      nodeid_t child_id = n-&gt;children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, trace);
        child.first(trace);
        return;
    }

    trace.parent_next();
  }
  
void prev(NodeRef&amp; rnode, Trace&amp; trace) {
    if (trace.last_index &lt; 0) {
      trace.parent_prev();
      return;
    }

    int index = trace.last_index - 1;
    Trie *n = (Trie*)rnode.node();
    for(; index &gt;= 0; i--)
      nodeid_t child_id = n-&gt;children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, trace);
        child.last(trace);
        return;
      }
    }
    
    nodeid_t *end_child = rnode.extra();
    if (*end_child) {
      NodeRef child(rnode.page, *end_child);
      add_to_trace(index, child, trace);
      child.last(trace);
    }
    else {
      trace.parent_prev();
    }
  }

void first(NodeRef&amp; rnode, Trace&amp; trace) {
    nodeid_t *end_child = rnode.extra();

    if (*end_child ) {
      NodeRef child(rnode.page, *end_child)
      add_to_trace(-1, child, trace);
      child.first(trace);
      return;
    }

    Trie *n = (Trie*)node;
    for(int index = 0; index &lt; 64; i++) {
      nodeid_t child_id = n-&gt;children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id)
        add_to_trace(index, child, trace);
        child.first(trace);
        return;
      }
    }
    assert(0);
  }
  
void last(NodeRef&amp; rnode, Trace&amp; trace) {
    Trie *n = (Trie*)rnode.node();
    for(int index = 63; index &gt;= 0; i--) {
      nodeid_t child_id = n-&gt;children[index];
      if (child_id) {
        NodeRef child(rnode.page, child_id);
        add_to_trace(index, child, trace);
        child.last(trace);
        return;
      }
    }
    assert(0);
  }


</t>
<t tx="michael.20141220220750.8">Insert multiple keys with a common part &gt; 256 bytes</t>
<t tx="michael.20141230111914.3">struct ReadMemoryCursor : public Cursor {
  Trace trace;
  NodeRef root;
  std::string decoded_key;
  
  MemoryCursor(NodeStorageInHeap&amp; nodes, NodeRef root_) 
    : trace(node, node), root(root_) { }
    
  bool is_valid() const { return trace.complete }
  void set(const Slice&amp; key, int read_forward=100) { 
      std::string encoded = to_base64(key);
      Slice ekey(encoded);
      if (trace.find(ekey))
        return;
      trace.reset();
      root.find(ekey, trace);
    }
    
  void first(int read_forward=100) { 
      root.first(trace);
    }
  void last(int read_forward=-100) { 
      root.last(trace);
    }
  void next(int read_forward=100) { 
      if (trace.size())
        trace.current().next(trace);
    }
  void prev(int read_forward=-100) {
      if (trace.size())
        trace.current().prev(trace);
    }
  
  Slice key() const { 
    decoded_key = decode(trace.key());
    return Slice(decoded_key);
  }
  Slice value() const { 
    return trace.value();
  }
  
  void set_value(const Slice&amp; value) { throw NotImpented(); }
};

struct WriteMemoryCursor : public ReadMemoryCursor {
  void set_value(const Slice&amp; value) {
    trace.set_value(value);
  }
}



MemoryStorage::MemoryStorage() {
  Trace trace(_nodes, _nodes);
  trace.push(root());
  trace.add(TempTrie());
}


std::shared_ptr&lt;Cursor&gt; read_cursor(const Slice&amp; namespace_) {
  std::string encoded = to_base64(namespace_);
  Trace trace(_nodes, _nodes);
  root().find(Slice(encoded), trace);
  if (!trace.complete) {
    // namespace does not exist
    return new NoWhereCursor();
  }
  
  return new ReadMemoryCursor(_nodes, trace.current());
}


std::shared_ptr&lt;Cursor&gt; write_cursor(const Slice&amp; namespace_) {
  std::string encoded = to_base64(namespace_);
  Slice ekey(encoded);
  Trace trace(_nodes, _nodes);
  trace.push(root());
  root().find(ekey, trace);
  
  if (!trace.complete) {
    TempTrie ns;
    trace.current().add_node(ekey.advance(trace.key.size()), ns)
  }
  
  return new WriteMemoryCursor(_nodes, trace.current());
}


</t>
</tnodes>
</leo_file>
