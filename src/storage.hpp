// declarations for the node storage
#ifndef _LARCH_LEAVES_STORAGE_HPP
#define _LARCH_LEAVES_STORAGE_HPP

#include "page.hpp"
#include <memory>
#include <vector>
#include <boost/unordered_set.hpp>

namespace larch_leaves {

// Translates a pageid to page offset
struct PageMap {
  virtual PageRef get_page(pageid_t id) = 0;
};

class NodeStorage {
public:
  // returns a page with at least neeeded_space free
  virtual PageRef free_page(size_t needed_space) = 0;

  // registers a page with no reserve
  virtual void no_reserve(const PageRef &page) = 0;

  // registers a page with reserve or releases a page if not used anymore
  virtual void with_reserve(const PageRef &page) = 0;
};

struct NodeStorageInHeap : public NodeStorage, public PageMap {
  typedef std::unique_ptr<Page> _page_ptr;
  typedef std::vector<_page_ptr> _page_container_t;
  typedef boost::unordered_set<pageid_t> _free_pages_t;
  _page_container_t _pages;
  _free_pages_t _free_pages;

  NodeStorageInHeap() {}

  PageRef get_page(pageid_t id) { return PageRef(_pages[id].get(), id, id); }

  PageRef free_page(size_t needed_space);
  void no_reserve(const PageRef &page);
  void with_reserve(const PageRef &page);
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

} // namespace larch_leaves

#endif // _LARCH_LEAVES_STORAGE_HPP
