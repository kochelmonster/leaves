// Trie Nodes
#ifndef _LEAVES__NODE_HPP
#define _LEAVES__NODE_HPP

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

/*
Generally the Tries are a smart version of a compressed trie
(https://www.geeksforgeeks.org/compressed-tries/)


Branch Node
===========

consist of a

- compressed part ["prefix"]
- a branch part


The layout is
------------------------
BlockHeader
------------------------
branch node header
------------------------
compressed:size,data
------------------------
branch meta (Array, Bit)
------------------------
Link Branch1
------------------------
Link Branch2
------------------------
....
-----------------------
Link BranchN
------------------------
Link None Branch
------------------------

the None Branch link is for a key that ends before the branch.
Example:
-------------------------
[thequick]  <- compressed
-------------------------
b|g|r|y    <- branches
-------------------------
Link for b (A)
-------------------------
Link for g (B)
-------------------------
Link for r (C)
-------------------------
Link for y (D)
-------------------------
Link for None Branch (E)
-------------------------

the key: thequickb -> follows Link A
the key: thequickr -> follows Link C
the key: thequick -> follows Link E



Leaf Node
=========

Leaf Node aggregates all leafes of a branch node
their layout is:

----------------
BlockHeader
----------------
leaf node header
----------------
leaf node 1:
key_size
value_size
key_value_data
----------------
leaf node 2:
key_size
value_size
key_value_data
----------------

Leaf Nodes

  - [L] The rest key (suffix) and value.

  A block can contain multiple leaf nodes. Leaf nodes grow from the end
  of the block to the start. In this way, branch nodes can grow without
  moving leaf nodes.

A block that contains branch nodes is called branch block in contrary
to a leaf block that only contains leaf nodes and is associated to a
branch block. (Be aware a branch block can also contain leaf nodes!)

a branch block can have size from 256 to to 4096 Bytes.
leaf blocks can be up to 4G.

The layout is CPU cache friendly
*/

template <typename BlockHeader>
struct _Compressed {
  using bsize_t = typename BlockHeader::bsize_t;

  bsize_t size;
  uint8_t key[0];

  template <typename Cursor>
  bool find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    size_t same = get_prefix(cursor.rest_key.data(), (const char*)key,
                             cursor.rest_key.size(), size, back.cmp);
    back.prefix = same;
    cursor._advance_key(same);
    return same == size;
  }

  template <typename Cursor>
  uint16_t step(Cursor& cursor) const {  // for first, next, prev, last
    cursor.stack.back().prefix = size;
    cursor.current_key.append((char*)key, size);
    return nodesize();
  }

  static bsize_t nodesize(bsize_t s) {
    return s ? align(sizeof(_Compressed) + s) : 0;
  }

  bsize_t nodesize() const { return nodesize(size); }
};

/*
A Branch for up to 15 keys
*/
template <typename BlockHeader>
struct _ArrayBranch {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;

  const static size_t COUNT = 15;
  uint8_t size;
  uint8_t keys[COUNT];
  offset_t links[0];

  template <typename Cursor>
  const offset_t* find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();

    assert(size <= _ArrayBranch::COUNT);
    back.branch_key = cursor.rest_key[0];
    char i = 0;
    for (; i < size; i++) {
      if (keys[i] == back.branch_key) {
        cursor._advance_key(1);
        return &links[i];
      }
    }
    back.cmp = Cursor::Transition::NOT_SAME;
    return nullptr;
  }

  template <typename Cursor>
  const offset_t* first(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.branch_key = 0xff;
    char imin = 0;
    for (char i = 0; i < size; i++) {
      if (keys[i] <= back.branch_key) {
        back.branch_key = keys[i];
        imin = i;
      }
    }
    cursor.current_key.push_back(back.branch_key);
    return &links[imin];
  }

  template <typename Cursor>
  const offset_t* last(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.branch_key = 0;
    char ilast = -1;
    for (char i = 0; i < size; i++) {
      if (keys[i] >= back.branch_key) {
        back.branch_key = keys[i];
        ilast = i;
      }
    }
    cursor.current_key.push_back(back.branch_key);
    return &links[ilast];
  }

  template <typename Cursor>
  const offset_t* next(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint8_t max = 0xff, min = back.branch_key;
    char inext = -1;
    for (char i = 0; i < size; i++) {
      if (keys[i] > min && keys[i] <= max) {
        max = back.branch_key = keys[i];
        inext = i;
      }
    }
    if (inext >= 0) {
      cursor._changed_branch_key();
      return &links[inext];
    }
    /* cursor.current_key.pop_back(); not needed:
      this node will be removed from stack and
      current_key resized to the nodes keypos
    */
    return nullptr;
  }

  template <typename Cursor>
  const offset_t* prev(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint8_t min = 0, max = back.branch_key;
    char iprev = -1;
    for (char i = 0; i < size; i++) {
      if (keys[i] >= min && keys[i] < max) {
        min = back.branch_key = keys[i];
        iprev = i;
      }
    }

    if (iprev >= 0) {
      cursor._changed_branch_key();
      return &links[iprev];
    }
    cursor.current_key.resize(back.keypos + back.prefix);
    // null_key might come
    return nullptr;
  }

  static bsize_t nodesize(bsize_t s) {
    return sizeof(_ArrayBranch) + s * sizeof(offset_t);
  }

  bsize_t nodesize() const { return nodesize(size); }
};

/* A sparse Array Branch
 */
template <typename BlockHeader>
struct _SparseBranch {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  typedef _SparseArray<offset_t> SparseArray;

  SparseArray trie;

  bsize_t count() const { return trie.count(); }

  template <typename Cursor>
  const offset_t* find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.branch_key = cursor.rest_key[0];
    if (trie.get(back.branch_key)) {
      cursor._advance_key(1);
      return &trie[back.branch_key];
    }
    back.cmp = Cursor::Transition::NOT_SAME;
    return nullptr;
  }

  template <typename Cursor>
  const offset_t* first(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.branch_key = trie.bits.first();
    cursor.current_key.push_back(back.branch_key);
    return &trie.values[0];
  }

  template <typename Cursor>
  const offset_t* last(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.branch_key = trie.bits.last();
    cursor.current_key.push_back(back.branch_key);
    return &trie.values[trie.count() - 1];
  }

  template <typename Cursor, typename F>
  const offset_t* step(Cursor& cursor, F&& move) const {
    typename Cursor::Transition& back = cursor.stack.back();
    bool exists = trie.get(back.branch_key);
    int sresult = move(back.branch_key);
    if (sresult != -1) {
      back.branch_key = sresult;
      cursor._changed_branch_key();
      return &trie[sresult];
    }
    if (exists) cursor.current_key.pop_back();
    return nullptr;
  }

  template <typename Cursor>
  const offset_t* next(Cursor& cursor) const {
    return step(cursor, [this](int key) -> int { return trie.bits.next(key); });
  }

  template <typename Cursor>
  const offset_t* prev(Cursor& cursor) const {
    return step(cursor, [this](int key) -> int { return trie.bits.prev(key); });
  }

  static constexpr bsize_t nodesize(bsize_t s) { return SparseArray::space(s); }

  bsize_t nodesize() const { return nodesize(trie.count()); }
};

// A leaf of the trie (a rest key and the value)
template <typename BlockHeader>
struct _Leaf {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;

  static const size_t MAX_LEAF_SIZE = 2048;
  /* BIG_VALUES are two time defered
    -> Normal Leaf with an offset as value
    -> pure value data in extra allocated block (with no header?) */

  hash_t hash;
  bsize_t value_size;
  bsize_t key_size;  // actually we only need 2 bytes but we align to 8
  uint8_t key_value[0];

  template <typename Cursor>
  void find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    back.found_leaf = this;
    size_t same = get_prefix(cursor.rest_key.data(), (const char*)key_value,
                             cursor.rest_key.size(), key_size, back.cmp);
    back.suffix = same;
    cursor._advance_key(back.suffix);
  }

  template <typename Cursor>
  void step(Cursor& cursor) const {  // for first, next, prev, last
    typename Cursor::Transition& back = cursor.stack.back();
    back.found_leaf = this;
    cursor.current_key.append((char*)key_value, key_size);
    back.suffix = key_size;
    back.cmp = 0;
  }

  Slice value() const {
    return Slice((char*)key_value + align(key_size), value_size);
  }

  Slice key() const { return Slice((char*)key_value, key_size); }

  static bsize_t nodesize(bsize_t ksize, size_t vsize) {
    bsize_t tmp = sizeof(_Leaf) + align(ksize) + align(vsize);
    return tmp <= MAX_LEAF_SIZE
               ? tmp
               : sizeof(_Leaf) + align(ksize) + sizeof(offset_t);
  }

  uint16_t nodesize() const { return nodesize(key_size, value_size); }
};

template <typename BlockHeader>
struct _LeafNode : public BlockHeader {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;
  typedef _Leaf<BlockHeader> Leaf;
  typedef _LeafNode<BlockHeader> LeafNode;
  typedef typename BlockHeader::template Pointer<LeafNode> ptr;

  uint8_t data[0];

  Leaf* leaf(const offset_t& ptr) {
    return (Leaf*)&data[(uint64_t)ptr & (uint64_t)~1];
  }

  const Leaf* leaf(const offset_t& ptr) const {
    return (Leaf*)&data[(uint64_t)ptr & (uint64_t)~1];
  }

  template <typename Storage>
  static ptr alloc(bsize_t size, Storage& storage) {
    return storage.alloc(size + sizeof(LeafNode));
  }
};

template <typename Cursor, typename Leaf>
void find_(Cursor& cursor, const Leaf* leaf) {
  leaf->find(cursor);
}

template <typename Cursor, typename Leaf>
void step_(Cursor& cursor, const Leaf* leaf) {
  leaf->step(cursor);
}

inline bool isleaf(uint64_t offset) { return offset & 1 == 1; };

inline void setleaf(uint64_t& offset) { offset |= 1; }

/* the metadata of a every block */
template <typename BlockHeader>
struct _BranchNode : public BlockHeader {
  using bsize_t = typename BlockHeader::bsize_t;
  using offset_t = typename BlockHeader::offset_t;
  using hash_t = typename BlockHeader::hash_t;
  typedef _Compressed<BlockHeader> Compressed;
  typedef _ArrayBranch<BlockHeader> ArrayBranch;
  typedef _SparseBranch<BlockHeader> SparseBranch;
  typedef _Leaf<BlockHeader> Leaf;
  typedef _BranchNode<BlockHeader> BranchNode;
  typedef _LeafNode<BlockHeader> LeafNode;
  typedef typename BlockHeader::template Pointer<BranchNode> ptr;
  using leaf_ptr = typename LeafNode::ptr;

  /*
  bit layout of bits
  struct {
    bsize_t has_compressed : 1;
    bsize_t has_null_leaf : 1;
    bsize_t has_array : 1
    bsize_t has_bit : 1;
    // if the block is in a free list the free block
    bsize_t size : 27;
  };*/
  static const uint16_t COMPRESSED = (uint16_t)8;
  static const uint16_t NULL_LEAF = (uint16_t)4;
  static const uint16_t ARRAY = (uint16_t)2;
  static const uint16_t SPARSE = (uint16_t)1;
  static const uint16_t BITS = COMPRESSED | NULL_LEAF | ARRAY | SPARSE;
  static const uint16_t SPACE = ~BITS;

  offset_t leaves;      // link to the associated leaf block
  bsize_t leaves_used;  // space used in leaves
  bsize_t leaves_free;  // free holes in leaves
  uint8_t data[0];

  bool has_compressed() const { return BlockHeader::b.bits & COMPRESSED; }
  bool has_null_leaf() const { return BlockHeader::b.bits & NULL_LEAF; }
  bool has_array() const { return BlockHeader::b.bits & ARRAY; }
  bool has_sparse() const { return BlockHeader::b.bits & SPARSE; }

  void set_compressed() { BlockHeader::b.bits |= COMPRESSED; }
  void set_null_leaf() { BlockHeader::b.bits |= NULL_LEAF; }
  void set_array() { BlockHeader::b.bits |= ARRAY; }
  void set_sparse() { BlockHeader::b.bits |= SPARSE; }

  void clear_compressed() { BlockHeader::b.bits &= ~COMPRESSED; }
  void clear_array() { BlockHeader::b.bits &= ~ARRAY; }
  void clear_null_leaf() { BlockHeader::b.bits &= ~NULL_LEAF; }

  template <typename Storage>
  static ptr alloc(bsize_t size, Storage& storage) {
    return storage.alloc(size + sizeof(BranchNode));
  }

  template <typename Storage>
  ptr cow_replace(Storage& storage) {
    ptr result = clone(storage);
    storage.free(this);
    return result;
  }

  template <typename Storage>
  ptr clone(Storage& storage) const {
    ptr branch = storage.alloc(BlockHeader::block_size);
    copy(*branch, *this, BlockHeader::b.used);
    return branch;
  }

  Compressed* compressed() { return (Compressed*)data; }
  const Compressed* compressed() const { return (const Compressed*)data; }
  void* end() { return (void*)&data[BlockHeader::b.used]; }

  // calculate the offset of a link pointer
  bsize_t olink(offset_t* link) { return (uint8_t*)link - data; }

  offset_t* plink(bsize_t offset) { return (offset_t*)&data[offset]; }

  // usable space
  size_t freespace() const {
    return BlockHeader::block_size - sizeof(BranchNode) - BlockHeader::b.used;
  }

  // find the next chunk of the key and update the stack
  template <typename Cursor>
  bool find(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    if (has_compressed()) {
      back.compressed = (const Compressed*)&data[ioffset];
      if (!back.compressed->find(cursor)) return false;
      ioffset += back.compressed->nodesize();
    }

    auto follow_link = [&cursor, &back](const offset_t* ptr) -> bool {
      if (!ptr) return false;
      return back.follow_link(cursor, ptr, find_<Cursor, Leaf>);
    };

    if (has_null_leaf()) {
      if (cursor.rest_key.empty())
        return follow_link((const offset_t*)&data[ioffset]);
      ioffset += sizeof(offset_t);
    }

    if (cursor.rest_key.empty()) {
      back.cmp = -1;
      return false;
    }

    assert(ioffset % sizeof(void*) == 0);

    if (has_array()) {
      back.array = (const ArrayBranch*)&data[ioffset];
      return follow_link(back.array->find(cursor));
    }

    if (has_sparse()) {
      back.sparse = (const SparseBranch*)&data[ioffset];
      return follow_link(back.sparse->find(cursor));
    }

    // special case there is only one item in the trie: a leaf
    assert(back.branch->b.used == sizeof(offset_t));
    assert(!back.branch->has_compressed());
    assert(!back.branch->has_array());
    assert(!back.branch->has_sparse());
    assert(back.branch->has_null_leaf());
    assert(cursor.stack.size == 1);
    return follow_link((offset_t*)back.branch->data);
  }

  template <typename Cursor>
  bool first(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();

    uint16_t ioffset = 0;
    if (has_compressed()) {
      back.compressed = ((const Compressed*)&data[ioffset]);
      ioffset += back.compressed->step(cursor);
    }

    if (has_null_leaf())
      return back.follow_link(cursor, (const offset_t*)&data[ioffset],
                              step_<Cursor, Leaf>);

    if (has_array()) {
      back.array = (const ArrayBranch*)&data[ioffset];
      return back.follow_link(cursor, back.array->first(cursor),
                              step_<Cursor, Leaf>);
    }

    assert(has_sparse());
    back.sparse = (const SparseBranch*)&data[ioffset];
    return back.follow_link(cursor, back.sparse->first(cursor),
                            step_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool last(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();

    uint16_t ioffset = 0;
    if (has_compressed()) {
      back.compressed = ((const Compressed*)&data[ioffset]);
      ioffset += back.compressed->step(cursor);
    }

    if (has_null_leaf()) ioffset += sizeof(offset_t);

    if (has_array()) {
      back.array = (const ArrayBranch*)&data[ioffset];
      return back.follow_link(cursor, back.array->last(cursor),
                              step_<Cursor, Leaf>);
    }

    if (has_sparse()) {
      back.sparse = (const SparseBranch*)&data[ioffset];
      return back.follow_link(cursor, back.sparse->last(cursor),
                              step_<Cursor, Leaf>);
    }

    return back.follow_link(cursor, (offset_t*)back.branch->data,
                            step_<Cursor, Leaf>);
  }

  template <typename Cursor>
  bool next(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    auto follow_link = [&cursor, &back](const offset_t* ptr) -> bool {
      if (!ptr) {
        cursor._pop();
        return true;
      }
      return back.follow_link(cursor, ptr, step_<Cursor, Leaf>);
    };

    if (back.found_leaf) {
      if (back.cmp < 0) {
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf->step(cursor);
        return false;
      }
      if (back.cmp == 0) {
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf = nullptr;
      }
    }

    if (back.compressed) {
      ioffset += back.compressed->nodesize();
      if (back.prefix != back.compressed->size) {
        assert(back.prefix < back.compressed->size);
        if (back.cmp > 0) {
          // behind this node
          cursor._pop();
          return true;
        }

        assert(back.cmp < 0);
        cursor.current_key.resize(back.keypos);
        return first(cursor);
      }
    }

    if (has_null_leaf()) ioffset += sizeof(offset_t);

    if (has_array()) {
      if (!back.array || back.cmp == Cursor::Transition::UNDEFINED) {
        back.array = (const ArrayBranch*)&data[ioffset];
        return follow_link(back.array->first(cursor));
      }
      return follow_link(back.array->next(cursor));
    }

    if (has_sparse()) {
      if (!back.array || back.cmp == Cursor::Transition::UNDEFINED) {
        back.sparse = (const SparseBranch*)&data[ioffset];
        return follow_link(back.sparse->first(cursor));
      }
      return follow_link(back.sparse->next(cursor));
    }

    // only one item
    if (back.cmp < 0) {
      return back.follow_link(cursor, (offset_t*)back.branch->data,
                              step_<Cursor, Leaf>);
    }

    cursor._pop();
    return false;
  }

  template <typename Cursor>
  bool prev(Cursor& cursor) const {
    typename Cursor::Transition& back = cursor.stack.back();
    uint16_t ioffset = 0;

    if (back.found_leaf) {
      if (back.cmp > 0) {
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf->step(cursor);
        return false;
      }
      if (back.cmp == 0) {
        cursor.current_key.resize(cursor.current_key.size() - back.suffix);
        back.found_leaf = nullptr;
      }
    }

    if (back.compressed) {
      ioffset += back.compressed->nodesize();
      if (back.prefix != back.compressed->size) {
        if (back.cmp < 0) {
          cursor._pop();
          return true;
        }

        assert(back.cmp > 0);
        cursor.current_key.resize(back.keypos);
        return last(cursor);
      }
    }

    if (!back.array) {
      cursor._pop();
      return true;
    }

    bsize_t null_offset = 0;
    if (has_null_leaf()) {
      null_offset = ioffset;
      ioffset += sizeof(offset_t);
    }

    auto follow_link = [&cursor, &back](const offset_t* ptr) -> bool {
      return back.follow_link(cursor, ptr, step_<Cursor, Leaf>);
    };

    const offset_t* link = nullptr;
    bool in_branch = false;
    if (has_array()) {
      back.array = (const ArrayBranch*)&data[ioffset];
      link = back.array->prev(cursor);
      in_branch = true;
    }

    if (has_sparse()) {
      back.sparse = (const SparseBranch*)&data[ioffset];
      link = back.sparse->prev(cursor);
      in_branch = true;
    }

    if (link) return follow_link(link);

    if (in_branch && has_null_leaf()) {
      back.array = nullptr;  // the next time walk to the next node
      return follow_link((const offset_t*)&data[null_offset]);
    }
    cursor._pop();
    return true;
  }

  template <typename OP>
  void iterate_links(OP oper) {
    bsize_t ioffset = 0;
    if (has_compressed()) ioffset = compressed()->nodesize();

    if (has_null_leaf()) {
      oper(*plink(ioffset));
      ioffset += sizeof(offset_t);
    }

    if (has_array()) {
      ArrayBranch* branch = (ArrayBranch*)&data[ioffset];
      assert(branch->size <= ArrayBranch::COUNT);
      for (int count = branch->size, i = 0; i < count; i++) {
        oper(branch->links[i]);
      }
    } else if (has_sparse()) {
      SparseBranch* branch = (SparseBranch*)&data[ioffset];
      for (int count = branch->count(), i = 0; i < count; i++) {
        oper(branch->trie.values[i]);
      }
    }
  }

  void add_compressed(const void* data, bsize_t size) {
    if (size) {
      Compressed* cn = compressed();
      cn->size = size;
      memcpy(cn->key, data, size);
      set_compressed();
      BlockHeader::b.used = cn->nodesize();
    }
  }

  bsize_t add_null_leaf() {
    bsize_t offset = BlockHeader::b.used;
    set_null_leaf();
    BlockHeader::b.used += sizeof(offset_t);
    return offset;
  }

  bsize_t add_array(uint8_t key1) {
    set_array();
    ArrayBranch* an = (ArrayBranch*)end();
    an->keys[0] = key1;
    an->size = 1;
    BlockHeader::b.used += an->nodesize();
    return olink(&an->links[0]);
  }

  bsize_t add_array(uint8_t key1, uint8_t key2, const offset_t& link2) {
    set_array();
    ArrayBranch* an = (ArrayBranch*)end();
    an->size = 2;
    an->keys[0] = key1;
    an->keys[1] = key2;
    an->links[1] = link2;
    BlockHeader::b.used += an->nodesize();
    return olink(&an->links[0]);
  }

  void copy_leaf(leaf_ptr dest, const leaf_ptr src) {
    if (leaves_free) {
      bsize_t ls = 0;
      iterate_links([src, &dest, &ls](offset_t& offset) {
        if (isleaf(offset)) {
          const Leaf* l = src->leaf(offset);
          memcpy(&dest->data[ls], l, l->nodesize());
          offset = ls;
          setleaf(offset);
          ls += l->nodesize();
        }
      });
      leaves_used = ls;
      leaves_free = 0;
      return;
    }

    memcpy(dest->data, src->data, leaves_used);
  }

  void check() const {
#if defined(DEBUG) && !defined(NDEBUG)
    assert(BlockHeader::b.used + sizeof(BranchNode) <= BlockHeader::block_size);
    bsize_t ioffset = 0;
    if (has_compressed()) {
      const Compressed* n = compressed();
      ioffset += n->nodesize();
    }

    if (has_null_leaf()) {
      offset_t n = *(const offset_t*)&data[ioffset];
      assert(isleaf(n));
      ioffset += sizeof(offset_t);
    }

    if (has_array()) {
      const ArrayBranch* n = (const ArrayBranch*)&data[ioffset];
      assert(n->size <= ArrayBranch::COUNT);
      for (int i = 0; i < n->size; i++) {
        assert(n->links[i] != 0);
      }
      ioffset += n->nodesize();
    }

    if (has_sparse()) {
      const SparseBranch* n = (const SparseBranch*)&data[ioffset];
      int c = n->count();
      for (int i = 0; i < c; i++) {
        assert(n->trie.values[i] != 0);
      }
      ioffset += n->nodesize();
    }

    assert(ioffset == BlockHeader::b.used);
    assert(ioffset > 0);
#endif
  }
};

#ifdef DEBUG

inline std::string bitstr(char bit) {
  std::stringstream cstr;
  if (isprint(bit) && bit != '"' && bit != '<' && bit != '>' && bit != ']' &&
      bit != '\\' && bit != '}' && bit != '{') {
    cstr << bit;
  } else {
    cstr << "0x" << std::hex << (unsigned)(unsigned char)bit << std::dec;
  }
  return cstr.str();
}

template <typename Storage>
struct _Dumper {
  using offset_t = typename Storage::offset_t;
  using block_ptr = typename Storage::block_ptr;
  using bsize_t = typename Storage::bsize_t;
  typedef _BranchNode<typename Storage::BlockHeader> BranchNode;
  typedef _LeafNode<typename Storage::BlockHeader> LeafNode;
  using branch_ptr = typename BranchNode::ptr;
  using leaf_ptr = typename LeafNode::ptr;
  using Leaf = typename BranchNode::Leaf;
  using Compressed = typename BranchNode::Compressed;
  using ArrayBranch = typename BranchNode::ArrayBranch;
  using SparseBranch = typename BranchNode::SparseBranch;

  static void dump_leaf(std::ostream& out, offset_t offset, branch_ptr branch,
                        Storage* storage) {
    leaf_ptr block = storage->resolve(branch->leaves);

    out << "id: " << (uint64_t)branch->leaves + offset << std::endl;
    out << "block: " << (uint64_t)storage->resolve(branch) << std::endl;
    out << "type: leaf" << std::endl;

    const Leaf* leaf = block->leaf(offset);

    out << "keysize: " << leaf->key_size << std::endl;
    out << "key: \"";
    for (int i = 0; i < leaf->key_size; i++) {
      out << "[" << bitstr(leaf->key_value[i]) << "]";
    }
    out << "\"" << std::endl;

    out << "valuesize: " << leaf->value_size << std::endl;
    out << "value: \"";
    int delta = align(leaf->key_size);
    for (size_t i = 0, end = std::min((size_t)leaf->value_size, (size_t)10);
         i < end; i++) {
      out << "[" << bitstr(leaf->key_value[i + delta]) << "]";
    }
    out << "\"" << std::endl;
    out << "---" << std::endl;
  }

  static void dump_link(std::ostream& out, block_ptr parent, offset_t offset,
                        Storage* storage) {
    if (isleaf(offset)) {
      dump_leaf(out, offset, parent, storage);
      return;
    }
    dump_branch(out, offset, storage);
  }

  static void dump_branch(std::ostream& out, offset_t offset,
                          Storage* storage) {
    branch_ptr block = storage->resolve(offset);
    leaf_ptr leaf = storage->resolve(block->leaves);
    bsize_t lspace = leaf->block_size - sizeof(LeafNode) - block->leaves_used;
    out << "id: " << offset << std::endl;
    out << "block: " << offset << std::endl;
    out << "size: " << block->block_size << std::endl;
    out << "space: " << block->b.used << std::endl;
    out << "freespace: " << block->freespace() << std::endl;
    out << "leaf_size: " << leaf->block_size << std::endl;
    out << "leaf_space: " << block->leaves_used << std::endl;
    out << "leaf_free: " << lspace << std::endl;
    out << "leaf_frag: " << block->leaves_free << std::endl;
    out << "type: branch" << std::endl;

    bsize_t ioffset = 0;
    if (block->has_compressed()) {
      const Compressed* n = (const Compressed*)&block->data[ioffset];
      out << "compressed: " << std::endl;
      out << "  size: " << (int)n->size << std::endl;
      out << "  key: \"";
      for (int i = 0; i < n->size; i++) {
        out << "[" << bitstr(n->key[i]) << "]";
      }
      out << "\"" << std::endl;
      ioffset += n->nodesize();
    }

    offset_t null_child = 0;
    if (block->has_null_leaf()) {
      null_child = *(const offset_t*)&block->data[ioffset];
      assert(isleaf(null_child));
      out << "nulllink: " << null_child + (uint64_t)block->leaves << std::endl;
      ioffset += sizeof(offset_t);
    }

    if (block->has_array()) {
      const ArrayBranch* n = (const ArrayBranch*)&block->data[ioffset];
      out << "branch: array" << std::endl;
      out << "key: \"";
      assert(n->size > 0);
      assert(n->size <= ArrayBranch::COUNT);
      for (int i = 0; i < n->size; i++) {
        out << "[" << bitstr(n->keys[i]) << "]";
      }
      out << "\"" << std::endl;
      out << "children: " << std::endl;
      for (int i = 0; i < n->size; i++) {
        uint64_t id = n->links[i];
        if (isleaf(n->links[i])) id += (uint64_t)block->leaves;
        out << "  - " << id << std::endl;
      }

      out << "---" << std::endl;

      if (null_child) dump_link(out, block, null_child, storage);

      for (int i = 0; i < n->size; i++) {
        dump_link(out, block, n->links[i], storage);
      }
      return;
    }

    if (!block->has_sparse()) {
      // value without key
      assert(null_child);
      out << "---" << std::endl;
      dump_link(out, block, null_child, storage);
      return;
    }

    const SparseBranch* n = (const SparseBranch*)&block->data[ioffset];
    out << "branch: trie" << std::endl;
    out << "key: \"";
    for (auto iter = n->trie.bits.first(); iter != -1;
         iter = n->trie.bits.next(iter)) {
      out << "[" << bitstr(iter) << "]";
    }

    out << "\"" << std::endl;
    out << "children: " << std::endl;
    for (int i = 0; i < n->count(); i++) {
      uint64_t id = n->trie.values[i];
      if (isleaf(n->trie.values[i])) id += (uint64_t)block->leaves;
      out << "  - " << id << std::endl;
    }

    out << "---" << std::endl;

    if (null_child) dump_link(out, block, null_child, storage);

    for (int i = 0, end = n->count(); i < end; i++) {
      assert(n->trie.values[i]);
      dump_link(out, block, n->trie.values[i], storage);
    }
  }
};
#endif
}  // namespace leaves

#endif  // _LEAVES__NODE_HPP
