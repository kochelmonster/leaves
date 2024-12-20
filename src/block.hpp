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
  uint8_t size;
  uint8_t keys[15];
  offset_ptr links[0];

  const offset_ptr* find(Trace& trace) const;

  void change_block(uint64_t old_, uint64_t new_) {
    for (uint8_t i = 0; i < size; i++) {
      links[i].change_block(old_, new_);
    }
  }

  void move_ioffset(const offset_ptr& pivot, int delta) {
    for (char i = 0; i < size; i++) {
      links[i].move_ioffset(pivot, delta);
    }
  }

  bsize_t nodesize() const {
    return sizeof(offset_ptr) * size + sizeof(size) + sizeof(keys);
  }

  static bsize_t nodesize(bsize_t s) {
    return sizeof(ArrayBranch) + s * sizeof(offset_ptr);
  }
};

struct TrieBranch {
  union Index {
    struct {
      uint8_t idx : 2;  // 4 which of bits
      uint8_t bit : 6;  // which bit of bits
    };
    uint8_t val;
  };

  uint64_t bits[4];  // 256 bits for compression
  offset_ptr links[0];

  const offset_ptr* find(Trace& trace) const;

  int index(uint8_t val) const {
    Index idx = {.val = val};
    uint64_t mask = (1 << idx.bit) - 1;
    int ones = 0;
    for (int i = 0; i < idx.idx; i++) ones += popcount(bits[i]);
    ones += popcount(bits[idx.idx] & mask);
    return ones;
  }

  void set(uint8_t val) {
    Index idx = {.val = val};
    bits[idx.idx] = 1 << idx.bit;
  }

  bsize_t count() const {
    return popcount(bits[0]) + popcount(bits[1]) + popcount(bits[2]) +
           popcount(bits[3]);
  }

  void move_ioffset(const offset_ptr& pivot, int delta) {
    bsize_t size = count();
    for (bsize_t i = 0; i < size; i++) {
      links[i].move_ioffset(pivot, delta);
    }
  }

  void change_block(uint64_t old_, uint64_t new_) {
    bsize_t size = count();
    for (bsize_t i = 0; i < size; i++) {
      links[i].change_block(old_, new_);
    }
  }
};

// A leaf of the trie (a rest key and the value)
struct Leaf {
  const static lsize_t BIG_VAL_SIZE = 64 * K;
  /* BIG_VALUES are two time defered
    -> Normal Leaf with an offset as value
    -> pure value data in extra allocated block (with no header?) */

  lsize_t value_size;
  bsize_t key_size;
  uint8_t key_value[0];

  void find(Trace& trace) const;
  Slice value() const { return Slice((char*)key_value + key_size, value_size); }
  Slice key() const { return Slice((char*)key_value + key_size, value_size); }

  const static uint16_t HEADER_SIZE = sizeof(value_size) + sizeof(bsize_t);
  uint16_t nodesize() const { return nodesize(key_size, value_size); }

  static bsize_t nodesize(bsize_t ksize, size_t vsize) {
    return sizeof(Leaf) + ksize +
           (vsize < BIG_VAL_SIZE ? vsize : sizeof(offset_ptr));
  }
};

/* the metadata of a every block */
struct BlockHeader {
  static const uint64_t COMPRESSED = (uint64_t)1 << 63;
  static const uint64_t VALUE = (uint64_t)1 << 62;
  static const uint64_t ARRAY = (uint64_t)1 << 61;
  static const uint64_t TRIE = (uint64_t)1 << 60;
  static const uint64_t BITS = COMPRESSED | VALUE | ARRAY | TRIE;
  static const uint64_t NEXT_FREE = ~BITS;

  // the blocks offset (==id)
  offset_ptr offset;

  // the transaction id the block was created
  tid_t txn_id;

  // the transaction id the block was freed
  tid_t free_txn_id;

  // 

  /*
  bit layout of bits
  struct {
    uint64_t has_compressed : 1;
    uint64_t has_value : 1;
    uint64_t has_array : 1
    uint64_t has_trie : 1;
    // if the block is in a free list the free block
    uint64_t next_free : 59; 
  };*/
  uint64_t bits;

  lsize_t lower_bound;  // the end of structural node
  lsize_t upper_bound;  // the start of KeyValueNodes
  offset_ptr leaves;    // link to the associated leaf block

  bool has_compressed() const { return bits & COMPRESSED; }
  bool has_value() const { return bits & VALUE; }
  bool has_array() const { return bits & ARRAY; }
  bool has_trie() const { return bits & TRIE; }
  uint64_t next_free() const { return bits & NEXT_FREE; }

  void set_next_free(uint64_t next) { bits = next & NEXT_FREE; }
  void set_compressed() { bits |= COMPRESSED; }
  void set_value() { bits |= VALUE; }
  void set_array() { bits |= ARRAY; }
  void set_trie() { bits |= TRIE; }

  void clear_compressed() { bits &= ~COMPRESSED; }

  bool isleaf() const { return bits & BITS == 0; }
  bool isbranch() const { return bits & BITS; }
};

struct Block : public BlockHeader {
  static const size_t HEADER_SIZE = sizeof(BlockHeader);
  static const size_t MAX_BRANCH_SIZE = 4096;
  static const uint16_t MAX_BRANCH_SPACE = MAX_BRANCH_SIZE - HEADER_SIZE;
  static const int MAX_BRANCH_POOL = get_pool(MAX_BRANCH_SPACE);
  static const int MAX_LEAF_POOL = get_pool(0xffff);

  uint8_t data[0];

  const Leaf* leaf(offset_ptr ptr) const {
    assert(ptr.start() == offset.offset);
    return (const Leaf*)&data[space() - ptr.ioffset()];
  }

  Leaf* leaf(offset_ptr ptr) {
    assert(ptr.start() == offset.offset);
    return (Leaf*)&data[space() - ptr.ioffset()];
  }

  Compressed* compressed() { return (Compressed*)data; }
  const Compressed* compressed() const { return (const Compressed*)data; }
  void* end() { return (void*)&data[lower_bound]; }

    // usable space
  size_t block_size() const { return BLOCK_SIZES[offset.pool_id].block_size; }
  size_t space() const { return block_size() - HEADER_SIZE; }
  size_t freespace() const { return upper_bound - lower_bound; }
  size_t used() const { return space() - freespace(); }

  // find the next chunk of the key and update the stack
  bool find(Trace& trace) const;

  // follows the link and update the stack
  bool follow_link(Trace& trace, const offset_ptr* link) const;

  // change the block part of all links pointing to leaf
  void change_leaf_blocks(const offset_ptr& old_, const offset_ptr& new_);

  // move all ioffsets of links pointing to leaves by delta
  // if their own ioffset < pivot.ioffset()
  void move_leaf_ioffsets(const offset_ptr& pivot, int delta);

  // find the first leaf that has a size >= space
  offset_ptr* find_leaf_to_move(bsize_t space);

  void add_compressed(bsize_t size, const void* data) {
    Compressed* cn = compressed();
    cn->size = size;
    memcpy(cn->key, data, size);
    set_compressed();
    lower_bound = cn->nodesize();
  }

  void add_value(const offset_ptr& offset) {
    offset_ptr* val = (offset_ptr*)end();
    *val = offset;
    set_value();
    lower_bound += sizeof(offset_ptr);
  }

  void add_array(uint8_t key1, const offset_ptr& link1, uint8_t key2,
                 const offset_ptr& link2) {
    set_array();
    ArrayBranch* an = (ArrayBranch*)end();
    an->keys[0] = key1;
    an->links[0] = link1;
    if (link2.offset) {
      an->size = 2;
      an->keys[1] = key2;
      an->links[1] = link2;
    } else
      an->size = 1;

    lower_bound += an->nodesize();
  }

  void copy(const Block* src) {
    memcpy(data, src->data, src->lower_bound);  // structural nodes
    memcpy(&data[src->upper_bound], &src->data[src->upper_bound],
           src->space() - src->upper_bound);  // key value nodes
    bits = src->bits;
    upper_bound = space() - (src->space() - src->upper_bound);
    lower_bound = src->lower_bound;
    leaves = src->leaves;
  }
};

struct block_ptr {
  Block* ptr;

  operator Block*() { return ptr; }
  block_ptr& operator=(const block_ptr& src) {
    ptr = src.ptr;
    return *this;
  }
  bool operator==(const block_ptr& other) const { return ptr == other.ptr; }
  bool operator==(const Block* other) const { return ptr == other; }
  operator bool() const { return ptr != nullptr; }

  Block* operator->() { return ptr; }
  void reset() { ptr = nullptr; }
  bool valid() const { return ptr != nullptr; }
};

#pragma pack(0)

}  // namespace leaves

#endif  // _LEAVES_BLOCKS_HPP
