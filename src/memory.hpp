// declarations for the node storage
#ifndef _LEAVES_MEMORY_HPP
#define _LEAVES_MEMORY_HPP

#include <algorithm>
#include <bit>
#include <boost/pool/object_pool.hpp>
#include <boost/unordered_map.hpp>
#include <fstream>
#include <leaves.hpp>
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
union Node;
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

enum BlockType {
  kContainer = 0,
  kTrieBlock,
  kValueBlock
};

struct PersistBlock {
  // the blocks offset (==id)
  offset_ptr offset;

  // the block size
  struct {
    uint64_t writable : 1;
    uint64_t type : 2;
    uint64_t size : 61;
  };

  // returns the pool id of the block
  uint16_t pool_id() const;
};

struct BlockContainer : public PersistBlock {
  struct FreedBlock {
    offset_ptr offset;
    tid_t txn_id;  // transaction that freed the block
  };

  // optimiation: if != 0 all blocks.txn_id are bigger than min_txn_id
  tid_t min_txn_id;

  // count of items in blocks
  size_t count;
  // next BlockContainer or 0
  offset_ptr next;

  static const size_t SIZE = 4 * K;
  static const size_t MAX_ITEMS =
      (SIZE - sizeof(PersistBlock) - sizeof(count) - sizeof(next) - sizeof(min_txn_id)) /
      sizeof(FreedBlock);

  // array of free blocks
  FreedBlock blocks[MAX_ITEMS];

  size_t used_bytes() const {
    return (sizeof(PersistBlock) + sizeof(count) + sizeof(next) +
            (sizeof(FreedBlock) * count));
  }

  void init(offset_ptr offset_) {
    type = kContainer;
    size = SIZE;
    offset = offset_;
    min_txn_id = 0;
    count = 0;
    next = 0;
  }
};

typedef uint16_t ssize_t;

enum NodeType { kNull, kBitTrie, kTrie, kValue, kLink, kCompressed, kEndType };

// A pointer inside a trie block.
struct node_ptr {
  union {
    struct {
      /* mini offset within the data attribute
         the real offset is moffset << 3
       */
      ssize_t moffset : 13;

      // type of the node
      ssize_t type : 3;
    };
    ssize_t val;
  };

  node_ptr(ssize_t val_ = 0) : val(val_) {}
  node_ptr(ssize_t moffset_, NodeType type_) : moffset(moffset_), type(type_) {}

  node_ptr& operator=(const node_ptr& src) {
    val = src.val;
    return *this;
  }

  ssize_t offset() const { return moffset << 3; }
};

struct TrieBlock : public PersistBlock {
  static const size_t SIZE = PAGE_SIZE;
  static const ssize_t MIN_SIZE = sizeof(offset_ptr);

  // count of used space in data array
  ssize_t used;

  static const size_t DATA_SIZE = SIZE - sizeof(PersistBlock) - sizeof(used);

  // memory of trie nodes
  char data[DATA_SIZE];

  void init(offset_ptr offset_) {
    offset = offset_;
    size = SIZE;
    type = kTrieBlock;
    used = 8;  // multiple of 8
    *resolve_ptr(0) = node_ptr();
  }

  // allocs size bytes and returns the offset of the allocated area
  node_ptr alloc(ssize_t size, NodeType type);
  Node* resolve(node_ptr ptr) { return (Node*)&data[ptr.offset()]; }
  const Node* resolve(node_ptr ptr) const {
    return (const Node*)&data[ptr.offset()];
  }
  node_ptr* resolve_ptr(ssize_t onode) const { return (node_ptr*)&data[onode]; }
};

// A block for a big value
struct ValueBlock : public PersistBlock {
  static const size_t OVERHEAD = sizeof(PersistBlock);
  char data[0];
};

#pragma pack(0)

const static BlockArea BLOCK_SIZES[] = {
    {BLOCK_SIZE1, 2 * K},                 // 0  -> 2048
    {BLOCK_SIZE1, 4 * K},                 // 1  -> 4096
    {BLOCK_SIZE1, 8 * K},                 // 2  -> 8192
    {BLOCK_SIZE1, 16 * K},                // 3  -> 16K
    {BLOCK_SIZE1, 32 * K},                // 4  -> 32K
    {BLOCK_SIZE1, 64 * K},                // 5  -> 64K
    {BLOCK_SIZE2, 128 * K},               // 6  -> 128K
    {BLOCK_SIZE2, 256 * K},               // 7  -> 256K
    {BLOCK_SIZE2, 512 * K},               // 8  -> 512K
    {BLOCK_SIZE2, M},                     // 9  -> 1M
    {BLOCK_SIZE2, 2 * M},                 // 10 -> 2M
    {BLOCK_SIZE2, 4 * M},                 // 11 -> 4M
    {BLOCK_SIZE3, 8 * M},                 // 12 -> 8M
    {BLOCK_SIZE3, 16 * M},                // 13 -> 16M
    {BLOCK_SIZE3, 32 * M},                // 14 -> 32M
    {BLOCK_SIZE3, 64 * M},                // 15 -> 64M
    {BLOCK_SIZE3, 128 * M},               // 16 -> 128M
    {BLOCK_SIZE3, 256 * M},               // 17 -> 256M
    {BLOCK_SIZE4, 512 * M},               // 18 -> 512M
    {BLOCK_SIZE4, G},                     // 19 -> 1G
    {BLOCK_SIZE4, 2 * G},                 // 20 -> 2G
    {BLOCK_SIZE4, 4 * G},                 // 21 -> 4G
    {BLOCK_SIZE1, BlockContainer::SIZE},  // Area for free blocks
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
};

struct DBMeta {
  // Block Pools
  BlockPool pools[BLOCK_POOL_COUNT + 1];

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

      DBMeta head[2];
    };
    char data[2 * K];
  };
};

typedef BlockUnion* block_ptr;

/*
Manages the memory blocks of the database
*/
struct DBMemory {
  typedef boost::unordered_map<offset_ptr, BlockUnion*> writeable_m;
  typedef boost::object_pool<BlockUnion> cow_pool_t;

  DBMemory(const char* path, size_t map_size = 0);
  ~DBMemory();

  // initialize a database file
  static void init(const char* path);

  size_t get_size() const { return region.get_size(); }

  const char* get_filename() const { return file.get_name(); }

  // get const pointer to a memory block
  block_ptr get_block(offset_ptr offset) const;

  // returns a heap version of a block with the same offset
  BlockContainer* get_writeable_container(offset_ptr offset);

  /* allocs a new block for copy on write, and returns a heap copy of that
     block. if the block is reclaimed from the free blocks, its transaction id
     must be lower than min_txn_id.
   */
  block_ptr alloc_cow_block(tid_t min_txn_id, size_t size = PAGE_SIZE);

  /* get the transaction block for the given offset.
     -> returns a writable cow block if it exists otherwise a readonly block
   */
  block_ptr get_txn_block(offset_ptr offset);

  /* clone a block into a newly allocated writable block
     if the new block is reclaimed from the free blocks, its transaction id must
     be lower than min_txn_id.
   */
  block_ptr clone_cow_block(tid_t min_txn_id, offset_ptr offset);

  /* Allocates a block from the database memory.
     if the block is reclaimed from the free blocks, its transaction id must be
     lower than min_txn_id.
  */
  offset_ptr alloc_block(tid_t min_txn_id, size_t size);

  // Allocs a new block in the pool area. It will not reuse a block
  offset_ptr alloc_new_block(int pool_id);

  // frees a a cow block
  void free_cow_block(BlockUnion* block);

  // Releases a block to free memory.
  void free_block(block_ptr block);

  BlockContainer* alloc_container();
  void free_container(BlockContainer* container);

  // writes data to the database file
  void write(offset_ptr offset, const void* data, size_t size);

  // Writes the ValueBlock at offset
  offset_ptr write_value(tid_t min_txn_id, const Slice& value);

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

  // map of active heap blocks
  writeable_m writeable_map;
  cow_pool_t writeable_pool;
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
