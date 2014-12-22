//@+leo-ver=4-thin
//@+node:michael.20141215222649.161:@shadow node.cpp
//@@language cplusplus
//@@tabwidth -2
//@<< includes >>
//@+node:michael.20141217010530.11:<< includes >>
#include "memory.h"
//@nonl
//@-node:michael.20141217010530.11:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.155:class NodeStorage (Implementation)
//@+others
//@+node:michael.20141215222649.159:class PersistentNodeManager
//@+others
//@+node:michael.20141215222649.168:class PageMap
class PersistantPageMap {
  virtual pageoffset_t get_offset(pageid_t pageid) { return 0 };
};


//@-node:michael.20141215222649.168:class PageMap
//@-others
//@nonl
//@-node:michael.20141215222649.159:class PersistentNodeManager
//@+node:michael.20141215222649.156:class NodeStorageInHeap
// NodeStorageInHeap implementation
// --------------------------------

PageRef NodeStorageInHeap::new_page() {
  if (_free_pages) {
    std::vector<_page_ptr>::iterator i;
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
    
    while(!_pages.empty() && !_pages-back().get()) {
      _pages.pop_back(); 
      _free_pages--;
    }
  }
  else {
    _pages[id].reset();
    _free_pages++;
  }
}


//@-node:michael.20141215222649.156:class NodeStorageInHeap
//@+node:michael.20141215222649.160:class MultiProcessNodeMemoryManager
//@-node:michael.20141215222649.160:class MultiProcessNodeMemoryManager
//@-others
//@-node:michael.20141215222649.155:class NodeStorage (Implementation)
//@-others
} // namespace larch_leaves 
//@-node:michael.20141215222649.161:@shadow node.cpp
//@-leo
