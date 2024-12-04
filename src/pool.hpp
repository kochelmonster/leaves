// declarations for managing the memory pool
#ifndef _LEAVES_POOL_HPP
#define _LEAVES_POOL_HPP

#include "block.hpp"

namespace leaves {

const size_t BLOCK_SIZE1 = 1 << 20;
const size_t BLOCK_SIZE2 = 1 << 28;
const size_t BLOCK_SIZE3 = 1 << 30;
const size_t BLOCK_SIZE4 = (size_t)1 << 34;
const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;


struct BlockArea {
  size_t area_size;
  size_t block_size;
};


const static BlockArea BLOCK_SIZES[] = {
    {BLOCK_SIZE1, 2 * K},    // 0  -> 2048
    {BLOCK_SIZE1, 4 * K},    // 1  -> 4096
    {BLOCK_SIZE1, 8 * K},    // 2  -> 8192
    {BLOCK_SIZE1, 16 * K},   // 3  -> 16K
    {BLOCK_SIZE1, 32 * K},   // 4  -> 32K
    {BLOCK_SIZE1, 64 * K},   // 5  -> 64K
    {BLOCK_SIZE2, 128 * K},  // 6  -> 128K
    {BLOCK_SIZE2, 256 * K},  // 7  -> 256K
    {BLOCK_SIZE2, 512 * K},  // 8  -> 512K
    {BLOCK_SIZE2, M},        // 9  -> 1M
    {BLOCK_SIZE2, 2 * M},    // 10 -> 2M
    {BLOCK_SIZE2, 4 * M},    // 11 -> 4M
    {BLOCK_SIZE3, 8 * M},    // 12 -> 8M
    {BLOCK_SIZE3, 16 * M},   // 13 -> 16M
    {BLOCK_SIZE3, 32 * M},   // 14 -> 32M
    {BLOCK_SIZE3, 64 * M},   // 15 -> 64M
    {BLOCK_SIZE3, 128 * M},  // 16 -> 128M
    {BLOCK_SIZE3, 256 * M},  // 17 -> 256M
    {BLOCK_SIZE4, 512 * M},  // 18 -> 512M
    {BLOCK_SIZE4, G},        // 19 -> 1G
    {BLOCK_SIZE4, 2 * G},    // 20 -> 2G
    {BLOCK_SIZE4, 4 * G},    // 21 -> 4G
};

const int BLOCK_POOL_COUNT = (sizeof(BLOCK_SIZES) / sizeof(BlockArea));

// returns the right pool id for a block size
inline constexpr int get_pool(size_t size) {
  for (int i = 0; i < BLOCK_POOL_COUNT; i++) {
    if (size <= BLOCK_SIZES[i].block_size) return i;
  }
  throw std::overflow_error((const char*)"block size too big");
  return BLOCK_POOL_COUNT;
}

typedef uint64_t offset_ptr;
typedef uint64_t tid_t;


// Allocates and frees Blocks
struct BlockPool {
  // current slot for next free memory position
  offset_ptr current;

  // the end of the currently allocated block
  offset_ptr last;

  // list of free blocks that can be used
  offset_ptr free_start;
  offset_ptr free_end;

  // list of free blocks of the last active transaction
  offset_ptr last_free_start;
  offset_ptr last_free_end;
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

struct DBTransaction {
  // Block Pools
  BlockPool pools[BLOCK_POOL_COUNT];

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

struct HeaderBlock {
  union {
    struct {
      // first bytes of the db file
      char signature[sizeof(SIGNATURE)];

      // version of the db file
      uint16_t db_version;

      // active db
      int active;

      DBTransaction txn[2];
    };
    char data[2 * K];
  };
};

}  // namespace leaves

#endif  // _LEAVES_POOL_HPP
