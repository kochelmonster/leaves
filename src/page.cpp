#include "node.hpp"

namespace larch_leaves {

static void check_node(Page* page, Page::ptr ptr, size_t depth) {
  Page::ptr *children;
  Node *node = page->node(ptr);
  size_t count = node->children(&children);

  assert(depth < PAGE_SIZE/ALIGN);
  assert(node->type != kRemoved);

  for (size_t i = 0; i < count; i++) {
    if (!children[i])
      continue;

    assert(children[i] < page->next_node);
    check_node(page, children[i], depth+1);
  }
}


void Page::check() {
  assert(next_node <= sizeof(data) / ALIGN);
  for (Page::entry_t i = 0; i < ENTRY_POINTS; i++) {
    Page::ptr entry = entry_points[i];
    if (!entry)
      break;

    check_node(this, entry - 1, 0);
  }

  ptr j = 1;
  for (ptr i = 0; i < next_node; i += node(i)->ptr_size()) {
    assert(i != j);
    assert(0 <= node(i)->type && node(i)->type < kRemoved);
    j = i;
  }
}


Page::ptr Page::new_node(size_t size_) {
  size_t node_size = page_pad(size_);
  ptr pointer = next_node;
  Node *node_ = node(pointer);
  memset(node_, 0, node_size);
  next_node += (Page::ptr)(node_size / ALIGN);
  assert(next_node <= sizeof(data) / ALIGN);
  node_->size = size_;
  node_->type = kRemoved;
  return pointer;
}

size_t Page::count() const {
  size_t count = 0;
  for (ptr i = 0; i < next_node; i += node(i)->ptr_size()) {
    count++;
  }
  return count;
}

void Page::adjust_pointers(Page::ptr old_ptr, int delta) {
  Node *node_;
  next_node += delta;
  assert(next_node <= sizeof(data) / ALIGN);

  for (ptr i = 0; i < next_node; i += node_->ptr_size()) {
    node_ = node(i);
    node_->adjust_children(old_ptr, delta);
  }

  for (entry_t i = 0; i < ENTRY_POINTS; i++) {
    if (entry_points[i] > old_ptr) // entry_points is one based => ">" not ">="
      entry_points[i] += delta;
  }
}

bool Page::defragment() {
  bool result = false;
  ptr hole_start = 0, hole_end = 0, next_i = 0;

  for (ptr i = 0; i < next_node; i = next_i) {
    Node *node_ = node(i);
    ptr ptr_size = node_->ptr_size();
    next_i = i + ptr_size;

    if (node_->type == kRemoved) {
      result = true;
      hole_end = next_i;
      for (entry_t j = 0; j < ENTRY_POINTS; j++) {
        if (entry_points[j] == i + 1) {
          entry_points[j] = 0;
          break;
        }
      }
      continue;
    }

    if (hole_start < hole_end) {
      TESTPOINT(DefragmentNodeMove);
      memmove(node(hole_start), node(hole_end), (next_node - hole_end) * ALIGN);
      adjust_pointers(hole_start, -(int)(hole_end - hole_start));
      i = next_i = hole_start;
      next_i += ptr_size;
      hole_end = 0;
    }

    hole_start = next_i;
  }

  next_node = hole_start; // if there were some holes at the end of page
  assert(next_node <= sizeof(data) / ALIGN);
  return result;
}

int Page::grow_node_by(Page::ptr ptr_, int delta) {
  Node *node_ = node(ptr_);

  size_t node_size = node_->size + delta;
  char *src = ((char *)node_) + page_pad(node_->size);
  char *dst = ((char *)node_) + page_pad(node_size);
  if (src == dst) {
    TESTPOINT(grow0);
    node_->size = node_size;
    return 0;
  }

  char *end = (char *)node(next_node);
  if (src == end) {
    TESTPOINT(grow1);
    node_->size = node_size;
    // growing the last node needs just moving the next_node pointer
    next_node = ptr_ + node_->ptr_size();
    assert(next_node <= sizeof(data) / ALIGN);
    return 0;
  }

  TESTPOINT(grow2);
  boost::uint64_t value = node_->start_grow();
  delta = (int)((dst - src) / ALIGN);
  memmove(dst, src, end - src);
  node_->size = node_size;
  node_->end_grow(value);

  adjust_pointers((ptr)((src - data) / ALIGN), delta);
  return delta;
}

bool PageRef::change_to_link(Page::ptr node_ptr, pageid_t page_id,
                             Page::entry_t entry) {
  PageLink link;
  Node *node_ = node(node_ptr);
  bool result = grow_node_by(
    node_ptr, (int)sizeof(link) + sizeof(Node::header_t) - node_->size) != 0;
  link.page_id = page_id;
  link.entry = entry;
  node_->type = kLink;
  node_->data(&link);
  return result;
}

#ifdef DEBUG
void PageRef::dump(std::ostream &out) {
  const char *t1 = "    ";
  const char *t2 = "      ";
  const char *t3 = "          ";
  out << t1 << "- id:         " << id << std::endl
      << t2 << "offset:     " << offset << std::endl
      << t2 << "node_count: " << page->count() << std::endl
      << t2 << "size:       " << size() << std::endl
      << t2 << "free_size:  " << free_size() << std::endl
      << t2 << "sum_size:   " << size() + free_size() << std::endl
      << t2 << "nodes: " << std::endl;

  Node *node_;
  for (Page::ptr i = 0; i < page->next_node; i += node_->ptr_size()) {
    node_ = node(i);
    out << t3 << "- ptr:   " << i << std::endl;
    NodeHandler::handlers[node_->type]->dump(page, node_, out);
  }
}
#endif
} // namespace larch_leaves
