// declarations for the memory mapped storage
#ifndef _LEAVES_BROWSER_HPP
#define _LEAVES_BROWSER_HPP

#include "block.hpp"
#include "pool.hpp"


namespace leaves {

typedef std::shared_ptr<BlockUnion> block_ptr;

/*
Manages the memory blocks of the database
*/
struct DBBrowser {
  DBBrowser();
  ~DBBrowser();

  size_t get_size() const;

  // get const pointer to a memory block
  block_ptr get_block(offset_ptr offset) const;

  /* allocs a new block for copy on write, and returns a heap copy of that
     block. if the block is reclaimed from the free blocks, its transaction id
     must be lower than min_txn_id.
   */
  block_ptr alloc_cow_block(tid_t min_txn_id);

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
  offset_ptr alloc_block(tid_t min_txn_id, int poolid);

  // Allocs a new block in the pool area. It will not reuse a block
  offset_ptr alloc_new_block(int pool_id);

  // frees a a cow block
  void free_cow_block(BlockUnion* block);

  // Releases a block to free memory.
  void free_block(block_ptr block, int pool_id=-1);

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

  // the pointer to the memory mapped area of the db
  HeaderBlock* db;

  // the active writable transaction of db
  DBTransaction txn;
};

}  // namespace leaves

#endif  // _LEAVES_BROWSER_HPP
