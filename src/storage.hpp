// declarations for the node storage
#ifndef _LARCH_LEAVES_STORAGE_HPP
#define _LARCH_LEAVES_STORAGE_HPP

#include <memory>
#include <vector>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>

#include <leaves.hpp>


#define POOL_COUNT  4
#define MAX_POOL_SIZE (96+8)

namespace leaves {


typedef uint16_t segment_index_t;

struct Storage;
struct ValueData;
struct TrieData;
struct CompressedData;
struct resolved_ptr;


#pragma pack(2)
struct segment_ptr {
  /* The persistent part of a segment pointer
     All Objects are align on a 8 byte boundary
     -> we keep the same address space by multiplying the delta with 8
        and use the spared 3 bit for specifying the node type.
  */

  uint32_t delta:29;
  uint32_t type:3;
  segment_index_t segment_id;

  segment_ptr(segment_index_t segment_id=0, uint32_t delta=0, uint32_t type=1)
    : delta(delta>>3), type(type), segment_id(segment_id) {}

  segment_ptr operator=(const resolved_ptr& other);

  segment_ptr operator=(segment_ptr* other) {
    return *this = *other;
  }

  segment_ptr operator+=(uint32_t diff) {
    delta += diff >> 3;
    return *this;
  }

  segment_ptr operator+(uint32_t diff) {
    return segment_ptr(segment_id, (delta << 3) + diff);
  }

  segment_ptr operator-(uint32_t diff) {
    return segment_ptr(segment_id, (delta << 3) - diff);
  }

  int64_t operator-(segment_ptr& other) {
    assert(segment_id == other.segment_id);
    return (delta - other.delta) << 3;
  }

  operator bool() const { return delta != 0; }
  bool operator!() const { return delta == 0; }

  resolved_ptr resolve(const Storage* storage) const;
};
#pragma pack(0)

enum NodeTypes {
  kValue = 0,
  kNull,
  kCompressed,
  kTrie,
};


struct resolved_ptr {
  // A resolved segment_ptr
  union {
    ValueData *value;
    TrieData *trie;
    CompressedData *compressed;
    segment_ptr *next;
    char* resolved;
    void* pointer;
  };
  segment_ptr me;

  resolved_ptr(): resolved(NULL) {}

  resolved_ptr(segment_ptr ptr, void* resolved)
    : resolved((char*)resolved), me(ptr) { }

  resolved_ptr(segment_index_t segment_id, void* address, void *base)
    : pointer(address), me(segment_id, (size_t)address-(size_t)base) {}

  NodeTypes type() const {
    return (NodeTypes)me.type;
  }

  const resolved_ptr& type(NodeTypes type) {
    me.type = type;
    return *this;
  }

  resolved_ptr resolve(segment_ptr ptr, const Storage* storage) const {
    if (ptr.segment_id == me.segment_id) {
      // we don't need storage
      return resolved_ptr(ptr, (void*)(resolved - me.delta + ptr.delta));
    }
    return resolved_ptr(ptr, raw_resolve(ptr, storage));
  }

  void* raw_resolve(segment_ptr ptr, const Storage* storage) const;
};



struct PPool {
  // The persistent part of Pool
  size_t node_size;
  size_t area_size;
  size_t used_nodes;
  size_t freed_nodes;
  segment_ptr current_area;
  segment_ptr next_node;
  segment_ptr next_free;
};

struct Pool {
  // the interface part of Pool

  Storage* storage;
  PPool* pool;

  void open(Storage* storage, PPool* pool) {
    this->storage = storage;
    this->pool = pool;
  }
  void create(Storage* storage, PPool* pool, size_t node_size, size_t area_count);

  resolved_ptr allocate();
  void free(const resolved_ptr& ptr);
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

  Storage(const char* path, const Options& options);
  ~Storage();

  resolved_ptr allocate(size_t size);
  void free(const resolved_ptr& ptr, size_t size);

  resolved_ptr mem_allocate(size_t size);
  void mem_free(const resolved_ptr& ptr);

  void flush();
  void flush_header();

  size_t get_segment_address(segment_index_t index) const {
    return (size_t)segments[index].region.get_address();
  }

  file_mapping file;
  size_t segment_size;
  size_t value_pool_start_size;
  size_t value_pool_increment;
  size_t value_pool_count;
  uint64_t* version;
  segment_ptr* start;
  segment_v segments;
  Pool pools[POOL_COUNT];
  Pool *value_pools;
};


inline segment_ptr segment_ptr::operator=(const resolved_ptr& other) {
  return *this = other.me;
}

inline resolved_ptr segment_ptr::resolve(const Storage* storage) const {
  return resolved_ptr(*this, ((char*)storage->get_segment_address(segment_id) + (delta<<3)));
}

inline void* resolved_ptr::raw_resolve(segment_ptr ptr, const Storage* storage) const {
  return (char*)storage->get_segment_address(ptr.segment_id) + (ptr.delta << 3);
}


}  // namespace leaves

#endif // _LARCH_LEAVES_STORAGE_HPP
