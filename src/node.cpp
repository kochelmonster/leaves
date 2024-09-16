#include "node.hpp"

#include "trace.hpp"

#ifdef DEBUG
#include <sstream>
#endif

namespace leaves {

namespace bit {
inline uint8_t upper(uint8_t value) { return value >> 4; }

inline uint8_t lower(uint8_t value) { return (value & 0x0F); }
}  // namespace bit

INLINE BitTrie& BitTrie::cast(Transition& trans) {
  return trans.node->bit_trie;
}

INLINE const BitTrie& BitTrie::cast(const Transition& trans) {
  return trans.node->bit_trie;
}

INLINE Trie& Trie::cast(Transition& trans) { return trans.node->trie; }

INLINE const Trie& Trie::cast(const Transition& trans) {
  return trans.node->trie;
}

/*
is_valid
------------------------------------------------------
*/
INLINE ssize_t is_valid_null(const Transition& trans) { return false; }

INLINE ssize_t is_valid_value(const Transition& trans) {
  return trans.index == 0;
}

/*
find
------------------------------------------------------
*/
INLINE bool find_null(Trace& cursor, Transition& trans) { return false; }

template <class TrieClass>
bool _find_inside_trie(Trace& cursor, Transition& trans, uint8_t bit) {
  const TrieClass& trie(TrieClass::cast(trans));

  trans.index = trie.find(bit);
  if (trans.index == -1) {
    trans.index = trie.next(bit);
    return false;
  }
  Transition next_trans(trans.derive(trie.offset(trans.index)));
  if (trans.trie_state == Upper) next_trans.trie_state = Lower;
  cursor.stack.push_back(next_trans);
  return true;
}

template <class TrieClass>
bool _find_trie(Trace& cursor, Transition& trans) {
  if (trans.trie_state == Upper) {
    if (cursor.rest_key.empty()) {
      cursor.stack.pop_back();
      return false;
    }
    return _find_inside_trie<TrieClass>(cursor, trans,
                                        bit::upper(cursor.rest_key[0]));
  }

  assert(cursor.rest_key.size());
  char key = cursor.rest_key[0];
  if (!_find_inside_trie<TrieClass>(cursor, trans, bit::lower(key)))
    return false;

  cursor.rest_key.iadvance(1);
  cursor.current_key.push_back(key);
  return true;
}

INLINE bool find_bit_trie(Trace& cursor, Transition& trans) {
  return _find_trie<BitTrie>(cursor, trans);
}

INLINE bool find_trie(Trace& cursor, Transition& trans) {
  return _find_trie<Trie>(cursor, trans);
}

INLINE bool find_value(Trace& cursor, Transition& trans) {
  if (cursor.rest_key.size() && trans.node->value.child.moffset) {
    trans.index = -1;
    cursor.stack.push_back(trans.derive(Value::offset()));
    return true;
  }

  trans.index = 0;
  return false;
}

INLINE bool find_link(Trace& cursor, Transition& trans) {
  cursor.stack.push_back(Transition(trans.node->link, 0));
  cursor.stack.back().trie_state = trans.trie_state;
  return true;
}

INLINE bool find_compressed(Trace& cursor, Transition& trans) {
  const Compressed& node = trans.node->compressed;
  size_t size = std::min((size_t)node.size, cursor.rest_key.size());
  trans.index = sign(memcmp(node.key, cursor.rest_key.data(), size));
  if (!trans.index) {
    if (node.size <= cursor.rest_key.size()) {
      cursor.current_key.append(node.key, node.size);
      cursor.rest_key.iadvance(node.size);
      cursor.stack.push_back(trans.derive(node.offset()));
      return true;
    }
    if (node.size < cursor.rest_key.size())
      trans.index = 1;
    else
      trans.index = -1;
  }
  return false;
}

/*
advance
------------------------------------------------------
*/

INLINE bool advance_null(Trace& cursor, const Transition& trans) {
  return false;
}

INLINE bool advance_trie(Trace& cursor, const Transition& trans) {
  char ckey = cursor.current_key[trans.keypos];
  if (trans.trie_state == Upper) {
    if (cursor.rest_key.empty()) return false;
    return bit::upper(ckey) == bit::upper(cursor.rest_key[0]);
  }
  if (bit::lower(ckey) == bit::lower(cursor.rest_key[0])) {
    cursor.rest_key.iadvance(1);
    return true;
  }
  return false;
}

INLINE bool advance_value(Trace& cursor, const Transition& trans) {
  return true;
}

INLINE bool advance_link(Trace& cursor, const Transition& trans) {
  return true;
}

INLINE bool advance_compressed(Trace& cursor, const Transition& trans) {
  const Compressed& node = trans.node->compressed;

  if (cursor.rest_key.size() < node.size) return false;

  if (memcmp(cursor.rest_key.data(), node.key, node.size)) return false;

  cursor.rest_key.iadvance(node.size);
  return true;
}

/*
get_size
------------------------------------------------------
*/
ssize_t get_size_null(const Transition& trans) { return 0; }

ssize_t get_size_bit_trie(const Transition& trans) {
  const BitTrie& trie = trans.node->bit_trie;
  size_t count = trie.count();
  if (count <= 3) return sizeof(trie.bits) + sizeof(node_ptr) * 3;
  assert(count <= 17);
  return sizeof(trie.bits) + sizeof(node_ptr) * 7;
}

ssize_t get_size_trie(const Transition& trans) { return sizeof(node_ptr) * 16; }

ssize_t get_size_value(const Transition& trans) {
  Value& node = trans.node->value;
  if (node.size <= Value::SMALL_SIZE)
    return sizeof(Value) + std::max(node.size, sizeof(offset_ptr)) -
           sizeof(offset_ptr);
  return sizeof(Value);
}

ssize_t get_size_link(const Transition& trans) { return sizeof(offset_ptr); }

ssize_t get_size_compressed(const Transition& trans) {
  const Compressed& node = trans.node->compressed;
  return std::max(sizeof(offset_ptr), sizeof(Compressed) + node.size);
}

/*
mark_deep_size
------------------------------------------------------
*/
INLINE ssize_t find_splitpoint_null(BlockSplitter& bs, Transition& trans) {
  return 0;
}

template <class T>
ssize_t find_splitpoint_trie(BlockSplitter& bs, Transition& trans) {
  ssize_t size = sizeof(T);
  const T& trie = T::cast(trans);
  int count = trie.count();
  for (int i = 0; i < count && !bs.is_finished; i++) {
    auto child = trans.derive(trie.offset(i));
    size += bs.find_splitpoint(child);
  }
  return size;
}

INLINE ssize_t find_splitpoint_bit_trie(BlockSplitter& bs, Transition& trans) {
  return find_splitpoint_trie<BitTrie>(bs, trans);
}

INLINE ssize_t find_splitpoint_trie(BlockSplitter& bs, Transition& trans) {
  return find_splitpoint_trie<Trie>(bs, trans);
}

INLINE ssize_t find_splitpoint_value(BlockSplitter& bs, Transition& trans) {
  Value& value = trans.node->value;
  ssize_t size = value.size;
  if (value.child.moffset) {
    auto child = trans.derive(value.offset());
    size += bs.find_splitpoint(child);
  }
  return size;
}

INLINE ssize_t find_splitpoint_link(BlockSplitter& bs, Transition& trans) {
  return sizeof(offset_ptr);
}

INLINE ssize_t find_splitpoint_compressed(BlockSplitter& bs,
                                          Transition& trans) {
  Compressed& node = trans.node->compressed;
  auto child = trans.derive(node.offset());
  return node.size + bs.find_splitpoint(child);
}

/*
move
------------------------------------------------------
*/

node_ptr move_null(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  return node_ptr();
}

node_ptr move_bit_trie(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  node_ptr result = dest->alloc(sizeof(BitTrie), kBitTrie);
  BitTrie& dnode = dest->resolve(result)->bit_trie;
  BitTrie& snode = src->resolve(psrc)->bit_trie;
  dnode.bits = snode.bits;
  int count = snode.count();
  for (int i = 0; i < count; i++) {
    node_ptr& cptr = snode.children[i];
    dnode.children[i] = move[cptr.type](src, cptr, dest);
  }
  return result;
}

node_ptr move_trie(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  node_ptr result = dest->alloc(sizeof(Trie), kTrie);
  Trie& dnode = dest->resolve(result)->trie;
  Trie& snode = src->resolve(psrc)->trie;
  int count = snode.count();
  for (int i = 0; i < count; i++) {
    node_ptr& cptr = snode.children[i];
    if (cptr.type) 
      dnode.children[i] = move[cptr.type](src, cptr, dest);
    else
      dnode.children[i] = cptr;
  }
  return result;
}

node_ptr move_value(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  Value& snode = src->resolve(psrc)->value;
  ssize_t size = Value::calc_alloc_size(snode.size);
  node_ptr result = dest->alloc(size, kValue);
  Value& dnode = dest->resolve(result)->value;
  dnode.size = snode.size;
  memcpy(&dnode, &snode, size);
  if (snode.child.moffset)
    dnode.child = move[snode.child.type](src, snode.child, dest);
  return result;
}

node_ptr move_link(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  node_ptr result = dest->alloc(sizeof(offset_ptr), kLink);
  dest->resolve(result)->link = src->resolve(psrc)->link;
  return result;
}

node_ptr move_compressed(TrieBlock* src, node_ptr psrc, TrieBlock* dest) {
  Compressed& snode = src->resolve(psrc)->compressed;
  ssize_t size = Compressed::calc_alloc_size(snode.size);
  node_ptr result = dest->alloc(size, kCompressed);
  Compressed& dnode = dest->resolve(result)->compressed;
  memcpy(&dnode, &snode, size);
  if (snode.child.type) {
    dnode.child = move[snode.child.type](src, snode.child, dest);
  }
  return result;
}

/*
set_value
------------------------------------------------------
*/

INLINE bool add_value(Trace& cursor, const Slice& value) {
  assert(cursor.rest_key.empty());

  auto result = cursor.alloc(Value::calc_alloc_size(value.size()), kValue);
  Transition& back(cursor.back());
  Node* old_node = back.node;
  node_ptr link_to_back(back.pnode->val);

  // set back to the value
  *back.pnode = result.ptr;
  back.resolve(cursor);  // back now points to new value
  back.index = 0;

  Value& node = back.node->value;
  node.child = link_to_back;
  node.size = value.size();

  if (value.size() <= Value::SMALL_SIZE) {
    memcpy(node.data, value.data(), node.size);
  } else {
    node.link = cursor.storage.write_value(value);
  }

  if (link_to_back.type == kValue) {
    // it is a replace => remove old data
    Value& old_val = old_node->value;
    if (old_val.size > Value::SMALL_SIZE) {
      cursor.storage.free_block(cursor.storage.get_block(old_val.link));
    }
    node.child = old_val.child;
    return true;
  }

  return false;
}

INLINE bool add_compressed(Trace& cursor) {
  assert(cursor.rest_key.size());

  auto result = cursor.alloc(
      Compressed::calc_alloc_size(cursor.rest_key.size()), kCompressed);
  Transition& back(cursor.back());
  *back.pnode = result.ptr;
  back.resolve(cursor);
  back.index = 0;
  Compressed& node = back.node->compressed;
  node.size = cursor.rest_key.size();
  memcpy(node.key, cursor.rest_key.data(), node.size);

  cursor.current_key.append(node.key, node.size);
  cursor.rest_key.iadvance(node.size);
  cursor.stack.push_back(back.derive(node.offset()).clear(cursor));

  return result.need_refresh;
}

INLINE void add_lower_bit_trie(Trace& cursor) {
  assert(cursor.rest_key.size());

  auto result = cursor.alloc(sizeof(BitTrie), kBitTrie);
  char key(cursor.rest_key[0]);
  Transition& back = cursor.back();

  *back.pnode = result.ptr;
  back.resolve(cursor);

  BitTrie& node(back.node->bit_trie);
  back.trie_state = Lower;
  back.index = node.add(bit::lower(cursor.rest_key[0]));

  cursor.rest_key.iadvance(1);
  cursor.current_key.push_back(key);
  cursor.stack.push_back(back.derive(node.offset(back.index)).clear(cursor));

  if (cursor.rest_key.size()) add_compressed(cursor);
}

INLINE void set_value_null(Trace& cursor, const Slice& value) {
  assert(cursor.rest_key.size());
  add_compressed(cursor);
}

template <class T>
void set_value_trie(Trace& cursor, const Slice& value) {
  Transition& back = cursor.back();
  T& trie = T::cast(cursor.back());

  char key = cursor.rest_key[0];
  if (back.trie_state == Upper) {
    back.index = trie.add(bit::upper(key));
    cursor.stack.push_back(back.derive(trie.offset(back.index)).clear(cursor));
    add_lower_bit_trie(cursor);
    return;
  }

  back.index = trie.add(bit::lower(key));
  cursor.rest_key.iadvance(1);
  cursor.current_key.push_back(key);
  cursor.stack.push_back(back.derive(trie.offset(back.index)).clear(cursor));

  if (cursor.rest_key.size()) add_compressed(cursor);
}

INLINE void set_value_bit_trie(Trace& cursor, const Slice& value) {
  Transition* back(&cursor.back());

  BitTrie* btrie = &back->node->bit_trie;
  if (btrie->count() == 7) {
    auto aptr = cursor.alloc(sizeof(Trie), kTrie);
    if (aptr.need_refresh) {
      back = &cursor.back();
      btrie = &back->node->bit_trie;
    }

    *back->pnode = aptr.ptr;
    back->resolve(cursor);

    Trie& trie = back->node->trie;
    int val = btrie->first();
    for (int i = 0; val >= 0; i++) {
      trie.children[val] = btrie->children[i];
      val = btrie->next(val);
    }

    set_value_trie(cursor, value);
    return;
  }

  set_value_trie<BitTrie>(cursor, value);
}

INLINE void set_value_trie(Trace& cursor, const Slice& value) {
  set_value_trie<Trie>(cursor, value);
}

INLINE void set_value_value(Trace& cursor, const Slice& value) {
  assert(cursor.rest_key.size());
  Transition& back = cursor.back();
  cursor.stack.push_back(back.derive(Value::offset()).clear(cursor));
  add_compressed(cursor);
}

INLINE void set_value_link(Trace& cursor, const Slice& value) { assert(0); }

inline node_ptr add(node_ptr src, ssize_t delta, NodeType type) {
  return node_ptr((src.offset() + delta) >> 3, type);
}

INLINE void set_value_compressed(Trace& cursor, const Slice& value) {
  Transition* back(&cursor.back());
  Compressed* node(&back->node->compressed);
  Slice& rest_key = cursor.rest_key;

  ssize_t size = std::min(node->size, (ssize_t)cursor.rest_key.size());
  const char *np = node->key, *rp = rest_key.data();
  ssize_t split_index;
  char key;
  for (split_index = 0; split_index < size; split_index++) {
    if (*np != *rp) {
      key = *np;
      break;
    }
    np++;
    rp++;
  }

  if (split_index == size) {
    /* the node must be splitted into two compressed node with a value in
       between [abcdefg] => [ab] -> [value] -> [cdefg]
     */

    assert(rest_key.size() < node->size);
    assert(rest_key.size() == size);
    // otherwise we would be at a child of this node

    cursor.current_key.append(rest_key.data(), size);
    rest_key.iadvance(size);

    ssize_t rest_size = node->size - size;

    auto result =
        cursor.alloc(Compressed::calc_alloc_size(rest_size), kCompressed);
    if (result.need_refresh) {
      back = &cursor.back();
      node = &back->node->compressed;
    }

    /* must be behind alloc (if node is moved
       and refound, size must be original) */
    node->size = size;

    node_ptr child(node->child);
    node->child = result.ptr;
    Transition trest = back->derive(node->offset()).resolve(cursor);
    Compressed& rest = trest.node->compressed;
    rest.child = child;
    rest.size = rest_size;
    memcpy(rest.key, np, rest_size);
    cursor.stack.push_back(trest);

    // Let Trace do the rest
    return;
  }

  /* the node must be splitted in two compressed nodes with a trie in between
      [abcdefg] => [ab] -> TU[c] -> TL[c] -> [defg]
  */

  ssize_t rest_size = node->size - split_index - 1;

  ssize_t space = 2 * sizeof(BitTrie);
  if (rest_size) space += Compressed::calc_alloc_size(rest_size);

  auto result = cursor.alloc(space, kBitTrie);
  if (result.need_refresh) {
    back = &cursor.back();
    node = &back->node->compressed;
  }
  node->size = split_index;  // see upper comment

  node_ptr trie_child, upper_ptr(result.ptr);
  node_ptr lower_ptr(add(upper_ptr, sizeof(BitTrie), kBitTrie));

  if (rest_size) {
    trie_child = add(lower_ptr, sizeof(BitTrie), kCompressed);
    Compressed& rest = cursor.resolve(trie_child)->compressed;
    memcpy(rest.key, node->key + split_index + 1, rest_size);
    rest.child = node->child;
    rest.size = rest_size;
  } else {
    /* no rest key left
       [abcdefg] => [ab] -> TU[c] -> TL[c]
     */
    upper_ptr.type = kBitTrie;
    trie_child = node->child;
  }

  // insert TrieUpper(TU) and TrieLower (TL) as child of node
  node->child = upper_ptr;

  Transition tupper = back->derive(node->offset()).resolve(cursor);
  BitTrie& upper = tupper.node->bit_trie;
  tupper.index = upper.add(bit::upper(key));
  upper.children[tupper.index] = lower_ptr;

  BitTrie& lower = cursor.resolve(lower_ptr)->bit_trie;
  lower.children[lower.add(bit::lower(key))] = trie_child;

  // insert TrieUpper(TU) and TrieLower (TL)
  if (split_index) {
    cursor.stack.push_back(tupper);
  } else {
    // the trie is at the begining -> skip node
    *back->pnode = upper_ptr;
    back->resolve(cursor);
  }

  cursor.current_key.append(rest_key.data(), split_index);
  rest_key.iadvance(split_index);

  // walk down the inserted tries
  while (cursor.stack.back().find(cursor));

  // Let Trace do the rest
}

#ifdef DEBUG

size_t dump_node(std::ostream& out, const TrieBlock* block, node_ptr pnode,
                 DBMemory* storage, int upper);

const char* handler_names[] = {"kNull",  "kBitTrie", "kTrie",
                               "kValue", "kLink",    "kCompressed"};

INLINE std::string idstr(const TrieBlock* block, node_ptr ptr) {
  std::stringstream cstr;
  cstr << handler_names[ptr.type] << "-" << ptr.moffset << "-" << block->offset;
  return cstr.str();
}

INLINE std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit)) {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}

INLINE void dump_id_space(std::ostream& out, const TrieBlock* block,
                          node_ptr pnode) {
  out << "id: " << idstr(block, pnode) << std::endl;
  out << "pspace: " << TrieBlock::DATA_SIZE - block->used << std::endl;
}

INLINE size_t dump_node_null(std::ostream& out, const TrieBlock* block,
                             node_ptr pnode, DBMemory* storage, int upper) {
  dump_id_space(out, block, pnode);
  out << "---" << std::endl;
  return 0;
}

INLINE size_t dump_node_link(std::ostream& out, const TrieBlock* block,
                             node_ptr pnode, DBMemory* storage, int upper) {
  dump_id_space(out, block, pnode);

  offset_ptr link = block->resolve(pnode)->link;
  out << "link: " << link << std::endl;

  const TrieBlock* next_block = &storage->get_block(link)->trie;
  if (storage->transaction_active()) {
    next_block = &storage->get_block(link)->trie;
  }

  node_ptr root = *next_block->resolve_ptr(0);
  out << "children: " << std::endl;
  out << "  - " << idstr(next_block, root) << std::endl;
  out << "---" << std::endl;
  return dump_node(out, next_block, root, storage, -1);
}

INLINE size_t dump_node_bittrie(std::ostream& out, const TrieBlock* block,
                                node_ptr pnode, DBMemory* storage, int upper) {
  const BitTrie& trie = block->resolve(pnode)->bit_trie;
  int size = trie.count();
  dump_id_space(out, block, pnode);
  out << "upper: " << (upper < 0 ? "True" : "False") << std::endl;
  out << "size: " << size << std::endl;
  out << "bits: " << std::hex << trie.bits << std::dec << std::endl;

  int indizes[17];
  out << "bitindex: [";
  unsigned int bits = trie.bits;
  int index = 0, i = 0;
  while (bits) {
    index = ctz(bits);
    out << index;
    indizes[i++] = index;
    bits &= ~(1 << index);
    if (bits) {
      out << ", ";
    }
  }
  out << "]" << std::endl;

  if (upper >= 0) {
    upper <<= 4;
    out << "byteindex: [";
    unsigned int bits = trie.bits;
    int index = 0;
    while (bits) {
      index = ctz(bits);
      out << '"';
      out << bitstr((char)(upper | index));
      out << '"';
      bits &= ~(1 << index);
      if (bits) {
        out << ", ";
      }
    }
    out << "]" << std::endl;
  }

  out << "children: " << std::endl;
  for (int i = 0; i < size; i++) {
    out << "  - " << idstr(block, trie.children[i]) << std::endl;
  }
  out << "---" << std::endl;
  /* upper < 0 means this trie object is the upper 4 bits
     otherwise it is the lower 4bits
   */
  size_t value_count = 0;
  for (int i = 0; i < size; i++) {
    int u = upper < 0 ? indizes[i] : -1;
    value_count += dump_node(out, block, trie.children[i], storage, u);
  }
  return value_count;
}

INLINE size_t dump_node_trie(std::ostream& out, const TrieBlock* block,
                             node_ptr pnode, DBMemory* storage, int upper) {
  const Trie& trie = block->resolve(pnode)->trie;
  int size = trie.count();
  dump_id_space(out, block, pnode);
  out << "upper: " << (upper < 0 ? "True" : "False") << std::endl;

  if (upper >= 0) {
    upper <<= 4;
    bool first = true;
    out << "bytes: [";
    for (int i = 0; i < 16; i++) {
      if (trie.children[i].moffset) {
        if (!first) {
          out << ", ";
        }
        first = false;
        out << '"';
        out << bitstr((char)(upper | i));
        out << '"';
      }
    }
    out << "]" << std::endl;
  } else {
    bool first = true;
    out << "bytes: [";
    for (int i = 0; i < 16; i++) {
      if (trie.children[i].moffset) {
        if (!first) {
          out << ", ";
        }
        first = false;
        out << '"';
        out << bitstr((char)i);
        out << '"';
      }
    }
    out << "]" << std::endl;
  }

  out << "children: " << std::endl;
  for (int i = 0; i < size; i++) {
    if (trie.children[i].moffset) {
      out << "  - " << idstr(block, trie.children[i]) << std::endl;
    }
  }
  out << "---" << std::endl;
  size_t value_count = 0;
  for (int i = 0; i < size; i++) {
    if (trie.children[i].moffset) {
      int u = upper < 0 ? i : -1;
      value_count += dump_node(out, block, trie.children[i], storage, u);
    }
  }
  return value_count;
}

INLINE size_t dump_node_value(std::ostream& out, const TrieBlock* block,
                              node_ptr pnode, DBMemory* storage, int upper) {
  const Value& node = block->resolve(pnode)->value;
  dump_id_space(out, block, pnode);
  out << "size: " << (int)node.size << std::endl;
  out << "value: \"";

  const char* p;
  if (node.size <= Value::SMALL_SIZE) {
    p = node.data;
  } else {
    p = storage->get_block(node.link)->value.data;
  }

  for (size_t i = 0; i < std::min(node.size, (size_t)10); i++) {
    out << "[" << bitstr(p[i]) << "]";
  }
  out << "\"" << std::endl;
  if (node.child.moffset) {
    out << "children: " << std::endl;
    out << "  - " << idstr(block, node.child) << std::endl;
    out << "---" << std::endl;
    return dump_node(out, block, node.child, storage, -1) + 1;
  }

  out << "children: []" << std::endl;
  out << "---" << std::endl;
  return 1;
}

INLINE size_t dump_node_compressed(std::ostream& out, const TrieBlock* block,
                                   node_ptr pnode, DBMemory* storage,
                                   int upper) {
  const Compressed& node = block->resolve(pnode)->compressed;
  dump_id_space(out, block, pnode);
  out << "size: " << (int)node.size << std::endl;
  out << "keys: \"";
  for (int i = 0; i < node.size; i++) {
    out << "[" << bitstr(node.key[i]) << "]";
  }
  out << "\"" << std::endl;
  out << "children: " << std::endl;
  out << "  - " << idstr(block, node.child) << std::endl;
  out << "---" << std::endl;
  return dump_node(out, block, node.child, storage, -1);
}

typedef size_t (*dump_node_t)(std::ostream& out, const TrieBlock* block,
                              node_ptr pnode, DBMemory* storage, int upper);

const dump_node_t dump_node_v[] = {dump_node_null, dump_node_bittrie,
                                   dump_node_trie, dump_node_value,
                                   dump_node_link, dump_node_compressed};

INLINE size_t dump_node(std::ostream& out, const TrieBlock* block,
                        node_ptr pnode, DBMemory* storage, int upper) {
  return dump_node_v[pnode.type](out, block, pnode, storage, upper);
}

#endif

}  // namespace leaves
