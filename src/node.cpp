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
void PageRef::defragment() {
    NodePtr new_ptrs[256]; 
    bsize_t node_count = 0;
    nodeid_t map[256]; // maps old nodeid_s to new node_ids
    size_t size[256]; // is needed later

    // create node map 
    NodePtr *ptrs = node_ptr;
    for(nodeid_t i = 0; i < page->node_count; i++) {
      map[i] = node_count;
      if (ptrs[i].type & REMOVE_BIT)
        continue;
        
      size[node_count] = get_node_size(i);
      memcpy(&new_ptrs[node_count++], &ptrs[i], sizeof(NodePtr));
    }
    if (node_count == page->node_count)
      return; // not defragmented
    
    // move nodes
    pageid_t *links = page->links;
    pageid_t new_links[256];
    bsize_t link_count = 0;
    size_t node_start = new_ptrs[0].ptr*16;

    for(nodeid_t id = 1; id < node_count; id++) {
      switch(new_ptrs[id].type) {
        case kLink:
          new_links[link_count] = links[new_ptrs[id].ptr];
          new_ptrs[id].ptr = link_count++;
          break;
          
        default:
          node_start -= size[id];
          memmove(&page->data[new_ptrs[id].ptr*16],
                  &page->data[node_start], size[id]);
      }
    }
      
    // update page administration
    page->free_start = (sizeof(page->data)-node_start)/16;
    page->link_count = link_count;
    page->node_count = node_count;
    update_node_ptr();
    memcpy(links, new_links, sizeof(pageid_t)*link_count);
    memcpy(node_ptr, new_ptrs, sizeof(NodePtr)*node_count);
      
    // map the child ids for each node
    for(nodeid_t i = 0; i < node_count; i++) {
      NodeRef node(*this, i);
      nodeid_t children[65];
      size_t child_count;
      child_count = node.get_children(children);
      for(size_t j = 0; j < child_count; j++)
        children[j] = map[children[j]];
      node.replace_children(children);
    }
  }
//@+node:michael.20141230111914.29: *3* new_node
NodeRef PageRef::new_node(size_t size) const {
    size_t free_start = sizeof(page->data)-((size_t)page->free_start)*16;
    free_start -= size;
    page->free_start = (sizeof(page->data)-free_start)/16;
    nodeid_t new_id = page->node_count++;
    NodePtr *node = node_ptr+new_id;
    node->ptr = free_start/16;
    return NodeRef(*this, new_id);
  }
//@+node:michael.20141230111914.30: *3* grow_node
void PageRef::grow_node(nodeid_t id, int size) {
    NodePtr* ptrs = node_ptr;
    size_t free_start = sizeof(page->data)-((size_t)page->free_start)*16;
    size_t node_start = ((size_t)ptrs[id].ptr) * 16;
    size_t node_size = get_node_size(id);
    
    if (size < 0)
      node_size += size;
      
    memmove(&page->data[free_start-size], &page->data[free_start],
            node_start-free_start+node_size);
    
    free_start += size;
    page->free_start = (sizeof(page->data)-free_start)/16;
    
    size /= 16;
    for(nodeid_t i = id; i < page->node_count; i++)
      ptrs[i].ptr -= size;
  }
//@+node:michael.20141230111914.31: *3* create_link
void PageRef::create_link(nodeid_t node_id, pageid_t page_id) const {
    NodePtr* nodes = node_ptr;
    nodes[node_id].ptr = page->link_count;
    nodes[node_id].type = kLink;
    
    char* p = (char*)nodes;
    memmove(p+sizeof(pageid_t), p, sizeof(NodePtr)*page->node_count);
    page->links[page->link_count++] = page_id;
    
    update_node_ptr();
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
        << t2 << "link_count: " << (int)page->link_count << std::endl
        << t2 << "size:       " << size() << std::endl
        << t2 << "free_size:  " << free_size() << std::endl
        << t2 << "sum_size:   " << size() + free_size() << std::endl
        << t2 << "nodes: " << std::endl;
      
    for(nodeid_t id = 0; id < count(); id++) {
      out << t3 << "- id:    " << (int)id << std::endl
          << t3 << "  ptr:   " << (int)node_ptr[id].ptr << std::endl;
      NodeRef node(*this, id);
      NodeHandler::handlers[node.type()]->dump(node, out);
    }
  }
#endif
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
