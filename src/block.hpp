// declarations for memory blocks
#ifndef _LEAVES_BLOCKS_HPP
#define _LEAVES_BLOCKS_HPP

#include <leaves.hpp>
#include <stdexcept>

#include "pool.hpp"
#include "port.hpp"

#ifdef HEADER_ONLY
#define INLINE inline
#else
#define INLINE
#endif

namespace leaves {

#pragma pack(1)

struct Trace;

/* A Block represents a trie node or a combination of trie nodes.

The layout of a block is:

HEADER
Compressed ["prefix"]
Value Offset [VO]
Branch node:  [BA] or [BT]

(space)

...
[L1]
[L0]

Branch Nodes:

  - ["prefix"]
    A Compressed Node (see https://www.geeksforgeeks.org/compressed-tries/)
    here the node contains "prefix".
    Can be omitted.

  - [O] a link to a Leaf node. Can be ommitted.

  - [BA|BT] either a [BA] (ArrayBranch) or a [BT] (TrieBranch).
    Can be ommitted. (Than the block only contains leaf nodes).

  Branch nodes are located at the beginning of the block.

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

typedef uint16_t bsize_t;  // size inside a branch block
typedef uint32_t lsize_t;  // size inside a leaf block

struct Compressed {
  bsize_t size;
  uint8_t key[0];
  bool find(Trace& trace) const;
  bsize_t nodesize() const { return size + sizeof(size); }
  static bsize_t nodesize(bsize_t s) { return s ? sizeof(Compressed) + s : 0; }
};

struct ArrayBranch {
  const static bsize_t COUNT = 15;
  uint8_t size;
  uint8_t keys[COUNT];
  offset_ptr links[0];

  const offset_ptr* find(Trace& trace) const;

  bsize_t nodesize() const {
    return sizeof(offset_ptr) * size + sizeof(size) + sizeof(keys);
  }

  static bsize_t nodesize(bsize_t s) {
    return sizeof(ArrayBranch) + s * sizeof(offset_ptr);
  }
};

struct TrieBranch {
  uint64_t bits[4];  // 256 bits for compression
  offset_ptr links[0];

  const offset_ptr* find(Trace& trace) const;

  static uint8_t idx(uint8_t val) { return val >> 6; }
  static uint8_t bit(uint8_t val) { return val & 63; }

  int index(uint8_t val) const {
    uint8_t idx_ = idx(val);
    uint64_t mask = (((uint64_t)1) << bit(val)) - 1;
    int ones = 0;
    for (int i = 0; i < idx_; i++) ones += popcount(bits[i]);
    ones += popcount(bits[idx_] & mask);
    return ones;
  }

  void set(uint8_t val) { bits[idx(val)] |= ((uint64_t)1) << bit(val); }

  bsize_t count() const {
    return popcount(bits[0]) + popcount(bits[1]) + popcount(bits[2]) +
           popcount(bits[3]);
  }
};

// A leaf of the trie (a rest key and the value)
struct Leaf {
  static const size_t MAX_LEAF_SIZE = 2047;
  /* BIG_VALUES are two time defered
    -> Normal Leaf with an offset as value
    -> pure value data in extra allocated block (with no header?) */

  lsize_t value_size;
  bsize_t key_size;
  uint8_t key_value[0];

  void find(Trace& trace) const;
  Slice value() const { return Slice((char*)key_value + key_size, value_size); }
  Slice key() const { return Slice((char*)key_value, key_size); }

  const static uint16_t HEADER_SIZE = sizeof(value_size) + sizeof(bsize_t);
  uint16_t nodesize() const { return nodesize(key_size, value_size); }

  static bsize_t nodesize(bsize_t ksize, size_t vsize) {
    lsize_t tmp = sizeof(Leaf) + ksize + vsize;
    return tmp <= MAX_LEAF_SIZE ? tmp
                                : sizeof(Leaf) + ksize + sizeof(offset_ptr);
  }
};

/* the metadata of a every block */
struct BlockHeader {
  // the blocks offset (==id)
  offset_ptr offset;

  // the transaction id the block was created
  tid_t txn_id;

  // the transaction id the block was created
  tid_t free_txn_id;

  static const uint64_t COMPRESSED = (uint64_t)1 << 63;
  static const uint64_t NULL_LEAF = (uint64_t)1 << 62;
  static const uint64_t ARRAY = (uint64_t)1 << 61;
  static const uint64_t TRIE = (uint64_t)1 << 60;
  static const uint64_t BITS = COMPRESSED | NULL_LEAF | ARRAY | TRIE;
  static const uint64_t NEXT_FREE = ~BITS;

  /*
  bit layout of bits
  struct {
    uint64_t has_compressed : 1;
    uint64_t has_null_leaf : 1;
    uint64_t has_array : 1
    uint64_t has_trie : 1;
    // if the block is in a free list the free block
    uint64_t next_free : 59;
  };*/
  uint64_t bits;

  bool has_compressed() const { return bits & COMPRESSED; }
  bool has_null_leaf() const { return bits & NULL_LEAF; }
  bool has_array() const { return bits & ARRAY; }
  bool has_trie() const { return bits & TRIE; }
  uint64_t next_free() const { return bits & NEXT_FREE; }

  void set_next_free(uint64_t next) { bits = next & NEXT_FREE; }
  void set_compressed() { bits |= COMPRESSED; }
  void set_null_leaf() { bits |= NULL_LEAF; }
  void set_array() { bits |= ARRAY; }
  void set_trie() { bits |= TRIE; }

  void clear_compressed() { bits &= ~COMPRESSED; }
  void clear_array() { bits &= ~ARRAY; }

  size_t block_size() const { return BLOCK_SIZES[offset.pool_id()].block_size; }
};

struct LeafBlock : public BlockHeader {
  static const size_t HEADER_SIZE = sizeof(BlockHeader);
  static const size_t MAX_LEAF_SIZE = 2047;
  uint8_t data[0];
  size_t space() const { return block_size() - HEADER_SIZE; }
  Leaf* leaf(const offset_ptr& ptr) { return (Leaf*)&data[ptr.offset()]; }
  const Leaf* leaf(const offset_ptr& ptr) const {
    return (Leaf*)&data[ptr.offset()];
  }
};

struct BranchBlockHeader : public BlockHeader {
  bsize_t used;         // space used
  offset_ptr leaves;    // link to the associated leaf block
  lsize_t leaves_used;  // space used in leaves
  lsize_t leaves_free;  // free holes in leaves
};

struct BranchBlock : public BranchBlockHeader {
  static const size_t HEADER_SIZE = sizeof(BranchBlockHeader);
  static const size_t MAX_SIZE = 4096;
  static const uint16_t MAX_SPACE = MAX_SIZE - HEADER_SIZE;
  static const int MAX_POOL = get_pool(MAX_SPACE);

  uint8_t data[0];

  Compressed* compressed() { return (Compressed*)data; }
  const Compressed* compressed() const { return (const Compressed*)data; }
  void* end() { return (void*)&data[used]; }

  // usable space
  size_t space() const { return block_size() - HEADER_SIZE; }
  size_t freespace() const { return space() - used; }

  // find the next chunk of the key and update the stack
  bool find(Trace& trace) const;

  // calculate the offset of a link pointer
  bsize_t olink(offset_ptr* link) { return (uint8_t*)link - data; }

  offset_ptr* plink(bsize_t offset) { return (offset_ptr*)&data[offset]; }

  template <typename OP>
  void iterate_links(OP oper) {
    bsize_t ioffset = 0;
    if (has_compressed()) ioffset = compressed()->nodesize();

    if (has_null_leaf()) {
      offset_ptr* ptr = plink(ioffset);
      oper(*ptr);
      ioffset += sizeof(offset_ptr);
    }

    if (has_array()) {
      ArrayBranch* branch = (ArrayBranch*)&data[ioffset];
      for (int count = branch->size, i = 0; i < count; i++) {
        oper(branch->links[i]);
      }
    } else if (has_trie()) {
      TrieBranch* branch = (TrieBranch*)&data[ioffset];
      for (int count = branch->count(), i = 0; i < count; i++) {
        oper(branch->links[i]);
      }
    }
  }

  void add_compressed(const void* data, bsize_t size) {
    if (size) {
      Compressed* cn = compressed();
      cn->size = size;
      memcpy(cn->key, data, size);
      set_compressed();
      used = cn->nodesize();
    }
  }

  bsize_t add_null_leaf() {
    bsize_t offset = used;
    set_null_leaf();
    used += sizeof(offset_ptr);
    return offset;
  }

  bsize_t add_array(uint8_t key1) {
    set_array();
    ArrayBranch* an = (ArrayBranch*)end();
    an->keys[0] = key1;
    an->size = 1;
    used += an->nodesize();
    return olink(&an->links[0]);
  }

  bsize_t add_array(uint8_t key1, uint8_t key2, const offset_ptr& link2) {
    set_array();
    ArrayBranch* an = (ArrayBranch*)end();
    an->size = 2;
    an->keys[0] = key1;
    an->keys[1] = key2;
    an->links[1] = link2;
    used += an->nodesize();
    return olink(&an->links[0]);
  }

  void copy(const BranchBlock* src) {
    memcpy(data, src->data, src->used);  // nodes
    bits = src->bits;
    used = src->used;
    leaves = src->leaves;
    leaves_used = src->leaves_used;
    leaves_free = src->leaves_free;
  }

  void copy_leaf(LeafBlock* dest, const LeafBlock* src) {
    if (leaves_free) {
      lsize_t ls = 0;
      iterate_links([src, dest, &ls](offset_ptr& ptr) {
        if (ptr.leaf()) {
          const Leaf* l = src->leaf(ptr);
          memcpy(&dest->data[ls], l, l->nodesize());
          ptr.set(LEAF_BLOCK, ls);
          ls += l->nodesize();
        }
      });
      leaves_used = ls;
      leaves_free = 0;
      return;
    }

    memcpy(dest->data, src->data, leaves_used);
  }
};

struct block_ptr {
  BlockHeader* ptr;

  BranchBlock* operator->() { return (BranchBlock*)ptr; }
  operator BranchBlock*() { return (BranchBlock*)ptr; }
  LeafBlock* leaf() { return (LeafBlock*)ptr; }

  block_ptr& operator=(const block_ptr& src) {
    ptr = src.ptr;
    return *this;
  }
  bool operator==(const block_ptr& other) const { return ptr == other.ptr; }
  bool operator==(const BlockHeader* other) const { return ptr == other; }
  operator bool() const { return ptr != nullptr; }

  void reset() { ptr = nullptr; }
  bool valid() const { return ptr != nullptr; }
};

inline size_t get_prefix(const char* str1, const char* str2, size_t size1,
                         size_t size2) {
  size_t i = 0;
  size_t limit = std::min(size1, size2) / sizeof(uint64_t);
  const uint64_t* wstr1 = reinterpret_cast<const uint64_t*>(str1);
  const uint64_t* wstr2 = reinterpret_cast<const uint64_t*>(str2);

  while (i < limit && wstr1[i] == wstr2[i]) {
    i++;
  }
  i *= sizeof(uint64_t);

  limit = std::min(size1, size2);
  while (i < limit && str1[i] == str2[i]) {
    i++;
  }
  return i;
}

#pragma pack(0)

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_HPP
