// declarations for the node storage
#ifndef _LARCH_LEAVES_STORAGE_HPP
#define _LARCH_LEAVES_STORAGE_HPP

#include <memory>
#include <vector>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <leaves.hpp>


#define PAGE_SIZE  4096
#define MAX_REGION_SIZE (PAGE_SIZE*((1<<13)-1))
#define MAX_VALUE (MAX_PAGE_SIZE - sizeof(MemoryObject))


namespace leaves {

struct Storage;
struct offset_ptr;


struct MemoryObject {
  struct  {
    uint16_t inpool:1;
    uint16_t type:3;
    #define pages  pool
    uint16_t pool:12;
  };
};

typedef MemoryObject Node;
struct CompressedData;
struct ValueData;
struct TrieData;
struct TableData;

union any_ptr {
  int64_t as_int;
  char* as_char;
  offset_ptr* next;
  MemoryObject* header;
  Node* node;
  CompressedData* compressed;
  ValueData *value;
  TrieData *trie;
  TableData *table;

  any_ptr(void* ptr=NULL) : as_char((char*)ptr) {}
  any_ptr(const offset_ptr& ptr);
};

#pragma pack(2)
struct offset_ptr {
  /* All Objects are align on a 8 byte boundary
     -> we keep the same address space by multiplying the delta with 8
        and use the spared 3 bit for specifying the node type.
  */
  int64_t delta;

  offset_ptr() : delta(0) {}
  offset_ptr(const offset_ptr& other) { *this = other; }
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

#pragma pack(0)


struct MemoryPool {
  size_t node_size;
  size_t area_size;
  size_t used_nodes;
  size_t free_nodes;
  offset_ptr current_area;
  offset_ptr next_free;
  offset_ptr next_node;

  void init(size_t node_size, size_t area_size);
  any_ptr allocate();
  void free(any_ptr ptr);
  void new_area(any_ptr ptr);
};

struct PageManager {
  size_t free_count;
  size_t db_size;
  offset_ptr next_free;
  offset_ptr next_page;

  void init(size_t db_size, any_ptr start);
  void grow(size_t size) { db_size += size; }
  any_ptr allocate(size_t size);
  void free(any_ptr ptr);
};

#define SIGNATURE "LarchLeaves"


struct FirstPage {
  char signature[sizeof(SIGNATURE)];
  uint16_t file_version;
  uint64_t version; // transaction version
  offset_ptr root;
  Node null;
  PageManager memory;
  MemoryPool pools[1];
};


using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::read_write;


struct Storage {
  Storage(const char* path, const Options& options);
  ~Storage();

  any_ptr allocate(size_t size);
  void free(any_ptr ptr);

  any_ptr mem_allocate(size_t size);

  void flush(bool async=true) { region.flush(0, 0, async); }
  void flush_header();

  FirstPage *header;
  file_mapping file;
  mapped_region region;
  size_t grow_size;
  uint16_t burst_size;  // size for burst tables in pages
};


#define POOL(max, index)  if (size <= max) return index

inline int pool_index(size_t size) {
  POOL(20, 0);
  POOL(44, 1);
  POOL(84, 2);
  POOL(132, 3);
  if (size <= 2560) {
    POOL(256, 4);
    POOL(512, 5);
    POOL(1024, 6);
    POOL(1536, 7);
    POOL(2048, 8);
    POOL(2560, 9);
  }
  POOL(3072, 10);
  POOL(4096, -1);
  POOL(5129, 11);
  POOL(6144, 12);
  POOL(7168, 13);
  POOL(8192, 14);
  return -1;
}

inline any_ptr::any_ptr(const offset_ptr& ptr) : node(ptr.resolve().node) { }

}  // namespace leaves

#endif // _LARCH_LEAVES_STORAGE_HPP
