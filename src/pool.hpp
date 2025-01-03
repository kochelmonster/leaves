// declarations for managing the memory pool
#ifndef _LEAVES_POOL_HPP
#define _LEAVES_POOL_HPP

#include <array>
#include "block.hpp"

namespace leaves {

const size_t AREA_SIZE1 = 1 << 20;
const size_t AREA_SIZE2 = 1 << 22;
const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

struct BlockArea {
  size_t area_size;
  size_t block_size;
};

const int POOL_COUNT = 24;
const int LEAF_BLOCK = POOL_COUNT;

// TODO: 3K Block einfügen == 3K + 1K = 4k (Den 1K in die free list)
// TODO: Anfangen mit 64 byte block

constexpr ::std::array<BlockArea, POOL_COUNT> generate_pool() {
  ::std::array<BlockArea, POOL_COUNT> result{0};
  int i = 0;
  for (; i < 9; i++) {
    result[i].area_size = AREA_SIZE1;
    result[i].block_size = 1 << (i + 8);
  };

  for (; i < POOL_COUNT; i++) {
    result[i].area_size = AREA_SIZE2,
    result[i].block_size = result[i - 1].block_size + 64 * K;
  }
  return result;
}

constexpr ::std::array<BlockArea, 24> BLOCK_SIZES = generate_pool();

const size_t BLOCK0_SIZE = BLOCK_SIZES[0].block_size;

// returns the right pool id for a block size
inline constexpr int get_pool(size_t size) {
  if (size >= BLOCK_SIZES[9].block_size) {
    if (size >= BLOCK_SIZES[POOL_COUNT - 1].block_size) return POOL_COUNT;
    return 9 + (size - 128 * K) / (64 * K);
  }
  int r = 0;
  while (size >>= 1) r++;
  return std::max(r - 7, 0);
}

struct offset_ptr {
  static const uint64_t OFFSET_MASK = ((uint64_t)1 << (uint64_t)59) - 1;

  uint64_t data;

  offset_ptr& operator=(offset_ptr src) {
    data = src.data;
    return *this;
  }

  bool operator==(offset_ptr cmp) const { return data == cmp.data; }
  bool operator!=(offset_ptr cmp) const { return data != cmp.data; }

  operator bool() const { return data != 0; }

  void set(uint8_t pool_id_, uint64_t offset_) {
    data = ((uint64_t)pool_id_ << 59) | (offset_ & OFFSET_MASK);
  }

  uint8_t pool_id() const { return data >> 59; }

  uint64_t offset() const { return data & OFFSET_MASK; }

  // the size of the block the ptr points to
  size_t size() const { return BLOCK_SIZES[pool_id()].block_size; }
  size_t mask() const { return OFFSET_MASK - (size() - 1); }

  uint64_t start() const { return data & mask(); }
  uint64_t ioffset() const { return data & ~mask(); }

  uint64_t merge(const offset_ptr& other) const {
    return offset() + (leaf() ? other.offset() : 0);
  }

  bool leaf() const { return pool_id() == LEAF_BLOCK; }
};

typedef uint64_t tid_t;

// Allocates and frees Blocks
struct BlockPool {
  // current slot for next free memory position
  uint64_t current;

  // the end of the currently allocated block
  uint64_t last;

  // list of free blocks that can be used
  uint64_t free_start;
  uint64_t free_end;

  // list of free blocks of the last active transaction
  uint64_t last_free_start;
  uint64_t last_free_end;

  // TODO: Aditional backup for the last free_start (multi database multi
  // thread)
};

/*
Differene of free and last_free

generally a list of free blocks looks like:

          Block1          Block2         Block3
start ->  next_free   ->  next_free  ->  next_free --> NULL
          txn_id = 3      txn_id = 4     txn_id = 5

- the txn_id is the transaction id that freed the block.
- the list is sorted according to the txn_id
- a freed block may be only reused if the cursor with
  the minimal txn_id is bigger than txn_id min(all cursor.txn_id) >
free_block.txn_id (otherwise the cursor could still use the block)

==> freed blocks from the current transaction have to be inserted at
the end of the list.

We have to postpone the appending operation until we now the transaction has
commited. Therefore new freed blocks from the current transaction are stored in
a separate list (last_free). This list will be appended to the original free
list in the next transaction, than we can sure the last transaction was
commited.
*/

// Active Transaction Data
struct DBTransaction {
  // Block Pools
  BlockPool pools[POOL_COUNT];

  /* the size of the file, this should be always equal the
     size of the database file. But in case of a crash during
     an transaction, the phyiscal file size could be bigger because
     of an alloc_new.
  */
  size_t file_size;

  // the transaction id of root
  tid_t txn_id;

  // pointer to the active root of the trie
  offset_ptr root;
};

}  // namespace leaves

#endif  // _LEAVES_POOL_HPP
