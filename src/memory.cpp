#include "memory.hpp"

#include <cstddef>
#include <filesystem>
#include <algorithm>

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
    
    auto trie_pool = get_pool(TRIE_PAGE_SIZE);
    const BlockArea& block_sizes = BLOCK_SIZES[trie_pool];
    BlockPool& pool = header.head[0].pools[trie_pool];
    pool.current = sizeof(header) + TRIE_PAGE_SIZE;
    pool.last = sizeof(header) + block_sizes.area_size;

    TrieBlock root;
    root.offset = sizeof(header);
    root.init(0);
    
    std::ofstream fhead(path, std::ios::out | std::ios::binary);
    fhead.write((char*)&header, sizeof(header));
    fhead.write((char*)&root, sizeof(root));
    fhead.close();

    std::filesystem::resize_file(path, pool.last);
  }
  else {
    std::ifstream fin(path);
    char signature[sizeof(SIGNATURE)];
    fin.read(signature, sizeof(signature));
    if (strcmp(signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");
  }
}

INLINE DBMemory::DBMemory(const char* path, size_t map_size) {
    memset(&head, 0, sizeof(head));

    size_t size = std::filesystem::file_size(path);
    file = file_mapping(path, read_only);

    if (!map_size) {
      map_size = (1+(size / T))*T;
    }

    region = mapped_region(file, read_only, 0, map_size);
    db = (HeaderBlock*)region.get_address();

    const DBMeta* head_ = get_active_head();

    // Header* backup = &((HeaderBlock*)db)->backup;
    free_start = 0;
    for(int i = 0; i < BLOCK_POOL_COUNT; i++) {
      free_start = std::max(free_start, head_->pools->last);
    }

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


INLINE BlockUnion* DBMemory::alloc_cow_block(tid_t max_transaction, size_t size) {
  offset_ptr offset = alloc_block(max_transaction, size);
  BlockUnion& block = writeable_map[offset];
  block.block.offset = offset;
  return &block;
}

INLINE BlockUnion* DBMemory::get_cow_block(tid_t max_transaction, offset_ptr offset) {
  auto found = writeable_map.find(offset);
  if (found != writeable_map.end())
    return &found->second;

  block_ptr org_block = get_block(offset);
  BlockUnion* block = alloc_cow_block(max_transaction, org_block->block.size);
  offset_ptr new_offset = block->block.offset;
  
  memcpy(block, org_block, block->block.size);
  block->block.offset = new_offset;
  
  free_block(org_block);
  return block;
}

INLINE BlockUnion* DBMemory::get_writeable_block(offset_ptr offset) {
  auto found = writeable_map.find(offset);
  if (found != writeable_map.end())
    return &found->second;

  BlockUnion& block = writeable_map[offset];

  auto org_block = get_block(offset);
  assert(org_block->block.size == PAGE_SIZE);
  memcpy(&block, org_block, PAGE_SIZE);

  return &block;
}

INLINE offset_ptr DBMemory::alloc_block(tid_t max_transaction, size_t size) {
    assert(head.root);

    auto pool_id = get_pool(size);
    BlockPool& pool = head.pools[pool_id];
    const BlockArea& area = BLOCK_SIZES[pool_id];

    offset_ptr* free(&pool.free);
    while (*free) {
        // Get the block from the free space
        BlockContainer& container = get_writeable_block(*free)->container;

        if (!container.count && pool_id == PAGE_POOL) {
          *free = container.next;
          return container.offset;
        }

        for(int i = container.count-1; i >= 0; i--) {
            block_ptr test = get_block(container.blocks[i]);
            if (test->block.transaction < max_transaction) {
              container.count--;
              memmove(&container.blocks[i], &container.blocks[i+1], 
                sizeof(offset_ptr)*(container.count-i));

              if (!container.count && pool_id != PAGE_POOL) {
                *free = container.next;
                free_block(block_ptr((BlockUnion*)(&container)));
              }

              return test->block.offset;
            }
        }
        free = &container.next;
    }

    return alloc_new_block(pool_id);
}

INLINE offset_ptr DBMemory::alloc_new_block(int pool_id) {
  assert(head.root);

  BlockPool& pool = head.pools[pool_id];
  const BlockArea& area = BLOCK_SIZES[pool_id];

  if (pool.current == pool.last) {
      // we have to create a new pool area
      if (free_start >= region.get_size()) {
        grow_file(free_start+area.area_size);
      }

      pool.current = free_start;
      pool.last == free_start+area.area_size;

      free_start += area.area_size;
    }

    offset_ptr result(pool.current);
    pool.current += area.block_size;
    return result;
}

INLINE void DBMemory::free_block(block_ptr block) {
  assert(head.root);

  auto pool_id = get_pool(block->block.block_size());
  BlockPool& pool = head.pools[pool_id];
  BlockContainer* container;

  if (! pool.free) {
    pool.free = alloc_new_block(PAGE_POOL);
    container = &get_writeable_block(pool.free)->container;
    container->init(pool.free);
  }
  else {
    container = &get_writeable_block(pool.free)->container;
  }

  if (container->count == BlockContainer::MAX_ITEMS) {
    offset_ptr offset = alloc_new_block(PAGE_POOL);
    BlockContainer& new_container = get_writeable_block(offset)->container;
    new_container.init(offset);
    new_container.next = pool.free;
    container->next = offset;
    container = &new_container;
    
  }

  container->blocks[container->count++] = block->block.offset;
}

INLINE void DBMemory::grow_file(size_t new_size) {
  std::filesystem::resize_file(get_filename(), new_size);
  region = mapped_region(file, read_only, 0, new_size);
  db = (HeaderBlock*)region.get_address();
}

INLINE void DBMemory::write(offset_ptr offset, const void* data, size_t size) {
  output.seekp(offset, std::ios_base::beg);
  output.write((const char*)data, size);
}

INLINE void DBMemory::write_value(tid_t transaction, offset_ptr offset, const Slice& value) {
  const BlockArea& area = BLOCK_SIZES[get_pool(value.size())];
  ValueBlock block;
  block.transaction = transaction;
  block.offset = offset;
  block.size = area.block_size;
  write(offset, &block, sizeof(block));
  write(offset+sizeof(block), value.data(), value.size());
}

INLINE void DBMemory::prepare_transaction() {
  memcpy(&head, get_active_head(), sizeof(head));
}
    
INLINE void DBMemory::write_transaction(int active) {
  offset_ptr offset = (((char*)&db->head[active]) - &db->data[0]);
  write(offset, &head, sizeof(head));
  for(auto iter = writeable_map.begin(); iter != writeable_map.end(); iter++) {
    write(iter->second.block.offset, &iter->second.block, PAGE_SIZE);
  }
}

INLINE void DBMemory::end_transaction() {
  memset(&head, 0, sizeof(head));
  writeable_map.clear();
}

INLINE void DBMemory::write_active(int active) {
  write(offsetof(HeaderBlock, active), &active, sizeof(active));
}

} // namespace leaves