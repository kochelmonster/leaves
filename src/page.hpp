#ifndef _LARCH_LEAVES_PAGE_H
#define _LARCH_LEAVES_PAGE_H

#include <boost/cstdint.hpp>
#include <cassert>
#include <string.h>

#ifdef DEBUG
#include <iomanip>
#include <iostream>
#endif

namespace larch_leaves {

#ifndef PAGE_SIZE
#define PAGE_SIZE (2*4096)
#endif

#ifndef ALIGN
#define ALIGN 16
#endif

#ifndef PAGE_RESERVE
#define PAGE_RESERVE (2*1024)
#endif

#define MAX_NODE_COUNT (PAGE_SIZE / ALIGN)
#define ENTRY_POINTS ((ALIGN / sizeof(boost::uint16_t)) - 1)

template <size_t align> size_t pad(size_t size) {
  return size + (align - 1) - ((size - 1) & (align - 1));
}

inline size_t page_pad(size_t size) { return pad<ALIGN>(size); }

inline size_t pointer_size(size_t size) { return page_pad(size) / ALIGN; }

struct Node;

/*
  The Page layout is
  +----------------------------+
  | Node 1 (size = k1 * ALIGN) |
  +----------------------------+
  | Node 2 (size = k2 * ALIGN) |
  +----------------------------+
  |                            |
  |       free_space           |
  |                            |
  +----------------------------+
  | next_node (2bytes)         |
  +----------------------------+
*/
struct Page {
  /*
    A pointer inside a page. The pointer is aligned i.e.
    To get the pointer of a inpage_ptr you have to calc:

      node_pointer = (char*)page_pointer) + inpage_ptr * ALIGN
  */
  typedef boost::uint16_t ptr;
  typedef boost::uint8_t entry_t;

  ptr next_node;

  /* a dictionary of entry points. ptr is one based: if entry_points[k] == 0 the
  entry_point is free. The real Page::ptr is entry_points[k]-1  */
  ptr entry_points[ENTRY_POINTS];

  char data[PAGE_SIZE - sizeof(ptr) - ENTRY_POINTS * sizeof(ptr)];

  Page() : next_node(0) { memset(entry_points, 0, sizeof(entry_points)); }

  // creates a new node (size must also consider the additional sizeof(header_t))
  ptr new_node(size_t size_);

  // the free size in bytes
  size_t free_size() const { return sizeof(data) - next_node * ALIGN; }

  // the size in bytes
  size_t size() const { return next_node * ALIGN; }

  // the node count
  size_t count() const;

  Node *node(ptr pointer) const { return (Node *)&data[pointer * ALIGN]; }

  // if true the page can take additional roots
  bool has_reserve() const { return free_size() > PAGE_RESERVE; }

  bool has_free_links() const {
    for (entry_t i = 0; i < ENTRY_POINTS; i++) {
      if (!entry_points[i])
        return true;
    }
    return false;
  }

  entry_t free_entry() const {
    for (entry_t i = 0; i < ENTRY_POINTS; i++) {
      if (!entry_points[i])
        return i;
    }
    assert(0);
    return 0;
  }

  // returns true if the page contains no node
  bool unused() const { return next_node == 0; }

  // defragment page returns true if trace has to be refreshed
  bool defragment();

  // adjust the pointers of the page
  void adjust_pointers(Page::ptr old_ptr, int delta);

  // grows a node by delta bytes; returns the delta of adjust_pointers
  int grow_node_by(Page::ptr ptr_, int delta);


  void check();
};

// a page is identified by a page id
// because of "copy on write" for concurrent storages
// there can be multiple pages with the same id
// but each page has a unique pageoffset_t
typedef boost::uint32_t pageid_t;     // page id
typedef boost::uint32_t pageoffset_t; // points to page inside file

struct PageLink {
  pageid_t page_id;
  Page::entry_t entry;
};

// the access class to Page
struct PageRef {
  Page *page;
  pageid_t id;
  pageoffset_t offset;

  PageRef(Page *page_, pageid_t id_, pageoffset_t offset_)
      : page(page_), id(id_), offset(offset_) {}

  PageRef() : PageRef(NULL, 0, 0) {}

  // returns true f the trace was refresh
  bool defragment() { return page->defragment(); }

  int grow_node_by(Page::ptr node_ptr, int delta) {
    return page->grow_node_by(node_ptr, delta);
  }

  // returns true if trace has to be refreshed
  bool change_to_link(Page::ptr node_ptr, pageid_t page_id,
                      Page::entry_t entry);

  size_t size() const { return page->size(); }

  size_t free_size() const { return page->free_size(); }

  bool has_reserve() const { return page->has_reserve(); }

  bool has_free_links() const { return page->has_free_links(); }

  bool unused() const { return page->unused(); }

  Page::entry_t free_entry() const { return page->free_entry(); }

  size_t count() const { return page->count(); }

  Node *node(Page::ptr ptr) const { return page->node(ptr); }

  Page::ptr new_node(size_t size_) { return page->new_node(size_); }

  Page::ptr end() const { return page->next_node; }

#ifdef DEBUG
  void dump(std::ostream &out);
#endif
};

} // namespace larch_leaves
#endif // _LARCH_LEAVES_PAGE_H
