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

INLINE Trie& Trie::cast(Transition& trans) { return trans.node->trie; }

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
  ssize_t next_offset = trans.onode + trie.offset(index);
  cursor.stack.push_back(Transition(trans.offset, next_offset));
  return true;
}

template <class TrieClass>
bool _find_trie(Trace& cursor, Transition& trans) {
  if (cursor.trie_state == Upper) {
    trans.trie_state = Upper;
    if (cursor.rest_key.empty()) return false;
    cursor.trie_state = Lower;
    return _find_inside_trie<TrieClass>(cursor, trans,
                                        bit::upper(cursor.rest_key[0]));
  }

  assert(cursor.rest_key.size());
  trans.trie_state = Lower;
  char key = cursor.rest_key[0];

  if (!_find_inside_trie<TrieClass>(cursor, trans, bit::lower(key)))
    return false;

  cursor.trie_state = Upper;
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
    ssize_t next_offset = trans.onode + offsetof(Value, child);
    cursor.stack.push_back(Transition(trans.offset, next_offset));
    return true;
  }

  trans.index = 0;
  return false;
}

INLINE bool find_link(Trace& cursor, Transition& trans) {
  cursor.stack.push_back(Transition(trans.node->link, 0));
  return true;
}

INLINE bool find_compressed(Trace& cursor, Transition& trans,
                            const TrieBlock& block) {
  const Compressed& node = trans.node->compressed;
  size_t size = std::min((size_t)node.size, cursor.rest_key.size());
  if (size < cursor.rest_key.size()) {
    trans.index = -1;
    return false;
  }

  trans.index = sign(memcmp(node.key, cursor.rest_key.data(), size));
  ssize_t next_offset = trans.onode + offsetof(Compressed, child);
  cursor.current_key.append(node.key, node.size);
  cursor.rest_key.iadvance(node.size);
  cursor.stack.push_back(Transition(trans.offset, next_offset));
  return true;
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
INLINE ssize_t mark_deep_size_null(Trace& cursor, const Transition& trans,
                                   TrieBlock& sizes) {
  return 0;
}

template <class T>
ssize_t mark_deep_size_trie(Trace& cursor, const Transition& trans,
                            TrieBlock& sizes) {
  ssize_t size = 0;
  T& trie = T::cast(trans);
  int count = trie.count();
  for (int i = 0; i < count; i++) {
    Transition child(trans.offset, trans.onode + trie.offset(i));
    size += child.mark_deep_size(cursor, sizes);
  }
  return size;
}

INLINE ssize_t mark_deep_size_trie(Trace& cursor, const Transition& trans,
                                   TrieBlock& sizes) {
  return mark_deep_size_trie<Trie>(cursor, trans, sizes);
}

INLINE ssize_t mark_deep_size_bit_trie(Trace& cursor, const Transition& trans,
                                       TrieBlock& sizes) {
  return mark_deep_size_trie<BitTrie>(cursor, trans, sizes);
}

INLINE ssize_t mark_deep_size_value(Trace& cursor, const Transition& trans,
                                    TrieBlock& sizes) {
  Value& value = trans.node->value;
  ssize_t size = value.size;
  if (value.child.offset) {
    Transition child(trans.offset, value.offset());
    size += child.mark_deep_size(cursor, sizes);
  }
  return size;
}

INLINE ssize_t mark_deep_size_link(Trace& cursor, const Transition& trans,
                                   TrieBlock& sizes) {
  return sizeof(offset_ptr);
}

INLINE ssize_t mark_deep_size_compressed(Trace& cursor, const Transition& trans,
                                         TrieBlock& sizes) {
  Compressed& node = trans.node->compressed;
  Transition child(trans.offset, node.offset());
  return node.size + child.mark_deep_size(cursor, sizes);
}

/*
set_value
------------------------------------------------------
*/

inline void create_leaf(Trace& cursor, Transition& trans, ssize_t onode,
                        const Slice& value) {
  if (cursor.rest_key.empty())
    create_value(cursor, trans, onode, value);
  else
    create_table(cursor, trans, onode, value);
}

INLINE void set_value_null(Trace& cursor, Transition& trans,
                           const Slice& value) {
  create_leaf(cursor, trans, 0, value);
}

template <class T> void set_value_trie(Trace& cursor, Transition& trans, const Slice& value) {
  ssize_t keypos = trans.keypos;
  char key = cursor.rest_key[0];

  T& trie = T::cast(trans.node);

  if (trans.trie_state == Lower) {
    // Grow !!
    trans.index = trie.add(bit::lower(key));
    create_leaf(cursor, trans, trie.offset(trans.index), value);
  }
  else {
    trans.index = trie.add(bit::upper(key));


    Transition& back =
        cursor.alloc(BitTrie::size(1), trie.offset(trans.index), kBitTrie);
    back.trie_state = Lower;
    back.keypos = keypos;
    BitTrie& lower = back.node->bit_trie;
  }
  
  cursor.rest_key = cursor.rest_key.advance(1);
  cursor.current_key.append(key, 1);

  // Grow!!
  back.index = lower.add(bit::lower(key));
  create_leaf(cursor, back, lower.offset(back.index), value);
}



void set_value_trie(Trace& cursor, Transition& trans, const Slice& value) {
  Trie& trie = trans.node->trie;
  assert(!trie.children[trans.index].offset);  // Replaces only in Value nodes

  ssize_t keypos = trans.keypos;

  if (trans.trie_state == Lower) {
    create_leaf(cursor, trans, trie.offset(trans.index), value);
    return;
  }

  Transition& back =
      cursor.alloc(BitTrie::size(1), trie.offset(trans.index), kBitTrie);
  back.trie_state = Lower;
  back.keypos = keypos;
  BitTrie& lower = back.node->bit_trie;
  char key = cursor.rest_key[0];
  cursor.rest_key = cursor.rest_key.advance(1);
  cursor.current_key.append(key, 1);
  back.index = lower.add(bit::lower(key));
  create_leaf(cursor, back, lower.offset(back.index), value);
}


void set_value_value(Trace& cursor, Transition& trans, const Slice& value);
void set_value_link(Trace& cursor, Transition& trans, const Slice& value);
void set_value_compressed(Trace& cursor, Transition& trans, const Slice& value);

/*
create
------------------------------------------------------
*/

INLINE void create_value(Trace& cursor, Transition& trans, ssize_t onode,
                         const Slice& value) {
  if (value.size() > Value::SMALL_SIZE) {
    Transition& back(cursor.alloc(sizeof(Value), onode, kValue));
    back.resolve(cursor);
    Value& node = back.node->value;
    offset_ptr offset = cursor.storage.alloc_block(value.size());
    cursor.storage.write_value(offset, value);
    node.child.val = 0;
    node.link = offset;
    node.size = value.size();
    return;
  }
  Transition& back(
      cursor.alloc(Value::calc_struct_size(value.size()), onode, kValue));
  Value& node = back.node->value;
  node.child.val = 0;
  node.size = value.size();
  memcpy(node.data, value.data(), node.size);
}

#if 0
INLINE node_ptr add_compressed(Trace& cursor, Transition& trans,
                               const Slice& value) {
  ssize_t size =
      sizeof(Compressed) + std::min(cursor.rest_key.size(), (size_t)256);
  Transition& end = cursor.alloc(size, kCompressed);
  Compressed& node = end.node->compressed;
  node.size = size;
  memcpy(node.key, cursor.rest_key.data(), size);
  cursor.rest_key = cursor.rest_key.advance(size);
  // if (cursor.rest_key.size())

  return end.node_ptr

             BlockUnion *
             block = cursor.storage.alloc_cow_block();
  ssize_t size = leaves::get_size[trans.upper.pnode->type](trans);
  trans.block->trie.free(trans.upper.pnode->offset + TrieBlock::MIN_SIZE,
                         size - TrieBlock::MIN_SIZE);
  trans.upper.pnode->type = kLink;
}
else {
  trans.upper.pnode->type = kCompressed;
  trans.upper.pnode->offset = offset;
}
}
#endif

}  // namespace leaves
