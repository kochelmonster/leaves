// declarations for the node storage
#ifndef _LARCH_LEAVES_STORAGE_HPP
#define _LARCH_LEAVES_STORAGE_HPP

#include <memory>
#include <vector>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>

#include <leaves.hpp>

#ifdef SMALL_PTR
#define MAX_POOL_SIZE 100
#else
#define MAX_POOL_SIZE 132
#endif

namespace leaves {

struct Storage;
struct offset_ptr;

struct Node {
  struct  {
    uint16_t type:3;
    uint16_t pool:13;
  };
};

struct CompressedData;
struct ValueData;
struct TrieData;

union any_ptr {
  int64_t as_int;
  char* as_char;
  offset_ptr* next;
  Node* node;
  CompressedData* compressed;
  ValueData *value;
  TrieData *trie;

  any_ptr(void* ptr=NULL) : as_char((char*)ptr) {}
  any_ptr(const offset_ptr& ptr);

  const any_ptr& type(uint8_t type) { node->type = type; return *this; }
};

#pragma pack(2)
#ifdef SMALL_PTR
struct offset_ptr {
  /* All Objects are align on a 8 byte boundary
     -> we keep the same address space by multiplying the delta with 8
        and use the spared 3 bit for specifying the node type.
  */
  struct bit48int {
    uint32_t v1;
    uint16_t v2;
  };

  struct offset_converter {
    union {
      bit48int d48;
      struct {
        int64_t delta:48;
        int64_t _:16;
      };
    };
    offset_converter(int64_t delta) : delta(delta) {}
    offset_converter(bit48int d48) : d48(d48) {}
  };

  bit48int delta;

  offset_ptr() : delta(offset_converter(0).d48) {}

  offset_ptr(any_ptr p) :
    delta(offset_converter(p.as_int - (int64_t)this).d48) { }

  void to_null() {
    delta = offset_converter(0).d48;
  }

  const offset_ptr& operator=(const offset_ptr& other) {
    int64_t odelta = offset_converter(other.delta).delta;
    if (!odelta)
      delta = other.delta;
    else
      *this = ((char*)&other) + odelta;
    return *this;
  }

  const offset_ptr& operator=(any_ptr p) {
    delta = offset_converter(p.as_int - (int64_t)this).d48;
    return *this;
  }

  const offset_ptr& operator+=(int64_t diff) {
    offset_converter c(delta);
    c.delta += diff;
    delta = c.d48;
    return *this;
  }

  any_ptr operator+(int64_t diff) {
    return resolve().as_char + diff;
  }

  int64_t operator-(offset_ptr& other) {
    return resolve().as_int - other.resolve().as_int;
  }

  operator bool() const { return offset_converter(delta).delta != 0; }
  bool operator!() const { return offset_converter(delta).delta == 0; }

  any_ptr resolve() const {
    assert(offset_converter(delta).delta != 0);
    return (any_ptr)(((char*)this) + offset_converter(delta).delta);
  }
};
#else

struct offset_ptr {
  /* All Objects are align on a 8 byte boundary
     -> we keep the same address space by multiplying the delta with 8
        and use the spared 3 bit for specifying the node type.
  */
  int64_t delta;

  offset_ptr() : delta(0) {}

  offset_ptr(any_ptr p) : delta(p.as_int - (int64_t)this) { }

  void to_null() {
    delta = 0;
  }

  const offset_ptr& operator=(const offset_ptr& other) {
    if (!other.delta)
      delta = 0;
    else
      *this = ((char*)&other) + other.delta;
    return *this;
  }

  const offset_ptr& operator=(any_ptr p) {
    delta = p.as_int - (int64_t)this;
    return *this;
  }

  const offset_ptr& operator+=(int64_t diff) {
    delta += diff;
    return *this;
  }

  any_ptr operator+(int64_t diff) {
    return resolve().as_char + diff;
  }

  int64_t operator-(offset_ptr& other) {
    return resolve().as_int - other.resolve().as_int;
  }

  operator bool() const { return delta != 0; }
  bool operator!() const { return delta == 0; }

  any_ptr resolve() const {
    assert(delta != 0);
    return (any_ptr)(((char*)this) + delta);
  }
};

#endif  // SMALL_PTR
#pragma pack(0)


struct PPool {
  // The persistent part of Pool
  size_t node_size;
  size_t area_size;
  size_t used_nodes;
  size_t freed_nodes;
  offset_ptr current_area;
  offset_ptr next_node;
  offset_ptr next_free;
};


struct Pool {
  // the interface part of Pool

  Storage* storage;
  PPool* pool;
  uint16_t index;

  void open(Storage* storage_, PPool* pool_, uint16_t index_) {
    storage = storage_;
    pool = pool_;
    index = index_;
  }
  void create(Storage* storage, PPool* pool, uint16_t index_, size_t node_size, size_t area_count);

  any_ptr allocate();
  void free(any_ptr ptr);
};


using boost::interprocess::file_mapping;
using boost::interprocess::managed_external_buffer;
using boost::interprocess::mapped_region;
using boost::interprocess::create_only_t;
using boost::interprocess::create_only;
using boost::interprocess::open_only_t;
using boost::interprocess::open_only;
using boost::interprocess::read_write;


struct Storage {
  Storage(const char* path, const Options& options);
  ~Storage();

  any_ptr allocate(size_t size);
  void free(any_ptr ptr);

  any_ptr mem_allocate(size_t size);

  void flush(bool async=true) { region.flush(0, 0, async); }
  void flush_header();

  file_mapping file;
  size_t grow_size;
  size_t value_pool_start_size;
  size_t value_pool_increment;
  size_t value_pool_count;
  void *null;
  uint64_t* version;
  offset_ptr* start;
  mapped_region region;
  managed_external_buffer memory;
  Pool *pools;
};


inline any_ptr::any_ptr(const offset_ptr& ptr) : node(ptr.resolve().node) { }

}  // namespace leaves

#endif // _LARCH_LEAVES_STORAGE_HPP
