// declarations for the memory mapped storage
#ifndef _LEAVES_MEMORY_HPP
#define _LEAVES_MEMORY_HPP

#include "block.hpp"
#include "pool.hpp"

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

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

typedef BlockUnion* block_ptr;


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
  void init_dbfile(const char* path, size_t map_size);

  // initialize the shared memory
  void init_shared();

  size_t get_size() const { return region.get_size(); }

  const char* get_filename() const { return file.get_name(); }

  bool transaction_active() const {
    return shared->transaction_active;
  }

  // get const pointer to a memory block
  block_ptr get_block(offset_ptr offset) const;

  // get the minimal transaction id of all active cursors
  tid_t get_min_txn_id();

  /* allocs a new block for copy on write, and returns a heap copy of that
     block. if the block is reclaimed from the free blocks, its transaction id
     must be lower than min_txn_id.
   */
  block_ptr alloc_cow_block();

  /* clone a block into a newly allocated writable block
     if the new block is reclaimed from the free blocks, its transaction id must
     be lower than min_txn_id.
   */
  block_ptr clone_cow_block(offset_ptr offset);

  /* Allocates a block from the database memory.
     if the block is reclaimed from the free blocks, its transaction id must be
     lower than min_txn_id.
  */
  block_ptr alloc_block(int poolid);

  // Allocs a new block in the pool area. It will not reuse a block
  offset_ptr alloc_new_block(int pool_id);

  // frees a a cow block
  void free_cow_block(BlockUnion* block);

  // Releases a block to free memory.
  void free_block(block_ptr block, int pool_id=-1);

  // write a value direct to the database
  offset_ptr write_value(const Slice& value);

  // returns the database active transaction
  const DBTransaction* get_active_txn() const;

  // returns a pointer to the active root block
  const BlockUnion* get_root() const;

  // Transaction methods
  bool start_transaction();
  void rollback();
  void prepare_commit();
  void commit();
  void end_transaction();

  // Cursor Methods
  int alloc_cursor(); // returns an id
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
  HeaderBlock* db;

  // the active writable transaction of db
  DBTransaction txn;
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
