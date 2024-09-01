#include "node.hpp"

#include "trace.hpp"

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

INLINE Node* TrieBlock::resolve(node_ptr ptr) const {
  return (Node*)&data[ptr.offset()];
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

  int index = trie.find(bit);
  if (index == -1) {
    trans.index = trie.next(bit);
    return false;
  }
  trans.index = index;
  ssize_t next_offset = trans.pnode->offset() + trie.offset(index);
  Transition next_trans(trans.offset, next_offset);
  if (trans.trie_state == Upper) next_trans.trie_state = Lower;
  cursor.stack.push_back(next_trans);
  return true;
}

template <class TrieClass>
bool _find_trie(Trace& cursor, Transition& trans) {
  if (trans.trie_state == Upper) {
    if (cursor.rest_key.empty()) {
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
  if (cursor.rest_key.size() && trans.node->value.child.val) {
    trans.index = -1;
    cursor.stack.push_back(trans.derive(Value::offset()));
    return true;
  }

  trans.index = 0;
  return false;
}

INLINE bool find_link(Trace& cursor, Transition& trans) {
  cursor.stack.push_back(Transition(trans.node->link, 0));
  return true;
}

INLINE bool find_compressed(Trace& cursor, Transition& trans) {
  const Compressed& node = trans.node->compressed;
  size_t size = std::min((size_t)node.size, cursor.rest_key.size());
  trans.index = sign(memcmp(node.key, cursor.rest_key.data(), size));
  if (!trans.index) {
    if (node.size == cursor.rest_key.size()) {
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
get_value
------------------------------------------------------
*/

INLINE Slice get_value_null(Trace& cursor, const Transition& trans) {
  return Slice();
}

// ! for wasm we need DataSlice that holds a smart pointer on the block
INLINE Slice get_value_value(Trace& cursor, const Transition& trans) {
  const Value& value = trans.node->value;
  if (value.size < Value::SMALL_SIZE) {
    return Slice(value.data, value.size);
  }
  block_ptr block = cursor.get_block(value.link);
  return Slice(block->value.data, value.size);
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
  if (bit::upper(ckey) == bit::upper(cursor.rest_key[0])) {
    cursor.rest_key = cursor.rest_key.advance(1);
    return true;
  }
  return false;
}

INLINE bool advance_value(Trace& cursor, const Transition& trans) {
  return !cursor.rest_key.empty();
}

INLINE bool advance_link(Trace& cursor, const Transition& trans) {
  return true;
}

INLINE bool advance_compressed(Trace& cursor, const Transition& trans) {
  const Compressed& node = trans.node->compressed;

  if (cursor.rest_key.size() < node.size) return false;

  if (memcmp(cursor.rest_key.data(), node.key, node.size)) return false;

  cursor.rest_key = cursor.rest_key.advance(node.size);
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
  ssize_t size = 0;
  const T& trie = T::cast(trans);
  int count = trie.count();
  for (int i = 0; i < count && !bm.is_finished; i++) {
    auto child = trans.derive(trie.offset(i));
    size += bm.find_splitpoint(child);
  }
  return size;
}

INLINE ssize_t find_splitpoint_bit_trie(BlockSplitter& bs, Transition& trans) {
  return find_splitpoint_trie<BitTrie>(bs, trans) + sizeof(BitTrie);
}

INLINE ssize_t find_splitpoint_trie(BlockSplitter& bs, Transition& trans) {
  return find_splitpoint_trie<Trie>(bs, trans) + sizeof(Trie);
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
  assert(0);
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
    if (cptr.type) dnode.children[i] = move[cptr.type](src, cptr, dest);
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
  if (snode.child.type) {
    dnode.child = move[snode.child.type](src, snode.child, dest);
  }
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
  assert(snode.child.type);
  dnode.child = move[snode.child.type](src, snode.child, dest);
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

  Value& node = cursor.resolve(result.ptr)->value;
  node.child.val = 0;
  node.size = value.size();

  if (value.size() <= Value::SMALL_SIZE) {
    memcpy(node.data, value.data(), node.size);
  } else {
    offset_ptr offset = cursor.storage.alloc_block(value.size());
    cursor.storage.write_value(offset, value);
    node.link = offset;
  }

  if (link_to_back.type == kValue) {
    // it is a replace => remove old data
    Value& old_val = old_node->value;
    if (old_val.size > Value::SMALL_SIZE) {
      cursor.storage.free_value(old_val.link);
    }
    back.block->trie.free(link_to_back.offset(),
                          Value::calc_alloc_size(old_val.size));
  } else if (link_to_back.type != kNull) {
    // add old back to value child
    assert(link_to_back.type != kLink);
    node.child = link_to_back;
  }

  return result.need_refresh;
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

  cursor.stack.push_back(back.derive(node.offset()).clear(cursor));
  cursor.current_key.append(node.key, node.size);
  cursor.rest_key.iadvance(node.size);

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
  T& trie = T::cast(cursor.back().node);

  back.found = true;

  char key = cursor.rest_key[0];
  if (back.trie_state == Upper) {
    back.index = trie.add(bit::upper(key));
    cursor.stack.push_back(back.derive(trie.offset(back.index)).clear(cursor));
    add_lower_bit_trie(cursor);
    return;
  }

  back.index = trie.add(bit::lower(key));
  cursor.rest_key.iadvance(1);
  cursor.current_key.append(key, 1);
  cursor.stack.push_back(back.derive(trie.offset(back.index)).clear(cursor));

  if (cursor.rest_key.size()) add_compressed(cursor);
}

void set_value_bit_trie(Trace& cursor, const Slice& value) {
  Transition* back(&cursor.back());

  BitTrie* btrie = &back->node->bit_trie;
  if (btrie->count() == 7) {
    auto aptr = cursor.alloc(sizeof(Trie), kTrie);
    if (aptr.need_refresh) {
      back = &cursor.back();
      btrie = &back->node->bit_trie;
    }
    ssize_t to_free = back->pnode->offset();
    *back->pnode = aptr.ptr;
    back->resolve(cursor);

    Trie& trie = back->node->trie;
    memset(&trie, 0, sizeof(trie));

    int val = btrie->first();
    for (int i = 0; val >= 0; i++) {
      trie.children[val] = btrie->children[i];
      val = btrie->next(val);
    }

    back->block->trie.free(to_free, sizeof(BitTrie));

    set_value_trie(cursor, value);
    return;
  }

  set_value_trie<BitTrie>(cursor, value);
}

void set_value_trie(Trace& cursor, const Slice& value) {
  set_value_trie<Trie>(cursor, value);
}

void set_value_value(Trace& cursor, const Slice& value) {
  assert(cursor.rest_key.size());
  Transition& back = cursor.back();
  cursor.stack.push_back(back.derive(Value::offset()).clear(cursor));
  add_compressed(cursor);
}

void set_value_link(Trace& cursor, const Slice& value) { assert(0); }

inline node_ptr add(node_ptr src, ssize_t delta, NodeType type) {
  return node_ptr((src.offset() + delta) >> 3, type);
}

void set_value_compressed(Trace& cursor, const Slice& value) {
  Transition* back(&cursor.back());
  Compressed* node(&back->node->compressed);
  Slice& rest_key = cursor.rest_key;

  ssize_t size = std::min(node->size, (ssize_t)cursor.rest_key.size());
  const char *np = node->key, *rp = rest_key.data();
  ssize_t split_index;
  char key_node, key_cursor;
  for (split_index = 0; split_index < node->size; split_index++) {
    if (*np != *rp) {
      key_node = *np;
      key_cursor = *rp;
      break;
    }
    np++;
    rp++;
  }

  cursor.current_key.append(rest_key.data(), split_index + 1);
  rest_key.iadvance(split_index + 1);

  ssize_t rest_size = node->size - split_index - 1;

  ssize_t space = Compressed::calc_alloc_size(rest_size) + sizeof(BitTrie) +
                  sizeof(BitTrie);

  bool two_lower = bit::upper(key_node) != bit::upper(key_cursor);
  if (two_lower) {
    // we need two lower bit tries
    space += sizeof(BitTrie);
  }
  auto result = cursor.alloc(space, kCompressed);

  if (result.need_refresh) {
    back = &cursor.back();
    node = &back->node->compressed;
  }

  node_ptr rest_ptr = result.ptr;
  Compressed& rest = cursor.resolve(rest_ptr)->compressed;
  rest.child = node->child;
  rest.size = rest_size;
  memcpy(rest.key, node->key + split_index + 1, rest_size);

  node_ptr upper_ptr =
      add(rest_ptr, Compressed::calc_alloc_size(rest_size), kBitTrie);
  Transition tupper = back->derive(node->offset());
  tupper.resolve(cursor);
  BitTrie& upper = tupper.node->bit_trie;

  cursor.stack.push_back(tupper);

  if (two_lower) {
    // resestablish the former node path
    node_ptr lower_node_ptr = add(upper_ptr, sizeof(BitTrie), kBitTrie);
    int index = upper.add(bit::upper(key_node));
    upper.children[index] = lower_node_ptr;
    Transition tlower_node = tupper.derive(BitTrie::offset(index));
    tlower_node.resolve(cursor);
    BitTrie& lower_node = tlower_node.node->bit_trie;
    index = lower_node.add(bit::lower(key_node));
    lower_node.children[index] = rest_ptr;

    // add the new path
    node_ptr lower_cursor_ptr = add(lower_node_ptr, sizeof(BitTrie), kBitTrie);
    index = upper.add(bit::upper(key_cursor));
    upper.children[index] = lower_cursor_ptr;
    Transition tlower_cursor = tupper.derive(BitTrie::offset(index));
    tlower_cursor.resolve(cursor);
    BitTrie& lower_cursor = tlower_cursor.node->bit_trie;
    index = lower_cursor.add(bit::lower(key_cursor));
    lower_cursor.children[index] = 0;
    cursor.stack.push_back(tlower_cursor);
  } else {
    // build trie structure
    node_ptr lower_ptr = add(upper_ptr, sizeof(BitTrie), kBitTrie);
    int index = upper.add(bit::upper(key_node));
    upper.children[index] = lower_ptr;
    Transition tlower = tupper.derive(BitTrie::offset(index));
    tlower.resolve(cursor);
    BitTrie& lower = tlower.node->bit_trie;
    index = lower.add(bit::lower(key_node));
    lower.children[index] = rest_ptr;

    index = lower.add(bit::lower(key_cursor));
    lower.children[index].val = 0;
    cursor.stack.push_back(tlower);
  }
  node_ptr free_ptr =
      add(*back->pnode, offsetof(Compressed, key) + split_index, kNull);
  back->block->trie.free(free_ptr.offset(), rest_size + 1);
  if (rest_key.size()) add_compressed(cursor);
}

}  // namespace leaves
