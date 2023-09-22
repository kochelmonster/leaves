#include "page.hpp"

#include "storage.hpp"
#include "trace.hpp"

namespace leaves {

void Page::grow(uint16_t offset, int delta) {
  if (delta < 0)
    memmove(&data[offset], &data[offset - delta], size - offset + delta);
  else
    memmove(&data[offset + delta], &data[offset], size - offset);

  size += delta;
  node_p *ie = &root - 1;
  for (node_t i = 1; i <= ie_count; i++, ie--) {
    if (ie->offset >= offset && ie->type != kNull) {
      ie->offset += delta;
    }
  }
}

node_t Page::alloc(uint16_t space, NodeType type) {
  node_t result;
  node_p *ie;

  if (ie_free_count > 0) {
    assert(size + space + ie_count * sizeof(node_p) < sizeof(data));
    result = ie_free_head;
    ie = &root - ie_free_head;
    ie_free_head = ie->offset;
    ie_free_count--;
  } else {
    assert(size + space + (ie_count + 1) * sizeof(node_p) < sizeof(data));
    result = ie_count++;
    ie = &root - result;
  }
  ie->type = type;
  ie->offset = size;
  size += space;
  return result;
}

void Page::free(node_t index, uint16_t size) {
  node_p *ie = &root - index;
  grow(ie->offset, -(int)size);
  if (index == ie_count) {
    ie_count--;
  } else {
    ie->offset = ie_free_head;
    ie->type = kNull;
    ie_free_head = index;
    ie_free_count++;
  }
}

bool Page::reserve(int space, uint16_t links) {
  // for page splitting there must at least one Pointer Size left at every page
  int needed = size + space + ie_count * sizeof(node_p) + sizeof(Page *);
  return needed + links * sizeof(node_p) < sizeof(data);
}

node_t Page::find_split_node() {
  node_p *ie = &root - 1;
  node_t second_best = 0;
  uint16_t middle = (sizeof(data) - ie_count * sizeof(node_p)) / 2;

  for (node_t i = 1; i < ie_count; i++, ie--) {
    if (ie->type >= kUpperTrie && ie->offset >= middle) {
      TESTPOINT(PageSplitAtTrie);
      return i;  // the best split is before a trie
    }
    if (!second_best && ie->offset > middle) {
      second_best = i;
    }
  }
  TESTPOINT(PageSplitAtNode);
  return second_best;
}

void Page::split(Storage &storage) {
  node_t nid = find_split_node();
  Page *child = storage.alloc_new_page();

  node_p *pie = get_ie(nid);
  NodeHandler::HANDLERS[pie->type]->copy_node(child, this, nid);
  pie->type = kHeapLink;
  pie->offset = size;
  get_node(pie)->pointer = child;

  Page tmp;
  tmp.init();
  NodeHandler::HANDLERS[root.type]->copy_node(&tmp, this, 0);
  memcpy(this, &tmp, sizeof(tmp));

  TESTPOINT(PageSplit);
}

stored_ptr Page::write_page(Storage &storage) {
  node_p *ie = &root - 1;
  assert(root.type != kNull);
  for (node_t i = 1; i < ie_count; i++, ie--) {
    if (ie->type == kHeapLink) {
      Node *node = get_node(ie);
      node->link.val = node->pointer->write_page(storage).val;
      ie->type = kLink;
    }
    else if (ie->type == kLink) {
      stored_ptr link = get_node(ie)->link;
      if (size < 2560 && reserve(link.size + MIN_SPACE, 10 + MIN_COUNT)) {
        TESTPOINT(PageMerge);
        // The page might be merged but should be split right the merge
        const Page *child = link.get<Page>(storage.view.get());
        if (reserve(child->size + MIN_SPACE,
                    child->ie_count - child->ie_free_count - 1 + MIN_COUNT)) {
          free(i, sizeof(child));
          add(child);
          storage.add_page_to_copied(link);
        }
      }
    }
  }
  return storage.write_page(this);
}

void Page::free_page(Storage &storage) {
  node_p *ie = &root - 1;
  for (node_t i = 1; i < ie_count; i++, ie--) {
    if (ie->type == kHeapLink) {
      Node *node = get_node(ie);
      node->pointer->free_page(storage);
    }
  }
  storage.free(this);
}

}  // namespace leaves