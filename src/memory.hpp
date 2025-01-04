// declarations for the memory mapped storage
#ifndef _LEAVES_MEMORY_HPP
#define _LEAVES_MEMORY_HPP

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include "block.hpp"

#ifdef TESTING
#include <string>
#include <unordered_map>
#endif

#ifdef HEADER_ONLY
#define INLINE inline
#else
#define INLINE
#endif

namespace leaves {

using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::shared_memory_object;

#pragma pack(1)
// The header of the database
struct Header {
  // first bytes of the db file
  char signature[SIGNATURE_SIZE];

  // version of the db file
  uint16_t db_version;

  // active db
  uint16_t active;

  char _padding[padding(2 * sizeof(uint16_t), 8) - 2 * sizeof(uint16_t)];

  DBTransaction txn[2];
};
#pragma pack(0)

// shared alloc structure for all read cursor
struct ReadCursor {
  pid_t pid;
  tid_t txn_id;
};

struct SharedMem {
  static const size_t READER_COUNT = 1024;

  // a ring buffer for registering readers
  ReadCursor readers[READER_COUNT];

  // the position of last registered reader
  uint32_t last_index;

  // number of readers
  uint32_t count;

  // if 1 a transaction is active
  uint32_t transaction_active;

  /* the lowest transaction used by the register readers
     memory may only regain pages with a transaction < max_free_transaction
   */
  tid_t max_free_transaction;
};

/*
Manages the memory blocks of the database
*/
struct DBMemory {
  DBMemory(const char* path, size_t map_size = 0);
  ~DBMemory();

  // initialize the shared memory
  void init_dbfile(const char* path, size_t map_size, std::string& shared_name);

  // initialize the shared memory
  void init_shared(const std::string& shared_name);

  size_t get_size() const { return region.get_size(); }

  const char* get_filename() const { return file.get_name(); }

  bool transaction_active() const { return shared->transaction_active; }

  // get const pointer to a memory block
  block_ptr get_block(offset_ptr ptr) const {
    return block_ptr{.ptr = (BlockHeader*)&data[ptr.start()]};
  }

  block_ptr get_block(uint64_t offset) const {
    return block_ptr{.ptr = (BlockHeader*)&data[offset]};
  }

  // get the minimal transaction id of all active cursors
  tid_t get_min_txn_id();

  /* clone a branch block into a newly allocated writable block
     if the new block is reclaimed from the free blocks, its transaction id
     must be lower than min_txn_id.
   */
  block_ptr clone_branch(block_ptr src);

  /* Allocates a block from the database memory.
     if the block is reclaimed from the free blocks, its transaction id must
     be lower than min_txn_id.
  */
  block_ptr alloc_block_by_pool(int pool_id);

  block_ptr alloc_block(size_t size) {
    return alloc_block_by_pool(get_pool(size));
  }

  // Allocs a new block in the pool area. It will not reuse a block
  offset_ptr alloc_new_block(int pool_id);

  // Releases a block to free memory.
  void free_block(block_ptr block);

  // returns the database active transaction
  const DBTransaction* active_txn() const;

  // Transaction methods
  bool start_transaction();
  void rollback();
  void prepare_commit();
  void commit();
  void end_transaction();

  // Cursor Methods
  int alloc_cursor();  // returns an id
  offset_ptr update_cursor(int id);
  void free_cursor(int id);

  // mappping for the shared memory
  shared_memory_object shared_object;
  mapped_region shared_region;
  SharedMem* shared;

  // mapping to the database file
  file_mapping file;
  mapped_region region;

  // the pointer to the memory mapped area of the db
  union {
    Header* db;
    uint8_t* data;
  };

  // the active writable transaction of db
  DBTransaction txn;
};

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
