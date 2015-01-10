//@+leo-ver=5-thin
//@+node:michael.20141215222649.161: * @file node.cpp
//@@language cplusplus
//@@tabwidth -2
//@+<< includes >>
//@+node:michael.20141217010530.11: ** << includes >>
#include <memory.h>
#include "node.h"
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.26: ** PageRef
//@+others
//@+node:michael.20141230111914.27: *3* defragment
bool PageRef::defragment(Trace& trace) const {
  // keep in mind: root is never removed, instead thethe page will be removed
  NodePtr* ptrs = node_ptr();
  char* page_start = &page->data[0];
  char* node_start = page_start + ptrs[0].ptr;
  char* free_start = page_start + ptrs[page->node_count-1].ptr;
  NodePtr new_ptrs[256];
  size_t node_count = 1;
  nodeid_t map[256]; // maps old nodeids to new nodeids
  bool nodes_moved = false;
  size_t hole_size = 0;
  bool moved = false;
   
  // closes the holes in the page 
  for(nodeid_t i = 1; i < page->node_count; i++) {
    size_t node_size = get_node_size(i);
    map[i] = node_count;
    
    if (ptrs[i].type & REMOVE_BIT) {
      // a hole

      // holes are often continuous to avoid many costly
      // memmove operations we will do it at thee next item
      hole_size += node_size;
      node_start -= node_size;
      moved = true; // hole_size can be 0 if node_size is 0
                    // ==> hole_size>0 cannot be used as moved indicator
      continue;
    }
    
    if (moved) {
      TESTPOINT(DefragmentNodeMove);
      // node_start points still to the last hole!
      memmove(free_start+hole_size, free_start, node_start-free_start);
      free_start += hole_size;
      node_start += hole_size;
      hole_size = 0;
      moved = false;
      nodes_moved = true;
    }
    
    node_start -= node_size;     
    memcpy(&new_ptrs[node_count], &ptrs[i], sizeof(NodePtr));
    new_ptrs[node_count].ptr = (inpage_ptr)(node_start-page_start);
      
    node_count++;
  }

  // if (do_move)
  //    the hole is at the end, we need not do anything just cut
  //    which is done at the next line
  page->node_count = node_count;
    
  if (!nodes_moved)
    return false;  // not defragmented 

  memcpy(&new_ptrs[0], &ptrs[0], sizeof(NodePtr)); // not done before
  memcpy(node_ptr(), new_ptrs, sizeof(NodePtr)*node_count);

  // map the child ids for each node
  map[0] = 0; // is not done before (and is needed for Trie!)
  for(nodeid_t i = 0; i < node_count; i++) {
    NodeRef node(*this, i);
    nodeid_t children[65];
    size_t child_count = node.get_children(children);
    for(size_t j = 0; j < child_count; j++)
      children[j] = map[children[j]];
    node.replace_children(children);
  }
  
  trace.refresh_trace();
  return true;
}
//@+node:michael.20141230111914.29: *3* new_node
NodeRef PageRef::new_node(size_t size_) const {
  NodePtr *ptrs = node_ptr();
  size_t free_start = count() ? ptrs[count()-1].ptr : sizeof(Page);
  free_start -= size_;
  nodeid_t new_id = page->node_count++;
  ptrs[new_id].ptr = free_start;
  return NodeRef(*this, new_id);
}
//@+node:michael.20141230111914.30: *3* grow_node_by
void PageRef::grow_node_by(nodeid_t node_id, int size) const {
  if (size == 0)
    return;

  NodePtr* ptrs = node_ptr();
  size_t free_start = ptrs[page->node_count-1].ptr;
  size_t node_start = ptrs[node_id].ptr;
  size_t node_size = get_node_size(node_id);
  
  if (size < 0)
    node_size += size;
    
  memmove(&page->data[free_start-size], &page->data[free_start],
          node_start-free_start+node_size);
  
  for(nodeid_t i = node_id; i < page->node_count; i++)
    ptrs[i].ptr -= size;
}
//@+node:michael.20141230111914.31: *3* change_to_link
void PageRef::change_to_link(nodeid_t node_id, pageid_t page_id) const {
  // first remove the old nodes space
  int delta = page_pad(sizeof(pageid_t)) - get_node_size(node_id);
  grow_node_by(node_id, delta);
  
  NodePtr *ptrs = node_ptr();
  ptrs[node_id].extra = 0;
  ptrs[node_id].type = kLink;
  *get_link(node_id) = page_id;
}
//@+node:michael.20150101205559.4: *3* dump
#ifdef DEBUG
void PageRef::dump(std::ostream& out) {
  const char* t1 = "    ";
  const char* t2 = "      ";
  const char* t3 = "          ";
  out << t1 << "- id:         " << id << std::endl
      << t2 << "offset:     " << offset << std::endl
      << t2 << "node_count: " << count() << std::endl
      << t2 << "size:       " << size() << std::endl
      << t2 << "free_size:  " << free_size() << std::endl
      << t2 << "sum_size:   " << size() + free_size() << std::endl
      << t2 << "nodes: " << std::endl;
    
  for(nodeid_t id = 0; id < count(); id++) {
    out << t3 << "- id:    " << (int)id << std::endl
        << t3 << "  ptr:   " << (int)node_ptr()[id].ptr << std::endl;
    NodeRef node(*this, id);
    NodeHandler::handlers[node.type()]->dump(node, out);
  }
}
#endif
//@-others

//@+node:michael.20150110130802.6: ** Trace
//@+node:michael.20150110130802.8: *3* find
void Trace::find(const Slice& key_) {
  size_t pl = common_prefix(key_.data(), key.data(), 
                            std::min(key_.size(), key.size()));
  std::vector<Transition>::iterator i;
  for(i = stack.begin(); i != stack.end(); i++) {
    if (i->end > pl) {
      stack.erase(i+1, stack.end());
      break;
    }
  }
  
  key.assign(key_.data(), key_.size());
  
  Transition &back = stack.back();
  if (back.node.is_leaf()) {
    if (back.end < key.size()) {
      stack.pop_back();
    } else {
      assert(back.end == key.size());
      return; // we are already here
    }
  }
  
  current().find(*this); // not back.node (pop_back!)
}
//@+node:michael.20150110130802.7: *3* reserve_space
#define MAX_PAGE_FREE_SIZE (sizeof(Page)-PAGE_HEADER_SIZE)

void Trace::reserve_space(size_t size_) {
  // during this loop current() can change its page!
  while (current().page.free_size() < size_ 
         || current().page.count() == 255) {
    size_t sizes[256];
    NodeRef root(current().page, 0);
    memset(sizes, 0, sizeof(sizes));
    calc_sizes(root, sizes);
    int best = sizeof(Page);
    nodeid_t best_id = 1;
            
    // i = 1: it makes no sense to move the root node
    for(size_t i = 1; i < root.page.count(); i++) {
      if (MAX_PAGE_FREE_SIZE - sizes[i] < size_) 
        // the new page must also have enough free space
        continue;
    
      int delta = abs((int)sizes[i]-sizeof(Page)/2);
      if (delta < best) {
        best = delta;
        best_id = i;
      }
    }
    
    PageRef newpage = storage.new_page();
    NodeRef to_move(NodeRef(root.page, best_id));

    move_node(newpage, to_move);
    root.page.change_to_link(best_id, newpage.id);
    if (!root.page.defragment(*this))
      refresh_trace();
  }
}
//@+node:michael.20141215222649.155: ** class NodeStorage (Implementation)
//@+others
//@+node:michael.20141215222649.159: *3* class PersistentNodeManager
//@+others
//@-others
//@+node:michael.20141215222649.156: *3* class NodeStorageInHeap
// NodeStorageInHeap implementation
// --------------------------------

PageRef NodeStorageInHeap::new_page() {
  if (_free_pages) {
    std::vector<_page_ptr>::iterator i;
    for(i = _pages.begin(); i != _pages.end(); i++) {
      if (!i->get()) {
        pageid_t id = i - _pages.begin();
        Page* page = new Page;
        i->reset(page);
        _free_pages--;
        return PageRef(page, id, (pageoffset_t)id);
      }
    }
    _free_pages = 0;
  }

  pageid_t id(_pages.size());
  Page* page = new Page;
  _pages.push_back(_page_ptr(page));
  return PageRef(page, id, (pageoffset_t)id);
}

void NodeStorageInHeap::free_page(const PageRef& page) {
  if (page.id == _pages.size()-1) {
    _pages.pop_back(); // remove last page

    // remove all pages at the end
    while(!_pages.empty() && !_pages.back().get()) {
      _pages.pop_back(); 
      _free_pages--;
    }
  }
  else {
    _pages[page.id].reset();
    _free_pages++;
  }
}


//@+node:michael.20141215222649.160: *3* class MultiProcessNodeMemoryManager
//@-others
//@-others
} // namespace larch_leaves 
//@-leo
