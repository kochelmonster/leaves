#include <fstream>
#include <filesystem>
#include <algorithm>

#include "storage.hpp"
#include "node.hpp"
#include "table.hpp"

#define SIGNATURE "LarchLeaves"

using namespace boost::interprocess;

namespace leaves {

#pragma pack(2)
struct NullData : public Node {
  // we abuse it to save some singletons
  uint64_t version;
  offset_ptr root;
};
#pragma pack(0)

struct StorageHeader {
  char signature[sizeof(SIGNATURE)];
  uint16_t file_version;
  uint16_t table_count;
  size_t value_pool_start_size;
  size_t value_pool_increment;
  size_t value_pool_count;
  uint32_t pools; // start pointer of the pools
  uint32_t null; // pointer to null structure
};

#define HEAD_ALIGN rbtree_best_fit<null_mutex_family>::Alignment
#define OFFSET (((sizeof(StorageHeader)+(HEAD_ALIGN-1))/HEAD_ALIGN)*HEAD_ALIGN)


void Pool::create(
      Storage* storage_, PPool* pool_, uint16_t index_, size_t node_size, size_t area_count) {
  storage = storage_;
  index = index_;
  pool = pool_;
  pool->used_nodes = 0;
  pool->freed_nodes = 0;
  pool->node_size = node_size;
  pool->area_size = area_count*node_size;
  pool->current_area = pool->next_node = storage->mem_allocate(pool->area_size);
  pool->next_free.to_null();
}

any_ptr Pool::allocate() {
  any_ptr result;

  if (pool->next_free) {
    result = pool->next_free.resolve();
    pool->next_free = *result.next;
    pool->freed_nodes--;
  }
  else if ((size_t)(pool->next_node - pool->current_area) >= pool->area_size) {
    pool->current_area = result = storage->mem_allocate(pool->area_size);
    pool->next_node = pool->current_area + (int64_t)pool->node_size;
  }
  else {
    result = pool->next_node.resolve();
    pool->next_node += pool->node_size;
  }

  pool->used_nodes++;
  result.node->pool = index;
  return result;
}

void Pool::free(any_ptr ptr) {
  pool->used_nodes--;
  pool->freed_nodes++;

  assert(ptr.node->pool == index);
  *(ptr.next) = pool->next_free;
  pool->next_free = ptr;
}


Storage::Storage(const char* path, const Options& options) {
  size_t offset = OFFSET;

  grow_size = options.grow_size;
  size_t max_db_size = std::min(options.max_db_size, MAX_DB_SIZE);

  std::ifstream fhead(path, std::ios::in|std::ios::binary);
  if (fhead.is_open()) {
    // load existing
    StorageHeader header;
    fhead.read((char*)&header, sizeof(header));
    fhead.close();

    if (strcmp(header.signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");

    table_count = header.table_count;
    value_pool_start_size = header.value_pool_start_size;
    value_pool_increment = header.value_pool_increment;
    value_pool_count = header.value_pool_count;

    size_t fsize = std::filesystem::file_size(path);
    file = file_mapping(path, read_write);
    region = mapped_region(file, read_write, offset, max_db_size);
    memory = managed_external_buffer(open_only, region.get_address(), fsize-offset);

    size_t address = (size_t)region.get_address();
    size_t count = value_pool_count+MAIN_POOL_COUNT;
    pools = new Pool[count];
    PPool* p = (PPool*)(address+header.pools);
    for(size_t i = 0; i < count; i++) {
      pools[i].open(this, p++, i+1);
    }

    NullData *internal((NullData*)(address+header.null));
    null = (void*)internal;
    version = &internal->version;
    root = &internal->root;
  }
  else {
    std::ofstream fhead(path, std::ios::out|std::ios::binary);
    fhead << std::string(OFFSET, (char)0);
    fhead.close();

    table_count = options.table_count;
    value_pool_start_size = std::max(options.value_pool_start_size, (size_t)100)+sizeof(ValueData);
    value_pool_start_size = ((value_pool_start_size+7)/8)*8;
    value_pool_increment = ((options.value_pool_increment+7)/8)*8;
    value_pool_count = std::max(
      (size_t)1, std::min(options.value_pool_count, (size_t)MAX_VALUE_POOL_COUNT));

    std::filesystem::resize_file(path, offset+grow_size);
    file = file_mapping(path, read_write);

    region = mapped_region(file, read_write, offset, max_db_size);
    memory = managed_external_buffer(create_only, region.get_address(), grow_size);

    size_t count = value_pool_count+MAIN_POOL_COUNT;
    pools = new Pool[count];
    PPool* p = (PPool*)memory.allocate(sizeof(PPool)*count);

#ifdef SMALL_PTR
    size_t pool_sizes[] = {16, 34, 64, MAX_POOL_SIZE, 0};
#else
    size_t pool_sizes[] = {20, 44, 84, MAX_POOL_SIZE, 0};
#endif
    size_t i = 0;
    pool_sizes[MAIN_POOL_COUNT-1] = TableData::calc_size(table_count);
    for(; i < MAIN_POOL_COUNT; i++) {
      pools[i].create(this, p++, i+1, pool_sizes[i], options.area_count);
    }

    size_t node_size = value_pool_start_size;
    for(; i < count; i++) {
      pools[i].create(this, p++, i+1, node_size, options.area_count);
      node_size += value_pool_increment;
    }

    NullData* internal = (NullData*)allocate(sizeof(NullData)).as_char;
    null = (void*)internal;
    internal->type = kNull;
    version = &internal->version;
    *version = 0;
    root = &internal->root;
    *root = internal;

    flush_header();
  }
}

Storage::~Storage() {
  flush_header();
  flush();
  delete pools;
}


void Storage::flush_header() {
  size_t address = (size_t)region.get_address();
  StorageHeader header;
  std::ofstream fhead(
    file.get_name(), std::ios::in|std::ios::out|std::ios::binary);
  strcpy(header.signature, SIGNATURE);
  header.file_version = 0;
  header.table_count = table_count;
  header.value_pool_start_size = value_pool_start_size;
  header.value_pool_increment = value_pool_increment;
  header.value_pool_count = value_pool_count;
  header.pools = (uint32_t)((size_t)pools[0].pool-address);
  header.null = (uint32_t)((size_t)null-address);
  fhead.write((char*)&header, sizeof(header));
  fhead.close();
}

void Storage::free(any_ptr ptr) {
  if (ptr.node->pool)
    pools[ptr.node->pool-1].free(ptr);
  else
    memory.deallocate(ptr.as_char);
};

any_ptr Storage::allocate(size_t size) {
  if (size <= MAX_POOL_SIZE)
    return pools[pool_index(size)].allocate();

  if (size <= value_pool_start_size)
    return pools[MAIN_POOL_COUNT].allocate();

  size_t index = ((size - value_pool_start_size) / value_pool_increment) + 1;
  if (index < value_pool_count)
    return pools[MAIN_POOL_COUNT+index].allocate();

  return mem_allocate(size);
}

any_ptr Storage::mem_allocate(size_t size) {
  any_ptr result = memory.allocate(size, std::nothrow);
  if (!result.as_int) {
    size_t fsize = std::filesystem::file_size(file.get_name());
    size_t grow = ((size+127+grow_size)/grow_size)*grow_size;

    std::filesystem::resize_file(file.get_name(), fsize + grow);
    memory.grow(grow);
    result = memory.allocate(size, std::nothrow);
    if (!result.as_int)
      throw std::bad_alloc();  // should never come here
  }
  result.node->pool = 0;
  return result;
}


} // namespace leaves
