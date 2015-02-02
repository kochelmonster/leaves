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
//@+node:michael.20150112164548.8: ** Page
// Page Methods
// ------------
//@+others
//@+node:michael.20141230111914.29: *3* new_node
nodeid_t Page::new_node(size_t size_) {
  NodePtr *ptrs = node_ptr;
  size_t free_start = count ? ptrs[count-1].offset : sizeof(Page);
  free_start -= size_;
  nodeid_t new_id = count++;
  ptrs[new_id].offset = (inpage_ptr)free_start;
  return new_id;
}
//@-others
//@+node:michael.20141230111914.26: ** PageRef
// PageRef Methods
// ---------------
//@+others
//@+node:michael.20141230111914.27: *3* defragment
bool PageRef::defragment(Trace& trace) const {
  // keep in mind: root is never removed, instead thethe page will be removed
  NodePtr* ptrs = page->node_ptr;
  size_t node_count = 1, old_count = count();
  char* page_start = &page->data[0];
  char* node_start = page_start + ptrs[0].offset;
  char* free_start = page_start + ptrs[count()-1].offset;
  NodePtr new_ptrs[MAX_NODE_COUNT];
  nodeid_t map[MAX_NODE_COUNT]; // maps old nodeids to new nodeids
  bool nodes_moved = false;
  size_t hole_size = 0;
  bool moved = false;
   
  // closes the holes in the page 
  for(size_t i = 1; i < old_count; i++) {
    size_t node_size = get_node_size((nodeid_t)i);
    map[i] = (nodeid_t)node_count;
    
    if (ptrs[i].type == kRemoved) {
      // a hole

      // holes are often continuous to avoid many costly
      // memmove operations we will do it at the next item
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
    new_ptrs[node_count].offset = (inpage_ptr)(node_start-page_start);
      
    node_count++;
  }

  // if (do_move)
  //    the hole is at the end, we need not do anything just cut
  //    which is done at the next line
  page->count = (boost::uint16_t)node_count;
    
  if (!nodes_moved)
    return false;  // not defragmented 

  // ptrs[0] ist still the same
  memcpy(&ptrs[1], &new_ptrs[1], sizeof(NodePtr)*(node_count-1));

  // map the child ids for each node
  map[0] = 0; // is not done before (and is needed for Trie!)
  for(size_t i = 0; i < node_count; i++, ptrs++) {
    NodeHandler *handler = NodeHandler::handlers[ptrs->type];
    Node* node = (Node*)&page->data[ptrs->offset];
    nodeid_t children[65];
    size_t child_count = handler->get_children(ptrs, node, children);
    for(size_t j = 0; j < child_count; j++)
      children[j] = map[children[j]];
    handler->replace_children(ptrs, node, children);
  }
  
  trace.refresh_trace();
  return true;
}
//@+node:michael.20141230111914.30: *3* grow_node_by
void PageRef::grow_node_by(nodeid_t node_id, int size) const {
  if (size == 0)
    return;

  NodePtr* ptrs = page->node_ptr;
  size_t count_ = count();
  size_t free_start = ptrs[count_-1].offset;
  size_t node_start = ptrs[node_id].offset;
  size_t node_size = get_node_size(node_id);
  
  if (size < 0)
    node_size += size;
    
  memmove(&page->data[free_start-size], &page->data[free_start],
          node_start-free_start+node_size);
  
  for(size_t i = node_id; i < count_; i++)
    ptrs[i].offset -= size;
}
//@+node:michael.20141230111914.31: *3* change_to_link
void PageRef::change_to_link(nodeid_t node_id, pageid_t page_id) const {
  // first remove the old nodes space
  int delta = (int)(page_pad(sizeof(pageid_t)) - get_node_size(node_id));
  assert(delta <= 0);
  grow_node_by(node_id, delta);
  
  NodePtr *ptr = page->node_ptr + node_id;
  ptr->extra = 0;
  ptr->type = kLink;
  *((pageid_t*)&page->data[ptr->offset]) = page_id;
}
//@-others

//@+node:michael.20150110130802.6: ** Trace
// Trace Methods
// -------------
//@+others
//@+node:michael.20150110130802.8: *3* find
void Trace::find(const Slice& key_) {
  size_t pl = common_prefix(key_.data(), key.data(), 
                            std::min(key_.size(), key.size()));
  std::vector<Transition>::iterator i;
  for(i = stack.begin(); i != stack.end(); i++) {
    if (i->end > pl) {
      stack.erase(i+1, stack.end());
      back = &stack.back();
      break;
    }
  }
  
  key.assign(key_.data(), key_.size());
  if (back->node.is_leaf()) {
    if (back->end < key.size()) {
      _pop();
    } else {
      assert(back->end == key.size());
      return; // we are already here
    }
  }
  
  current().find(*this); // not back.node (pop_back!)
}
//@+node:michael.20150110130802.7: *3* reserve_space
#define MAX_PAGE_FREE_SIZE (sizeof(Page)-PAGE_HEADER_SIZE)

struct BestFitting {
  size_t best;
  size_t sizes[MAX_NODE_COUNT];
  size_t min_size;
  nodeid_t best_id;
  bool found;
    
  BestFitting(Page* page, size_t min_size_) 
    : best(PAGE_SIZE), min_size(min_size_), best_id(1), found(false) {
      page->init_sizes(sizes);
    }
};


size_t calc_sizes(const NodeRef& node, BestFitting& bf) {
  if (bf.found)
    return 0;

  nodeid_t children[65];
  size_t size = bf.sizes[node.id];
  size_t count = node.get_children(children);
  
  for(size_t i = 0; i < count; i++) {
    size += calc_sizes(NodeRef(node.page, children[i]), bf);
    if (bf.found)
      return 0;
  }
    
  if (size >= bf.min_size && size + bf.min_size <= MAX_PAGE_FREE_SIZE) {
    size_t delta;
    if (size > PAGE_SPLIT_SIZE) {
      delta = size - PAGE_SPLIT_SIZE;
      delta += delta / 2;
    }
    else {
      delta = PAGE_SPLIT_SIZE - size;
    }
    if (delta < bf.best) {
      bf.best = delta;
      bf.best_id = node.id;
      if (bf.best < 512)
        bf.found = true;
    }
  }

  return size;
}

void Trace::reserve_space(size_t size_) {
  // during this loop current() can change its page!
  while (current().page.free_size() < size_) {
    NodeRef root(current().page, 0);    
    BestFitting bf(root.page.page, size_);
    //try {
      calc_sizes(root, bf);
    //}
    //catch(...) {}

    PageRef newpage = storage.new_page();
    NodeRef to_move(root.page, bf.best_id);
    
    move_node(newpage, to_move);
    root.page.change_to_link(bf.best_id, newpage.id);
    if (!root.page.defragment(*this))
      refresh_trace();
  }
}
//@+node:michael.20150111191610.4: *3* move_node
nodeid_t Trace::move_node(const PageRef& new_page, const NodeRef& node) {
  if (new_page.id == node.page.id)
    return node.id;
    
  nodeid_t children[65];
  size_t count = node.get_children(children);
  
  NodeRef new_node(new_page, new_page.new_node(node.size()));
  copy_node(new_node, node);
      
  for(size_t i = 0; i < count; i++) {
    NodeRef child(node.page, children[i]);
    children[i] = move_node(new_page, child);
  }
  
  new_node.replace_children(children);
  node.page.free_node(node.id);
  return new_node.id;
}
//@+node:michael.20150110130802.26: *3* merge_pages
void Trace::merge_pages() {
  PageRef page(current().page);
  
  // this is never called by the trace root
  // => stack.size() >= 2
  
  for(int i = (int)stack.size()-2; i >= 0; i--) {
    NodeRef& link(stack[i].node);
    
    if (link.page.id != page.id) {
      assert(link.type() == kLink);
      
      if (link.page.free_size() >= page.size()) {
        TESTPOINT(MergePages);
        
        NodeRef& parent(stack[i-1].node); // the child's parent
        NodeRef page_root(page, 0);
        
        nodeid_t child_id = move_node(parent.page, page_root);
                  
        // replace the link with the child
        nodeid_t children[65];
        size_t count = parent.get_children(children);
        for(size_t i = 0; i < count; i++) {
          if (children[i] == link.id) {
            children[i] = child_id;
            break;
          }
        }
        parent.replace_children(children);
                  
        link.page.free_node(link.id);
        if (!link.page.defragment(*this))
          refresh_trace();
          
        map.free_page(page);
      }
      return;
    }
  }
}
//@-others
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
        pageid_t id = (pageid_t)(i - _pages.begin());
        Page* page = new Page;
        i->reset(page);
        _free_pages--;
        return PageRef(page, id, (pageoffset_t)id);
      }
    }
    _free_pages = 0;
  }

  pageid_t id((pageid_t)_pages.size());
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
