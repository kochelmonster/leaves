#include "page.hpp"
#include "storage.hpp"
#include "trace.hpp"

namespace leaves {


uint16_t WritablePage::alloc(size_t size) {
  assert(end.offset + size <= sizeof(content) + sizeof(overflow));
  uint16_t result = end.offset;
  end.offset += size;
  return result;
}

void WritablePage::scale_node(size_t start, int delta)  {
  if (!delta) {
    return;
  }

  node_p root = node_p::b(0, end.type);
  if (delta > 0) {
    size_t move_size = end.offset - start;
    alloc(delta);
    memmove(&content[start+delta], &content[start], move_size);
    if (!start)
      root.offset = delta;

    adjust_pointers(root, start, delta);
  }
  else {
    end.offset += delta;
    memmove(&content[start], &content[start-delta], end.offset-start);
    if (end.offset)
      adjust_pointers(root, start+1, delta); // we may not adjust pointer <= start 
  }
}

bool WritablePage::split(Storage& storage) {
  if (end.offset > sizeof(content) || too_small && free() < SPLIT_SIZE) {
    SplitCandidate candidate;
    node_p root_ = node_p::b(0, root.type);
    find_split_link(root_, candidate);
    location_p new_page_loc = storage.alloc();
    WritablePage* new_page = storage.get_writable(new_page_loc);
    new_page->root.type = new_page_loc.type = candidate._link->type;
    move_node(candidate._link, new_page);
       
    // Replace original node with a link
    *candidate._link = alloc(sizeof(Link), kLink);
    Link& link_ = node(*candidate._link)->link;
    link_.loc = new_page_loc;
    assert(end.offset <= sizeof(content));
    if (end.offset > sizeof(content)) {
      *(char*)NULL = 1;
    }
    return true;
  }
  return false;
}

bool WritablePage::merge(Storage& storage) {
  uint16_t end_ = end.offset;
  if (backed_end != end.offset && free() > SPLIT_SIZE) {
    node_p root_ = node_p::b(0, root.type);
    merge_node(storage, root_);
  }
  return end_ != end.offset;
}



} // namespace leaves