#include "page.hpp"
#include "storage.hpp"
#include "trace.hpp"

namespace leaves {

node_p node_p::b(uint16_t offset_, uint16_t type_) {
  node_p result;
  result.offset = offset_;
  result.type = type_;
  return result;
}

node_p node_p::null = node_p::b(0, 0);

location_p location_p::b(uint64_t page_) {
  location_p result;
  result.page = page_;
  result.node = node_p::b();
  return result;
}

location_p location_p::b(uint64_t page_, node_p node_) {
  location_p result;
  result.page = page_;
  result.node = node_;
  return result;
}

location_p location_p::b(uint64_t page_, uint16_t offset, uint16_t type) {
  location_p result;
  result.page = page_;
  result.offset = offset;
  result.type = type;
  return result;
}

namespace bit {
char upper(char value) { return value >> 4; }

char lower(char value) { return (value & 0x0F); }
}  // namespace bit

struct LinkHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
  static node_p create(Trace &trace, WritablePage *page, location_p loc);
};

struct LeafHandler : public NodeHandler {
  bool valid(const Trace &trace) const { return trace.rest_key.empty(); }
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta) {}
};

struct EndLeafHandler : public LeafHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void change_value(Trace &trace, const Slice &value);
  void insert_compressed(Trace &trace, const Slice &value);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
  static node_p create(Trace &trace, WritablePage *page, const Slice &value);
};

struct MiddleLeafHandler : public LeafHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
};

struct CompressedHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void split_at_middle(Trace &trace, Location &last, size_t pos,
                       const Slice &value);
  void split_at_middle_leaf(Trace &trace, Location &last, size_t pos,
                            const Slice &value);
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
  static node_p create(Trace &trace, WritablePage *page, const Slice &key,
                       const Slice &value);
};

struct NullHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
};

struct TrieHandler : public NodeHandler {
  virtual char bits(char value) const = 0;

  bool find(Trace &trace);
  void adjust_pointers(WritablePage *page, node_p npos, size_t start,
                       int delta);
  node_p merge_node(Storage &storage, WritablePage *page, node_p npos);
  node_p move_node(WritablePage *src, node_p *npos, WritablePage *dest);
  size_t find_split_link(Page *page, node_p npos, SplitCandidate& candidate);
  node_p *add(Trace &trace, Location &last);
  static void create(Trace &trace, WritablePage *page, node_p pos,
                     char node_key, node_p child, const Slice &key,
                     const Slice &value);
  static size_t calc_size(char key1, char key2) {
    if (bit::upper(key1) == bit::upper(key2)) {
      return 2 * sizeof(Trie) + 3 * sizeof(node_p);
    }
    return 3 * sizeof(Trie) + 4 * sizeof(node_p);
  }
};

struct UpperTrieHandler : public TrieHandler {
  char bits(char value) const;
  void insert(Trace &trace, const Slice &value);
};

struct LowerTrieHandler : public TrieHandler {
  char bits(char value) const;
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  static node_p create(Trace &trace, WritablePage *page, const Slice &value);
};

bool LinkHandler::find(Trace &trace) {
  Location last = trace.back();
  trace.stack.push_back(last.next(&last.node->link.loc));
  return true;
}

void LinkHandler::insert(Trace &trace, const Slice &value) { assert(0); }

void LinkHandler::adjust_pointers(WritablePage *page, node_p npos, size_t start,
                                  int delta) {}

node_p LinkHandler::merge_node(Storage &storage, WritablePage *page,
                               node_p npos) {
  Link &link = page->node(npos)->link;
  const Page *csrc = storage.get_newest(link.loc.page);

  if (link.loc.type == kEndLeaf && csrc->node(0)->endleaf.size > BIG_VALUE) {
    return npos;
  }

  if (csrc->end.offset < page->free()) {
    location_p old = link.loc;
    npos.type = link.loc.type;
    WritablePage *src = storage.get_writable(old);
    page->scale_node(npos.offset, -(int)sizeof(Link));  // remove the link
    node_p root = node_p::b(0, npos.type);
    npos = src->move_node(&root, page);
    storage.free(old, PAGE_SIZE);
  }

  return npos;
}

node_p LinkHandler::move_node(WritablePage *src, node_p *npos,
                              WritablePage *dest) {
  node_p result = dest->alloc(sizeof(Link), kLink);
  memcpy(dest->node(result), src->node(*npos), sizeof(Link));
  src->scale_node(npos->offset, -(int)sizeof(Link));
  return result;
}

size_t LinkHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  return sizeof(Link);
}

node_p LinkHandler::create(Trace &trace, WritablePage *page, location_p pos) {
  node_p result = page->alloc(sizeof(Link), kLink);
  Node *node = page->node(result);
  node->link.loc = pos;
  return result;
}

LinkHandler linkHandler;

bool EndLeafHandler::find(Trace &trace) { return false; }

void EndLeafHandler::insert(Trace &trace, const Slice &value) {
  if (trace.rest_key.empty())
    change_value(trace, value);
  else
    insert_compressed(trace, value);
}

void EndLeafHandler::insert_compressed(Trace &trace, const Slice &value) {
  location_p *pnode = &trace.stack.back().pnode;
  WritablePage *page = trace.storage.get_writable(pnode->page);
  size_t space = sizeof(MiddleLeaf);
  uint16_t offset = pnode->offset;

  page->scale_node(offset, space);

  MiddleLeaf &leaf = page->node(offset)->middleleaf;
  leaf.leaf = node_p::b(offset + space, pnode->type);
  leaf.child = CompressedHandler::create(trace, page, trace.rest_key, value);
  trace.change_link(offset, kMiddleLeaf);
}

void EndLeafHandler::change_value(Trace &trace, const Slice &value) {
  // change the value
  Location last = trace.back();
  EndLeaf &leaf = last.node->endleaf;
  size_t leaf_struct_size = sizeof(EndLeaf) + leaf.size;
  size_t new_struct_size = sizeof(EndLeaf) + value.size();

  if (leaf_struct_size > PAGE_SIZE) {
    // Big Page
    if (PAGE_ROUND_UP(leaf_struct_size) == PAGE_ROUND_UP(new_struct_size)) {
      // the page can be reused
      trace.storage.write_value(last.loc, value);
      return;
    }

    // free the leaf page
    trace.storage.free(last.loc, leaf_struct_size);

    // move back in stack
    trace.stack.pop_back();
    last = trace.back();

    // the node must be a link!
    assert(trace.stack.back().plink.type == kLink);

    leaf_struct_size = sizeof(Link);
  }

  // we are in an ordinary page
  int delta = new_struct_size - leaf_struct_size;
  if (delta < 0 || (int)last.wpage->free() > delta) {
    EndLeaf &leaf = last.node->endleaf;
    // the new value fits in the same page
    last.wpage->scale_node(last.loc.node.offset, delta);
    trace.change_link(last.loc.node.offset, kEndLeaf);
    leaf.size = value.size();
    memcpy(&leaf.data[0], value.data(), leaf.size);
    return;
  }

  // We have to insert a link and write the value at a new page.
  last.wpage->scale_node(last.loc.node.offset, sizeof(Link) - leaf_struct_size);
  Link &link = last.node->link;
  trace.change_link(last.loc.node.offset, kLink);
  link.loc = trace.storage.alloc(new_struct_size);
  link.loc.type = kEndLeaf;
  trace.storage.write_value(link.loc, value);
}

node_p EndLeafHandler::create(Trace &trace, WritablePage *page,
                              const Slice &value) {
  node_p result;
  Node *node;
  /* node_size must be at least the size of link, to be able to extend the leaf,
     on a full page*/
  size_t node_size = sizeof(EndLeaf) + value.size();

  if (page->free() < node_size) {
    page->too_small = true;
    size_t complete_page_size = node_size + sizeof(page->end);
    location_p ploc = trace.storage.alloc(complete_page_size);

    ploc.type = kEndLeaf;
    result = LinkHandler::create(trace, page, ploc);
    if (value.size() > BIG_VALUE) {
      trace.storage.write_value(ploc, value);
      return result;
    }
    WritablePage *dest = trace.storage.get_writable(ploc);
    node = dest->node(dest->alloc(node_size, kEndLeaf).offset);
  } else {
    result = page->alloc(node_size, kEndLeaf);
    node = page->node(result);
  }

  node->endleaf.size = value.size();
  memcpy(&node->endleaf.data[0], value.data(), value.size());
  return result;
}

node_p EndLeafHandler::merge_node(Storage &storage, WritablePage *page,
                                  node_p npos) {
  return npos;
}

node_p EndLeafHandler::move_node(WritablePage *src, node_p *npos,
                                 WritablePage *dest) {
  EndLeaf &sleaf = src->node(*npos)->endleaf;
  size_t node_size = sleaf.struct_size();
  node_p result = dest->alloc(node_size, kEndLeaf);
  EndLeaf &dleaf = dest->node(result)->endleaf;
  memcpy(&dleaf, &sleaf, node_size);
  uint16_t offset = npos->offset;
  *npos = node_p::null;  // stop adjust_pointer
  src->scale_node(offset, -node_size);
  return result;
}

size_t EndLeafHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  return page->node(npos)->endleaf.struct_size();
}

EndLeafHandler endLeafHandler;

bool MiddleLeafHandler::find(Trace &trace) {
  Location last = trace.back();
  if (trace.rest_key.empty()) {
    trace.push_back(last.next(&last.node->middleleaf.leaf));
  } else {
    trace.push_back(last.next(&last.node->middleleaf.child));
  }
  return true;
}

void MiddleLeafHandler::insert(Trace &trace, const Slice &value) {
  assert(0);  // may never happen
}

void MiddleLeafHandler::adjust_pointers(WritablePage *page, node_p npos,
                                        size_t start, int delta) {
  MiddleLeaf &node = page->node(npos)->middleleaf;
  if (node.child.offset >= start) {
    node.child.offset += delta;
  }
  if (node.leaf.offset >= start) {
    node.leaf.offset += delta;
  }
  page->adjust_pointers(node.child, start, delta);
}

node_p MiddleLeafHandler::merge_node(Storage &storage, WritablePage *page,
                                     node_p npos) {
  MiddleLeaf &leaf = page->node(npos)->middleleaf;
  leaf.child = page->merge_node(storage, leaf.child);
  leaf.leaf = page->merge_node(storage, leaf.leaf);
  return npos;
}

node_p MiddleLeafHandler::move_node(WritablePage *src, node_p *npos,
                                    WritablePage *dest) {
  MiddleLeaf &sleaf = src->node(*npos)->middleleaf;
  node_p result = dest->alloc(sizeof(MiddleLeaf), kMiddleLeaf);
  MiddleLeaf &dleaf = dest->node(result)->middleleaf;

  // to stop adjust pointer
  dleaf.child = src->move_node(&sleaf.child, dest);
  dleaf.leaf = src->move_node(&sleaf.leaf, dest);
  uint16_t offset = npos->offset;
  *npos = node_p::null;  // stop adjust_pointer
  src->scale_node(offset, -(int)sizeof(MiddleLeaf));
  return result;
}

size_t MiddleLeafHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  MiddleLeaf &leaf = page->node(npos)->middleleaf;
  size_t size1 = page->find_split_link(leaf.child, candidate);
  if (candidate.set_link(&leaf.child, size1))
    return size1;

  size_t size2 = page->find_split_link(leaf.leaf, candidate);
  if (candidate.set_link(&leaf.leaf, size2))
    return size2;

  return size1 + size2;
}

MiddleLeafHandler middleLeafHandler;

bool CompressedHandler::find(Trace &trace) {
  Location last = trace.back();
  const Compressed &node = last.node->compressed;
  size_t size = std::min(trace.rest_key.size(), (size_t)node.size);
  if (size == node.size && !memcmp(trace.rest_key.data(), node.key, size)) {
    trace.current_key.append(node.key, node.size);
    trace.rest_key = trace.rest_key.advance(size);
    trace.push_back(last.next(&node.child));
    return true;
  }

  return false;
}

void CompressedHandler::insert(Trace &trace, const Slice &value) {
  Location last = trace.back();
  const Compressed &node = last.node->compressed;
  Slice &key(trace.rest_key);

  size_t size = std::min((size_t)node.size, key.size());
  size_t i;
  for (i = 0; i < size; i++) {
    if (node.key[i] != key[i]) {
      split_at_middle(trace, last, i, value);
      return;
    }
  }

  assert(i < node.size);
  split_at_middle_leaf(trace, last, i, value);
}

void CompressedHandler::split_at_middle(Trace &trace, Location &last,
                                        size_t pos, const Slice &value) {
  /*
      node = [abcdefg]
      key =  [abhij]
                    first   trie   second
      [abcdefg] ==> [ab] -> |c| -> [efg]
                            |h| -> [ij]
  */
  Compressed &node = last.node->compressed;
  size_t node_struct_size = sizeof(Compressed) + node.size;
  char node_key = node.key[pos];
  size_t trie_size = TrieHandler::calc_size(node_key, trace.rest_key[pos]);
  uint8_t second_size = node.size - pos - 1;
  size_t first_struct_size = pos ? sizeof(Compressed) + pos : 0;
  size_t second_struct_size =
      second_size ? sizeof(Compressed) + second_size : 0;

  assert(first_struct_size + trie_size + second_struct_size > node_struct_size);
  int delta =
      first_struct_size + trie_size + second_struct_size - node_struct_size;
  node_p node_child = node.child;
  node_child.offset += delta;

  last.wpage->scale_node(last.loc.node.offset + node_struct_size, delta);

  node_p child1;
  if (second_size) {
    child1 = node_p::b(last.loc.node.offset + first_struct_size + trie_size,
                       kCompressed);
    Compressed &second = last.page->node(child1)->compressed;
    memmove(&second.key[0], &node.key[pos + 1], second_size);
    second.size = second_size;
    second.child = node_child;
  } else
    child1 = node_child;

  node_p trie_offset =
      node_p::b(last.loc.node.offset + first_struct_size, kUpperTrie);
  if (pos) {
    node.size = pos;
    node.child = trie_offset.replace(kUpperTrie);
  } else {
    trace.change_link(last.loc.node.offset, kUpperTrie);
  }

  TrieHandler::create(trace, last.wpage, trie_offset, node_key, child1,
                      trace.rest_key.advance(pos), value);
}

void CompressedHandler::split_at_middle_leaf(Trace &trace, Location &last,
                                             size_t pos, const Slice &value) {
  /* key is a substring of node
      node = [abcdefg]
      key =  [ab]
                    first  MiddleLeaf   rest
      [abcdefg] ==> [ab] ->    []       -> [cefg]
  */
  Compressed &node = last.node->compressed;
  size_t node_struct_size = sizeof(Compressed) + node.size;
  size_t leaf_size = sizeof(MiddleLeaf);
  uint8_t second_size = node.size - pos;
  size_t first_struct_size = sizeof(Compressed) + pos;
  size_t second_struct_size = sizeof(Compressed) + second_size;

  assert(first_struct_size + leaf_size + second_struct_size > node_struct_size);
  last.wpage->scale_node(
      last.loc.node.offset + node_struct_size,
      first_struct_size + leaf_size + second_struct_size - node_struct_size);

  node_p second_offset = node_p::b(
      last.loc.node.offset + first_struct_size + leaf_size, kCompressed);
  Compressed &second = last.page->node(second_offset)->compressed;
  second.child = node.child;

  memmove(&second.key[0], &node.key[pos], second_size);
  second.size = second_size;

  node.size = pos;
  node.child = node_p::b(last.loc.node.offset + first_struct_size, kMiddleLeaf);

  MiddleLeaf &leaf = last.page->node(node.child)->middleleaf;
  leaf.child = second_offset;
  leaf.leaf = EndLeafHandler::create(trace, last.wpage, value);
}

void CompressedHandler::adjust_pointers(WritablePage *page, node_p npos,
                                        size_t start, int delta) {
  Compressed &node = page->node(npos)->compressed;
  if (node.child.offset >= start) {
    node.child.offset += delta;
  }
  page->adjust_pointers(node.child, start, delta);
}

node_p CompressedHandler::merge_node(Storage &storage, WritablePage *page,
                                     node_p npos) {
  Compressed &node = page->node(npos)->compressed;
  node.child = page->merge_node(storage, node.child);
  return npos;
}

node_p CompressedHandler::move_node(WritablePage *src, node_p *npos,
                                    WritablePage *dest) {
  Compressed &snode = src->node(*npos)->compressed;
  size_t node_size = snode.struct_size();
  node_p result = dest->alloc(node_size, kCompressed);
  Compressed &dnode = dest->node(result)->compressed;
  memcpy(&dnode, &snode, node_size);

  // to avoid adjust_pointer
  dnode.child = src->move_node(&snode.child, dest);
  uint16_t offset = npos->offset;
  *npos = node_p::null;  // stop adjust_pointer
  src->scale_node(offset, -node_size);
  return result;
}

size_t CompressedHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  Compressed &node = page->node(npos)->compressed;
  size_t size = page->find_split_link(node.child, candidate);
  if (candidate.set_link(&node.child, size)) {
    return size;
  }

  return size + node.struct_size();
}

node_p CompressedHandler::create(Trace &trace, WritablePage *page,
                                 const Slice &key, const Slice &value) {
  size_t csize = std::min(key.size(), (size_t)255);

  if (csize) {
    size_t node_size = sizeof(Compressed) + csize;
    if (page->free() < node_size) {
      page->too_small = true;
      location_p ppos = trace.storage.alloc();
      ppos = ppos.replace(
          create(trace, trace.storage.get_writable(ppos), key, value));
      return LinkHandler::create(trace, page, ppos);
    }
    node_p result = page->alloc(node_size, kCompressed);
    Node *node = page->node(result);
    node->compressed.size = csize;
    memcpy(node->compressed.key, key.data(), csize);
    node->compressed.child = create(trace, page, key.advance(csize), value);
    return result;
  }

  return EndLeafHandler::create(trace, page, value);
}

CompressedHandler compressedHandler;

bool TrieHandler::find(Trace &trace) {
  if (trace.rest_key.empty()) return false;

  Location last = trace.back();
  Trie &node = last.node->trie;

  int index = node.index(bits(trace.rest_key[0]));
  if (index < 0) return false;

  trace.push_back(last.next(&node.children[index]));
  return true;
}

void TrieHandler::adjust_pointers(WritablePage *page, node_p npos, size_t start,
                                  int delta) {
  Trie &trie = page->node(npos)->trie;
  size_t count = trie.count();
  for (size_t i = 0; i < count; i++) {
    if (trie.children[i].offset >= start) {
      trie.children[i].offset += delta;
    }
    page->adjust_pointers(trie.children[i], start, delta);
  }
}

node_p TrieHandler::merge_node(Storage &storage, WritablePage *page,
                               node_p npos) {
  Trie &trie = page->node(npos)->trie;
  size_t count = trie.count();
  for (size_t i = 0; i < count; i++) {
    trie.children[i] = page->merge_node(storage, trie.children[i]);
  }
  return npos;
}

node_p TrieHandler::move_node(WritablePage *src, node_p *npos,
                              WritablePage *dest) {
  Trie &strie = src->node(*npos)->trie;
  size_t node_size = strie.struct_size();
  node_p result = dest->alloc(node_size, npos->type);
  Trie &dtrie = dest->node(result)->trie;
  memcpy(&dtrie, &strie, node_size);
  size_t count = strie.count();
  for (size_t i = 0; i < count; i++) {
    // to avoid adjust_pointer
    dtrie.children[i] = src->move_node(&strie.children[i], dest);
  }

  uint16_t offset = npos->offset;
  *npos = node_p::null;  // stop adjust_pointer
  src->scale_node(offset, -node_size);
  return result;
}

size_t TrieHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  Trie &trie = page->node(npos)->trie;
  size_t count = trie.count();
  size_t sum = trie.struct_size();
  for (size_t i = 0; i < count; i++) {
    size_t size = page->find_split_link(trie.children[i], candidate);
    if (candidate.set_link(&trie.children[i], size)) {
      return size;
    }
    sum += size;
  }
  return sum;
}

node_p *TrieHandler::add(Trace &trace, Location &last) {
  Trie &trie = last.node->trie;
  assert((1 << bits(trace.rest_key[0]) & trie.bits) == 0);  // A new key!
  last.wpage->scale_node(last.loc.node.offset + trie.struct_size(),
                         sizeof(node_p));
  return trie.add(bits(trace.rest_key[0]));
}

void TrieHandler::create(Trace &trace, WritablePage *page, node_p pos,
                         char key1, node_p child, const Slice &key,
                         const Slice &value) {
  Trie &upper = page->node(pos)->trie;
  char key2 = key[0];

  upper.bits = (1 << bit::upper(key1)) | (1 << bit::upper(key2));

  if (upper.count() == 1) {
    // key1 and key2 have the same upper bits
    node_p pos_lower =
        node_p::b(pos.offset + sizeof(Trie) + sizeof(node_p), kLowerTrie);
    upper.children[0] = pos_lower;
    Trie &lower = page->node(pos_lower)->trie;

    lower.bits = (1 << bit::lower(key1)) | (1 << bit::lower(key2));
    lower.children[lower.index(bit::lower(key1))] = child;
    lower.children[lower.index(bit::lower(key2))] =
        CompressedHandler::create(trace, page, key.advance(1), value);
    return;
  }

  node_p pos_lower1 =
      node_p::b(pos.offset + sizeof(Trie) + 2 * sizeof(node_p), kLowerTrie);
  node_p pos_lower2 =
      node_p::b(pos_lower1.offset + sizeof(Trie) + sizeof(node_p), kLowerTrie);

  upper.children[upper.index(bit::upper(key1))] = pos_lower1;
  upper.children[upper.index(bit::upper(key2))] = pos_lower2;

  Trie &lower1 = page->node(pos_lower1)->trie;
  lower1.bits = 1 << bit::lower(key1);
  lower1.children[0] = child;

  Trie &lower2 = page->node(pos_lower2)->trie;
  lower2.bits = 1 << bit::lower(key2);
  lower2.children[0] =
      CompressedHandler::create(trace, page, key.advance(1), value);
}

char UpperTrieHandler::bits(char value) const { return bit::upper(value); };

void UpperTrieHandler::insert(Trace &trace, const Slice &value) {
  if (trace.rest_key.empty()) {
    size_t space = sizeof(MiddleLeaf);
    location_p &pnode = trace.stack.back().pnode;
    uint16_t offset = pnode.offset;
    WritablePage *page = trace.storage.get_writable(pnode.page);
    page->scale_node(offset, space);
    MiddleLeaf &leaf = page->node(offset)->middleleaf;
    leaf.leaf = EndLeafHandler::create(trace, page, value);
    leaf.child = node_p::b(offset + space, kUpperTrie);
    trace.change_link(offset, kMiddleLeaf);
    return;
  }
  Location last = trace.back();
  node_p *child = add(trace, last);
  *child = LowerTrieHandler::create(trace, last.wpage, value);
}

UpperTrieHandler upperTrieHandler;

char LowerTrieHandler::bits(char value) const { return bit::lower(value); }

bool LowerTrieHandler::find(Trace &trace) {
  if (TrieHandler::find(trace)) {
    trace.current_key.push_back(trace.rest_key[0]);
    trace.rest_key = trace.rest_key.advance(1);
    return true;
  }
  return false;
}

void LowerTrieHandler::insert(Trace &trace, const Slice &value) {
  Location last = trace.back();
  node_p *child = add(trace, last);
  *child = CompressedHandler::create(trace, last.wpage,
                                     trace.rest_key.advance(1), value);
}

node_p LowerTrieHandler::create(Trace &trace, WritablePage *page,
                                const Slice &value) {
  node_p result;
  size_t trie_struct_size = sizeof(Trie) + sizeof(node_p);
  Node *node;
  result = page->alloc(trie_struct_size, kLowerTrie);
  node = page->node(result);

  Trie &trie = node->trie;
  trie.bits = 1 << bit::lower(trace.rest_key[0]);
  trie.children[0] =
      CompressedHandler::create(trace, page, trace.rest_key.advance(1), value);
  return result;
}

LowerTrieHandler lowerTrieHandler;

bool NullHandler::find(Trace &trace) { return false; }

void NullHandler::insert(Trace &trace, const Slice &value) {
  Location last = trace.back();
  node_p result =
      CompressedHandler::create(trace, last.wpage, trace.rest_key, value);
  trace.change_link(0, result.type);
}

node_p NullHandler::merge_node(Storage &storage, WritablePage *page,
                               node_p npos) {
  return npos;
}

node_p NullHandler::move_node(WritablePage *src, node_p *npos,
                              WritablePage *dest) {
  return *npos;
}

size_t NullHandler::find_split_link(Page *page, node_p npos, SplitCandidate& candidate) {
  assert(0);
  return 0;
}

void NullHandler::adjust_pointers(WritablePage *page, node_p npos, size_t start,
                                  int delta) {}

NullHandler nullHandler;

NodeHandler *NodeHandler::HANDLERS[] = {
    &nullHandler,       &endLeafHandler,   &middleLeafHandler, &linkHandler,
    &compressedHandler, &upperTrieHandler, &lowerTrieHandler};

#ifdef DEBUG

const char *handler_names[] = {
    "kNull",       "kLeaf",      "kMiddleLeaf", "kLink",
    "kCompressed", "kUpperTrie", "kLowerTrie",
};

void dump_char(std::ostream &out, char bit) {
  if (isalnum(bit)) {
    out << bit;
  } else {
    out << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
}

std::ostream &operator<<(std::ostream &out, node_p loc) {
  out << handler_names[loc.type] << "-" << (uint16_t)loc.offset;
  return out;
}

std::ostream &operator<<(std::ostream &out, location_p loc) {
  out << loc.node << "P" << (uint64_t)loc.page;
  return out;
}

struct DumpBase {
  virtual void dump(std::ostream &out, location_p loc, Storage *storage) {
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;
    out << "---" << std::endl;
  }
};

struct NullDumper : public DumpBase {};

void dump_node(std::ostream &out, location_p loc, Storage *storage);

struct EndLeafDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;

    const EndLeaf &leaf = storage->node(loc)->endleaf;
    uint32_t size = std::min(leaf.size, (uint32_t)1);
    size_t space = sizeof(EndLeaf) + leaf.size;

    out << "space: " << space << std::endl;
    out << "size: " << (int)leaf.size << std::endl;
    out << "value: \"";
    for (uint32_t i = 0; i < size; i++) {
      out << "[";
      dump_char(out, leaf.data[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "---" << std::endl;
  }
};

struct MiddleLeafDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;
    out << "space: " << sizeof(MiddleLeaf) << std::endl;

    const MiddleLeaf &leaf = storage->node(loc)->middleleaf;

    out << "children: " << std::endl;
    out << "  - " << leaf.leaf << "P" << loc.page << std::endl;
    out << "  - " << leaf.child << "P" << loc.page << std::endl;
    out << "---" << std::endl;
    dump_node(out, loc.replace(leaf.leaf), storage);
    dump_node(out, loc.replace(leaf.child), storage);
  }
};

struct LinkDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;
    out << "space: " << sizeof(Link) << std::endl;

    const Link &link = storage->node(loc)->link;

    out << "children: " << std::endl;
    out << "  - " << link.loc << std::endl;
    out << "---" << std::endl;
    dump_node(out, link.loc, storage);
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    const Compressed &node_ = storage->node(loc)->compressed;
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;
    out << "space: " << sizeof(Compressed) + node_.size << std::endl;
    out << "size: " << (int)node_.size << std::endl;
    out << "keys: \"";
    for (int i = 0; i < node_.size; i++) {
      out << "[";
      dump_char(out, node_.key[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << node_.child << "P" << loc.page << std::endl;
    out << "---" << std::endl;
    dump_node(out, loc.replace(node_.child), storage);
  }
};

struct TrieDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    const Trie &trie = storage->node(loc)->trie;
    out << "id: " << loc << std::endl;
    out << "pspace: " << storage->page(loc)->end << std::endl;

    int size = trie.count();
    out << "space: " << sizeof(Trie) + size * sizeof(node_p) << std::endl;
    out << "size: " << size << std::endl;
    out << "bits: " << std::hex << trie.bits << std::dec << std::endl;

    out << "bitindex: [";
    unsigned int bits = trie.bits;
    int index = 0;
    while (bits) {
      index = ctz(bits);
      out << index;
      bits &= ~(1 << index);
      if (bits) {
        out << ", ";
      }
    }
    out << "]" << std::endl;

    out << "children: " << std::endl;
    for (int i = 0; i < size; i++) {
      out << "  - " << trie.children[i] << "P" << loc.page << std::endl;
    }
    out << "---" << std::endl;
    for (int i = 0; i < size; i++) {
      dump_node(out, loc.replace(trie.children[i]), storage);
    }
  }
};

NullDumper null_dumper;
EndLeafDumper endleaf_dumper;
MiddleLeafDumper middleleaf_dumper;
LinkDumper link_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;

DumpBase *dumpers[] = {&null_dumper, &endleaf_dumper,  &middleleaf_dumper,
                       &link_dumper, &compress_dumper, &trie_dumper,
                       &trie_dumper};

void dump_node(std::ostream &out, location_p loc, Storage *storage) {
  dumpers[loc.type]->dump(out, loc, storage);
}

#endif

}  // namespace leaves