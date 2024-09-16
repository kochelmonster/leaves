// declarations for the memory mapped storage
#ifndef _LEAVES_MEMORY_CPP
#define _LEAVES_MEMORY_CPP

#include "memory.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <boost/process/v2/pid.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using namespace boost::interprocess;

namespace leaves {

INLINE DBMemory::DBMemory(const char* path, size_t map_size) {
  memset(&txn, 0, sizeof(txn));
  init_dbfile(path, map_size);
  init_shared();
}

INLINE DBMemory::~DBMemory() {
  shared_memory_object::remove(shared_object.get_name());
}

INLINE void DBMemory::init_dbfile(const char* path, size_t map_size) {
  if (!std::filesystem::is_regular_file(path)) {
    HeaderBlock header;
    memset(&header, 0, sizeof(header));
    strcpy(header.signature, SIGNATURE);
    header.db_version = 0;
    header.active = 0;
    header.txn[0].root = sizeof(header);
    header.txn[0].txn_id = 1;

    auto trie_pool = get_pool(TrieBlock::SIZE);
    const BlockArea& block_sizes = BLOCK_SIZES[trie_pool];
    BlockPool& pool = header.txn[0].pools[trie_pool];
    pool.current = sizeof(header) + TrieBlock::SIZE;
    pool.last = sizeof(header) + block_sizes.area_size;

    header.txn[0].file_size = pool.last;

    TrieBlock root;
    root.offset = sizeof(header);
    root.txn_id = 1;
    root.init();

    std::ofstream fhead(path, std::ios::out | std::ios::binary);
    fhead.write((char*)&header, sizeof(header));
    fhead.write((char*)&root, sizeof(root));
    fhead.close();

    std::filesystem::resize_file(path, header.txn[0].file_size);

    TESTPOINT(DBMemory::init::1);
  } else {
    TESTPOINT(DBMemory::init::2);
    std::ifstream fin(path);
    char signature[sizeof(SIGNATURE)];
    fin.read(signature, sizeof(signature));
    if (strcmp(signature, SIGNATURE)) {
      TESTPOINT(DBMemory::init::3);
      throw std::runtime_error("wrong filetype");
    }
  }

  size_t file_size = std::filesystem::file_size(path);
  file = file_mapping(path, read_write);

  if (!map_size) {
    map_size = (1 + (file_size / T)) * T;
    // for Valgrind 
    map_size = 20 * G;
  }

  region = mapped_region(file, read_write, 0, map_size);
  db = (HeaderBlock*)region.get_address();

  assert(db->txn[0].file_size <= file_size);
  assert(db->txn[1].file_size <= file_size);
}

INLINE void DBMemory::init_shared() {
  std::vector<std::string> parts;
  boost::split(parts, get_filename(), boost::is_any_of("/\\"));
  std::string name = *parts.rbegin();
  name.insert(0, "shared-");
  try {
    shared_object = shared_memory_object(create_only, name.c_str(), read_write);
    shared_object.truncate(sizeof(SharedMem));
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
    memset(shared, 0, sizeof(SharedMem));

    int next_active = (db->active + 1) & 1;
    if (db->txn[db->active].txn_id < db->txn[next_active].txn_id) {
      // an unfinished transaction
      commit();
      TESTPOINT(UnfinishedCommit);
    }
  } catch (...) {
    shared_object = shared_memory_object(open_only, name.c_str(), read_write);
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
  }
}

INLINE const DBTransaction* DBMemory::get_active_txn() const {
  return &db->txn[db->active];
}

INLINE const BlockUnion* DBMemory::get_root() const {
  return get_block(get_active_txn()->root);
}

const int TRIE_POOL = get_pool(TrieBlock::SIZE);

INLINE block_ptr DBMemory::alloc_cow_block() {
  BlockUnion* block = alloc_block(TRIE_POOL);
  block->trie.init();
  return block;
}

INLINE void DBMemory::free_cow_block(BlockUnion* block) {
  assert(block->meta.type == kTrieBlock);
  free_block(get_block(block->meta.offset), TRIE_POOL);
}

INLINE block_ptr DBMemory::clone_cow_block(offset_ptr offset) {
  block_ptr org_block = get_block(offset);
  assert(org_block->meta.type == kTrieBlock);

  BlockUnion* block = alloc_cow_block();

  block->trie.used = org_block->trie.used;
  memcpy(block->trie.data, org_block->trie.data, block->trie.used);

  free_block(org_block, TRIE_POOL);
  return block;
}

INLINE block_ptr DBMemory::alloc_block(int pool_id) {
  assert(txn.root);
  BlockPool& pool = txn.pools[pool_id];
  if (pool.free_start) {
    tid_t min_txn_id = get_min_txn_id();
    block_ptr free_block = get_block(pool.free_start);
    if (free_block->meta.free_txn_id < min_txn_id) {
      TESTPOINT(DBMemory::alloc_block::1);
      pool.free_start = free_block->meta.next_free;
      if (!pool.free_start) pool.free_end = 0;
      free_block->meta.txn_id = txn.txn_id;
      return free_block;
    }
    TESTPOINT(DBMemory::alloc_block::2);
  }

  TESTPOINT(DBMemory::alloc_block::3);
  offset_ptr new_offset = alloc_new_block(pool_id);
  block_ptr free_block = get_block(new_offset);
  free_block->meta.txn_id = txn.txn_id;
  free_block->meta.offset = new_offset;
  return free_block;
}

INLINE offset_ptr DBMemory::alloc_new_block(int pool_id) {
  assert(txn.root);

  BlockPool& pool = txn.pools[pool_id];
  const BlockArea& area = BLOCK_SIZES[pool_id];

  if (pool.current == pool.last) {
    // we have to create a new pool area
    pool.last = txn.file_size + area.area_size;
    std::filesystem::resize_file(get_filename(), pool.last);
    pool.current = txn.file_size;
    txn.file_size = pool.last;
    TESTPOINT(DBMemory::alloc_new_block::1);
  }

  offset_ptr result(pool.current);
  pool.current += area.block_size;
  TESTPOINT(DBMemory::alloc_new_block::2);
  return result;
}

INLINE void DBMemory::free_block(block_ptr block, int pool_id) {
  assert(txn.root);  // transacrion is active
  if (pool_id < 0) pool_id = get_pool(block->meta.size);

  block->meta.free_txn_id = txn.txn_id;

  BlockPool& pool = txn.pools[pool_id];
  if (pool.last_free_start) {
    block->meta.next_free = pool.last_free_start;
    pool.last_free_start = block->meta.offset;
    TESTPOINT(DBMemory::free_block::1);
  } else {
    block->meta.next_free = 0;
    pool.last_free_start = pool.last_free_end = block->meta.offset;
    TESTPOINT(DBMemory::free_block::2);
  }
}

INLINE offset_ptr DBMemory::write_value(const Slice& value) {
  auto pool_id = get_pool(value.size() + ValueBlock::OVERHEAD);
  ValueBlock& block = alloc_block(pool_id)->value;
  const BlockArea& area = BLOCK_SIZES[get_pool(value.size())];
  block.type = kValueBlock;
  block.size = area.block_size;
  memcpy(block.data, value.data(), value.size());
  return block.offset;
}

INLINE bool DBMemory::start_transaction() {
  if (ipcdetail::atomic_cas32(&shared->transaction_active, 1, 0) == 1) {
    TESTPOINT(DBMemory::start_transaction::1);
    return false;
  }

  memcpy(&txn, get_active_txn(), sizeof(txn));
  txn.txn_id++;

  // add the free pools
  for (int i = 0; i < BLOCK_POOL_COUNT; i++) {
    BlockPool& pool = txn.pools[i];

    if (pool.last_free_start) {
      if (pool.free_end) {
        get_block(pool.free_end)->meta.next_free = pool.last_free_start;
        TESTPOINT(DBMemory::start_transaction::2);
      }
      else {
        pool.free_start = pool.last_free_start;
        TESTPOINT(DBMemory::start_transaction::3);
      }

      pool.free_end = pool.last_free_end;
      pool.last_free_start = pool.last_free_end = 0;
    }
  }
  return true;
}

INLINE void DBMemory::rollback() {
  end_transaction();
  shared->transaction_active = 0;
}

INLINE void DBMemory::prepare_commit() {
  int active = (db->active + 1) & 1;
  memcpy(&db->txn[active], &txn, sizeof(txn));
  region.flush();
  end_transaction();
}

INLINE void DBMemory::commit() {
  db->active = (db->active + 1) & 1;
  region.flush();
  shared->transaction_active = 0;
}

INLINE void DBMemory::end_transaction() { memset(&txn, 0, sizeof(txn)); }

INLINE int DBMemory::alloc_cursor() {
  uint32_t last_index = shared->last_index;

  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
    ReadCursor& cursor = shared->readers[
        (i + last_index) % SharedMem::READER_COUNT];
    if (!cursor.txn_id) {
      if (ipcdetail::atomic_cas32(&shared->last_index, last_index, i+1) == last_index) {
        cursor.txn_id = get_active_txn()->txn_id;
        cursor.pid = boost::process::v2::current_pid();
        return i;
      }
      else {
        TESTPOINT(DBMemory::alloc_cursor::1);
        return alloc_cursor();
      }
    }
  }
  throw std::runtime_error("Cannot allocate cursor");
}

INLINE offset_ptr DBMemory::update_cursor(int id) {
  ReadCursor& cursor = shared->readers[id];
  const DBTransaction* txn = get_active_txn();
  if (txn->txn_id != cursor.txn_id) {
    cursor.txn_id = txn->txn_id;
    shared->max_free_transaction = 0;
  }
  return txn->root;
}

INLINE void DBMemory::free_cursor(int id) {
  shared->readers[id].txn_id = 0;
  shared->max_free_transaction = 0; // invalidate the transaction
}

INLINE tid_t DBMemory::get_min_txn_id() {
  if (shared->max_free_transaction)
    return shared->max_free_transaction;

  tid_t min_trans = get_active_txn()->txn_id;
  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
      ReadCursor& cursor = shared->readers[i];
      if (cursor.txn_id)
        min_trans = std::min(min_trans, cursor.txn_id);
  }

  shared->max_free_transaction = min_trans;
  return min_trans;
}


}  // namespace leaves

#endif  // _LEAVES_MEMORY_CPP