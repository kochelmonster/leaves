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
<v t="michael.20141215222649.104"><vh>class Cursor (Implementation)</vh>
<v t="michael.20141215222649.198"><vh>is_valid()</vh></v>
<v t="michael.20141215222649.203"><vh>set()</vh></v>
</v>
<v t="michael.20141215222649.27"><vh>class Storage (Declaration)</vh></v>
<v t="michael.20141215222649.44"><vh>class Storage (Implementation)</vh>
<v t="michael.20141215222649.189"><vh>~Storage()</vh></v>
<v t="michael.20141215222649.188"><vh>open()</vh></v>
<v t="michael.20141215222649.190"><vh>cursor()</vh></v>
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
<v t="michael.20141215222649.72" a="E"><vh>Page</vh>
<v t="michael.20141215222649.67"><vh>struct Page</vh></v>
<v t="michael.20141215222649.73"><vh>struct PageRef</vh></v>
</v>
</v>
</v>
</v>
</v>
<v t="michael.20141215222649.14" a="E"><vh>Files</vh>
<v t="michael.20141215222649.16" a="E"><vh>@path include/larch</vh>
<v t="michael.20141215222649.17"><vh>@shadow leaves.h</vh></v>
<v t="michael.20141215222649.65" a="E"
expanded="michael.20141215222649.72,"><vh>@shadow node.h</vh></v>
<v t="michael.20141219202729.4"><vh>@shadow leaf.h</vh></v>
</v>
<v t="michael.20141215222649.15" a="E"><vh>@path src</vh>
<v t="michael.20141220220750.5"><vh>@shadow port.h</vh></v>
<v t="michael.20141215222649.81"><vh>@shadow bittrie.h</vh></v>
<v t="michael.20141219202729.8"><vh>@shadow bittrie.cpp</vh></v>
<v t="michael.20141215222649.161"><vh>@shadow node.cpp</vh></v>
<v t="michael.20141215222649.45"><vh>@shadow leaves.cpp</vh></v>
</v>
</v>
</v>
<v t="michael.20141215222649.8"><vh>Tests</vh>
<v t="michael.20141220220750.8"><vh>compressed size &gt; 256</vh></v>
<v t="michael.20141220220750.17"><vh>remove than next() prev()</vh></v>
<v t="michael.20141220220750.18"><vh>add than next() prev()</vh></v>
</v>
<v t="michael.20141219202729.5" a="E"><vh>edit</vh>
<v t="michael.20141215222649.72" a="E"></v>
<v t="michael.20141219202729.2"><vh>struct NodeRef</vh></v>
<v t="michael.20141215222649.99"><vh>NodeHandler</vh></v>
<v t="michael.20141220220750.15"><vh>struct Trace</vh></v>
<v t="michael.20141215222649.83" a="E"></v>
</v>
</vnodes>
<tnodes>
<t tx="michael.20141215222649.10"></t>
<t tx="michael.20141215222649.104">// find prefix common to s1 and s2
inline size_t prefix(const char* s1, const *char s2, size_t size) {
  size_t i;
  for(i = 0; i &lt; size; i++, s1++, s2++) {
    if (*s1 != *s2)
      break;
  }
  return i;
}

// Cursor implementation
// ---------------------
@others

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
// the equal part fit into a page
struct Compressed {
  nodeid_t child;
  bsize_t size;
  char data[]
};


struct CompressHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(Compressed* c, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      trace.key.append(c-&gt;data, c-&gt;size);
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.gt_node();
      if (key.size() &lt; n-&gt;size)
        return;
        
      if (memcmp(key.data(), n-&gt;data, n-&gt;size) == 0) {
        NodeRef child(rnode.page, n-&gt;child);
        add_to_trace(n, child, trace);
        child.find(key.advance(n-&gt;size), trace);
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
    
  void reinsert(Trace&amp; trace, TempCompressed&amp; me, 
                nodeid_t child, NodRef&amp; rnode, trieindex_t index) {
      BitTrie* bn;
      switch(me.size()) {
        case 0:
          child = trace.move_node(rnode.page, child);
          bn = (BitTrie*)trace.parent().node();
          bn-&gt;add(index, child);
          break;
          
        case 1:
          trace.add_node(TempTrie(), Slice(index));
          bn = (BitTrie*)trace.parent().node();
          bn-&gt;add(index, trace.child_of_parent());
          child = trace.move_node(rnode.page, child);
          bn = (BitTrie*)trace.current().node();
          bn-&gt;add(me.data()[0], child);
          trace.pop()
          break;
          
        default: {
          trace.add(me, Slice(index));
          child = trace.move_node(rnode.page, child);
          Compressed *n = (Compressed*)trace.current().node()
          n-&gt;child = child;
          trace.pop();
          break;
      }
    }
    
  void add(const Slice&amp; key, const Slice&amp; value, NodRef&amp; rnode, Trace&amp; trace) {
      Compressed *n = (Compressed*)rnode.node();
      size_t prefix_size = common_prefix(key.data(), n-&gt;data,
                                         std::min(n-&gt;size, key.size()));
      trieindex_t first = n-&gt;data[0], second = n-&gt;data[1];
      TempCompressed me(n-&gt;data+prefix_size+1, n-&gt;size-prefix_size-1);
      nodeid_t child = n-&gt;child
      
      switch(prefix_size) {
        case 0: {
            // insert one trie
            trace.change_node(TempTrie());
            reinsert(trace, me, rnode, child, first);
            break;
          }
        
        case 1: {
            // insert two tries
            trace.change_node(TempTrie());
            trace.add_node(TempTrie(), Slice(first));
            BitTrie* bn = (BitTrie*)trace.parent().node();
            bn-&gt;add(first, trace.child_of_parent());
            reinsert(trace, me, rnode, child, second);
            break;
          }
            
        default: {
            // another compressed
            TempCompressed compressed(key.data(), prefix_size);
            first = n-&gt;data[prefix_size+1];
            trace.change_node(compressed);
            trace.add(trie, Slice(compressed.data(), compressed.size());
            n = (Compressed*)trace.current().node();
            n-&gt;child = trace.child_of_parent();
            reinsert(trace, me, rnode, child, first);
            break;
          }
      }

      trace.current().add(key.advance(prefix_size), value, context);
    }
};

static CompressedHandler compressed;


</t>
<t tx="michael.20141215222649.13"></t>
<t tx="michael.20141215222649.135">class NodeStorage {
 public:
  virtual std::shared_ptr&lt;PageMap&gt; get_pagemap(version_t version) = 0;
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
    
  virtual std::shared_ptr&lt;PageMap&gt; get_pagemap(version_t version) {
      return _pagemap;
    }
    
  virtual PageRef get_page(pageoffset_t offset, pageid_t id) {
       return PageRef(_pages[offset].get(), offset, id);
    }
    
  virtual PageRef new_page();
  virtual void free_page(pageid_t id);
    
 private:
  HeapPageMap _pagemap;
  typedef std::unique_ptr&lt;Page&gt; _page_ptr;
  std::vector&lt;_page_ptr&gt; _pages;
  size_t _free_pages;  // count of _free_pages inside the vector
};


class PersistentNodeStorage : public PersistentNodeStorage {
 public:
  PersistentNodeStorage(const char* path);
  virtual std::shared_ptr&lt;PageMap&gt; get_pagemap(version_t version) = 0;
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
<t tx="michael.20141215222649.190">std::shared_ptr&lt;Cursor&gt; Storage::cursor(const Slice&amp; namespace_, 
                                        bool signed_compare=false) {
  if signed_compare
    return new SignedNamespaceCursor(*this, namespace_);
  
  return new UnsignedNamespaceCursor(*this, namespace_);
}

std::shared_ptr&lt;Cursor&gt; Storage::cursor(bool signed_compare=false) {
  if (signed_compare)
    return new SignedCursor(*this);
    
  return new UnsignedCursor(*this);
}

</t>
<t tx="michael.20141215222649.198">bool Cursor::is_valid() const {
  if (_search_trace.empty())
    return false;
    
  return is_leaf(_search_trace.back());
}

</t>
<t tx="michael.20141215222649.203">void set(const Slice&amp; key, int read_forward=100) {
  if (! is_valid())
    return;

  // to do less search we try to start from a common
  // prefix    
  size_t prefix_len = prefix(key.data(), _key.data(), 
                             std::min(key.size(), _key.size()));
  _search_trace.resize(prefix_len+1);
  _key.assign(key.data(), key.size());  
  
  NodeRef top = _search_trace.back();
  Slice rest_key(_key.data()+prefix_len, key.size()-prefix_len);
  
  for(size_t i = prefix_len; i &lt; _key.size(); i++) {
    byte_t index = trie_index(*rest_key.data());
    top = find_next(_storage, top, index);
    
    if (!top.is_valid()) {
      top = _search_trace.back();
      break;
    }
    
    eat(top, rest_key);
    _search_trace.push_back(top);
  }
      
  if (is_leaf(top) &amp;&amp; ! key_equal(top, rest_key)) {
    _search_trace.pop_back();
  }
}

</t>
<t tx="michael.20141215222649.23">class Cursor {
 public:
 
  // returns true if changes have to be commited
  bool is_dirty() const;
  
  // returns true if cursor is on a valid position
  bool is_valid() const;
 
  // sets the cursor to key
  virtual void set(const Slice&amp; key, int read_forward=100);
  
  // sets the cursor to first
  void first(int read_forward=100);
  void last(int read_forward=-100) throw Error;
  void next(int read_forward=100) throw Error;
  void prev(int read_forward=-100) throw Error;
  bool is_valid() const;

  void add(const Slice&amp; key) throw Error;
  Slice key() const;
  Slice value() const;
  void set_value(const Slice&amp; value);
  void start_commit() throw Error;
  bool is_comitting() const;
  void end_commit() throw Error;
  void commit() throw Error;

 protected:
  virtual byte_t trie_index(char key) = 0;
  
  Cursor(Storage&amp; storage) : _storage(storage), _transaction(0) {}

 private:
  versiont_t _transaction;
  std::shared_ptr&lt;Storage&gt; _storage;
  std::vector&lt;NodePtr&gt; _search_trace;
  std::string _key;
  
  friend Storage;
};


</t>
<t tx="michael.20141215222649.27">class Storage {
 public:
  // opens a storage in directory "path"
  // path must exists and end with a separator ("/" or "\")
  // is path is NULL a in memory database is build
  static std::shared_ptr&lt;Storage&gt; open(
      const char* path, const Options&amp; options);

  // returns a cursor inside a namespace
  // if signed_compare is true the trie table is [-127, 127] instead of [0, 255]
  std::shared_ptr&lt;Cursor&gt; read_cursor(const Slice&amp; namespace_);
  std::shared_ptr&lt;Cursor&gt; write_cursor(const Slice&amp; namespace_);
  
  Storage() {}
  ~Storage();
  
private:
  std::unique_ptr&lt;NodeMemoryManager&gt; _node_memory_manager;
  std::unique_ptr&lt;LeafMemoryManager&gt; _leaf_memory_manager;
};


</t>
<t tx="michael.20141215222649.38">struct Options {
  // if true Storage starts with multiprocess support
  bool multiprocess;
  
  // size of a map window
  size_t window_size;
  
  // the maximum size of key+value
  // must be smaller than window_size
  size_t max_leaf_size;
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
  +----------------------+ &lt;-- link_start poinst to start of links
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

</t>
<t tx="michael.20141215222649.72">@others</t>
<t tx="michael.20141215222649.73">struct Node;

struct PageRef {
  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_) :
    page(page_), id(id_), offset(offset_) { }

  NodePtr* get_node_ptr(nodeid_t, id) {
      return &amp;page-&gt;node_ptr[id];
    }
 
  nodetype_t type(nodeid_t id) const {
      return page-&gt;node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&amp;_page-&gt;data[page-&gt;node_ptr[id].ptr*16];
    }
    
  nodeid_t* extra(nodeid_t id) const {
      return page-&gt;node_ptr[id].extra;
    }
    
  pageid_t* link(nodeid_t id) const {
      bsize_t linkid = page-&gt;node_ptr[id].ptr;
      pageid_t* links = (pageid_t*)&amp;page.data[page.link_start];
      return links+linkid;
    }

  size_t free_size() const {
      return 4096 - page.free_start*16 - PAGE_HEADER_SIZE 
        - (page-&gt;last_node+1)*sizeof(NodePtr)
    }

  Page *_page;
  pageid_t _id;
  pageoffset_t _offset;
};

</t>
<t tx="michael.20141215222649.8"></t>
<t tx="michael.20141215222649.83">/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/



struct LeafMixin {
  void next(NodeRef&amp; rnode, Trace&amp; trace) {
      trace.parent_next();
    }
    
  void prev(Node* rnode, PageRef&amp; page, Trace&amp; trace) {
      trace.parent_prev();
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
// the leag is saved in the page
struct PageLeaf {
  boost::uint8_t key_size;
  boost::uint8_t value_size;
  char data[];
  
  size_t size() const {
      return node_size(sizeof(key_size)+sizeof(value_size)+key_size+value_size);
    }
};


struct PageLeafHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(PageLeaf *n, Trace&amp; trace) {
      trace.key.append(n-&gt;data, n-&gt;key_size);
      trace.complete = true;
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) {
      PageLeaf *n = (PageLeaf*)rnode.node();
      if (key.size() == n-&gt;key_size)
          &amp;&amp; memcmp(key.data(), n-&gt;data, n-&gt;key_size) == 0) {
        add_to_trace(n, trace);
      }
    }
  
  void first(NodeRef&amp; rnode, Trace&amp; trace) {
      add_to_trace((PageLeaf*)rnode.node(), context);
    }
    
  void last(NodeRef&amp; rnode, Trace&amp; trace) {
      add_to_trace((PageLeaf*)rnode.node(), context);
    }
    
  void add(const Slice&amp; key, const Slice&amp; value, NodeRef&amp; rnode, Trace&amp; trace) {
      PageLeaf *n = (PageLeave*)rnode.node();
      size_t prefix_size = common_prefix(key.data(), n-&gt;data, 
                                         std::min(key.size(), n-&gt;key_size));
      Slice mykey(n-&gt;data+prefix_size, n-&gt;key_size-prefix_size);
      Slice myvalue(n-&gt;data+key_size, n-&gt;value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      
      switch(prefix_size) {
        case 0:
          trace.change_node(trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            trace.change_node(parent_trie);
            trace.add_node(trie, key.slice(1));
            BitTrie* pn = (BitTrie*)trace.parent().node();
            pn-&gt;add(key.trie_index(), trace.child_of_parent());
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            trace.change_node(compressed);
            trace.add_node(trie, prefix_size)
            n = (PageLeaf*)trace.parent().node();
            n.child = trace.child_of_parent();
          }
      }

      trace.add_node(me, Slice());
      trace.key.append(mykey.data(), mykey.size());
      trace.complete = true;
      
      BitTrie* tn = (BitTrie*)trace.parent().node();
      tn-&gt;add(mykey.trie_index(), trace.child_of_parent());

      // pop back trace to trie
      trace.pop();
      trace.current().add(key.advance(prefix_size), value, context);
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

struct TrieHandler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      if (key &gt;=0)
        trace.key.append((char)key);
      else
        trace.complete = true;
    }
  
  void add(const Slice&amp; key, const Slice&amp; value, NodeRef&amp; rnode, Trace&amp; trace) {
      Trie *n = (Trie*)rnode.node();
      TempLeaf child(key.advance(1), value);
      trace.add_node(child, key.slice(1));  // trace is now at child
      
      if (key.empty()) {
        *trace.parent().extra() = trace.child_of_parent())
      }
      else {
        n = (Trie*)trace.parent().node();
        n-&gt;children[key.trie_index()] = trace.child_of_parent();
      }
    }
    
  bool remove_last_index(NodeRef&amp; rnode, Trace&amp; trace) {
      if (trace.last_index &lt; 0) {
        *rnode.exra() = 0;
        return true;
      }

      Trie *n = (Trie*)rnode.node();
      n-&gt;children[trace.last_index] = 0;
      
      size_t count = 0;
      for(int i = 0; i &lt; 64; i++) {
        if (n-&gt;children[i])
          count++;
      }
      if (count &lt; 56) {
        TempTrie trie(n);
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
  
  int next_bit(trieindex_t index) const {
      boost::uint64_t mask = ~0;
      mask &lt;&lt;= index;
      return ffs(bits &amp; mask) - 1;
    }
    
  int prev_bit(trieindex_t index) const {
      boost::uint64_t mask = ~0;
      mask &lt;&lt;= index;
      return 64 - clz(bits &amp; ~mask);
    }
    
  void add(trieindex_t index, nodeid_t node) {
      bits |= 1 &lt;&lt; index;
      int child_index = get_child_index(index); 
      memove(children+child_index, children+child_index+1,
             count()-child_index-1);
      children[child_index] = node;
    }
    
  void remove(trieindex_t index) {
      int child_index = get_child_index(index);
      assert(child_index &gt;= 0);
      bits &amp;= ~(1 &lt;&lt; index);
      memmove(children+child_index+1, children+child_index,
              count()-child_index-1);
    }
};


struct BitTrieHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef&amp; child, Trace&amp; trace) {
      trace.push(child);
      if (key &gt;= 0)
        trace.key.append((char)key);
      else
        trace.complete = true;
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
    
  void add(const Slice&amp; key, const Slice&amp; value, NodeRef&amp; rnode, Trace&amp; trace) {
      BitTrie *n = (BitTrie*)rnode.node();
      TempLeaf child(key.advance(1), value);
      
      if (key.empty()) {
        trace.add_node(child, Slice());
        *trace.parent().node().extra() = trace.child_of_parent();
        trace.complete = true;
        return;
      }
      
      size_t count = n-&gt;count();
      switch(count) {
        case 56: {
            TempTrie trie(n);
            trace.change_node(trie);
            trace.add_node(child, key.slice(1));
            Trie *np = (Trie*)trace.parent().node();
            np-&gt;children[key.trie_index()] = trace.child_of_parent();
            break;
          }
      
        case 8:
        case 24:
        case 40:
          trace.grow_by(16);
          
        default: 
          trace.add_node(child, key.slice(1));
          // renew pointer add could change it
          n = (BitTrie*)trace.parent().node();
          n-&gt;add(key.trie_index(), trace.child_of_parent());
        }
      }
      key = key.adavance(2):
      trace.key.append(key.data(), key.size());
      trace.complete = true;
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
          trace.shrink_node_by(16);
      }
       
      return true;
    }
    
  void change_node(NodeRef&amp; child, int index, Trace&amp; trace) {
      switch(child.type()) {
        case kCompressed: {
            Compressed* n = (Compressed*)child.node();
            if (n-&gt;size &lt; 0xff) {
              TempCompressed compressed;
              compressed.node.data[0] = (char)index;
              memcpy(node.data+1, n-&gt;data, n-&gt;size);
              compressed.node.size = n-&gt;size+1;
              child.page.free_node(child.id, trace.map);
              trace.change_node(compressed);
            }
            break;
          }
                
        case kPageLeaf: 
        case kLeaf: {
          TempLeaf leaf(child);
          string key_buffer;
          if (index &gt;= 0) {
            // we have to enhance the key
            key_buffer.append(leaf.key.data(), leaf.key.size());
            key_buffer.insert(0, 1, (char)index);
            leaf.key = Slice(key_buffer.data(), key_buffer.size());
          }
          child.page.free_node(child.id, trace.map);
          trace.change_node(leaf);
        }
      }
    }
};


static BitTrieHandler bittrie;


</t>
<t tx="michael.20141215222649.99">// Interface for NodeRefs

typedef boost::uint8_t trieindex_t;


struct NodeHandler {
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(const Slice&amp; key, NodeRef&amp; rnode, Trace&amp; trace) = 0;
  virtual void next(NodeRef&amp; rnode, Trace&amp; trace) = 0;
  virtual void prev(NodeRef&amp; rnode, Trace&amp; trace) = 0;
  virtual void first(NodeRef&amp; rnode, Trace&amp; trace) = 0;
  virtual void last(NodeRef&amp; rnode, Trace&amp; trace) = 0;
  virtual void add(const Slice&amp; key, const Slice&amp; value, 
                   NodeRef&amp; rnode, Trace&amp; trace) = 0;
  // remove the traces last_index
  virtual bool remove_last_index(NodeRef&amp; rnode, Trace&amp; trace) {
      return false;
    }
    
  static NodeHandler* handlers[200];
};

</t>
<t tx="michael.20141217010530.13">struct NodePtr {
  PagePtr page;
  nodeid_t node;
};</t>
<t tx="michael.20141219202729.2">struct Node { };
struct Trace;

// A reference to a Node
struct NodeRef {
  NodeRef(const PageRef&amp; page_, nodeid_t id_)
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
  void find(const Slice&amp; key, Trace&amp; trace) {
      trie_handlers[type()]-&gt;find(key, *this, trace);
    }
    
  void next(Trace&amp; trace) {
      trie_handlers[type()]-&gt;next(*this, trace);
    }
    
  void prev(Trace&amp; trace) {
      trie_handlers[type()]-&gt;prev(*this, trace);
    }
    
  void first(Trace&amp; trace) {
      trie_handlers[type()]-&gt;first(*this, trace);
    }
    
  void last(Trace&amp; trace) {
      trie_handlers[type()]-&gt;last(*this, trace);
    }
   
  void add(const Slice&amp; key, const Slice&amp; value, Trace&amp; trace) {
      trie_handlers[type()]-&gt;add(key, value, *this, trace);
    }

  void remove_last(Trace&amp; trace) {
      trie_handlers[type()]-&gt;remove_last(*this, trace);
    }

  PageRef page;
  nodeid_t id;
};



</t>
<t tx="michael.20141219202729.5">@language cplusplus
@tabwidth -2
</t>
<t tx="michael.20141220220750.15">// A stack trace inside a trie
// nodes[0] is the first node after root
// nodes[K] is the current node (almost always a leaf)
// key[0] causes the transition from root to nodes[1]
struct Trace {
  struct Transition {
    NodeRef node;
    size_t index; // the index inside Trace.key identifiying
                  // the part causing the transition to node
    Transition(NodeRef&amp; node_, size_t index_)
        : node(node_), index(index_) { }
  };

  std::vector&lt;Transition&gt; stack;
  std::string key;
  int last_index; // the key index before the last pop (-1 if complete)
  bool complete;  // true if key is completely
  Pagemap&amp; map;   // needed for some operations
  NodeStorage&amp; storage;  // needed for some operations
  
  Trace(Pagemap&amp; map_, NodeStorage&amp; storage_) : 
      map(map_), storage(storage_), complete(false) { }
  
  void reset() {
      nodes.clear(0);
      key.clear();
      complete(false);
    }
    
  void push(NodeRef&amp; node) {
      size_t index = key.size();
      stack.push_back(Transition(node, index));
    }
    
  void pop(bool skiplink=true) {
      Transitions&amp; back(stack.back());
      last_index = complete ? -1 : key[back.index];
      complete = false;
      key.resize(back.index);
      stack.pop_back();
      if (skiplink &amp;&amp; current().id == kLink)
        stack.pop_back();
    }
    
  NodeRef&amp; parent() {
      NodeRef parent = stack[stack.size()-2];
      return parent.node.id == kLink ? stack[stack.size()-3] : parent;
    }
    
  NodeRef&amp; current() {
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
        NodeRef&amp; me(current()); 
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
  
 void add_node(TempNode&amp; new_node, const Slice&amp; transition_key) {
      // transistion_key causes transitions from parent to node
      size_t size = new_node.size();
      if (current().page.free_size() &lt; size + sizeof(NodePtr))
        split_page();
        
      NodeRef&amp; new_ref(current().page.new_node(new_size));
      memcpy(new_ref.node(), new_node.node, size);
      push(new_ref);
      key.append(transition_key.data(), transition_key.size());
   }
  
  // change current node to type
  void change_node(TempNode&amp; new_node) {
      NodeRef &amp;node(current());
      size_t size = node.size(), new_size = new_node.size();
      nodeid_t nodeid = node.id;
      if (node.page.free_size() + size &lt; new_size) 
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

  nodeid_t move_node(PageRef&amp; page, nodeid_t id) {
      NodeRef&amp; node(current());
      if (node.page.id == page.id)
        return id;
  
      // move the node an its children to current.page
            
  
  
    }
};</t>
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
</tnodes>
</leo_file>
