#include "node.hpp"
#include <algorithm>
#include <memory.h>

namespace larch_leaves {
// Trace Methods
// -------------
void Trace::find(const Slice &key_) {
  const char *newp = key_.data(), *oldp = key.data();
  size_t key_size = key_.size();
  std::vector<Transition>::iterator i;
  for (i = stack.begin(); i != stack.end(); i++) {
    if (i->end > key_size || memcmp(newp+i->start, oldp+i->start, i->end-i->start)) {
      stack.erase(i + 1, stack.end());
      back = &stack.back();
      break;
    }
  }
  
  key.assign(key_.data(), key_.size());
  if (back->node.is_leaf()) {
    if (back->end < key.size()) {
      // not found
      _pop();
    } else {
      assert(back->end == key.size());
      return; // we are already here
    }
  }
  current().find(*this); // not back.node (pop_back!)
}

struct SecondChoice {
  size_t size;
  Page::ptr candidate;

  SecondChoice() : size(0) {}
};
  

static size_t find_node_to_move(PageRef &page, Page::ptr ptr, size_t needed_size,
                         Page::ptr *best_candidate, SecondChoice *second_choice) {
  Page::ptr *children;
  Node *node = page.node(ptr);
  size_t size_ = page_pad(node->size);
  size_t count = node->children(&children);

  for (size_t i = 0; i < count; i++) {
    if (!children[i])
      continue;

    size_t csize =
        find_node_to_move(page, children[i], needed_size, 
          best_candidate, &second_choice[i]);
    if (*best_candidate)
      return csize;

    if (csize > second_choice->size) {
      second_choice->candidate = children[i];
      second_choice->size = csize;
    }

    size_ += csize;
  }

  if (size_ >= needed_size && node->type != kLink) 
    *best_candidate = ptr;

  return size_;
}

// moves the node to new page returning the ptr on the new_page
// precondition: there must be enough space in new_page
static Page::ptr move_node(PageRef &new_page, NodeRef &node) {
  Page::ptr *src_children, *dst_children;
  size_t count = node.children(&src_children);
  Page::ptr dst_ptr = new_page.new_node(node.size());
  Node *dst_node = new_page.node(dst_ptr);
  memcpy(dst_node, node.node, node.pad_size());
  dst_node->children(&dst_children);

  for (size_t i = 0; i < count; i++) {
    if (src_children[i]) {
      NodeRef child(node.page, src_children[i]);
      dst_children[i] = move_node(new_page, child);
    }
  }

  node.type(kRemoved);
  return dst_ptr;
}


void Trace::reserve_space(size_t size_) {
  // during this loop current() can change its page!
  PageRef &page = current().page;
  size_t free_size = page.free_size();

  if (free_size < size_) {
    size_t needed_size =
        std::max((size_t)size_ - free_size + LINK_SIZE, (size_t)PAGE_RESERVE);

    SecondChoice second_choice[ENTRY_POINTS];
    Page::ptr candidate;
    size_t candidate_size;

    for (Page::entry_t i = 0; i < ENTRY_POINTS; i++) {
      Page::ptr entry = page.page->entry_points[i];
      if (!entry)
        continue;

      entry--;

      candidate = 0;
      candidate_size = find_node_to_move(page, entry, needed_size, 
        &candidate, &second_choice[i]);
      if (candidate && candidate != entry)
        break;

      candidate = 0;
    }

    if (!candidate) {
      candidate_size = 0;
      for (Page::entry_t i = 0; i < ENTRY_POINTS; i++) {
        if (second_choice[i].size > candidate_size) {
          candidate_size = second_choice[i].size;
          candidate = second_choice[i].candidate;
        }
      }
    }
    assert(candidate);

    // + size_ because current() could move to new page than the newpage will grow by size_
    PageRef new_page = storage.free_page(candidate_size + size_);
    NodeRef to_move(page, candidate);
    Page::ptr dst = move_node(new_page, to_move);
    Page::entry_t ientry = new_page.free_entry();
    new_page.page->entry_points[ientry] = dst + 1;
        
    to_move.type(kLink); // defragment should not remove the node
    
    // first defragment because change_to_link can grow the node
    // which can lead to a page overflow

    // hack: use entry[0] (which is always 1) to track changes of candidate
    page.page->entry_points[0] = candidate + 1;
    int refresh = page.defragment(); 
    candidate = page.page->entry_points[0] - 1;
    page.page->entry_points[0] = 1; // restore
    refresh += page.change_to_link(candidate, new_page.id, ientry);

    if (refresh)
      refresh_trace(page.id);

    if (!new_page.has_reserve() || !new_page.has_free_links())
      storage.no_reserve(new_page);
  }
}

// returns true if the complete trie is removed
bool Trace::remove() {
  if (!is_valid())
    throw NoValidPosition();

  bool result = true;
  PageRef last_page(current().page);

  while (size()) {
    NodeRef &me(current());
    if (me.page.id != last_page.id) {
      last_page.defragment();
      if (last_page.has_reserve()) {
        // the page change garanties that a links has been removed
        // => last_page.has_free_links() is True
        assert(last_page.has_free_links());
        storage.with_reserve(last_page);
      }
      last_page = me.page;
    }

    if (me.remove_child(*this)) {
      result = false;
      break;
    }
    me.page.node(me.ptr())->type = kRemoved;
    _pop();
  }

  last_page.defragment();
  if (last_page.has_reserve() && last_page.has_free_links())
    storage.with_reserve(last_page);

  if (!result)
    condense_trie();

  return result;
}

inline bool has_more_than_one_child(NodeRef &rnode) {
  size_t count = rnode.count();
  return count > 2 ||
         (count == 2 && rnode.node->children_[0]); // the last is for trie
}


static void remove_nodes(Trace& trace, size_t start, size_t end) {
  PageRef last_page(trace.stack[start].node.page);
  for(size_t i = start; i < end; i++) {
    NodeRef &rnode(trace.stack[i].node);
    if (rnode.page.id != last_page.id) {
      last_page.defragment();
      if (last_page.has_reserve() && last_page.has_free_links())
        trace.storage.with_reserve(last_page);
    }
    rnode.type(kRemoved);
  }

  last_page.defragment();
  if (last_page.has_reserve() && last_page.has_free_links())
    trace.storage.with_reserve(last_page);
}

static void condense(Trace& trace, size_t start) {
  size_t i;
  for(i = start; i < trace.size(); i++) {
    NodeRef &rnode(trace.stack[i].node);
    if (has_more_than_one_child(rnode))
      break;
  }

  if (i-start < 2)
    return;

  TempNode rest;
  NodeRef& snode(trace.stack[start].node);

  if (i == trace.size()) {
    TESTPOINT(Condense1);
    assert(trace.current().is_leaf());
    rest.to_leaf(trace.current().data());
    trace.current().type(kRemoved);
  }
  else {
    NodeRef& rnode(trace.stack[i].node);
    if (rnode.page.id != snode.page.id) {
      TESTPOINT(Condense2);
      // change the page entry to rnode
      for(size_t j = i-1; j > 0; j--) {
        NodeRef& rlink(trace.stack[j].node);
        if (rlink.type() == kLink) {
          PageLink *link = (PageLink*)rlink.data().data();
          assert(link->page_id == rnode.page.id);
          rnode.page.page->entry_points[link->entry] = rnode.ptr() + 1;
          rest.to_link(link->page_id, link->entry);
          break;
        }
      }
    }
    else {
      if (! rnode.page.has_free_links()) {
        TESTPOINT(Condense3);
        return; // cannot condense
      }

      // keeps the link to the rest
      rest.to_link(rnode.page.id, rnode.page.free_entry());
      TESTPOINT(Condense4);
    }
  }

  // for condensation an add is simulated
  // --------------------------------------

  trace.key.resize(std::min(trace.stack[i-1].end, trace.key.size()));
  remove_nodes(trace, start, i);
  trace.stack.resize(start);
  trace.back = &trace.stack.back();

  assert(trace.current().type() == kTrie || trace.current().type() == kBitTrie);
  trace.current().remove_child(trace);

  // Now add
  trace.add(rest);
}


void Trace::condense_trie() {
  // returns the stack index of a node that can eat its children
  cut_key();
  current().first(*this);

  // i == 1 because root will always be a trie
  for(size_t i = 1; i < size(); i++) {
    NodeRef &rnode(stack[i].node);
    if (! has_more_than_one_child(rnode)) {
        condense(*this, i);
    }
  }
}

} // namespace larch_leaves
