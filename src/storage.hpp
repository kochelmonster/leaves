// declarations for the node storage
#ifndef _LEAVES_STORAGE_HPP
#define _LEAVES_STORAGE_HPP

#include <algorithm>
#include <bit>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/pool/object_pool.hpp>
#include <fstream>

#include "page.hpp"

namespace leaves {

using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;
using boost::interprocess::shared_memory_object;

#define TRANSACTION_COUNT 10

const int STORAGE_POOL_COUNT = 29;

const uint64_t BLOCK_SIZE1 = 1 << 16;
const uint64_t BLOCK_SIZE2 = 1 << 20;
const uint64_t BLOCK_SIZE3 = 1 << 28;
const uint64_t BLOCK_SIZE4 = 1 << 30;
const uint64_t BLOCK_SIZE5 = (uint64_t)1 << 34;

const static uint64_t block_size_per_pool[STORAGE_POOL_COUNT] = {
    BLOCK_SIZE1,  // 0 -> 16
    BLOCK_SIZE1,  // 1 -> 32
    BLOCK_SIZE1,  // 2 -> 64
    BLOCK_SIZE1,  // 3 -> 128
    BLOCK_SIZE1,  // 4 -> 256
    BLOCK_SIZE1,  // 5 -> 512
    BLOCK_SIZE2,  // 6 -> 1024
    BLOCK_SIZE2,  // 7 -> 2048
    BLOCK_SIZE2,  // 8 -> 4096
    BLOCK_SIZE2,  // 9 -> 8192
    BLOCK_SIZE2,  // 10 -> 16K
    BLOCK_SIZE2,  // 11 -> 32K
    BLOCK_SIZE2,  // 12 -> 64K
    BLOCK_SIZE3,  // 13 -> 128K
    BLOCK_SIZE3,  // 14 -> 256K
    BLOCK_SIZE3,  // 15 -> 512K
    BLOCK_SIZE3,  // 16 -> 1M
    BLOCK_SIZE3,  // 17 -> 2M
    BLOCK_SIZE3,  // 18 -> 4M
    BLOCK_SIZE4,  // 19 -> 8M
    BLOCK_SIZE4,  // 20 -> 16M
    BLOCK_SIZE4,  // 21 -> 32M
    BLOCK_SIZE4,  // 22 -> 64M
    BLOCK_SIZE4,  // 23 -> 128M
    BLOCK_SIZE5,  // 24 -> 256M
    BLOCK_SIZE5,  // 25 -> 512M
    BLOCK_SIZE5,  // 26 -> 1G
    BLOCK_SIZE5,  // 27 -> 2G
    BLOCK_SIZE5,  // 28 -> 4G
};

typedef uint64_t offset_t;

#pragma pack(1)

struct MarkedBlocks {
  static const int REF_COUNT =
      (PAGE_SIZE - 2 * sizeof(offset_t)) / sizeof(offset_t);

  union {
    struct {
      uint16_t count;
      uint8_t pool_id;
    };
    offset_t _padding;
  };

  union {
    offset_t next;
    MarkedBlocks* next_mem;
  };

  offset_t refs[REF_COUNT];
};

struct Transaction {
  uint64_t id;

  // the address of the root page
  stored_ptr root;

  /* a reference to the head of a list of MarkedBlocks Pages containing all
   storage blocks copied in this transaction */
  offset_t copied_head;

  /* a reference to the head of a list of MarkedBlocks Page containing all
  storage blocks created in this transaction */
  offset_t txn_head;
};

union PageMemory {
  MarkedBlocks marker;
  Page page;
};

struct StoragePool {
  // current slot for next free memory position
  offset_t scurrent;

  // the end of the currently allocated block
  offset_t slast;

  // offset to a MarkedBlocks Page with free slots
  offset_t sfree;

  // the same as above but a pointer to a Page in the heap
  MarkedBlocks* mfree;
};

struct Header {
  union {
    struct {
      char signature[sizeof(SIGNATURE)];
      uint16_t db_version;
      uint16_t txn_version;
      offset_t freed_head;
      offset_t free_block;
      uint64_t transaction_id;
      Transaction txn[TRANSACTION_COUNT];
      StoragePool pools[STORAGE_POOL_COUNT];
    };
    char _padding[PAGE_SIZE];
  };
  Header() {}
};

struct SharedMem {
  size_t size;
  uint32_t txn_ref_count[TRANSACTION_COUNT];
  uint32_t transaction;
};

#pragma pack(0)

struct MemoryView : public MemoryViewBase {
  MemoryView(const char* path);

  Page* get_page(offset_t offset) const { return (Page*)(start + offset); }
  MarkedBlocks* get_blocks(offset_t offset) const {
    return (MarkedBlocks*)(start + offset);
  }

  const Slice get_value(stored_ptr vid) const {
    char* ptr = start + vid.offset;
    if (vid.size > 20) {
      return Slice(ptr, vid.size - 20);
    }
    if (vid.size < 12) return Slice(ptr + sizeof(uint16_t), *(uint16_t*)ptr);

    return Slice(ptr + sizeof(uint32_t), *(uint32_t*)ptr);
  }

  bool is_stored(void* pointer) const {
    return start <= pointer && pointer <= start + region.get_size();
  }

  size_t get_size() const { return region.get_size(); }

  const char* get_filename() const { return file.get_name(); }

  const Header* get_header() const { return (const Header*)start; }

  file_mapping file;
  mapped_region region;
};

typedef std::shared_ptr<MemoryView> MemoryView_ptr;
typedef boost::object_pool<PageMemory> page_pool_t;

struct Storage {
  Storage(const char* path);
  ~Storage();

  void init_shared();

  int get_page_id(const Page* page) const {
    return ((char *)page - (char *)view->start) / PAGE_SIZE;
  }

  /* returns a heap copy of a storage node */
  Page* get_writable_page(stored_ptr ploc);

  Page* alloc_new_page() { return page_pool.malloc()->page.init(); }
  void free(Page* page) { page_pool.free((PageMemory*)page); }
  void free(MarkedBlocks* page);
  
  void merge_marked_to_free(offset_t head);
  void free_marked(MarkedBlocks* marker_storage[]);
  void free_marked(offset_t head);
  offset_t write_marked(MarkedBlocks* marker_storage[]);
  offset_t write_marked(MarkedBlocks* blocks, offset_t next);
  void write_pools();

  void add_to_marked(stored_ptr value, int pool_id,
                     MarkedBlocks* marker_storage[]);
  void add_page_to_copied(stored_ptr page) {
    add_to_marked(page, page.PAGE_POOL, copied_heads);
  }
  void add_value_to_copied(stored_ptr value) {
    add_to_marked(value, value.pool_id(), copied_heads);
  }

  stored_ptr new_value(const Slice& value);

  offset_t alloc_storage_block(int pool_id);
  MarkedBlocks* get_free_blocks(int pool_id);
  MarkedBlocks* new_free_blocks(int pool_id);
  void free(offset_t ptr, int pool_id);
  void merge_to_free(MarkedBlocks* src, int pool_id);

  const Header* get_header() const { return view->get_header(); }

  uint64_t transaction_id() const { return get_header()->transaction_id; }

  const Transaction& current() const {
    return get_header()->txn[transaction_id() % TRANSACTION_COUNT];
  }

  stored_ptr root() const { return current().root; }

  stored_ptr write_page(Page* copy);

  void check_size();
  bool start_transaction();
  void rollback();
  void prepare_commit(stored_ptr root);
  void commit();

  // the storage header
  Header header;
  std::ofstream output;
  MemoryView_ptr view;
  page_pool_t page_pool;
  shared_memory_object shared_object;
  mapped_region shared_region;
  SharedMem* shared;

  // Attributes for the current transaction
  uint64_t active_transaction;
  MarkedBlocks* copied_heads[STORAGE_POOL_COUNT];
  MarkedBlocks* txn_heads[STORAGE_POOL_COUNT];
};

}  // namespace leaves

#endif  // _LEAVES_STORAGE_HPP
