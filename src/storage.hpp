// declarations for the node storage
#ifndef _LEAVES_STORAGE_HPP
#define _LEAVES_STORAGE_HPP

#include <fstream>
#include <memory>
#include <vector>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <map>
#include "page.hpp"


namespace leaves {

using boost::interprocess::file_mapping;
using boost::interprocess::mapped_region;

struct StorageHeader;

#define POOL_COUNT 100


typedef std::map<uint64_t, WritablePage*> pagemap_t;


struct PagePool {
  WritablePage* free_list;
  WritablePage* free_block;
  WritablePage* pool;
  
  PagePool(): free_list(NULL) , free_block(NULL){
    free_block = pool = new WritablePage[POOL_COUNT];
  }
  ~PagePool() {
    delete[] pool;
  }
  WritablePage* alloc() {
    if (free_list) {
      WritablePage *result = free_list;
      free_list = (WritablePage*)free_list->next_mem;
      return result;
    }

    if (free_block < pool + POOL_COUNT) {
      return free_block++;
    }
    return NULL;
  }

  void free(WritablePage * page) {
    page->next_mem = free_list;
    free_list = page;
  }
};

struct Storage {
  Storage(const char* path, size_t size=START_SIZE, size_t delta=INCREMENT_SIZE);
  ~Storage();

  location_p alloc(size_t size=PAGE_SIZE);
  void free(location_p id, size_t size);
  void increase(size_t size);
  void flush();

  Node* node(location_p pos) {
    return (start + pos.page)->node(pos.node);
  }

  Page* page(uint64_t pos, bool writable=false) {
      return writable ? get_writable(pos): start + pos;
    }

  Page* page(location_p pos, bool writable=false) {
      return page(pos.page, writable);
    }

  location_p locate(const Page* page) const {
      return location_p::b(page - start);
    }

  bool readonly(const Page* page) const {
      return start < page && page < start + block_end;
    }

  size_t offset(uint64_t pos) const {
      return pos * PAGE_SIZE;
    }

  size_t offset(location_p pos) const {
      return pos.page * PAGE_SIZE + pos.offset;
    }

  size_t offset(const Page* page) const {
      return offset(locate(page));
    }

  uint64_t transaction_id() const {
      return start->header.transaction_id;
    }

  void transaction_inc();

  Page *get_newest(uint64_t pos);

  WritablePage* get_writable(uint64_t pos);

  WritablePage* get_writable(location_p pos) {
    return get_writable(pos.page);
  }

  void write_value(location_p pos, const Slice& value);
  bool rearrange_pages();
    
  file_mapping file;
  mapped_region *region;
  std::ofstream output;
  size_t delta;
  size_t block_end;
  PagePool tmpmem;
  pagemap_t pages_to_write;
  Page *start;
};

} // namespace leaves

#endif // _LEAVES_STORAGE_HPP

