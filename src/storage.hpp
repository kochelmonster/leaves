// declarations for the node storage
#ifndef _LARCH_LEAVES_STORAGE_HPP
#define _LARCH_LEAVES_STORAGE_HPP

#include <memory>
#include <vector>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>


#ifndef AREA_COUNT
#define AREA_COUNT 100
#endif


#define NODE_INCREMENT  24
#define POOL_COUNT  5

namespace larch_leaves {


typedef uint16_t segment_index_t;

struct Storage;


#pragma pack(2)
struct segment_ptr {
  // The persistent part of a segment pointer

  union {
    uint32_t delta;
    uint32_t upper:30;
    uint32_t type:2;
  };
  segment_index_t segment_id;

  segment_ptr(segment_index_t segment_id=0, uint32_t delta=1)
    : delta(delta), segment_id(segment_id) {}

  segment_ptr operator=(segment_ptr* other) {
    (*this) = *other;
    return *this;
  }

  segment_ptr operator+=(uint32_t diff) {
    delta += diff;
    return *this;
  }

  segment_ptr operator+(uint32_t diff) {
    return segment_ptr(segment_id, (delta + diff) & 0xFFFFFFFC);
  }

  segment_ptr operator-(uint32_t diff) {
    return segment_ptr(segment_id, (delta - diff) & 0xFFFFFFFC);
  }

  int64_t operator-(segment_ptr& other) {
    assert(segment_id == other.segment_id);
    return (int64_t)(delta & 0xFFFFFFFC) - (int64_t)(other.delta & 0xFFFFFFFC);
  }

  operator bool() const { return delta != 1; }
  bool operator!() const { return delta == 1; }

  void* resolve(const Storage* storage) const;
};
#pragma pack(0)


struct PPool {
  // The persistent part of Pool
  size_t node_size;
  size_t area_size;
  segment_ptr current_area;
  segment_ptr next_node;
  segment_ptr next_free;
};

struct Pool {
  // the interface part of Pool

  struct Free {
    segment_ptr next;
  };

  Storage* storage;
  PPool* pool;

  void open(Storage* storage, PPool* pool) {
    this->storage = storage;
    this->pool = pool;
  }
  void create(Storage* storage, PPool* pool, size_t node_size, size_t area_size);

  segment_ptr allocate();

  void free(const segment_ptr& ptr) {
    ((Free*)ptr.resolve(storage))->next = pool->next_free;
    pool->next_free = ptr;
  }
};


using boost::interprocess::file_mapping;
using boost::interprocess::managed_external_buffer;
using boost::interprocess::mapped_region;
using boost::interprocess::create_only_t;
using boost::interprocess::create_only;
using boost::interprocess::open_only_t;
using boost::interprocess::open_only;
using boost::interprocess::read_write;


struct Segment {
  mapped_region region;
  managed_external_buffer memory;

  Segment(create_only_t, file_mapping& file, size_t offset, size_t size)
    : region(file, read_write, offset, size),
      memory(create_only, region.get_address(), size)
  { }

  Segment(open_only_t, file_mapping& file, size_t offset, size_t size)
    : region(file, read_write, offset, size),
      memory(open_only, region.get_address(), size)
  { }
};


struct Storage {
  typedef std::vector<Segment> segment_v;

  Storage(const char* path, size_t segment_size);
  ~Storage();

  segment_ptr allocate(size_t size);
  void free(segment_ptr ptr);
  void flush();
  void flush_header();

  size_t get_segment_address(segment_index_t index) const {
    return (size_t)segments[index].region.get_address();
  }

  file_mapping file;
  size_t segment_size;
  Pool pools[POOL_COUNT];
  uint64_t* version;
  segment_ptr start;
  segment_v segments;
};


inline void* segment_ptr::resolve(const Storage* storage) const {
  return (void*)(storage->get_segment_address(segment_id) + (delta & 0xFFFFFFFC));
}

}

#endif // _LARCH_LEAVES_STORAGE_HPP
