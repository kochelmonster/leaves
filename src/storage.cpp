#include <fstream>
#include <filesystem>
#include "storage.hpp"
#include <iostream>


#define NODE_INCREMENT  24
#define PCOUNT(pools)  (sizeof(pools)/sizeof(Pool))
#define SIGNATURE "LarchLeaves"


using namespace boost::interprocess;

namespace larch_leaves {

struct StorageHeader {
  char signature[sizeof(SIGNATURE)];
  uint16_t file_version;
  uint16_t segment_count;
  uint32_t pools; // start pointer of the 5 main pools
  uint32_t version; // pointer to database version
  segment_ptr start;
};


void Pool::create(
    Storage* storage, PPool* pool, size_t node_size, size_t area_size) {
  this->storage = storage;
  this->pool = pool;
  pool->node_size = node_size;
  pool->area_size = area_size;
  pool->current_area = pool->next_node = storage->allocate(area_size);
  pool->next_free = segment_ptr();
}

segment_ptr Pool::allocate() {
  if (pool->next_free) {
    segment_ptr result(pool->next_free);
    pool->next_free = ((Free*)result.resolve(storage))->next;
    return result;
  }
  if ((size_t)(pool->next_node - pool->current_area) > pool->area_size) {
    pool->current_area = storage->allocate(pool->area_size);
    pool->next_node = pool->current_area + (uint32_t)pool->node_size;
    return pool->current_area;
  }
  segment_ptr result(pool->next_node);
  pool->next_node += pool->node_size;

  return result;
}


Storage::Storage(const char* path, size_t segment_size) :
    segment_size(segment_size) {
  StorageHeader header;
  size_t offset = sizeof(StorageHeader);

  std::ifstream fhead(path, std::ios::in|std::ios::binary);
  if (fhead.is_open()) {
    // load existing
    fhead.read((char*)&header, sizeof(header));
    fhead.close();

    if (strcmp(header.signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");

    file = file_mapping(path, read_write);
    start = header.start;

    for(size_t i = 0; i < header.segment_count; i++, offset += segment_size) {
      segments.push_back(Segment(open_only, file, offset, segment_size));
    }

    size_t address = (size_t)segments[0].region.get_address();
    PPool* p = (PPool*)(address+header.pools);
    for(size_t i = 0; i < PCOUNT(pools); i++) {
      pools[i].open(this, p++);
    }

    version = (uint64_t*)(address+header.version);
  }
  else {
    StorageHeader header;
    std::ofstream fhead(path, std::ios::out|std::ios::binary);
    fhead.write((char*)&header, sizeof(header));
    fhead.close();

    std::filesystem::resize_file(path, offset+segment_size);
    file = file_mapping(path, read_write);

    // create a new one
    segments.push_back(Segment(create_only, file, offset, segment_size));
    PPool* p = (PPool*)segments[0].memory.allocate(
      sizeof(PPool)*PCOUNT(pools));
    for(size_t i = 0; i < PCOUNT(pools); i++) {
      size_t node_size = 8 + i*NODE_INCREMENT;
      pools[i].create(this, p++, node_size, node_size*AREA_COUNT);
    }

    version = (uint64_t*)pools[0].allocate().resolve(this);
    *version = 0;
    flush_header();
  }
}

Storage::~Storage() {
  flush_header();
  flush();
}


void Storage::flush_header() {
  size_t address = (size_t)segments[0].region.get_address();
  StorageHeader header;
  std::ofstream fhead(
    file.get_name(), std::ios::in|std::ios::out|std::ios::binary);
  strcpy(header.signature, SIGNATURE);
  header.file_version = 0;
  header.start = start;
  header.segment_count = segments.size();
  header.pools = (uint32_t)(((size_t)pools[0].pool)-address);
  header.version = (uint32_t)(((size_t)version)-address);
  fhead.write((char*)&header, sizeof(header));
  fhead.close();
}

void Storage::flush() {
  for(segment_v::iterator i = segments.begin(); i != segments.end(); i++)
    i->region.flush();
}


segment_ptr Storage::allocate(size_t size) {
  Segment& back(segments.back());
  size_t address = (size_t)back.memory.allocate(size, std::nothrow);

  if (address) {
    return segment_ptr(
      segments.size()-1, address-(size_t)back.region.get_address());
  }

  std::filesystem::resize_file(
    file.get_name(), sizeof(StorageHeader)+segment_size*(segments.size()+1));
  segments.push_back(Segment(create_only, file,
    sizeof(StorageHeader) + segments.size()*segment_size, segment_size));
  return allocate(size);
}
}
