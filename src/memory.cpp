#include "memory.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using namespace boost::interprocess;

namespace leaves {

INLINE void DBMemory::init(const char* path) {
  if (!std::filesystem::is_regular_file(path)) {
    HeaderBlock header;
    memset(&header, 0, sizeof(header));
    strcpy(header.signature, SIGNATURE);
    header.db_version = 0;
    header.active = 0;
    header.head[0].root = sizeof(header);
    header.head[0].txn_id = 1;

    auto trie_pool = get_pool(TrieBlock::SIZE);
    const BlockArea& block_sizes = BLOCK_SIZES[trie_pool];
    BlockPool& pool = header.head[0].pools[trie_pool];
    pool.current = sizeof(header) + TrieBlock::SIZE;
    pool.last = sizeof(header) + block_sizes.area_size;

    header.head[0].file_size = pool.last;

    TrieBlock root;
    root.offset = sizeof(header);
    root.size = TrieBlock::SIZE;
    root.writable = 0;
    root.init();

    std::ofstream fhead(path, std::ios::out | std::ios::binary);
    fhead.write((char*)&header, sizeof(header));
    fhead.write((char*)&root, sizeof(root));
    fhead.close();

    std::filesystem::resize_file(path, header.head[0].file_size);

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
}

INLINE DBMemory::DBMemory(const char* path, size_t map_size) {
  memset(&head, 0, sizeof(head));

  size_t file_size = std::filesystem::file_size(path);
  file = file_mapping(path, read_only);

  if (!map_size) {
    map_size = (1 + (file_size / T)) * T;
  }

  region = mapped_region(file, read_only, 0, map_size);
  db = (HeaderBlock*)region.get_address();

  assert(db->head[0].file_size <= file_size);
  assert(db->head[1].file_size <= file_size);

  writeable_map.reserve(128);

  output.open(path,
              std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
}

INLINE const DBMeta* DBMemory::get_active_head() const {
  return &db->head[db->active];
}

INLINE const BlockUnion* DBMemory::get_root() const {
  return get_block(get_active_head()->root);
}

INLINE BlockUnion* DBMemory::alloc_cow_block(tid_t min_txn_id, size_t size) {
  offset_ptr offset = alloc_block(min_txn_id, size);
  BlockUnion& block = writeable_map[offset];
  block.block.offset = offset;
  block.block.size = size;
  block.block.writable = 1;
  return &block;
}

INLINE void DBMemory::free_cow_block(BlockUnion* block) {
  free_block(get_block(block->block.offset));
  writeable_map.erase(block->block.offset);
}

INLINE BlockUnion* DBMemory::get_cow_block(tid_t min_txn_id,
                                           offset_ptr offset) {
  auto found = writeable_map.find(offset);
  if (found != writeable_map.end()) return &found->second;

  block_ptr org_block = get_block(offset);
  BlockUnion* block = alloc_cow_block(min_txn_id, org_block->block.size);
  offset_ptr new_offset = block->block.offset;

  memcpy(block, org_block, block->block.size);
  block->block.offset = new_offset;

  free_block(org_block);
  return block;
}

INLINE BlockUnion* DBMemory::get_writeable_block(offset_ptr offset,
                                                 size_t size) {
  auto found = writeable_map.find(offset);
  if (found != writeable_map.end()) return &found->second;

  BlockUnion& block = writeable_map[offset];
  memcpy(&block, get_block(offset), size);
  block.block.offset = offset;
  block.block.size = size;
  block.block.writable = 1;
  return &block;
}

INLINE offset_ptr DBMemory::alloc_block(tid_t min_txn_id, size_t size) {
  assert(head.root);

  auto pool_id = get_pool(size);
  BlockPool& pool = head.pools[pool_id];
  const BlockArea& area = BLOCK_SIZES[pool_id];

  // Try to get the block from the freed space
  offset_ptr* free(&pool.free);
  while (*free) {
    // find a freed block that is not in use by another cursor:
    // freed.txn_id < min_txn_id
    BlockContainer& container =
        get_writeable_block(*free, BlockContainer::SIZE)->container;

    for (int i = container.count - 1; i >= 0; i--) {
      BlockContainer::FreedBlock& freed = container.blocks[i];
      if (freed.txn_id < min_txn_id) {
        // the block is surely not used by any other read cursor
        block_ptr pblock = get_block(freed.offset);
        container.count--;
        memmove(&container.blocks[i], &container.blocks[i + 1],
                sizeof(BlockContainer::FreedBlock) * (container.count - i));

        if (!container.count) {
          // free the container
          TESTPOINT(DBMemory::alloc_block::1);
          *free = container.next;  // remove from list
          free_container(&container);
        }
        return pblock->block.offset;
      }
      TESTPOINT(DBMemory::alloc_block::2);
    }
    free = &container.next;
    TESTPOINT(DBMemory::alloc_block::3);
  }

  TESTPOINT(DBMemory::alloc_block::4);
  return alloc_new_block(pool_id);
}

INLINE offset_ptr DBMemory::alloc_new_block(int pool_id) {
  assert(head.root);

  BlockPool& pool = head.pools[pool_id];
  const BlockArea& area = BLOCK_SIZES[pool_id];

  if (pool.current == pool.last) {
    // we have to create a new pool area
    pool.last = head.file_size + area.area_size;
    std::filesystem::resize_file(get_filename(), pool.last);
    pool.current = head.file_size;
    head.file_size = pool.last;
  }

  offset_ptr result(pool.current);
  pool.current += area.block_size;
  return result;
}

INLINE void DBMemory::free_block(block_ptr block) {
  assert(head.root);

  auto pool_id = get_pool(block->block.size);
  BlockPool& pool = head.pools[pool_id];
  BlockContainer* container;

  if (!pool.free) {
    TESTPOINT(DBMemory::free_block::1);
    container = alloc_container();
    pool.free = container->offset;
  } else {
    TESTPOINT(DBMemory::free_block::2);
    container =
        &get_writeable_block(pool.free, BlockContainer::SIZE)->container;
  }

  if (container->count == BlockContainer::MAX_ITEMS) {
    // the container is full -> create a new one and put it at the beginning of
    // the list
    TESTPOINT(DBMemory::free_block::3);
    container = alloc_container();
    container->next = pool.free;
    pool.free = container->offset;
  }

  BlockContainer::FreedBlock& freed = container->blocks[container->count++];
  freed.offset = block->block.offset;
  freed.txn_id = head.txn_id;
}

INLINE BlockContainer* DBMemory::alloc_container() {
  assert(head.root);

  BlockPool& pool = head.pools[FREE_POOL];
  const BlockArea& area = BLOCK_SIZES[FREE_POOL];

  if (pool.free) {
    // Get the block from the freed space
    BlockContainer& container =
        get_writeable_block(pool.free, BlockContainer::SIZE)->container;

    if (!container.count) {
      // the container is empty and free to use
      TESTPOINT(DBMemory::alloc_container::1);
      pool.free = container.next;  // remove from list
      container.next = 0;
      return &container;
    }

    TESTPOINT(DBMemory::alloc_container::2);
    BlockContainer::FreedBlock& freed = container.blocks[--container.count];
    return &get_writeable_block(freed.offset, BlockContainer::SIZE)->container;
  }

  TESTPOINT(DBMemory::alloc_container::3);
  offset_ptr offset = alloc_new_block(FREE_POOL);
  BlockContainer& result =
      get_writeable_block(offset, BlockContainer::SIZE)->container;
  result.init(offset);
  return &result;
}

void DBMemory::free_container(BlockContainer* container) {
  assert(head.root);
  assert(container->count == 0);

  BlockPool& pool = head.pools[FREE_POOL];
  if (!pool.free) {
    TESTPOINT(DBMemory::free_container::1);
    container->next = 0;
    pool.free = container->offset;
    return;
  }

  BlockContainer* pool_container =
      &get_writeable_block(pool.free, BlockContainer::SIZE)->container;

  if (pool_container->count == BlockContainer::MAX_ITEMS) {
    // the container is full -> create a new one and put it at the beginning of
    // the list
    TESTPOINT(DBMemory::free_container::2);
    offset_ptr offset = alloc_new_block(FREE_POOL);
    pool_container =
        &get_writeable_block(offset, BlockContainer::SIZE)->container;
    pool_container->init(offset);
    pool_container->next = pool.free;
    pool.free = offset;
  }

  BlockContainer::FreedBlock& freed =
      pool_container->blocks[pool_container->count++];
  freed.offset = container->offset;
  freed.txn_id = 0;
}

INLINE void DBMemory::write(offset_ptr offset, const void* data, size_t size) {
  output.seekp(offset, std::ios_base::beg);
  output.write((const char*)data, size);
}

INLINE void DBMemory::write_value(offset_ptr offset, const Slice& value) {
  const BlockArea& area = BLOCK_SIZES[get_pool(value.size())];
  ValueBlock block;
  block.offset = offset;
  block.size = area.block_size;
  block.writable = 0;
  write(offset, &block, sizeof(block));
  write(offset + sizeof(block), value.data(), value.size());
}

INLINE void DBMemory::prepare_transaction() {
  memcpy(&head, get_active_head(), sizeof(head));
  head.txn_id++;
}

INLINE void DBMemory::write_transaction() {
  int active = (db->active + 1) & 1;
  offset_ptr offset = (((char*)&db->head[active]) - &db->data[0]);
  write(offset, &head, sizeof(head));
  for (auto iter = writeable_map.begin(); iter != writeable_map.end(); iter++) {
    BlockUnion& block = iter->second;
    block.block.writable = 0;
    write(block.block.offset, &block, block.block.size);
  }
  output.flush();
}

INLINE void DBMemory::end_transaction() {
  memset(&head, 0, sizeof(head));
  writeable_map.clear();
}

INLINE void DBMemory::commit_transaction() {
  int active = (db->active + 1) & 1;
  write(offsetof(HeaderBlock, active), &active, sizeof(active));
  output.flush();
}

}  // namespace leaves