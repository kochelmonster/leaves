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
<v t="michael.20141215222649.9"><vh>Source</vh>
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
<v t="michael.20141215222649.137"><vh>LinkNode</vh></v>
<v t="michael.20141219202729.12" a="E"><vh>TrieNodes</vh>
<v t="michael.20141215222649.93" a="E"><vh>Node64</vh>
<v t="michael.20141220220750.2"><vh>Navigation</vh></v>
</v>
<v t="michael.20141215222649.94"><vh>Node257</vh></v>
<v t="michael.20141215222649.126"><vh>Compressed</vh></v>
<v t="michael.20141215222649.95" a="E"><vh>BitNode</vh>
<v t="michael.20141215222649.96"><vh>32bit</vh></v>
<v t="michael.20141215222649.97"><vh>64bit</vh></v>
</v>
</v>
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
<v t="michael.20141215222649.73"><vh>class PageRef</vh></v>
</v>
</v>
</v>
</v>
</v>
<v t="michael.20141215222649.14"><vh>Files</vh>
<v t="michael.20141215222649.16"><vh>@path include/larch</vh>
<v t="michael.20141215222649.17"><vh>@shadow leaves.h</vh></v>
<v t="michael.20141215222649.65" a="E"><vh>@shadow node.h</vh></v>
<v t="michael.20141219202729.4"><vh>@shadow leaf.h</vh></v>
</v>
<v t="michael.20141215222649.15"><vh>@path src</vh>
<v t="michael.20141215222649.81" a="E"><vh>@shadow bittrie.h</vh></v>
<v t="michael.20141219202729.8" a="E"
expanded="michael.20141215222649.83,michael.20141219202729.12,michael.20141215222649.93,michael.20141215222649.95,"><vh>@shadow bittrie.cpp</vh></v>
<v t="michael.20141215222649.161"><vh>@shadow node.cpp</vh></v>
<v t="michael.20141215222649.45"><vh>@shadow leaves.cpp</vh></v>
</v>
</v>
</v>
<v t="michael.20141215222649.8"><vh>Tests</vh></v>
<v t="michael.20141219202729.5" a="E"><vh>edit</vh>
<v t="michael.20141215222649.81" a="E"></v>
<v t="michael.20141219202729.8" a="E"
expanded="michael.20141215222649.83,michael.20141219202729.12,michael.20141215222649.93,michael.20141215222649.95,"></v>
<v t="michael.20141215222649.65" a="E"
expanded="michael.20141215222649.72,"></v>
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
struct Compressed {
  nodeid_t child;
  boost::uint8_t size;
  char data[]
};



struct CompressHandler : public NodeHandler, public LeafMixin {
  void add_to_trace(Compressed* c, NodeRef&amp; rnode, HandlerContext&amp; context) {
            context.trace.nodes.push_back(rnode);
      context.trace.key.append(c-&gt;data, c-&gt;size);
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      Compressed *n = (Compressed*)rnode.gt_node();
      add_to_trace(n, rnode, context);
      if (key.size() &lt; n-&gt;size)
        return;
        
      if (memcmp(key.data(), n-&gt;data, n-&gt;size) == 0) {
        NodeRef(rnode.page, n-&gt;child).find(key.advance(n-&gt;size), context);
      }
    }
    
  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      add_to_trace((Compressed*)rnode.get_node(), rnode, context);
    }
    
  void last(NodeRef&amp; rnode, HandlerContext&amp; context) {
      add_to_trace((Compressed*)rnode.get_node(), rnode, context);
    }
    
  void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
      Compressed *n = (Compressed*)rnode.get_node();
      context.trace.nodes.pop_back();
      context.trace.key.resize(key.size()-n-&gt;size);
    }
};

static CompressedHandler compressed;</t>
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
struct Link {
  pageid_t page;
};

struct LinkHandler : public NodeHandler, public LinkMixin {
  NodeRef link_node(NodeRef&amp; rnode, HandlerContext&amp; context) {
      Link *l = (Link*)node.get_node();
      PageRef next_page(context.map.get_page(l-&gt;page));
      return NodeRef(next_page, 0);
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      link_node(rnode, context).find(key, context);
    }
    
    void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      link_node(rnode, context).first(context);
    }
    
    void last(NodeRef&amp; rnode, HandlerContext&amp; context) {
      link_node(rnode, context).last(context);
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
  std::shared_ptr&lt;Cursor&gt; cursor(const Slice&amp; namespace, bool signed_compare=false);
  std::shared_ptr&lt;Cursor&gt; cursor(bool signed_compare=false);
  
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
<t tx="michael.20141215222649.67">// Nodes are identfied by a node id.
// the node id is the index of the node pointer table.

typedef boost::uint8_t nodetype_t;
typedef boost::uint8_t nodeid_t;
typedef boost::uint8_t bsize_t;  // byte size

// A pointer inside a page with a 16 byte alignment (256*16 = 4096)
typedef boost::uint8_t inpage_ptr;

struct NodePtr {
  inpage_ptr ptr;      // position inside page
  nodetype_t type;     // node type
  unsigned char extra; // is used by several nodes and
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
  +----------------------+ &lt;-- free_start points to the end all nodes
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
      bsize_t count;  // node count
      inpage_ptr free_start;
      NodePtr node_ptr[];
    };
  };      
      
  Page() : last_node(0), holes(0), first_hole(0), free_start(0) { }

};

</t>
<t tx="michael.20141215222649.72">@others</t>
<t tx="michael.20141215222649.73">// a page is identified by a page id
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
      return &amp;_page-&gt;node_ptr[id];
    }
 
  nodetype_t type(nodeid_t id) const {
      return _page-&gt;node_ptr[id].type;
    }
    
  Node* get_node(nodeid_t id) const {
      return (Node*)&amp;_page-&gt;data[_page-&gt;node_ptr[id].ptr*16];
    }
    
  

  size_t free_size() const {
      return 4096 - _page.free_start*16 - PAGE_HEADER_SIZE 
        - (_page-&gt;last_node+1)*sizeof(NodePtr)
    }

 private:
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


// links do not implement these methods
struct LinkMixin {
  virtual void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
      assert(0);
    }
  virtual void prev(NodeRef&amp; rnode, HandlerContext&amp; context)  {
      assert(0);
    }
    
  virtual void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
      assert(0);
    }
};


struct LeafMixin {
  void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
      parent_next(rnode, context);
    }
    
  void prev(Node* rnode, PageRef&amp; page, HandlerContext&amp; context) {
      parent_prev(rnode, context);
    }
  
};

@others

NodeHandler* NodeHandler::handlers[200] = {
  &amp;leaf;
  &amp;pageleaf,
  &amp;link,
  &amp;node256,
  &amp;node257,
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
  void add_to_trace(NodeRef&amp; rnode, PageLeaf *n, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      context.trace.key.append(n-&gt;data, n-&gt;key_size);
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      PageLeaf *n = (PageLeaf*)rnode.get_node();
      if (key.size() == n-&gt;key_size)
          &amp;&amp; memcmp(key.data(), n-&gt;data, n-&gt;key_size) == 0) {
        add_to_trace(rnode, n, context);
      }
    }
  
  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      add_to_trace(rnode, (PageLeaf*)rnode.get_node(), context);
    }
    
  void last(NodeRef&amp; rnode, HandlerContext&amp; context) {
      add_to_trace(rnode, (PageLeaf*)rnode.get_node(), context);
    }
  
  void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
      PageLeaf *n = (PageLeave*)rnode.get_node();
      context.trace.nodes.pop_back();
      context.trace.key.resize(key.size()-n-&gt;key_size);
    }
    
  void add(const Slice&amp; key, const Slice&amp; value,
           NodeRef&amp; rnode, HandlerContext&amp; context) {
      PageLeaf *n = (PageLeave*)rnode.get_node();
      
      PageManipulator pm(context);      
      size_t prefix = common_prefix(key.data(), n-&gt;data, 
                                    std::min(key.size(), n-&gt;key_size));
      Slice mykey(n-&gt;data+prefix, n-&gt;key_size - prefix);
      Slice myvalue(n-&gt;data+key_size, n-&gt;value_size);
      Slice restkey = key.advance(prefix);
      TempLeaf child1(mykey, myvalue), child2(rest_key, value);
      TempTrie trie;
      int index1 = trie.add(mykey), index2 = trie.add(rest_key);
      
      switch(prefix) {
        case 0:
          pm.change(rnode.id, trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            int index = parent_trie.add(n-&gt;data[0]);
            pm.change(rnode.id, parent_trie);
            pm.add(trie);
            parent_trie.set_child(index, trie);
            break;
          }
            
        default: {
            TempCompressed compressed(n-&gt;data, prefix);
            pm.change(rnode.id, compressed);
            pm.add(trie)
            compressed.link(trie);
          }
      }
                  
      pm.add(child1);
      trie.set_child(index1);
      pm.add(child2);
      trie.set_child(index2);
    }
};

static PageLeafHandler pageleaf;</t>
<t tx="michael.20141215222649.85">// a leaf node with data in Leaf memory
struct Leaf {
  offset_t pointer;
};


struct LeafHandler : public NodeHandler, public LeafMixin {
  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
    }
    
  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void last(NodeRef&amp; rnode, HandlerContext&amp; context) {
      //add_to_trace((Leaf*)node, page, context);
    }
    
  void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
    }
};

static LeafHandler leaf;</t>
<t tx="michael.20141215222649.9"></t>
<t tx="michael.20141215222649.93">// Node with the complete alphabet 
// children count is &gt; 56
struct Node64 {
  nodeid_t children[64];
};

struct Node256Handler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      context.trace.key.append((char)key);
    }
  
  void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.pop_back();
      context.trace.key.pop_back();
  }
  
  @others
};

static Node256Handler node256;


</t>
<t tx="michael.20141215222649.94">// Node with 257 values (256 for each byte value + 1 for end)
struct Node257 {
  nodeid_t children[256];
  nodeid_t end;
};

struct Node257Handler : public Node256Handler {
  virtual void add_to_trace(int key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      if (key &gt;=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }
    
  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      if (key.empty()) {
        Node257 *n = (Node257*)rnode.get_node();
        if (n-&gt;end) { 
          add_to_trace(-1, rnode, context);
          NodeRef(rnode.page, n-&gt;end).first(context);
        }
        return;
      }
      Node256Handler::find(key, node, page, context);
    }
  
  void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        nodeid_t child = _next(rnode, context); // child must be != NULL
        NodeRef(rnode.page, child).first();
        return;
      }
      Node256Handler::next(rnode, context);
    }
    
  void prev(NodeRef&amp; rnode, HandlerContext&amp; context) {
      if (context.trace.last_was_empty) {
        parent_prev(rnode, context);
        return;
      }
    
      if (_prev(rnode, page, context)) {
        Node257 *n = (Node257*)node.get_node();
        if (n-&gt;end) {
          context.trace.key.pop_back();
          context.trace.last_was_empty = true;
          NodeRef(rnode.page, n-&gt;end).last(context);
        }
        else {
          parent_prev(rnode, context);
      }
    }
  }

  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      Node257 *n = (Node257*)rnode.get_node();
      if (n-&gt;end) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, n-&gt;end).first();
      }
      else {
        Node256Handler::first(node, page, context);
      }
    }
  
  void pop(Node* node, HandlerContext&amp; context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
  }
};

static Node257Handler node257;


    
</t>
<t tx="michael.20141215222649.95">// Node with a range
@others

inline int popcount64(boost::uint64_t b) {
  b = (b &amp; 0x5555555555555555LU) + (b &gt;&gt; 1 &amp; 0x5555555555555555LU);
  b = (b &amp; 0x3333333333333333LU) + (b &gt;&gt; 2 &amp; 0x3333333333333333LU);
  b = b + (b &gt;&gt; 4) &amp; 0x0F0F0F0F0F0F0F0FLU;
  b = b + (b &gt;&gt; 8);
  b = b + (b &gt;&gt; 16);
  b = b + (b &gt;&gt; 32) &amp; 0x0000007F;
  return (int) b;
}




struct BitNode32 {
  boost::uint32_t bits[1];
  nodeid_t end;
  nodeid_t children[];
  
  int get_child_index(trieindex_t index) {
      if (bits[0] &amp; (1 &lt;&lt; index)) {
        boost::uint32_t m = ~0;
        m &gt;&gt;= (31-index)
        return popcount32(bits[0] &amp; m);
      }
      return -1;
    }
};

struct BitNode64 {
  union {
    boost::uint32_t bits[2];
    boost::uint64_t lbits[1];
  };
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode128 {
  union {
    boost::uint32_t bits[4];
    boost::uint64_t lbits[2];
  };
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode192 {
  boost::uint32_t bits[6];
  nodeid_t end;
  nodeid_t children[];
};

struct BitNode256 {
  union {
    boost::uint32_t bits[8];
    boost::uint64_t lbits[4];
  };
  nodeid_t end;
  nodeid_t children[];
};

template&lt;typename node_type, size_t offset, size_t handler_id&gt;
struct BitNodeHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      if (key &gt;=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      node_type *n = (node_type*)rnode.get_node();
      if (key.empty()) {
        if (n-&gt;end) {
          add_to_trace(-1, n, page, context);
          NodeRef(page, n-&gt;end.ptr, n-&gt;end.type).first(context);
        }
        return;
      }  
          
      trieindex_t index = key.trie_index();
      if (offset &lt;= index &amp;&amp; index &lt; offset+node_type::range) {
        int child_index = n-&gt;get_child_index(index-offset);
        if (child_index &gt;= 0) {
          add_to_trace(index, rnode, context);
          nodeid_t child = n-&gt;children[child_index];
          NodeRef(rnode.page, child).find(key.advance(1), context);
        }
      }
    }
    
  void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
      node_type *n = (node_type*)rnode.get_node();
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        NodeRef(rnode.page, n-&gt;children[0]).first();
        return;
      }
      
      trieindex_t *index = (trieindex_t*)&amp;trace.key.back();
      int child_index = n-&gt;get_child_index(*index-offset) + 1;
      *index = n-&gt;get_next_bit(*index-offset) + offset;
      
      if (child_index &lt; n-&gt;count()) {
        NodeRef(rnode.page, n-&gt;children[child_index]).first();
        return
      }
      
      parent_next(rnode, context);
    }
    
  void prev(NodeRef&amp; rnode, HandlerContext&amp; context) {
      if (context.trace.last_was_empty) {
        parent_prev(rnode, context);
        return;
      }
      
      node_type *n = (node_type*)rnode.get_node();
      trieindex_t *index = (trieindex_t*)&amp;trace.key.back();
      int child_index = n-&gt;get_child_index(*index-offset) - 1;
      *index = n-&gt;get_prev_bit(*index-offset) + offset;
      if (child_index &gt;= 0) {
        NodeRef(rnode.page, n-&gt;children[child_index]).last();
        return
      }
    
      if (n-&gt;end) {
        context.trace.key.pop_back();
        context.trace.last_was_empty = true;
        NodeRef(rnode.page, n-&gt;end).last(context);
        return;
      }
  
      parent_prev(rnode context);
    }
    
  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      node_type *n = (node_type*)rnode.get_node();
      if (n-&gt;end) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, n-&gt;end).first();
      }
      else {
        trieindex_t index = n-&gt;get_first_bit() + offset;
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, n-&gt;children[0]).first();
      }
    }
    
  void last(Node* node, PageRef&amp; page, HandlerContext&amp; context) {
      trieindex_t index = n-&gt;get_last_bit() + offset;
      bsize_t last = n-&gt;count() - 1;
      add_to_trace(index, rnode, context);
      NodeRef(rnode.page, n-&gt;children[last]).first();
    }
    
  void pop(Node* node, HandlerContext&amp; context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
    }
};</t>
<t tx="michael.20141215222649.96">#if UINTPTR_MAX == 0xffffffff

// maybe there is no intrenic

#ifdef __GNUC__
#  define popcount __builtin_popcountl
#endif



typedef unsigned long word_t;
#  elif UINTPTR_MAX == 0xffffffffffffffff
#    define popcount __builtin_popcountll
#  endif




#ifdef _MSC_VER
#  include &lt;intrin.h&gt;

#ifdef _M_X64
#pragma intrinsic(_BitScanForward64)
#define popcount __popcnt64
typedef boost::uint64_t word_t;
#else
#pragma intrinsic(_BitScanForward)
#define popcount __popcnt
typedef boost::uint32_t word_t;
#endif //_M_X64
#endif


#endif</t>
<t tx="michael.20141215222649.97">#if UINTPTR_MAX == 0xffffffffffffffff


#ifdef __GNUC__
#include &lt;stdint.h&gt;
#  if UINTPTR_MAX == 0xffffffff
#    define popcount __builtin_popcountl
typedef unsigned long word_t;
#  elif UINTPTR_MAX == 0xffffffffffffffff
#    define popcount __builtin_popcountll
#  endif




#ifdef _MSC_VER
#  include &lt;intrin.h&gt;

#ifdef _M_X64
#pragma intrinsic(_BitScanForward64)
#define popcount __popcnt64
typedef boost::uint64_t word_t;
#else
#pragma intrinsic(_BitScanForward)
#define popcount __popcnt
typedef boost::uint32_t word_t;
#endif //_M_X64
#endif



#endif</t>
<t tx="michael.20141217010530.13">struct NodePtr {
  PagePtr page;
  nodeid_t node;
};</t>
<t tx="michael.20141219202729.12">@others</t>
<t tx="michael.20141219202729.5"></t>
<t tx="michael.20141220220750.2">bool _prev(NodeRef&amp; rnode, HandlerContext&amp; context) {
    trieindex_t *index = (trieindex_t*)&amp;context.trace.key.back());
    Node64 *n = (Node64*)rnode.get_node();
    while(*index &gt; 0) {
      (*index)--;
      nodeid_t child = n-&gt;children[*index];
      if (child) {
        NodeRef(rnode.page, child).last();
        return false;
      }
    }
    return true;
  }

nodeid_t _next(NodeRef&amp; rnode, HandlerContext&amp; context) {
    trieindex_t *index = (trieindex_t*)&amp;trace.key.back();
    Node64 *n = (Node64*)rnode.get_node();
    while(*index &lt; 0xff) {
      (*index)++;
      nodeid_t child = n-&gt;children[*index];
      if (child) 
        return child;
    }
    return 0;
  }

void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
    if (key.empty())
      // this node type has no empty
      return;

    Node256 *n = (Node256*)node.get_node();
    trieindex_t index = key.trie_index();
    nodeid_t child = n-&gt;children[index];
    if (child) {
      add_to_trace(index, rnode, context);
      NodeRef(rnode.page, child).find(key.advance(1), context);
    }
  }

void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
    nodeid_t child = _next(node, page, context);
    if (child)
      NodeRef(rnode.page, child).first();
    else
      parent_next(rnode, context);
  }
  
void prev(NodeRef&amp; rnode, HandlerContext&amp; context) {
    if (_prev(rnode, context)) 
      parent_prev(rnode, context);
  }

void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
    Node256 *n = (Node256*)node;
    for(int index = 0; index &lt;= 0xff; i++) {
      nodeid_t child = n-&gt;children[index];
      if (child) {
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, child).first();
        return;
      }
    }
    assert(0);
  }
  
void last(NodeRef&amp; rnode, HandlerContext&amp; context) {
    Node256 *n = (Node256*)rnode.get_node();
    for(int index = 0xff; index &gt;= 0; i--) {
      nodeid_t child = n-&gt;children[index];
      if (child) {
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, child).last(context);
        return;
      }
    }
    assert(0);
  }</t>
</tnodes>
</leo_file>
