#include "storage.hpp"

namespace larch_leaves {

PageRef NodeStorageInHeap::free_page(size_t needed_space) {
  for (_free_pages_t::iterator i = _free_pages.begin(); i != _free_pages.end();
       i++) {
    Page *page(_pages[*i].get());
    if (!page) {
      page = new Page;
      _pages[*i].reset(page);
    }

    if (page->free_size() >= needed_space)
      return PageRef(page, *i, *i);
  }

  pageid_t id((pageid_t)_pages.size());
  Page *page = new Page;
  _pages.push_back(_page_ptr(page));
  _free_pages.insert(id);
  return PageRef(page, id, (pageoffset_t)id);
}

void NodeStorageInHeap::no_reserve(const PageRef &page) {
  _free_pages.erase(page.id);
}

void NodeStorageInHeap::with_reserve(const PageRef &page) {
  if (page.page->next_node == 0)
    _pages[page.id].reset();

  _free_pages.insert(page.id);

  if (page.id == _pages.size() - 1) {
    while (!_pages.back()) {
      _pages.pop_back();
      _free_pages.erase((pageid_t)_pages.size());
    }
  }
}

} // namespace larch_leaves
