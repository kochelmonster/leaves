//@+leo-ver=4-thin
//@+node:michael.20141215222649.161:@shadow memory.cpp
//@@language cplusplus
//@@tabwidth -4
//@<< includes >>
//@+node:michael.20141217010530.11:<< includes >>
#include "memory.h"
//@nonl
//@-node:michael.20141217010530.11:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.155:class NodeMemoryManager (Implementation)
//@+others
//@+node:michael.20141215222649.156:class HeapNodeManager
HeapNodeMemoryManager::HeapNodeMemoryManager(): _free_pages(0) {


PageRef HeapNodeMemoryManager::get_read_page(pageid_t pageid) {
  return get_page(pageid);
}
  
PageRef HeapNodeMemoryManager::get_write_page(pageid_t pageid) {
  return get_page(pageid);
}
 
PageRef HeapNodeMemoryManager::new_page() {
  if (_free_pages) {
    std::vector<_page_ptr>::iterator i;
    for(i = _pages.begin(); i != _pages.end(); i++) {
      if (!(*i).get()) {
        *i = new Page;
        _free_pages--;
        return PageRef((*i).get(), pageid, std::shared_ptr<MemorySegment>());
    }
    _free_pages = 0;
  }
  
  Page* page = new Page;
  _pages.push_back(page);
  return PageRef((*i).get(), pageid, std::shared_ptr<MemorySegment>());
}

void HeapNodeMemoryManager::free_page(pageid_t pageid) {
  if (pageid == _pages.size()-1) {
    _pages.pop_back(); // remove last page
    // remove all pages at the end
    
    while(!_pages.empty() && !_pages-back().get()) {
      _pages.pop_back(); 
      _free_pages--;
    }
  }
  else {
    _pages[pageid].reset();
    _free_pages++;
  }
}

//@-node:michael.20141215222649.156:class HeapNodeManager
//@+node:michael.20141215222649.168:class PageMap
class PageMap {
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


//@-node:michael.20141215222649.168:class PageMap
//@+node:michael.20141215222649.159:class PersistentNodeManager
//@+others
//@-others
//@nonl
//@-node:michael.20141215222649.159:class PersistentNodeManager
//@+node:michael.20141215222649.169:class MemorySegment
// a memory segment of a file

typedef boost::interprocess::mapped_region MemorySegment;
  
//@-node:michael.20141215222649.169:class MemorySegment
//@+node:michael.20141215222649.160:class MultiProcessNodeMemoryManager
//@-node:michael.20141215222649.160:class MultiProcessNodeMemoryManager
//@-others
//@-node:michael.20141215222649.155:class NodeMemoryManager (Implementation)
//@-others
} // namespace larch_leaves 
//@-node:michael.20141215222649.161:@shadow memory.cpp
//@-leo
