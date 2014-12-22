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
<v t="michael.20141215222649.13" a="E"><vh>Logical</vh>
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
<v t="michael.20141215222649.10" a="E"><vh>BitTrie</vh>
<v t="michael.20141215222649.83" a="E"><vh>Nodes</vh>
<v t="michael.20141215222649.85"><vh>Leaf</vh></v>
<v t="michael.20141215222649.84"><vh>PageLeaf</vh></v>
<v t="michael.20141215222649.126"><vh>Compressed</vh></v>
<v t="michael.20141215222649.137"><vh>LinkNode</vh></v>
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
<v t="michael.20141215222649.73"><vh>class PageRef</vh></v>
</v>
</v>
</v>
</v>
</v>
<v t="michael.20141215222649.14"><vh>Files</vh>
<v t="michael.20141215222649.16"><vh>@path include/larch</vh>
<v t="michael.20141215222649.17"><vh>@shadow leaves.h</vh></v>
<v t="michael.20141215222649.65"><vh>@shadow node.h</vh></v>
<v t="michael.20141219202729.4"><vh>@shadow leaf.h</vh></v>
</v>
<v t="michael.20141215222649.15"><vh>@path src</vh>
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
</v>
<v t="michael.20141219202729.5"><vh>edit</vh>
<v t="michael.20141215222649.81"></v>
<v t="michael.20141219202729.8"></v>
<v t="michael.20141215222649.65"></v>
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
    
  void add(const Slice&amp; key, const Slice&amp; value,
           NodRef&amp; rnode, HandlerContext&amp; context) {
      Compressed *n = (Compressed*)rnode.get_node();
      
      PageManipulator pm(context);      
      size_t prefix_size = common_prefix(key.data(), n-&gt;data,
                                         std::min(n-&gt;size, key.size()));
      TempCompressed me(n-&gt;data+prefix_size, n-&gt;size-prefix_size);
      TempTrie trie;
      nodeid_t child = n-&gt;child;
      switch(prefix_size) {
        case 0: {
            pm.change(rnode.id, trie);
            pm.add(me, me.data());
            break;
          }
        
        case 1: {
            TempTrie parent_trie;
            pm.change(rnode.id, parent_trie);
            pm.add(trie);
            
            BitTrie* bn = (BitTrie*)pm.parent().get_node();
            bn-&gt;add(key.trie_index(), trie.id);
            
            if (me.size() == 1) {
              child = pm.move(rnode.page, child);
              bn = (BitTrie*)pm.current().get_node();
              bn-&gt;add(me.data().trie_index(), child);
            }
            else {  
              pm.add(me, me.data());
              child = pm.move(rnode.page, child);
              n = (Compressed*)pm.current().get_node();
              n-&gt;child = child;
            }
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            pm.change(rnode.id, compressed);
            pm.add(trie)
            compressed.link(trie);
          }
      }

      pm.add(me);
      trie.set_child(index);
      
      // pop back trace to trie
      context.trace.nodes.back().pop(context);
      context.trace.nodes.back().add(key.advance(prefix_size), value, context);

      Slice mykey(n-&gt;data+prefix_size, n-&gt;key_size-prefix_size);
      TempCompressed me(mykey, myvalue);
                                                                        
      Slice mykey(n-&gt;data+prefix_size, n-&gt;key_size-prefix_size);
      Slice myvalue(n-&gt;data+key_size, n-&gt;value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      int index = trie.add(mykey);


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

struct LinkHandler : public NodeHandler, public LeafMixin {
  NodeRef link_node(NodeRef&amp; rnode, HandlerContext&amp; context) {
      Link *l = (Link*)node.get_node();
      PageRef next_page(context.get_page(l-&gt;page));
      context.trace.nodes.push_back(rnode);
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
    
  void pop(NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.pop_back();
      NodeRef&amp; parent(context.trace.nodes.back());
      parent.pop(context);
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
    
  nodeid_t get_extra(nodeid_t id) const {
      return _page-&gt;node_ptr[id].extra;
    }
    
  void get_extra(nodeid_t id, nodeid_t v) const {
      _page-&gt;node_ptr[id].extra = v;
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
      size_t prefix_size = common_prefix(key.data(), n-&gt;data, 
                                         std::min(key.size(), n-&gt;key_size));
      Slice mykey(n-&gt;data+prefix_size, n-&gt;key_size-prefix_size);
      Slice myvalue(n-&gt;data+key_size, n-&gt;value_size);
      TempLeaf me(mykey, myvalue);
      TempTrie trie;
      int index = trie.add(mykey);
      
      switch(prefix_size) {
        case 0:
          pm.change(rnode.id, trie);
          break;
        
        case 1: {
            TempTrie parent_trie;
            pm.change(rnode.id, parent_trie);
            pm.add(trie, key.slice(1));
            BitTrie* pn = (BitTrie*)pm.parent.get_node();
            pn-&gt;add(key.trie_index(), trie.id);
            break;
          }
            
        default: {
            TempCompressed compressed(key.data(), prefix_size);
            pm.change(rnode.id, compressed);
            pm.add(trie, prefix_size)
            n = (PageLeaf*)pm.parent().get_node();
            n.child = 
          }
      }

      pm.add(me, mykey);
      
      BitTrie* tn = (BitTrie*)pm.parent.get_node();
      tn-&gt;add(mykey.trie_index(), trie.id);

      // pop back trace to trie
      pm.pop();
      pm.current().add(key.advance(prefix_size), value, context);
    }
};

static PageLeafHandler pageleaf;</t>
<t tx="michael.20141215222649.85">// a leaf node with data in Leaf memory
struct Leaf {
  offset_t pointer;
  size_t size;
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

static LeafHandler leaf;
</t>
<t tx="michael.20141215222649.9"></t>
<t tx="michael.20141215222649.93">// Node with the complete alphabet 
// children count is &gt; 56
struct Trie {
  nodeid_t children[64];
};

struct TrieHandler : public NodeHandler {
  virtual void add_to_trace(int key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      if (key &gt;=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }
  
  void pop(Node* node, HandlerContext&amp; context) {
      context.trace.nodes.pop_back();
      if (context.trace.last_was_empty)
        context.trace.last_was_empty = false
      else
        context.trace.key.pop_back();
  }
  
  void add(const Slice&amp; key, const Slice&amp; value,
           NodeRef&amp; rnode, HandlerContext&amp; context) {
      Trie *n = (Trie*)rnode.get_node();
      PageManipulator pm(context);
      TempLeaf child(key.advance(1), value);
      pm.add(child, key.slice(1));  // trace is now at child
      
      if (key.empty()) {
        pm.parent().extra(child.id)
      }
      else {
        n = (Trie*)pm.parent().get_node();
        n-&gt;children[key.trie_index()] = child.id;
      }
    }
    
  @others
};

static TrieHandler trie;


</t>
<t tx="michael.20141215222649.95">// Node with a range
@others


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
};


struct BitTrieHandler : public NodeHandler {
  void add_to_trace(int key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      context.trace.nodes.push_back(rnode);
      if (key &gt;=0)
        context.trace.key.append((char)key);
      else
        context.trace.last_was_empty = true;
    }

  void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
      if (key.empty()) {
        nodeid_t end_child = rnode.extra();
        if (end_child) { 
          add_to_trace(-1, rnode, context);
          NodeRef(rnode.page, end_child).first(context);
        }
        return;
      }
        
      trieindex_t index = key.trie_index();
      BitTrie *n = (BitTrie*)rnode.get_node();
      int child_index = n-&gt;get_child_index(index);
      if (child_index &gt;= 0) {
        add_to_trace(index, rnode, context);
        nodeid_t child = n-&gt;children[child_index];
        NodeRef(rnode.page, child).find(key.advance(1), context);
      }
    }
    
  void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      if (context.trace.last_was_empty) {
        context.trace.last_was_empty = false;
        nodeid_t first_bit = n-&gt;first_bit();
        context.trace.key.append((char)first_bit);
        NodeRef(rnode.page, n-&gt;children[0]).first();
        return;
      }
      
      trieindex_t *index = (trieindex_t*)&amp;trace.key.back();
      int child_index = n-&gt;get_child_index(*index) + 1;
      if (child_index &lt; n-&gt;count()) {
        *index = n-&gt;next_bit(*index)
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
      
      BitTrie *n = (BitTrie*)rnode.get_node();
      trieindex_t *index = (trieindex_t*)&amp;trace.key.back();
      int child_index = n-&gt;get_child_index(*index) - 1;
      if (child_index &gt;= 0) {
        *index = n-&gt;prev_bit(*index);
        NodeRef(rnode.page, n-&gt;children[child_index]).last();
        return
      }
    
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        context.trace.key.pop_back();
        context.trace.last_was_empty = true;
        NodeRef(rnode.page, n-&gt;end).last(context);
        return;
      }
  
      parent_prev(rnode context);
    }
    
  void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      nodeid_t end_child = rnode.extra();
      if (end_child) {
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, end_child).first();
      }
      else {
        trieindex_t index = n-&gt;first_bit();
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, n-&gt;children[0]).first();
      }
    }
    
  void last(Node* node, PageRef&amp; page, HandlerContext&amp; context) {
      trieindex_t index = n-&gt;last_bit();
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
    
  void add(const Slice&amp; key, const Slice&amp; value,
           NodeRef&amp; rnode, HandlerContext&amp; context) {
      BitTrie *n = (BitTrie*)rnode.get_node();
      PageManipulator pm(context);      
      TempLeaf child(key.advance(1), value);
      
      if (key.empty()) {
        pm.add(child);
        pm.parent().get_node().extra(child.id);
        return;
      }
      
      size_t count = n-&gt;count();
      switch(count) {
        case 56: {
            TempTrie trie(n);
            pm.change(rnode.id, trie);
            pm.add(child);
            Trie *np = (Trie*)pm.parent().get_node();
            np-&gt;children[key.trie_index()] = child.id;
            break;
          }
      
        case 8:
        case 24:
        case 40:
          pm.grow_by(16);
          
        default: 
          pm.add(child);
          // renew pointer add could change it
          n = (BitTrie*)pm.parent().get_node();
          n-&gt;add(key.trie_index(), child.id);
      }
    }    
};

static BitTrieHandler bittrie;</t>
<t tx="michael.20141217010530.13">struct NodePtr {
  PagePtr page;
  nodeid_t node;
};</t>
<t tx="michael.20141219202729.5"></t>
<t tx="michael.20141220220750.2">void find(const Slice&amp; key, NodeRef&amp; rnode, HandlerContext&amp; context) {
    if (key.empty()) {
      nodeid_t end_child = rnode.extra();
      if (end_child) { 
        add_to_trace(-1, rnode, context);
        NodeRef(rnode.page, end_child).first(context);
      }
      return;
    }

    Trie *n = (Trie*)node.get_node();
    trieindex_t index = key.trie_index();
    nodeid_t child = n-&gt;children[index];
    if (child) {
      add_to_trace(index, rnode, context);
      NodeRef(rnode.page, child).find(key.advance(1), context);
    }
  }

void next(NodeRef&amp; rnode, HandlerContext&amp; context) {
    trieindex_t *index;
        
    if (context.trace.last_was_empty) {
      context.trace.last_was_empty = false;
      context.trace.key.append((char)0);
      index = (trieindex_t*)&amp;trace.key.back();
    }
    else {
      index = (trieindex_t*)&amp;trace.key.back();
      (*index)++;
    }
        
    Trie*n = (Trie*)rnode.get_node();
    for(; *index &lt; 64; (*index)++) {
      nodeid_t child = n-&gt;children[*index];
      if (child) {
        NodeRef(rnode.page, child).first();
        return;
    }

    parent_next(rnode, context);
  }
  
void prev(NodeRef&amp; rnode, HandlerContext&amp; context) {
    if (context.trace.last_was_empty) {
      parent_prev(rnode, context);
      return;
    }

    trieindex_t *index = (trieindex_t*)&amp;context.trace.key.back());
    Trie *n = (Trie*)rnode.get_node();
    while(*index &gt; 0) {
      (*index)--;
      nodeid_t child = n-&gt;children[*index];
      if (child) {
        NodeRef(rnode.page, child).last();
        return;
      }
    }
    
    nodeid_t end_child = rnode.extra();
    if (end_child) {
      context.trace.key.pop_back();
      context.trace.last_was_empty = true;
      NodeRef(rnode.page, end_child).last(context);
    }
    else {
      parent_prev(rnode, context);
    }
  }

void first(NodeRef&amp; rnode, HandlerContext&amp; context) {
    nodeid_t end_child = rnode.extra();

    if (end_child ) {
      add_to_trace(-1, rnode, context);
      NodeRef(rnode.page, end_child ).first();
    }

    Trie *n = (Trie*)node;
    for(int index = 0; index &lt; 64; i++) {
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
    Trie *n = (Trie*)rnode.get_node();
    for(int index = 63; index &gt;= 0; i--) {
      nodeid_t child = n-&gt;children[index];
      if (child) {
        add_to_trace(index, rnode, context);
        NodeRef(rnode.page, child).last(context);
        return;
      }
    }
    assert(0);
  }</t>
<t tx="michael.20141220220750.8">Insert multiple keys with a common part &gt; 256 bytes</t>
</tnodes>
</leo_file>
