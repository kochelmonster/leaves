#include <boost/interprocess/file_mapping.hpp>
namespace larch_leaves {
// a page is identified by a page id
// because of copy on write for concurrent storages
// there can be multiple pages with the same id
// but each page has a unquie pageoffset_t
typedef boost::uint32_t pageid_t;     // page id 
typedef boost::uint32_t pageoffset_t; // points to page inside file

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
  +----------------------+  <-- free_area points here
  | node[1] (>= 16 byte) |
  +----------------------+
  | node[0] (>= 16 byte) |
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


class PageRef {
 public:
  PageRef(Page *page, pageidt pageid, std::shared_ptr<MemorySegment> segment) :
    _page(page), _segment(segment) { }

  // returns the node as pointer  
  NodePtr node(byte_t node_id) const {
    PageNodeRef& nref(page_->node_ref[node_id]);
    return { _segment, nref.type, &_page->data[nref.ptr*16] };
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
  
  byte_t count() const { return _page->node_count; }
 
 private:
  std::shared_ptr<MemorySegment> _segment;
  pageid_t pageid;
  Page *_page;
};
// maps pageids to page offsets
class PageMap {
 public:

  // get the newest page below version
  pageoffset_t get_newest_below(pageid_t page, version_t version);
  
  // get the newest page
  pageoffset_t get_newest(pageid_t page);
  
  // get the oldest page
  pageoffset_t get_oldest(pageid_t page);
  
  void set_newest(pageid_t id, pageoffset_t offset);
  void set_oldest(pageid_t id, pageoffset_t offset);
  
  // returns the needed size for page_count pages
  size_t calc_size(size_t page_count);
  
  // addapt the 
  void increase(size_t end_size);
  
  
 private:
  struct Entry {
    versiont_t version[2];
    pageoffset_t offset[2];
  };

  boost::uint32_t file_magic;
  boost::uint32_t file_version;
  version_t last_read, last_write;
  size_t node_count;
  size_t free_node_count;
  union {
    char data[];
    Entry entries[];
  };
};


class FreePageMap {
}
// Memory mangement nodes in pages
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
    return PageRef(_pages[pageid].get(), pageid, std::shared_ptr<MemorySegment>());
  }

  virtual PageRef get_read_page(pageid_t pageid);
  virtual PageRef get_write_page(pageid_t pageid);
  virtual PageRef new_page();
  virtual void free_page(pageid_t pageid);
    
 private:
  typedef std::unique_ptr<Page> _page_ptr;
  std::vector<_page_ptr> _pages;
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
class LeafMemoryManager {
 public:
  virtual char* get_data(const PageLeaf& leaf) = 0;
  virtual PageLeaf allocate(size_t size) = 0;
  virtual void free(const PageLeaf& leaf) = 0;
  virtual void flush(async=true) { }
};


class HeapLeafMemoryManager {
 public:
  virtual char* get_data(const PageLeaf& leaf) {
    return (char*)leaf.pointer;
  }
  
  virtual PageLeaf allocate(size_t size) {
    return { 0, (offset_t)new char[size] };
  }
  
  virtual void free(const PageLeaf& leaf) {
    delete[] (char*)leaf.pointer;
  }
};


class PersistentLeafMemoryManager {
 public:
  PersistentMemoryManager(const char* path);
};
}

