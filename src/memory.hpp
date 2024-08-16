// declarations for the node storage
#ifndef _LEAVES_MEMORY_HPP
#define _LEAVES_MEMORY_HPP

#include <algorithm>
#include <bit>
#include <fstream>
#include <leaves.hpp>
#include <unordered_map>
#ifdef WASM
#else
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#endif

#ifdef TESTING
#include <string>
#include <vector>
#endif

#ifdef HEADER_ONLY
#define INLINE inline
#else
#define INLINE
#endif

namespace leaves {

#ifndef WASM
using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::shared_memory_object;
#endif

const size_t PAGE_SIZE = 1 << 16;
const size_t BLOCK_SIZE1 = 1 << 20;
const size_t BLOCK_SIZE2 = 1 << 28;
const size_t BLOCK_SIZE3 = 1 << 30;
const size_t BLOCK_SIZE4 = (size_t)1 << 34;

struct BlockArea {
  size_t area_size;
  size_t block_size;
};

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;


typedef uint64_t offset_ptr;
typedef uint64_t tid_t;

struct Storage;
struct Trace;
union BlockUnion;

#pragma pack(1)

// Allocates and frees Blocks from and to Memory
struct BlockPool {
  // current slot for next free memory position
  offset_ptr current;

  // the end of the currently allocated block
  offset_ptr last;

  // offset to a MarkedBlocks Page with free slots
  offset_ptr free;
};

struct PersistBlock {
  // the blocks offset (==id)
  offset_ptr offset;

  // the block size
  size_t size;

  // returns the pool id of the block
  uint16_t pool_id() const;
};


struct BlockContainer : public PersistBlock {
  struct FreedBlock {
    offset_ptr offset;
    tid_t txn_id;  // transaction that freed the block
  };

  // count of items in blocks
  size_t count;
  // next BlockContainer or 0
  offset_ptr next;

  static const size_t SIZE = 4 * K;
  static const size_t MAX_ITEMS = (SIZE - sizeof(PersistBlock) -
                                   sizeof(count) - sizeof(next)) /
                                  sizeof(FreedBlock);

  // array of free blocks
  FreedBlock blocks[MAX_ITEMS];

  void init(offset_ptr offset_) {
    assert(size == SIZE);
    offset = offset_;
    count = 0;
    next = 0;
  }
};


typedef uint16_t ssize_t;

struct TrieBlock : public PersistBlock {
  struct FreeBlock {
    // pointer to the next free block
    ssize_t next;

    // size of this free block
    ssize_t size;

    // size of all free blocks
    ssize_t free;
  };

  static const size_t SIZE = PAGE_SIZE;
  static const ssize_t MIN_SIZE = sizeof(offset_ptr);

  /* offsets to FreeBlocks
     0: Blocks of size 8 byte
     1: Blocks of size 16 byte
     2: Blocks of size 32 bytes
     3: Blocks > 32 bytes
   */
  ssize_t free_blocks[4];

  // count of used space in data array
  ssize_t used;

  static const size_t DATA_SIZE = SIZE - sizeof(PersistBlock) -
                                  sizeof(free_blocks) - sizeof(used);

  // memory of trie nodes
  char data[DATA_SIZE];

  void init() {
    assert(size == SIZE);
    memset(free_blocks, 0, sizeof(free_blocks));
    used = 2;
    data[0] = data[1] = 0;  // pointer to kNull
  }

  static inline int block_id(ssize_t size) {
    return std::min((size >> 3) - 3, 3);
  }

  // allocs size bytes and returns the offset of the allocated area
  ssize_t alloc(ssize_t size);
  void free(ssize_t offset, ssize_t size);
};

// A block for a big value
struct ValueBlock : public PersistBlock {
  static const size_t OVERHEAD = sizeof(PersistBlock);
  char data[0];
};

const size_t BURST_PAGE_SIZE = 2 * PAGE_SIZE;

/*
  A block for a burst table

  The Table layout is
  +--------------------------------+
  | item_offset[0]                  |
  +--------------------------------+
  | item_offset[1]                  |
  +--------------------------------+
  |                                |
  |       free_space               |
  |                                |
  +--------------------------------+
  | DataItem[1]                    |
  +--------------------------------+
  | DataItem[0]                    \
  +--------------------------------+

  item_offset are growing from top to bottom
  the item offset is sorted for DataItem.key_data[0..key_size]

  DataItem is growing from bottom to top
*/

struct TableBlock : public PersistBlock {
  struct Item {
    size_t data_size;
    ssize_t key_size;
    char key_data[];

    int compare(const Slice& other) const;
  };

  // count of offsets
  ssize_t count;

  // the start of the item section (growing backwards)
  ssize_t item_start;

  union {
    ssize_t offsets[0];
    char data[BURST_PAGE_SIZE - sizeof(PersistBlock)];
  };

  void init() {
    assert(size == BURST_PAGE_SIZE);
    count = 0;
    item_start = (ssize_t)sizeof(data);
  }

  ssize_t available_space() const {
    return item_start - sizeof(ssize_t) * count;
  }

  Item* get_item(uint16_t index) const;
  int find(const Slice& key) const;
  void insert(Trace& cursor, const Slice& value);
  ssize_t add_item(Trace& cursor, const Slice& value);
};

#pragma pack(0)


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
    {BLOCK_SIZE1, BlockContainer::SIZE},    // Area for free blocks
};

const int BLOCK_POOL_COUNT = (sizeof(BLOCK_SIZES) / sizeof(BlockArea)) - 1;
// The pool that is used for free block containers
const int FREE_POOL = BLOCK_POOL_COUNT;


// returns the right pool id for a block size
inline int get_pool(size_t size) {
  for (int i = 0; i < BLOCK_POOL_COUNT; i++) {
    if (size <= BLOCK_SIZES[i].block_size) return i;
  }
  assert(0);
  return BLOCK_POOL_COUNT - 1;
}


union BlockUnion {
  PersistBlock block;
  BlockContainer container;
  TrieBlock trie;
  ValueBlock value;
  TableBlock table;
};

struct DBMeta {
  // Block Pools
  BlockPool pools[BLOCK_POOL_COUNT+1];

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

      DBMeta head[2];
    };
    char data[2*K];
  };
};

typedef BlockUnion* block_ptr;

/*
Manages the memory blocks of the database
*/
struct DBMemory {
  typedef std::unordered_map<offset_ptr, BlockUnion> writeable_m;

  DBMemory(const char* path, size_t map_size = 0);

  // initialize a database file
  static void init(const char* path);

  size_t get_size() const { return region.get_size(); }

  const char* get_filename() const { return file.get_name(); }

  // get const pointer to a memory block
  block_ptr get_block(offset_ptr offset) const;

  // returns a heap version of a block with the same offset
  BlockUnion* get_writeable_block(offset_ptr offset, size_t size);

  /* allocs a new block for copy on write, and returns a heap copy of that
     block. if the block is reclaimed from the free blocks, its transaction id
     must be lower than min_txn_id.
   */
  BlockUnion* alloc_cow_block(tid_t min_txn_id, size_t size = PAGE_SIZE);

  /* allocs a new block for copy on write, creates a heap version
     and copies the given block to that heap copy.

     txn_id is the currently active transaction id.

     if the block is reclaimed from the free blocks, its transaction id must be
     lower than min_txn_id.
   */
  BlockUnion* get_cow_block(tid_t txn_id, tid_t min_txn_id, offset_ptr offset);

  /* Allocates a block from the database memory.
     if the block is reclaimed from the free blocks, its transaction id must be
     lower than min_txn_id.
  */
  offset_ptr alloc_block(tid_t min_txn_id, size_t size);

  // Allocs a new block in the pool area. It will not reuse a block
  offset_ptr alloc_new_block(int pool_id);

  // Releases a block to free memory.
  void free_block(tid_t txn_id, block_ptr block);

  BlockContainer* alloc_container();
  void free_container(BlockContainer* container);

  // writes data to the database file
  void write(offset_ptr offset, const void* data, size_t size);

  // Writes the ValueBlock at offset
  void write_value(offset_ptr offset, const Slice& value);

  // returns the database active head
  const DBMeta* get_active_head() const;

  // returns a pointer to the active root block
  const BlockUnion* get_root() const;

  // prepares the head attribute for transaction
  void prepare_transaction();

  // write the current transaction to the db
  void write_transaction();

  // write active
  void commit_transaction();

  // clears head and writeables
  void end_transaction();

  file_mapping file;
  mapped_region region;

  // the pointer to the memory mapped area of the db
  HeaderBlock* db;

  // the active writable head of db
  DBMeta head;

  // stream to write to the database file
  std::ofstream output;

  offset_ptr file_size;

  // map of active heap blocks
  writeable_m writeable_map;
};

inline block_ptr DBMemory::get_block(offset_ptr offset) const {
  return (block_ptr)(&db->data[offset]);
}

#ifdef TESTING

struct TestPoints {
  typedef std::unordered_map<std::string, int> points_m;

  static points_m tp_output;

  static void clear() { tp_output.clear(); }

  static void testpoint(const char* str) { tp_output[str] += 1; }
};

inline TestPoints::points_m TestPoints::tp_output;

#define TESTPOINT(x) TestPoints::testpoint(#x)
#else
#define TESTPOINT(x)
#endif

}  // namespace leaves

#endif  // _LEAVES_MEMORY_HPP
