#include <fstream>
#include <filesystem>
#include <algorithm>

#include "storage.hpp"
#include "node.hpp"

#define SIGNATURE "LarchLeaves"

using namespace boost::interprocess;

namespace leaves {

struct Internal {
  uint64_t version;
  segment_ptr start;
};

struct StorageHeader {
  char signature[sizeof(SIGNATURE)];
  uint16_t file_version;
  size_t value_pool_start_size;
  size_t value_pool_increment;
  size_t value_pool_count;
  size_t segment_size;
  uint16_t segment_count;
  uint32_t pools; // start pointer of the 5 main pools
  uint32_t value_pools; // start pointer of the value pools
  uint32_t internal; // pointer to internal structure
};

#define HEAD_ALIGN rbtree_best_fit<null_mutex_family>::Alignment
#define OFFSET (((sizeof(StorageHeader)+(HEAD_ALIGN-1))/HEAD_ALIGN)*HEAD_ALIGN)


void Pool::create(Storage* storage, PPool* pool, size_t node_size, size_t area_count) {
  this->storage = storage;
  this->pool = pool;
#ifdef CHECK_MEM
  node_size += sizeof(size_t);
#endif
  pool->used_nodes = 0;
  pool->freed_nodes = 0;
  pool->node_size = node_size;
  pool->area_size = area_count*node_size;
  pool->current_area = pool->next_node = storage->allocate(pool->area_size);
  pool->next_free = segment_ptr();
}

segment_ptr Pool::allocate() {
  segment_ptr result;

  if (pool->next_free) {
    result = pool->next_free;
    pool->next_free = ((Free*)result.resolve(storage))->next;
    pool->freed_nodes--;
  }
  else if ((size_t)(pool->next_node - pool->current_area) >= pool->area_size) {
    pool->current_area = storage->allocate(pool->area_size);
    pool->next_node = pool->current_area + (uint32_t)pool->node_size;
    result = pool->current_area;
  }
  else {
    result = pool->next_node;
    pool->next_node += pool->node_size;
  }
  result.type = 0;
#ifdef CHECK_MEM
  char* mem = (char*)result.resolve(storage);
  *((size_t*)(mem+pool->node_size-sizeof(size_t))) = pool->node_size;
#endif
  pool->used_nodes++;
  return result;
}

void Pool::free(const segment_ptr& ptr) {
  #ifdef CHECK_MEM
    char* mem = (char*)ptr.resolve(storage);
    assert(*((size_t*)(mem+pool->node_size-sizeof(size_t))) == pool->node_size);
  #endif
  ((Free*)ptr.resolve(storage))->next = pool->next_free;
  pool->next_free = ptr;
  pool->used_nodes--;
  pool->freed_nodes++;
}


Storage::Storage(const char* path, const Options& options) {
  size_t offset = OFFSET;

  std::ifstream fhead(path, std::ios::in|std::ios::binary);
  if (fhead.is_open()) {
    // load existing
    StorageHeader header;
    fhead.read((char*)&header, sizeof(header));
    fhead.close();

    if (strcmp(header.signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");

    segment_size = header.segment_size;
    value_pool_start_size = header.value_pool_start_size;
    value_pool_increment = header.value_pool_increment;
    value_pool_count = header.value_pool_count;

    file = file_mapping(path, read_write);
    for(size_t i = 0; i < header.segment_count; i++, offset += segment_size) {
      segments.push_back(Segment(open_only, file, offset, segment_size));
    }

    size_t address = (size_t)segments[0].region.get_address();
    PPool* p = (PPool*)(address+header.pools);
    for(size_t i = 0; i < POOL_COUNT; i++) {
      pools[i].open(this, p++);
    }

    p = (PPool*)(address+header.value_pools);
    value_pools = new Pool[value_pool_count];
    for(size_t i = 0; i < value_pool_count; i++) {
      value_pools[i].open(this, p++);
    }

    Internal *internal((Internal*)(address+header.internal));
    version = &internal->version;
    start = &internal->start;
  }
  else {
    std::ofstream fhead(path, std::ios::out|std::ios::binary);
    fhead << std::string(OFFSET, (char)0);
    fhead.close();

    segment_size = options.segment_size;
    value_pool_start_size = std::max(options.value_pool_start_size, (size_t)100)+sizeof(ValueData);
    value_pool_increment = options.value_pool_increment;
    value_pool_count = std::min(options.value_pool_count, (size_t)MAX_VALUE_POOL_COUNT);

    std::filesystem::resize_file(path, offset+segment_size);
    file = file_mapping(path, read_write);

    // create a new one
    segments.push_back(Segment(create_only, file, offset, segment_size));
    PPool* p = (PPool*)segments[0].memory.allocate(sizeof(PPool)*POOL_COUNT);

    pools[0].create(this, p++, 16, options.area_count);
    size_t node_size = 4;
    for(size_t i = 1; i < POOL_COUNT; i++) {
      node_size += NODE_INCREMENT;
      pools[i].create(this, p++, node_size, options.area_count);
    }

    p = (PPool*)segments[0].memory.allocate(sizeof(PPool)*value_pool_count);
    value_pools = new Pool[value_pool_count];
    node_size = value_pool_start_size;
    for(size_t i = 0; i < value_pool_count; i++) {
      value_pools[i].create(this, p++, node_size, options.area_count);
      node_size += value_pool_increment;
    }

    Internal* internal = (Internal*)pools[0].allocate().resolve(this);

    version = &internal->version;
    *version = 0;

    start = &internal->start;
    *start = segment_ptr();

    flush_header();
  }
}

Storage::~Storage() {
  flush_header();
  flush();
  delete value_pools;
}


void Storage::flush_header() {
  size_t address = (size_t)segments[0].region.get_address();
  StorageHeader header;
  std::ofstream fhead(
    file.get_name(), std::ios::in|std::ios::out|std::ios::binary);
  strcpy(header.signature, SIGNATURE);
  header.file_version = 0;
  header.value_pool_start_size = value_pool_start_size;
  header.value_pool_increment = value_pool_increment;
  header.value_pool_count = value_pool_count;
  header.segment_size = segment_size;
  header.segment_count = segments.size();
  header.pools = (uint32_t)(((size_t)pools[0].pool)-address);
  header.value_pools = (uint32_t)(((size_t)value_pools[0].pool)-address);
  header.internal = (uint32_t)(((size_t)version)-address);
  fhead.write((char*)&header, sizeof(header));
  fhead.close();
}

void Storage::flush() {
  for(segment_v::iterator i = segments.begin(); i != segments.end(); i++)
    i->region.flush();
}

void Storage::free(segment_ptr ptr) {
  segments[ptr.segment_id].memory.deallocate(ptr.resolve(this));
};

segment_ptr Storage::allocate(size_t size) {
  size_t index = 0;
  for(segment_v::iterator i = segments.begin(); i != segments.end(); i++, index++) {
    size_t address = (size_t)i->memory.allocate(size, std::nothrow);
    if (address)
      return segment_ptr(index, address-(size_t)i->region.get_address());
  }

  size_t new_offset = OFFSET + segments.size()*segment_size;
  std::filesystem::resize_file(file.get_name(), new_offset + segment_size);
  segments.push_back(Segment(create_only, file, new_offset, segment_size));
  Segment& back(segments.back());
  size_t address = (size_t)back.memory.allocate(size, std::nothrow);
  if (!address)
    throw std::bad_alloc();
  return segment_ptr(segments.size()-1, address-(size_t)back.region.get_address());
}
} // namespace leaves
