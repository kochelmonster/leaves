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

Page *Link::make_page_to_move(Trace &trace, size_t nsize) {
  const Node *croot = trace.storage.node(loc);
  if (loc.type == kEndLeaf &&
      croot->endleaf.struct_size() + nsize >= PAGE_SIZE) {
    // big page -> no move to the big page put an ordinary page before
    location_p plink_page = trace.storage.alloc();
    Page *link_page = trace.storage.get_writable(plink_page);
    Link &tmp = link_page->node(link_page->alloc(trace, sizeof(Link)))->link;
    tmp.loc = loc;
    loc = plink_page;
    loc.type = kLink;
    link_page->end.type = kLink;
    return link_page;
  }
  return trace.storage.get_writable(loc);
}

namespace bit {
char upper(char value) { return value >> 4; }

char lower(char value) { return (value & 0x0F); }
}  // namespace bit

struct LinkHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta);
  bool move_node(Trace &trace, Page *page, node_p *offset);
  static node_p create(Trace &trace, Page *page, location_p loc);
};

struct LeafHandler : public NodeHandler {
  bool valid(const Trace &trace) const { return trace.rest_key.empty(); }
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta) {}
};

struct EndLeafHandler : public LeafHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void change_value(Trace &trace, const Slice &value);
  void insert_compressed(Trace &trace, const Slice &value);
  bool move_node(Trace &trace, Page *page, node_p *offset);
  static node_p create(Trace &trace, Page *page, const Slice &value);
};

struct MiddleLeafHandler : public LeafHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta);
  bool move_node(Trace &trace, Page *page, node_p *offset);
};

struct CompressedHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void split_at_middle(Trace &trace, Location &last, size_t pos,
                       const Slice &value);
  void split_at_middle_leaf(Trace &trace, Location &last, size_t pos,
                            const Slice &value);
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta);
  bool move_node(Trace &trace, Page *page, node_p *offset);
  static node_p create(Trace &trace, Page *page, const Slice &key,
                       const Slice &value);
};

struct NullHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta);
  bool move_node(Trace &trace, Page *page, node_p *offset);
};

struct TrieHandler : public NodeHandler {
  virtual char bits(char value) const = 0;

  bool find(Trace &trace);
  void adjust_pointers(Page *page, node_p npos, size_t start, int delta);
  bool move_node(Trace &trace, Page *page, node_p *offset);

  node_p *add(Trace &trace, Location &last);
  static void create(Trace &trace, Page *page, node_p pos, char node_key,
                     node_p child, const Slice &key, const Slice &value);
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
  static node_p create(Trace &trace, Page *page, const Slice &value);
};

bool LinkHandler::find(Trace &trace) {
  Location last = trace.back();
  trace.stack.push_back(last.next(&last.node->link.loc));
  return true;
}

void LinkHandler::insert(Trace &trace, const Slice &value) { assert(0); }

void LinkHandler::adjust_pointers(Page *page, node_p npos, size_t start,
                                  int delta) {}

bool LinkHandler::move_node(Trace &trace, Page *page, node_p *offset) {
  return false;
}

node_p LinkHandler::create(Trace &trace, Page *page, location_p pos) {
  node_p result = page->alloc(trace, sizeof(Link), kLink);
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
  size_t space = sizeof(MiddleLeaf);
  location_p &pnode = trace.stack.back().pnode;
  uint16_t offset = pnode.offset;
  Page *page = trace.storage.page(pnode.page, true);
  page->scale_node(trace, offset, space);

  MiddleLeaf &leaf = page->node(offset)->middleleaf;
  leaf.leaf = node_p::b(offset + space, kEndLeaf);
  leaf.child = CompressedHandler::create(trace, page, trace.rest_key, value);
  trace.change_link(offset, kMiddleLeaf);
}

void EndLeafHandler::change_value(Trace &trace, const Slice &value) {
  // change the value
  Location last = trace.back();
  EndLeaf &leaf = last.node->endleaf;
  size_t leaf_struct_size = sizeof(EndLeaf) + leaf.size;
  size_t new_struct_size =
      std::max(sizeof(EndLeaf) + value.size(), sizeof(Link));

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
  if (delta < 0 || (int)last.page->free() > delta) {
    EndLeaf &leaf = last.node->endleaf;
    // the new value fits in the same page
    last.page->scale_node(trace, last.loc.node.offset, delta);
    trace.change_link(last.loc.node.offset, kEndLeaf);
    leaf.size = value.size();
    memcpy(&leaf.data[0], value.data(), leaf.size);
    return;
  }

  // We have to insert a link and write the value at a new page.
  last.page->scale_node(trace, last.loc.node.offset,
                        sizeof(Link) - leaf_struct_size);
  Link &link = last.node->link;
  trace.change_link(last.loc.node.offset, kLink);
  link.loc = trace.storage.alloc(new_struct_size);
  link.loc.type = kEndLeaf;
  trace.storage.write_value(link.loc, value);
}

node_p EndLeafHandler::create(Trace &trace, Page *page, const Slice &value) {
  node_p result;
  Node *node;
  /* node_size must be at least the size of link, to be able to extend the leaf,
     on a full page*/
  size_t node_size = std::max(sizeof(EndLeaf) + value.size(), sizeof(Link));

  if (page->free() < node_size) {
    location_p ploc = trace.storage.alloc(node_size);
    ploc.type = kEndLeaf;
    result = LinkHandler::create(trace, page, ploc);
    if (node_size > PAGE_SIZE) {
      trace.storage.write_value(ploc, value);
      return result;
    }
    Page *dest = trace.storage.get_writable(ploc);
    dest->end.type = kEndLeaf;
    node = dest->node(0);
  } else {
    result = page->alloc(trace, node_size, kEndLeaf);
    node = page->node(result);
  }

  node->endleaf.size = value.size();
  memcpy(&node->endleaf.data[0], value.data(), value.size());
  return result;
}

bool EndLeafHandler::move_node(Trace &trace, Page *page, node_p *npos) {
  EndLeaf &leaf = page->node(*npos)->endleaf;

  size_t node_size = leaf.struct_size();
  if (node_size <= sizeof(Link) || !npos->offset) {
    // nothing to gain
    return false;
  }

  location_p ploc = trace.storage.alloc();
  Page *pnew = trace.storage.get_writable(ploc);
  pnew->alloc(trace, node_size);
  memcpy(&pnew->node(0)->endleaf, &leaf, node_size);
  pnew->end.type = ploc.type = kEndLeaf;

  // change lead to link
  page->scale_node(trace, npos->offset, sizeof(Link)-node_size);
  Link &link = page->node(*npos)->link;
  link.loc = ploc;
  npos->type = kLink;
  return true;
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

void MiddleLeafHandler::adjust_pointers(Page *page, node_p npos, size_t start,
                                        int delta) {
  MiddleLeaf &node = page->node(npos)->middleleaf;
  if (node.child.offset >= start) {
    node.child.offset += delta;
  }
  page->adjust_pointers(node.child, start, delta);
}

bool MiddleLeafHandler::move_node(Trace &trace, Page *page, node_p *npos) {
  Node *node = page->node(*npos);
  return (page->move_node(trace, &node->middleleaf.leaf) ||
          page->move_node(trace, &node->middleleaf.child));
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

  last.page->scale_node(trace, last.loc.node.offset + node_struct_size, delta);

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

  TrieHandler::create(trace, last.page, trie_offset, node_key, child1,
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
  last.page->scale_node(
      trace, last.loc.node.offset + node_struct_size,
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
  leaf.leaf = EndLeafHandler::create(trace, last.page, value);
}

void CompressedHandler::adjust_pointers(Page *page, node_p npos, size_t start,
                                        int delta) {
  Compressed &node = page->node(npos)->compressed;
  if (node.child.offset >= start) {
    node.child.offset += delta;
  }
  page->adjust_pointers(node.child, start, delta);
}

bool CompressedHandler::move_node(Trace &trace, Page *page, node_p *pnpos) {
  Compressed &node = page->node(*pnpos)->compressed;
  if (node.child.type == kLink && pnpos->offset) {
    Link &link = page->node(node.child)->link;
    size_t node_size = node.struct_size();
    Page *dest = link.make_page_to_move(trace, node_size);
    location_p ploc = link.loc;

    // copy the node to the new page
    dest->end.type = kCompressed;
    Compressed &dnode = dest->node(0)->compressed;
    dest->scale_node(trace, 0, node_size);
    memcpy(&dnode, &node, node_size);
    dnode.child = node_p::b(node_size, ploc.type);

    // remove the link
    page->scale_node(trace, node.child.offset, -(int)sizeof(Link));

    // change the compressed to link
    page->scale_node(trace, pnpos->offset, sizeof(Link) - node_size);
    Link &nlink = page->node(*pnpos)->link;
    nlink.loc = location_p::b(ploc.page, 0, kCompressed);
    pnpos->type = kLink;
    return true;
  }
  return page->move_node(trace, &node.child);
}

node_p CompressedHandler::create(Trace &trace, Page *page, const Slice &key,
                                 const Slice &value) {
  size_t csize = std::min(key.size(), (size_t)255);
  size_t node_size = sizeof(Compressed) + csize;

  if (page->free() < node_size + sizeof(Link)) {
    location_p ppos = trace.storage.alloc();
    create(trace, trace.storage.get_writable(ppos), key, value);
    return LinkHandler::create(trace, page, ppos);
  }

  if (csize) {
    node_p result = page->alloc(trace, node_size, kCompressed);
    Node *node = page->node(result);
    node->compressed.size = csize;
    memcpy(node->compressed.key, key.data(), csize);
    node->compressed.child = create(trace, page, key.advance(csize), value);
    if (result.offset == 0) {
      page->end.type = kCompressed;
    }
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

void TrieHandler::adjust_pointers(Page *page, node_p npos, size_t start,
                                  int delta) {
  Trie &trie = page->node(npos)->trie;
  size_t count = trie.size();
  for (size_t i = 0; i < count; i++) {
    if (trie.children[i].offset >= start) {
      trie.children[i].offset += delta;
    }
    page->adjust_pointers(trie.children[i], start, delta);
  }
}

bool TrieHandler::move_node(Trace &trace, Page *page, node_p *pnpos) {
  Trie &trie = page->node(*pnpos)->trie;
  size_t count = trie.size();
  bool can_move = pnpos->offset != 0;

  for (size_t i = 0; i < count; i++) {
    if (trie.children[i].type != kLink) {
      can_move = false;
      if (page->move_node(trace, &trie.children[i])) return true;
    }
  }

  if (can_move) {
    // Move the node to the first child
    assert(trie.children[0].type == kLink);
    Link &link = page->node(trie.children[0])->link;
    size_t trie_size = trie.struct_size();
    size_t links_size = (trie.size() - 1) * sizeof(Link);
    Page *dest = link.make_page_to_move(trace, trie_size + links_size);
    location_p ploc = link.loc;

    dest->scale_node(trace, 0, trie_size + links_size);

    // copy trie to new dest
    Trie &dtrie = dest->node(0)->trie;
    memcpy(&dtrie, &trie, trie_size);
    dtrie.children[0] =
        node_p::b(trie_size + links_size, ploc.type);  // the former root
    dest->end.type = pnpos->type;

    node_p link_offset = node_p::b(trie_size, kLink);
    for (size_t i = 1; i < count; i++) {
      dtrie.children[i] = link_offset;
      Link &link = dest->node(link_offset)->link;
      link.loc = page->node(trie.children[i])->link.loc;
      link_offset.offset += sizeof(Link);
    }

    // remove links from old page
    for (size_t i = 0; i < count; i++) {
      page->scale_node(trace, trie.children[i].offset, -(int)sizeof(Link));
    }

    // trie -> link
    page->scale_node(trace, pnpos->offset, sizeof(Link) - trie_size);
    Link &nlink = page->node(*pnpos)->link;
    ploc.type = pnpos->type;
    nlink.loc = ploc;
    pnpos->type = kLink;
    return true;
  }

  return false;
}

node_p *TrieHandler::add(Trace &trace, Location &last) {
  Trie &trie = last.node->trie;
  assert((1 << bits(trace.rest_key[0]) & trie.bits) == 0);  // A new key!
  last.page->scale_node(trace, last.loc.node.offset + trie.struct_size(),
                        sizeof(node_p));
  return trie.add(bits(trace.rest_key[0]));
}

void TrieHandler::create(Trace &trace, Page *page, node_p pos, char key1,
                         node_p child, const Slice &key, const Slice &value) {
  Trie &upper = page->node(pos)->trie;
  char key2 = key[0];

  upper.bits = (1 << bit::upper(key1)) | (1 << bit::upper(key2));

  if (upper.size() == 1) {
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
    Page *page = trace.storage.page(pnode.page, true);
    page->scale_node(trace, offset, space);
    MiddleLeaf &leaf = page->node(offset)->middleleaf;
    leaf.leaf = EndLeafHandler::create(trace, page, value);
    leaf.child = node_p::b(offset + space, kUpperTrie);
    trace.change_link(offset, kMiddleLeaf);
    return;
  }
  Location last = trace.back();
  node_p *child = add(trace, last);
  *child = LowerTrieHandler::create(trace, last.page, value);
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
  *child = CompressedHandler::create(trace, last.page,
                                     trace.rest_key.advance(1), value);
}

node_p LowerTrieHandler::create(Trace &trace, Page *page, const Slice &value) {
  node_p result;
  size_t trie_struct_size = sizeof(Trie) + sizeof(node_p);
  Node *node;
  if (page->free() < trie_struct_size + sizeof(Link)) {
    result = page->alloc(trace, sizeof(Link), kLink);
    Link &link = page->node(result)->link;
    link.loc = trace.storage.alloc();
    link.loc.type = kLowerTrie;
    page = trace.storage.get_writable(link.loc);
    node = page->node(0);
    page->end.type = kLowerTrie;
  } else {
    result = page->alloc(trace, trie_struct_size, kLowerTrie);
    node = page->node(result);
  }

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
  CompressedHandler::create(trace, last.page, trace.rest_key, value);
  trace.change_link(0, kCompressed);
}

bool NullHandler::move_node(Trace &trace, Page *page, node_p *pnpos) {
  return false;
}

void NullHandler::adjust_pointers(Page *page, node_p npos, size_t start,
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
  if (isprint(bit)) {
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
    out << "---" << std::endl;
  }
};

struct NullDumper : public DumpBase {};

void dump_node(std::ostream &out, location_p loc, Storage *storage);

struct EndLeafDumper : public DumpBase {
  void dump(std::ostream &out, location_p loc, Storage *storage) {
    out << "id: " << loc << std::endl;

    const EndLeaf &leaf = storage->node(loc)->endleaf;
    uint32_t size = std::min(leaf.size, (uint32_t)16);
    size_t space = std::max(sizeof(Link), sizeof(EndLeaf) + leaf.size);

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

    int size = trie.size();
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