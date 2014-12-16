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
<v t="michael.20141215222649.12" a="E"><vh>API</vh>
<v t="michael.20141215222649.38"><vh>struct Options</vh></v>
<v t="michael.20141215222649.23"><vh>class Cursor (Declaration)</vh></v>
<v t="michael.20141215222649.104" a="E"><vh>class Cursor (Implementation)</vh>
<v t="michael.20141215222649.198"><vh>is_valid()</vh></v>
<v t="michael.20141215222649.203"><vh>set()</vh></v>
</v>
<v t="michael.20141215222649.27"><vh>class Storage (Declaration)</vh></v>
<v t="michael.20141215222649.44" a="E"><vh>class Storage (Implementation)</vh>
<v t="michael.20141215222649.189"><vh>~Storage()</vh></v>
<v t="michael.20141215222649.188"><vh>open()</vh></v>
<v t="michael.20141215222649.190"><vh>cursor()</vh></v>
</v>
</v>
<v t="michael.20141215222649.10" a="E"><vh>BitTrie</vh>
<v t="michael.20141215222649.83" a="E"><vh>Nodes</vh>
<v t="michael.20141215222649.99"><vh>NodeHandler</vh></v>
<v t="michael.20141215222649.85"><vh>Leaf</vh></v>
<v t="michael.20141215222649.84"><vh>PageLeaf</vh></v>
<v t="michael.20141215222649.137"><vh>LinkNode</vh></v>
<v t="michael.20141215222649.93"><vh>Node256</vh></v>
<v t="michael.20141215222649.94"><vh>Node257</vh></v>
<v t="michael.20141215222649.126"><vh>CompressNode</vh></v>
<v t="michael.20141215222649.95" a="E"><vh>BitNode</vh>
<v t="michael.20141215222649.96"><vh>32bit</vh></v>
<v t="michael.20141215222649.97"><vh>64bit</vh></v>
</v>
</v>
</v>
<v t="michael.20141215222649.63" a="E"><vh>Memory Managment</vh>
<v t="michael.20141215222649.11" a="E"><vh>@rst memory_manager.html</vh>
<v t="michael.20141215222649.149" a="E"><vh>Links</vh>
<v t="michael.20141215222649.151"><vh>page types</vh></v>
<v t="michael.20141215222649.135"><vh>class NodeMemoryManager (Declaration)</vh></v>
<v t="michael.20141215222649.155" a="E"><vh>class NodeMemoryManager (Implementation)</vh>
<v t="michael.20141215222649.156"><vh>class HeapNodeManager</vh></v>
<v t="michael.20141215222649.168"><vh>class PageMap</vh></v>
<v t="michael.20141215222649.159"><vh>class PersistentNodeManager</vh></v>
<v t="michael.20141215222649.169"><vh>class MemorySegment</vh></v>
<v t="michael.20141215222649.160"><vh>class MultiProcessNodeMemoryManager</vh></v>
</v>
<v t="michael.20141215222649.136"><vh>class LeafMemoryManager</vh></v>
<v t="michael.20141215222649.72" a="E"><vh>Page</vh>
<v t="michael.20141215222649.67"><vh>struct Page</vh></v>
<v t="michael.20141215222649.73"><vh>class PageRef (Declaration)</vh></v>
</v>
</v>
</v>
</v>
</v>
<v t="michael.20141215222649.14" a="E"><vh>Files</vh>
<v t="michael.20141215222649.16" a="E"><vh>@path include/larch</vh>
<v t="michael.20141215222649.17"><vh>@auto leaves.h</vh></v>
</v>
<v t="michael.20141215222649.15" a="E"><vh>@path src</vh>
<v t="michael.20141215222649.81"><vh>@auto bittrie.h</vh></v>
<v t="michael.20141215222649.65"><vh>@auto memory.h</vh></v>
<v t="michael.20141215222649.161"><vh>@auto memory.cpp</vh></v>
<v t="michael.20141215222649.45" a="E"><vh>@auto leaves.cpp</vh></v>
</v>
</v>
</v>
<v t="michael.20141215222649.8"><vh>Tests</vh></v>
<v t="michael.20141215222649.19"><vh>@persistence</vh>
<v t="michael.20141215222649.20"><vh>@data:@auto leaves.h</vh>
<v t="michael.20141215222649.202"><vh>@gnxs</vh></v>
</v>
<v t="michael.20141215222649.49"><vh>@data:@auto storage.cpp</vh>
<v t="michael.20141215222649.204"><vh>@gnxs</vh></v>
</v>
<v t="michael.20141215222649.69"><vh>@data:@auto memory.h</vh>
<v t="michael.20141215222649.191"><vh>@gnxs</vh></v>
</v>
<v t="michael.20141215222649.87"><vh>@data:@auto bittrie.h</vh>
<v t="michael.20141215222649.163"><vh>@gnxs</vh></v>
</v>
<v t="michael.20141215222649.165"><vh>@data:@auto memory.cpp</vh>
<v t="michael.20141215222649.186"><vh>@gnxs</vh></v>
</v>
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
struct CompressNode {
  nodeid_t child;
  byte_t size;
  char data[]
};</t>
<t tx="michael.20141215222649.13"></t>
<t tx="michael.20141215222649.135">// Memory mangement nodes in pages
class NodeMemoryManager {
 public:
  virtual PageRef get_read_page(pageid_t pageid) = 0;
  virtual PageRef get_write_page(pageid_t pageid) = 0;
  virtual PageRef new_page() = 0;
  virtual void free_page(pageid_t pageid) = 0;
  virtual void flush(async=true) { }
  
  // increase the count of active writers
  virtual void inc_writer() { }
  
  // decreas the count of active writers
  virtual void dec_writer() { }
};


class HeapNodeMemoryManager : public NodeMemoryManager {
 public:
  HeapNodeMemoryManager() {}
 
  PageRef get_page(pageid_t pageid) {
    return PageRef(_pages[pageid].get(), pageid, std::shared_ptr&lt;MemorySegment&gt;());
  }

  virtual PageRef get_read_page(pageid_t pageid);
  virtual PageRef get_write_page(pageid_t pageid);
  virtual PageRef new_page();
  virtual void free_page(pageid_t pageid);
    
 private:
  typedef std::unique_ptr&lt;Page&gt; _page_ptr;
  std::vector&lt;_page_ptr&gt; _pages;
  size_t _free_pages;  // count of _free_pages inside the vector

};


class PersistentNodeMemoryManager : public NodeMemoryManager {
 public:
  PersistentNodeMemoryManager(const char* path);
  virtual PageRef get_read_page(pageid_t pageid);
  virtual PageRef get_write_page(pageid_t pageid);
  virtual PageRef new_page();
  virtual void free_page(pageid_t pageid);
  virtual void flush(async=true)
  
 private:
  void grow_file(size_t size);
  void shrink_file(size_t size);
 
  PagePositionMap Page;
  SegmentCache _segment_cache;
  boost::interprocess::file_mapping _file_mapping;
}


class MultiProcessNodeMemoryManager : public PersistentNodeMemoryManager {
 public:
  MultiProcessNodeMemoryManager(const char* path);
  virtual PageRef get_read_page(pageid_t pageid);
  virtual PageRef get_write_page(pageid_t pageid);
  virtual PageRef new_page();
  virtual void free_page(pageid_t pageid);
}
</t>
<t tx="michael.20141215222649.136">class LeafMemoryManager {
 public:
  virtual char* get_data(const PageLeaf&amp; leaf) = 0;
  virtual PageLeaf allocate(size_t size) = 0;
  virtual void free(const PageLeaf&amp; leaf) = 0;
  virtual void flush(async=true) { }
};


class HeapLeafMemoryManager {
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


class PersistentLeafMemoryManager {
 public:
  PersistentMemoryManager(const char* path);
};
</t>
<t tx="michael.20141215222649.137">// links to another page
struct LinkNode {
  pageid_t page;
};</t>
<t tx="michael.20141215222649.14"></t>
<t tx="michael.20141215222649.149">@language cplusplus
@tabwidth -2
</t>
<t tx="michael.20141215222649.15"></t>
<t tx="michael.20141215222649.151">// a page is identified by a page id
// because of copy on write for concurrent storages
// there can be multiple pages with the same id
// but each page has a unquie pageoffset_t
typedef boost::uint32_t pageid_t;     // page id 
typedef boost::uint32_t pageoffset_t; // points to page inside file</t>
<t tx="michael.20141215222649.155">@others
</t>
<t tx="michael.20141215222649.156">HeapNodeMemoryManager::HeapNodeMemoryManager(): _free_pages(0) {


PageRef HeapNodeMemoryManager::get_read_page(pageid_t pageid) {
  return get_page(pageid);
}
  
PageRef HeapNodeMemoryManager::get_write_page(pageid_t pageid) {
  return get_page(pageid);
}
 
PageRef HeapNodeMemoryManager::new_page() {
  if (_free_pages) {
    std::vector&lt;_page_ptr&gt;::iterator i;
    for(i = _pages.begin(); i != _pages.end(); i++) {
      if (!(*i).get()) {
        *i = new Page;
        _free_pages--;
        return PageRef((*i).get(), pageid, std::shared_ptr&lt;MemorySegment&gt;());
    }
    _free_pages = 0;
  }
  
  Page* page = new Page;
  _pages.push_back(page);
  return PageRef((*i).get(), pageid, std::shared_ptr&lt;MemorySegment&gt;());
}

void HeapNodeMemoryManager::free_page(pageid_t pageid) {
  if (pageid == _pages.size()-1) {
    _pages.pop_back(); // remove last page
    // remove all pages at the end
    
    while(!_pages.empty() &amp;&amp; !_pages-back().get()) {
      _pages.pop_back(); 
      _free_pages--;
    }
  }
  else {
    _pages[pageid].reset();
    _free_pages++;
  }
}
</t>
<t tx="michael.20141215222649.159">@others</t>
<t tx="michael.20141215222649.16"></t>
<t tx="michael.20141215222649.160"></t>
<t tx="michael.20141215222649.163">gnx: michael.20141215222649.82
unl: includes
gnx: michael.20141215222649.83
unl: Nodes
gnx: michael.20141215222649.99
unl: Nodes--&gt;NodeHandler
gnx: michael.20141215222649.85
unl: Nodes--&gt;Leaf
gnx: michael.20141215222649.84
unl: Nodes--&gt;PageLeaf
gnx: michael.20141215222649.137
unl: Nodes--&gt;LinkNode
gnx: michael.20141215222649.93
unl: Nodes--&gt;Node256
gnx: michael.20141215222649.94
unl: Nodes--&gt;Node257
gnx: michael.20141215222649.126
unl: Nodes--&gt;CompressNode
gnx: michael.20141215222649.95
unl: Nodes--&gt;BitNode
gnx: michael.20141215222649.96
unl: Nodes--&gt;BitNode--&gt;32bit
gnx: michael.20141215222649.97
unl: Nodes--&gt;BitNode--&gt;64bit
</t>
<t tx="michael.20141215222649.165">gnx: michael.20141215222649.161
</t>
<t tx="michael.20141215222649.168">class PageMap {
  PageMap

  // get the newest page below version
  pageoffset_t get_newest_below(pageid_t page, version_t version);
  
  // get the newest page
  pageoffset_t get_newest(pageid_t page);
  
  // get the oldest page
  pageoffset_t get_oldest(pageid_t page);
  
  void set_newest(pageid_t id, pageoffset_t offset);
  void set_oldest(pageid_t id, pageoffset_t offset);
};


</t>
<t tx="michael.20141215222649.169">// a memory segment of a file

typedef boost::interprocess::mapped_region MemorySegment;
  
</t>
<t tx="michael.20141215222649.186">gnx: michael.20141215222649.162
unl: includes
gnx: michael.20141215222649.155
unl: class NodeMemoryManager (Implementation)
gnx: michael.20141215222649.156
unl: class NodeMemoryManager (Implementation)--&gt;class HeapNodeManager
gnx: michael.20141215222649.168
unl: class NodeMemoryManager (Implementation)--&gt;class PageMap
gnx: michael.20141215222649.159
unl: class NodeMemoryManager (Implementation)--&gt;class PersistentNodeManager
gnx: michael.20141215222649.169
unl: class NodeMemoryManager (Implementation)--&gt;class MemorySegment
gnx: michael.20141215222649.160
unl: class NodeMemoryManager (Implementation)--&gt;class MultiProcessNodeMemoryManager
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
<t tx="michael.20141215222649.19"></t>
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
<t tx="michael.20141215222649.191">gnx: michael.20141215222649.66
unl: includes
gnx: michael.20141215222649.110
unl: namespace larch_leaves
gnx: michael.20141215222649.151
unl: namespace larch_leaves--&gt;page types
gnx: michael.20141215222649.67
unl: namespace larch_leaves--&gt;struct Page
gnx: michael.20141215222649.73
unl: namespace larch_leaves--&gt;class PageRef (Declaration)
gnx: michael.20141215222649.134
unl: namespace larch_leaves--&gt;class PageMap
gnx: michael.20141215222649.135
unl: namespace larch_leaves--&gt;class NodeMemoryManager (Declaration)
gnx: michael.20141215222649.136
unl: namespace larch_leaves--&gt;class LeafMemoryManager
</t>
<t tx="michael.20141215222649.198">bool Cursor::is_valid() const {
  if (_search_trace.empty())
    return false;
    
  return is_leaf(_search_trace.back());
}

</t>
<t tx="michael.20141215222649.20">gnx: michael.20141215222649.17
</t>
<t tx="michael.20141215222649.202">gnx: michael.20141215222649.18
unl: header comment
gnx: michael.20141215222649.22
unl: includes
gnx: michael.20141215222649.24
unl: namespace larch_leave
gnx: michael.20141215222649.122
unl: namespace larch_leave--&gt;simple types
gnx: michael.20141215222649.109
unl: namespace larch_leave--&gt;struct NodePtr
gnx: michael.20141215222649.38
unl: namespace larch_leave--&gt;struct Options
gnx: michael.20141215222649.23
unl: namespace larch_leave--&gt;class Cursor (Declaration)
gnx: michael.20141215222649.27
unl: namespace larch_leave--&gt;class Storage (Declaration)
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
  
  NodePtr top = _search_trace.back();
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
}</t>
<t tx="michael.20141215222649.204">gnx: michael.20141215222649.47
unl: incluces
gnx: michael.20141215222649.46
unl: namespace larch_leave
gnx: michael.20141215222649.104
unl: namespace larch_leave--&gt;class Cursor (Implementation)
gnx: michael.20141215222649.198
unl: namespace larch_leave--&gt;class Cursor (Implementation)--&gt;is_valid()
gnx: michael.20141215222649.203
unl: namespace larch_leave--&gt;class Cursor (Implementation)--&gt;set()
gnx: michael.20141215222649.44
unl: namespace larch_leave--&gt;class Storage (Implementation)
gnx: michael.20141215222649.189
unl: namespace larch_leave--&gt;class Storage (Implementation)--&gt;~Storage()
gnx: michael.20141215222649.188
unl: namespace larch_leave--&gt;class Storage (Implementation)--&gt;open()
gnx: michael.20141215222649.190
unl: namespace larch_leave--&gt;class Storage (Implementation)--&gt;cursor()
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
<t tx="michael.20141215222649.49">gnx: michael.20141215222649.45
</t>
<t tx="michael.20141215222649.5"></t>
<t tx="michael.20141215222649.6"></t>
<t tx="michael.20141215222649.63"></t>
<t tx="michael.20141215222649.67">
// A pointer inside a page with a 16 byte alignment (256*16 = 4096)
typedef byte_t in_page_ptr; 

struct PageNodeRef {
  byte_t type;
  in_page_ptr ptr;
};

/*
  A Page has a size 4096 bytes and can contain maximal
  256 nodes.
  
  it has the following layout (2 nodes in the page):
  +----------------------+
  | node_count (1 byte)  |
  +----------------------+
  | free_area (1 byte)   |
  +----------------------+
  | node_ref[0] (2 byte) |
  +----------------------+
  | node_ref[1] (2 byte) |
  +----------------------+
  |                      |
  |                      |
  +----------------------+  &lt;-- free_area points here
  | node[1] (&gt;= 16 byte) |
  +----------------------+
  | node[0] (&gt;= 16 byte) |
  +----------------------+
  
  node 0 is always an ancestor of all other nodes
  and is called the page root
*/
struct Page {
  union {
    byte_t data[4096];
    struct {
      byte_t node_count;
      in_page_ptr free_area;
      PageNodeRef node_ref[256];
    };
  };
  Page() : node_count(0), free_area(255) { }
};

</t>
<t tx="michael.20141215222649.69">gnx: michael.20141215222649.65
</t>
<t tx="michael.20141215222649.72"></t>
<t tx="michael.20141215222649.73">
class PageRef {
 public:
  PageRef(Page *page, pageidt pageid, std::shared_ptr&lt;MemorySegment&gt; segment) :
    _page(page), _segment(segment) { }

  // returns the node as pointer  
  NodePtr node(byte_t node_id) const {
    PageNodeRef&amp; nref(page_-&gt;node_ref[node_id]);
    return { _segment, nref.type, &amp;_page-&gt;data[nref.ptr*16] };
  }
    
  // adds a new node to a parent node
  // retuns false if page would overflow
  bool add(byte_t parent_id, byte index, byte type, void* node, size_t size);
  bool add_root(byte type, void* node, size_t size);
  
  // removes a node from a parent node
  // returns true if the parent node beomes empty
  bool remove(byte_t parent_id, byte index)
  
  // returns the size of nodeid and its descendants
  size_t tree_size(byte_t node_id) const
  
  // defragments the page (after split, remove)
  void defragment();
  
  byte_t count() const { return _page-&gt;node_count; }
 
 private:
  std::shared_ptr&lt;MemorySegment&gt; _segment;
  pageid_t pageid;
  Page *_page;
};</t>
<t tx="michael.20141215222649.8"></t>
<t tx="michael.20141215222649.83">/* 
  All trie nodes are optmized to work inside pages.



  The basic idea is to use a bit field to compress the trie nodes children
  vector. 
*/

struct NodeRef {
  byte_t type;
  void* ptr;
};


@others
</t>
<t tx="michael.20141215222649.84">// a leaf node wich key and values are &lt;= 256 bytes
struct PageLeaf {
  byte_t key_size;
  byte_t value_size;
  char data[];
};</t>
<t tx="michael.20141215222649.85">// a leaf node with data in Leaf memory
struct PageLeaf {
  size_t segment_no;
  offset_t pointer;
};
</t>
<t tx="michael.20141215222649.87">gnx: michael.20141215222649.81
</t>
<t tx="michael.20141215222649.9"></t>
<t tx="michael.20141215222649.93">// Node with 256 values
struct Node256 {
  nodeid_t children[256];
};

</t>
<t tx="michael.20141215222649.94">// Node with 257 values (256 for each byte value + 1 for end)
struct Node257 {
  nodeid_t children[256];
  nodeid_t end;
};
</t>
<t tx="michael.20141215222649.95">// Node with a range
@others


struct BitNode32 {
  boost::uint32_t bits[1];
};

struct BitNode64 {
  boost::uint32_t bits[2];
};

struct BitNode128 {
  boost::uint32_t bits[4];
};

struct BitNode192 {
  boost::uint32_t bits[6];

};

struct BitNode256 {
  boost::uint32_t  bits[8];
};


</t>
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
<t tx="michael.20141215222649.99">
// Interface for NodeRefs
struct NodeHandler {
  // returns a 16 byte aligned size
  virtual size_t size() const = 0;
  
  // returns true if the node is a leaf
  virtual bool is_leaf() const = 0;
  
  // get the child index by key (signed and unsigned version)
  virtual byte get_child_index(unsigned key) const;
  virtual byte get_child_index(char key) const;
  
  // returns the next node of child index NULL if not found
  virtual NodeRef* find(byte_t child_index, Storage&amp; storage) = 0;
  
  // returns the end node of NULL if None there
  virtual NodeRef* find_end(Storage&amp; storage) = 0;

  // adds a new node (this is the parent)
  // returns the Ref to the new parent (if the parent changed)
  virtual NodeHandler* add(byte_t child_index, const Slice&amp; rest_key, 
                           const Slice&amp; value, Storage&amp; storage) = 0;
                           
  virtual NodeHandler* add_end(const Slice&amp; rest_key, 
                               const Slice&amp; value, Storage&amp; storage) = 0;

  // removes a node 
  virtual void remove(byte child_index);
  virtual void remove_end();
};

struct LeafNodeHandler : public NodeHandler {
  bool key_equal(const Slice&amp; rest_key) const = 0;
};

  </t>
</tnodes>
</leo_file>
