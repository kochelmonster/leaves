#include "page.hpp"
#include "storage.hpp"
#include "trace.hpp"
#ifdef DEBUG
#include <iostream>
#endif

namespace leaves {

namespace bit {
char upper(char value) { return value >> 4; }

char lower(char value) { return (value & 0x0F); }
}  // namespace bit

struct LinkHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node);
};

struct PointerLinkHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node);
};

struct ValueHandler : public NodeHandler {
  bool valid(const Trace &trace) const;
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void change_value(Trace &trace, const Slice &value);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node);
};

struct CompressedHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  void split_with_trie(Trace &trace, size_t pos, const Slice &value);
  void split_with_leaf(Trace &trace, size_t pos, const Slice &value);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node);
  static node_t create(Storage &storage, Page *page, const Slice &key,
                       const Slice &value);
  static node_t create(Page *page, const Slice &key, node_t value);
};

struct NullHandler : public NodeHandler {
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node) { return 0; }
};

struct TrieHandler : public NodeHandler {
  virtual char bits(char value) const = 0;
  int find_index(Trace &trace, Trie &node);
  node_t copy_node(Page *dest, const Page *src, node_t id);
  uint16_t get_size(const Node *node);
};

struct UpperTrieHandler : public TrieHandler {
  char bits(char value) const;
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
};

struct LowerTrieHandler : public TrieHandler {
  char bits(char value) const;
  bool find(Trace &trace);
  void insert(Trace &trace, const Slice &value);
  static node_t create(Trace &trace, const Slice &value);
};

bool LinkHandler::find(Trace &trace) {
  trace.push_to_stack(trace.stack_back().get_node()->link);
  return true;
}

void LinkHandler::insert(Trace &trace, const Slice &value) { assert(0); }

node_t LinkHandler::copy_node(Page *dest, const Page *src, node_t id) {
  node_t nid = dest->alloc(sizeof(stored_ptr), kLink);
  dest->get_node(nid)->link.val = src->get_node(id)->link.val;
  return nid;
}

uint16_t LinkHandler::get_size(const Node *node) { return sizeof(stored_ptr); }

LinkHandler linkHandler;

bool PointerLinkHandler::find(Trace &trace) {
  trace.push_to_stack(trace.stack_back().get_node()->pointer);
  return true;
}

void PointerLinkHandler::insert(Trace &trace, const Slice &value) { assert(0); }

node_t PointerLinkHandler::copy_node(Page *dest, const Page *src, node_t id) {
  node_t nid = dest->alloc(sizeof(Page *), kHeapLink);
  dest->get_node(nid)->pointer = src->get_node(id)->pointer;
  return nid;
}

uint16_t PointerLinkHandler::get_size(const Node *node) { return sizeof(Page *); }

PointerLinkHandler pointerlinkHandler;

bool ValueHandler::valid(const Trace &trace) const {
  return trace.rest_key.empty();
}

bool ValueHandler::find(Trace &trace) {
  Transition &back = trace.stack_back();
  Value &node = back.get_node()->value;
  if (node.child && trace.rest_key.size()) {
    trace.push_to_stack(node.child);
    return true;
  }
  return false;
}

void ValueHandler::insert(Trace &trace, const Slice &value) {
  Transition &back = trace.stack_back();
  Value &node = back.get_node()->value;
  if (trace.rest_key.empty()) {
    trace.storage.add_value_to_copied(node.value);
    node.value.val = trace.storage.new_value(value).val;
  } else {
    assert(node.child == 0);
    node.child = CompressedHandler::create(
        trace.storage, trace.stack_back().page, trace.rest_key, value);
  }
}

node_t ValueHandler::copy_node(Page *dest, const Page *src, node_t id) {
  node_t nid = dest->alloc(sizeof(Value), kValue);
  const Value &svalue = src->get_node(id)->value;
  Value &dvalue = dest->get_node(nid)->value;
  dvalue.value.val = svalue.value.val;
  dvalue.child = svalue.child ? src->copy_node(dest, svalue.child) : 0;
  return nid;
}

uint16_t ValueHandler::get_size(const Node *node) { return sizeof(Value); }

ValueHandler valueHandler;

bool CompressedHandler::find(Trace &trace) {
  Compressed &node = trace.stack_back().get_node()->compressed;
  size_t size = std::min(trace.rest_key.size(), (size_t)node.size);
  if (size == node.size && !memcmp(trace.rest_key.data(), node.key, size)) {
    trace.current_key.append(node.key, node.size);
    trace.rest_key = trace.rest_key.advance(size);
    trace.push_to_stack(node.child);
    return true;
  }

  return false;
}

void CompressedHandler::insert(Trace &trace, const Slice &value) {
  Compressed &node = trace.stack_back().get_node()->compressed;
  Slice &key(trace.rest_key);
  size_t size = std::min((size_t)node.size, key.size());
  size_t i;
  for (i = 0; i < size; i++) {
    if (node.key[i] != key[i]) {
      split_with_trie(trace, i, value);
      return;
    }
  }

  assert(i < node.size);
  split_with_leaf(trace, i, value);
}

node_t CompressedHandler::copy_node(Page *dest, const Page *src, node_t id) {
  const Compressed &snode = src->get_node(id)->compressed;
  node_t nid = dest->alloc(snode.struct_size(), kCompressed);
  Compressed &dnode = dest->get_node(nid)->compressed;
  memcpy(&dnode, &snode, snode.struct_size());
  dnode.child = src->copy_node(dest, snode.child);
  return nid;
}

uint16_t CompressedHandler::get_size(const Node *node) {
  return node->compressed.struct_size();
}

inline void set_lower(Trie &trie, char key, node_t child) {
  trie.children[trie.index(bit::lower(key))] = child;
}

inline void set_upper(Trie &trie, char key, node_t child) {
  trie.children[trie.index(bit::upper(key))] = child;
}

void CompressedHandler::split_with_trie(Trace &trace, size_t pos,
                                        const Slice &value) {
  /*
      node = [abcdefg]
      key =  [abhij]
                    first   trie   second
      [abcdefg] ==> [ab] -> |c| -> [efg]
                            |h| -> [ij]
  */
  Transition &back = trace.stack_back();
  Page *page = back.page;
  node_p *pie = back.get_ie();
  Compressed &first = page->get_node(pie)->compressed;
  char nkey = trace.rest_key[pos], okey = first.key[pos];
  Trie tmp_trie;
  tmp_trie.bits = (1 << bit::upper(nkey)) | (1 << bit::upper(okey));

  char buffer[255];
  memcpy(buffer, first.key, first.size);
  Slice key(buffer, first.size);
  node_t child = first.child;

  Node *node;
  if (pos) {
    // shrink node to first
    int nsize = Compressed::calc_size(pos);
    int osize = Compressed::calc_size(key.size());
    page->grow(pie->offset + nsize, nsize - osize);
    first.size = pos;
    node_t tid = first.child = page->alloc(tmp_trie.struct_size(), kUpperTrie);
    node = page->get_node(tid);
    TESTPOINT(CompressSplitTrie1);
  } else {
    // change node to trie
    int trie_size = tmp_trie.struct_size();
    page->grow(pie->offset + trie_size, trie_size - (int)first.struct_size());
    pie->type = kUpperTrie;
    node = page->get_node(pie);
    TESTPOINT(CompressSplitTrie0);
  }

  // add Upper Trie
  Trie &utrie = node->trie;
  utrie.bits = tmp_trie.bits;

  if (utrie.count() == 1) {
    // add lower trie
    tmp_trie.bits = (1 << bit::lower(nkey)) | (1 << bit::lower(okey));
    node_t lnid = utrie.children[0] =
        back.page->alloc(tmp_trie.struct_size(), kLowerTrie);
    Trie &ltrie = page->get_node(lnid)->trie;
    ltrie.bits = tmp_trie.bits;
    set_lower(ltrie, okey,
              CompressedHandler::create(page, key.advance(pos + 1), child));
    set_lower(ltrie, nkey,
              CompressedHandler::create(
                  trace.storage, page, trace.rest_key.advance(pos + 1), value));
    TESTPOINT(CompressSplitTrieUpper1);
    return;
  }

  // add lower trie
  TESTPOINT(CompressSplitTrieUpper2);
  assert(utrie.count() == 2);
  tmp_trie.bits = 1;
  size_t tsize = tmp_trie.struct_size();
  node_t lnid1 = page->alloc(tsize, kLowerTrie);
  node_t lnid2 = page->alloc(tsize, kLowerTrie);
  set_upper(utrie, okey, lnid1);
  set_upper(utrie, nkey, lnid2);
  Trie &ltrie1 = back.page->get_node(lnid1)->trie;
  Trie &ltrie2 = back.page->get_node(lnid2)->trie;

  ltrie1.bits = 1 << bit::lower(okey);
  ltrie2.bits = 1 << bit::lower(nkey);
  ltrie1.children[0] =
      CompressedHandler::create(page, key.advance(pos + 1), child);
  ltrie2.children[0] = CompressedHandler::create(
      trace.storage, page, trace.rest_key.advance(pos + 1), value);
}

void CompressedHandler::split_with_leaf(Trace &trace, size_t pos,
                                        const Slice &value) {
  /* key is a substring of node
      node = [abcdefg]
      key =  [ab]
                    first  MiddleLeaf   rest
      [abcdefg] ==> [ab] ->    []       -> [cefg]
  */

  Transition &back = trace.stack_back();
  Page *page = back.page;
  node_p *pie = back.get_ie();
  Compressed &first = page->get_node(pie)->compressed;

  node_t nid = back.page->alloc(sizeof(Value), kValue);
  node_p *pie_value = back.page->get_ie(nid);
  Value *val = &back.page->get_node(pie_value)->value;
  val->value.val = trace.storage.new_value(value).val;

  if (!pos) {  // Insert value before first
    // exchange the index entries
    node_p tmp;
    tmp.val = pie->val;
    pie->val = pie_value->val;
    pie_value->val = tmp.val;
    val->child = nid;  // first is now in nid
    return;
  }

  // shrink node to first
  char buffer[255];
  memcpy(buffer, first.key, first.size);
  Slice key(buffer, first.size);
  node_t child = first.child;

  int nsize = Compressed::calc_size(pos);
  int osize = Compressed::calc_size(first.size);
  page->grow(pie->offset + nsize, nsize - osize);
  first.size = pos;
  first.child = nid;
  val = &back.page->get_node(pie_value)->value;
  val->child = CompressedHandler::create(page, key.advance(pos), child);
}

node_t CompressedHandler::create(Page *page, const Slice &key, node_t value) {
  // only called by split_with_trie, split_with_leaf
  if (key.empty()) return value;

  assert(key.size() < 255);
  assert(page->reserve(Compressed::calc_size(key.size()), 1));

  node_t nid = page->alloc(Compressed::calc_size(key.size()), kCompressed);
  Compressed &node = page->get_node(nid)->compressed;
  node.size = key.size();
  memcpy(node.key, key.data(), node.size);
  node.child = value;
  return nid;
}

node_t CompressedHandler::create(Storage &storage, Page *page, const Slice &key,
                                 const Slice &value) {
  size_t csize = std::min(key.size(), (size_t)255);

  if (csize) {
    if (!page->reserve(
            Compressed::calc_size(csize) + sizeof(Value) + sizeof(Page *), 2)) {
      Page *new_page = storage.alloc_new_page();
      node_t nid = page->alloc(sizeof(Page *), kHeapLink);
      page->get_node(nid)->pointer = new_page;
      create(storage, new_page, key, value);
      return nid;
    }

    node_t nid = page->alloc(Compressed::calc_size(csize), kCompressed);
    Compressed &node = page->get_node(nid)->compressed;
    node.size = csize;
    memcpy(node.key, key.data(), node.size);
    node.child = create(storage, page, key.advance(csize), value);
    return nid;
  }

  node_t nid = page->alloc(sizeof(Value), kValue);
  Value &value_ = page->get_node(nid)->value;
  value_.value.val = storage.new_value(value).val;
  value_.child = 0;
  return nid;
}

CompressedHandler compressedHandler;

int TrieHandler::find_index(Trace &trace, Trie &node) {
  if (trace.rest_key.empty()) return -1;
  return node.index(bits(trace.rest_key[0]));
}

node_t TrieHandler::copy_node(Page *dest, const Page *src, node_t id) {
  const node_p *pie = src->get_ie(id);
  const Trie &strie = src->get_node(pie)->trie;
  node_t nid = dest->alloc(strie.struct_size(), (NodeType)pie->type);
  Trie &dtrie = dest->get_node(nid)->trie;
  dtrie.bits = strie.bits;
  int count = strie.count();
  for (int i = 0; i < count; i++) {
    dtrie.children[i] = src->copy_node(dest, strie.children[i]);
  }
  return nid;
}

uint16_t TrieHandler::get_size(const Node *node) { return node->trie.struct_size(); }

char UpperTrieHandler::bits(char value) const { return bit::upper(value); };

bool UpperTrieHandler::find(Trace &trace) {
  Trie &node = trace.stack_back().get_node()->trie;
  int index = find_index(trace, node);
  if (index < 0) return false;

  trace.push_to_stack(node.children[index]);
  return true;
}

void UpperTrieHandler::insert(Trace &trace, const Slice &value) {
  Transition back = trace.stack_back();
  node_p *pie_trie = back.get_ie();

  if (trace.rest_key.empty()) {
    // insert Value before trie
    node_t nid = back.page->alloc(sizeof(Value), kValue);
    node_p *pie_value = back.page->get_ie(nid);
    node_p tmp;
    // exchange the index entries
    tmp.val = pie_trie->val;
    pie_trie->val = pie_value->val;
    pie_value->val = tmp.val;

    Value &val = back.page->get_node(pie_trie)->value;  // exchanged!
    val.value.val = trace.storage.new_value(value).val;
    val.child = nid;
    return;
  }

  Trie &trie = back.page->get_node(pie_trie)->trie;
  int osize = trie.struct_size();
  int nsize = trie.struct_size(1);
  back.page->grow(pie_trie->offset + osize, nsize - osize);
  assert((1 << bits(trace.rest_key[0]) & trie.bits) == 0);  // A new key!
  int idx = trie.add(bits(trace.rest_key[0]));
  trie.children[idx] = LowerTrieHandler::create(trace, value);
}

UpperTrieHandler upperTrieHandler;

char LowerTrieHandler::bits(char value) const { return bit::lower(value); }

bool LowerTrieHandler::find(Trace &trace) {
  Trie &node = trace.stack_back().get_node()->trie;
  int index = find_index(trace, node);
  if (index < 0) return false;

  trace.current_key.push_back(trace.rest_key[0]);
  trace.rest_key = trace.rest_key.advance(1);
  trace.push_to_stack(node.children[index]);
  return true;
}

void LowerTrieHandler::insert(Trace &trace, const Slice &value) {
  assert(trace.rest_key.size() >= 1);

  Transition &back = trace.stack_back();
  node_p *pie = back.get_ie();
  Trie &trie = back.page->get_node(pie)->trie;
  int osize = trie.struct_size();
  int nsize = trie.struct_size(1);
  back.page->grow(pie->offset + osize, nsize - osize);
  assert((1 << bits(trace.rest_key[0]) & trie.bits) == 0);  // A new key!
  int idx = trie.add(bits(trace.rest_key[0]));
  trie.children[idx] = CompressedHandler::create(
      trace.storage, back.page, trace.rest_key.advance(1), value);
}

node_t LowerTrieHandler::create(Trace &trace, const Slice &value) {
  Transition &back = trace.stack_back();
  node_t nid = back.page->alloc(sizeof(Trie) + sizeof(node_t), kLowerTrie);
  Trie &trie = back.page->get_node(nid)->trie;
  trie.bits = 1 << bit::lower(trace.rest_key[0]);
  trie.children[0] = CompressedHandler::create(
      trace.storage, back.page, trace.rest_key.advance(1), value);
  return nid;
}

LowerTrieHandler lowerTrieHandler;

bool NullHandler::find(Trace &trace) { return false; }

void NullHandler::insert(Trace &trace, const Slice &value) {
  CompressedHandler::create(trace.storage, trace.stack_back().page,
                            trace.rest_key, value);
}

node_t NullHandler::copy_node(Page *dest, const Page *src, node_t id) {
  assert(0);
  return 0;
}

NullHandler nullHandler;

NodeHandler *NodeHandler::HANDLERS[] = {
    &nullHandler,        &valueHandler,      &linkHandler,
    &pointerlinkHandler, &compressedHandler, &upperTrieHandler,
    &lowerTrieHandler,
};

#ifdef DEBUG

void dump_char(std::ostream &out, char bit) {
  if (isalnum(bit)) {
    out << bit;
  } else {
    out << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
}

struct DumpBase {
  virtual void dump(std::ostream &out, Page *page, node_t nid,
                    Storage *storage) {
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;
    out << "type: kNull" << std::endl;
    out << "pspace: " << page->size << std::endl;
    out << "---" << std::endl;
  }
};

struct NullDumper : public DumpBase {
  virtual void dump(std::ostream &out, Page *page, node_t nid,
                    Storage *storage) {
    assert(0);
  }
};

void dump_node(std::ostream &out, Page *page, node_t nid, Storage *storage);

struct ValueDumper : public DumpBase {
  void dump(std::ostream &out, Page *page, node_t nid, Storage *storage) {
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;
    out << "type: kValue" << std::endl;
    out << "pspace: " << page->size << std::endl;

    const Value &leaf = page->get_node(nid)->value;
    Slice val = storage->view->get_value(leaf.value);
    uint32_t size = std::min((uint32_t)val.size(), (uint32_t)128);
    size_t space = sizeof(Value);

    out << "space: " << space << std::endl;
    out << "size: " << val.size() << std::endl;
    if (leaf.child) {
      out << "children: " << std::endl;
      out << "  - " << (int)leaf.child << "P" << storage->get_page_id(page)
          << std::endl;
    }
    out << "value: \"";
    for (uint32_t i = 0; i < size; i++) {
      out << "[";
      dump_char(out, val.data()[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "---" << std::endl;

    if (leaf.child) dump_node(out, page, leaf.child, storage);
  }
};

struct LinkDumper : public DumpBase {
  void dump(std::ostream &out, Page *page, node_t nid, Storage *storage) {
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;
    out << "type: kLink" << std::endl;
    out << "pspace: " << page->size << std::endl;
    out << "space: " << sizeof(stored_ptr) << std::endl;

    stored_ptr link = page->get_node(nid)->link;

    Page *next = link.get<Page>(storage->view.get());
    out << "children: " << std::endl;
    out << "  - " << 0 << "P" << storage->get_page_id(next) << std::endl;
    out << "---" << std::endl;
    dump_node(out, next, 0, storage);
  }
};

struct HeapLinkDumper : public DumpBase {
  void dump(std::ostream &out, Page *page, node_t nid, Storage *storage) {
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;
    out << "type: kHeapLink" << std::endl;
    out << "pspace: " << page->size << std::endl;
    out << "space: " << sizeof(Page *) << std::endl;

    Page *child = page->get_node(nid)->pointer;

    out << "children: " << std::endl;
    out << "  - " << 0 << "P" << storage->get_page_id(page) << std::endl;
    out << "---" << std::endl;
    dump_node(out, child, 0, storage);
  }
};

struct CompressDumper : public DumpBase {
  void dump(std::ostream &out, Page *page, node_t nid, Storage *storage) {
    const Compressed &node_ = page->get_node(nid)->compressed;
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;
    out << "type: kCompressed" << std::endl;
    out << "pspace: " << page->size << std::endl;
    out << "space: " << node_.struct_size() << std::endl;
    out << "size: " << (int)node_.size << std::endl;
    out << "keys: \"";
    for (int i = 0; i < node_.size; i++) {
      out << "[";
      dump_char(out, node_.key[i]);
      out << "]";
    }
    out << "\"" << std::endl;
    out << "children: " << std::endl;
    out << "  - " << (int)node_.child << "P" << storage->get_page_id(page)
        << std::endl;
    out << "---" << std::endl;
    dump_node(out, page, node_.child, storage);
  }
};

struct TrieDumper : public DumpBase {
  void dump(std::ostream &out, Page *page, node_t nid, Storage *storage) {
    const Trie &trie = page->get_node(nid)->trie;
    out << "id: " << (int)nid << "P" << storage->get_page_id(page) << std::endl;

    node_p *pie = page->get_ie(nid);
    switch (pie->type) {
      case kUpperTrie:
        out << "type: kUpperTrie" << std::endl;
        break;

      case kLowerTrie:
        out << "type: kLowerTrie" << std::endl;
        break;

      default:
        assert(0);
    }

    out << "pspace: " << page->size << std::endl;

    int size = trie.count();
    assert(size >= 1);
    out << "space: " << trie.struct_size() << std::endl;
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
      out << "  - " << (int)trie.children[i] << "P"
          << storage->get_page_id(page) << std::endl;
    }
    out << "---" << std::endl;
    for (int i = 0; i < size; i++) {
      dump_node(out, page, trie.children[i], storage);
    }
  }
};

NullDumper null_dumper;
ValueDumper value_dumper;
LinkDumper link_dumper;
HeapLinkDumper heap_link_dumper;
CompressDumper compress_dumper;
TrieDumper trie_dumper;

DumpBase *dumpers[] = {
    &null_dumper,     &value_dumper, &link_dumper, &heap_link_dumper,
    &compress_dumper, &trie_dumper,  &trie_dumper,
};

void dump_node(std::ostream &out, Page *page, node_t nid, Storage *storage) {
  node_p *pie = page->get_ie(nid);
  dumpers[pie->type]->dump(out, page, nid, storage);
}

#endif

}  // namespace leaves
