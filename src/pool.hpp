// declarations for managing the memory pool
#ifndef _LEAVES_POOL_HPP
#define _LEAVES_POOL_HPP

#include "array"
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

const int AREA_COUNT = 24;

/*
GENERATE POOL
*/
constexpr ::std::array<BlockArea, AREA_COUNT> fill_block_sizes() {
  ::std::array<BlockArea, AREA_COUNT> result{0};
  int i = 0;
  for (; i < 9; i++) {
    result[i].area_size = AREA_SIZE1;
    result[i].block_size = 1 << (i + 8);
  };

  for (; i < AREA_COUNT; i++) {
    result[i].area_size = AREA_SIZE2,
    result[i].block_size = result[i - 1].block_size + 64 * K;
  }
  return result;
}

constexpr ::std::array<BlockArea, 24> BLOCK_SIZES = fill_block_sizes();

const size_t BLOCK0_SIZE = BLOCK_SIZES[0].block_size;

// returns the right pool id for a block size
inline constexpr int get_pool(size_t size) {
  if (size >= BLOCK_SIZES[9].block_size) {
    if (size >= BLOCK_SIZES[AREA_COUNT - 1].block_size) return AREA_COUNT;
    return 9 + (size - 128 * K) / (64 * K);
  }
  int r = 0;
  while (size >>= 1) r++;
  return std::max(r - 7, 0);
}

struct offset_ptr {
  static const uint64_t MASK_BASE = ((uint64_t)1 << (uint64_t)59) - 1;
  uint64_t pool_id : 5;
  uint64_t offset : 59;

  offset_ptr& operator=(offset_ptr src) {
    pool_id = src.pool_id;
    offset = src.offset;
    return *this;
  }

  bool operator==(offset_ptr cmp) const { return offset == cmp.offset; }
  bool operator!=(offset_ptr cmp) const { return offset != cmp.offset; }

  operator bool() const { return offset != 0; }

  void set(uint8_t pool_id_, uint64_t offset_) {
    pool_id = pool_id_;
    offset = offset_;
  }

  // the size of the block the ptr points to
  size_t size() const { return BLOCK_SIZES[pool_id].block_size; }

  // masking out the inner block offset (blocks <= 64K)
  size_t mask() const { return MASK_BASE - (std::min(size(), 64 * K) - 1); }

  // the start address of the block
  uint64_t start() const { return offset & mask(); }

  // offset inside block data
  uint16_t ioffset() const { return offset & ~mask(); }

  void change_block(uint64_t old_, uint64_t new_) {
    if (start() == old_) offset = new_ + ioffset();
  }

  void move_ioffset(const offset_ptr& pivot, int delta) {
    // this is a offset to leaf -> they grow from end to start.
    if (pivot.start() == start() && ioffset() >= pivot.ioffset())
      offset -= delta;
  }
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
  BlockPool pools[AREA_COUNT];

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
