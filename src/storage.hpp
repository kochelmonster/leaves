// declarations for the node storage
#ifndef _LEAVES_STORAGE_HPP
#define _LEAVES_STORAGE_HPP

#include "memory.hpp"

namespace leaves {

struct ReadCursor {
  pid_t pid;
  tid_t transaction;
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

struct Storage {
  typedef std::shared_ptr<DBMemory> DBMemory_ptr;

  Storage(const char* path);
  ~Storage();

  // initialize the shared memory
  void init_shared();

  // transactional methods
  bool start_transaction();
  void rollback();
  void prepare_commit();
  void commit();

  block_ptr get_block(offset_ptr offset) const {
    return memory->get_block(offset);
  }

  BlockUnion* get_cow_block(offset_ptr offset) {
    return memory->get_cow_block(get_max_transaction(), offset);
  }

  BlockUnion* alloc_cow_block(size_t size=PAGE_SIZE) {
    return memory->alloc_cow_block(get_max_transaction(), size);
  }

  offset_ptr alloc_block(size_t size) {
    return memory->alloc_block(get_max_transaction(), size);
  }

  void write_value(offset_ptr offset, const Slice& value) {
    memory->write_value(offset, value);
  }

  // allocate space for registering a read cursor
  int alloc_cursor(offset_ptr root);

  // free the register space
  void free_cursor(int id);

  // get the max transaction that can be freed
  tid_t get_max_transaction();

  tid_t active_transaction;

  // The database memory
  DBMemory_ptr memory;

  // shared memory for all clients
  shared_memory_object shared_object;
  mapped_region shared_region;
  SharedMem* shared;
};

}  // namespace leaves

#endif  // _LEAVES_STORAGE_HPP
