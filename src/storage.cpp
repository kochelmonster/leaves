#include <fstream>
#include <filesystem>
#include <algorithm>

#include "storage.hpp"
#include "node.hpp"
#include "table.hpp"

using namespace boost::interprocess;

namespace leaves {

size_t pool_sizes[] = {
//NodeSize  CountOfNodes in area (the best in a multiple of 4096)
  20,       1024,    // a trie with 2 nodes
  44,       93,      // a trie with 5 nodes
  84,       195,     // a trie with 10 nodes
  132,      31,      // a trie with 16 nodes
  256,      16,
  512,      8,
  1024,     4,
  1536,     8,
  2048,     8,
  2560,     5,
  3072,     6,
  5120,     8,
  6144,     6,
  7168,     7,
  8192,     4,
  0,        0,};


namespace fs = std::filesystem;

Storage::Storage(const char* path, const Options& options) {
    grow_size = options.grow_size;
  size_t max_db_size = std::min(options.max_db_size, MAX_DB_SIZE);
  size_t fsize = 0;

  burst_size = options.burst_size;
  max_key_size = (((burst_size*PAGE_SIZE)-sizeof(TableData))/sizeof(DataItem))/MIN_BURST_ITEMS;

  fs::file_status fstat(fs::status(path));
  if (fs::is_regular_file(fstat))
    fsize = fs::file_size(path);

  if (fsize >= sizeof(FirstPage)) {
    // load existing
    file = file_mapping(path, read_write);
    region = mapped_region(file, read_write, 0, max_db_size);
    header = (FirstPage*)region.get_address();

    if (strcmp(header->signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");

    return;
  }

  if (fsize)
    throw std::runtime_error("wrong filetype");

  std::ofstream fhead(path, std::ios::out|std::ios::binary);
  fhead << SIGNATURE;
  fhead.close();

  fs::resize_file(path, PAGE_SIZE+grow_size);
  file = file_mapping(path, read_write);
  region = mapped_region(file, read_write, 0, max_db_size);
  header = (FirstPage*)region.get_address();

  header->memory.init(grow_size, ((char*)header)+PAGE_SIZE);

  strcpy(header->signature, SIGNATURE);
  header->file_version = 0;
  header->version = 0;
  header->root = &header->null;
  header->null.type = kNull;

  for(size_t i = 0; pool_sizes[i]; i+=2) {
    header->pools[i/2].init(pool_sizes[i], pool_sizes[i+1]*pool_sizes[i]);
  }

  flush();
}

Storage::~Storage() {
  flush();
}


void Storage::free(any_ptr ptr) {
  if (ptr.node->inpool)
    header->pools[ptr.node->pool].free(ptr);
  else
    header->memory.free(ptr);
};

any_ptr Storage::allocate(size_t size) {
  int index = pool_index(size);
  if (index >= 0) {
    MemoryPool& pool(header->pools[index]);
    any_ptr result = pool.allocate();
    if (!result.as_int) {
      pool.new_area(mem_allocate(pool.area_size));
      result = pool.allocate();
    }

    result.node->inpool = true;
    result.node->pool = index;
    return result;
  }

  return mem_allocate(size);
}

any_ptr Storage::mem_allocate(size_t size) {
  any_ptr result = header->memory.allocate(size);
  if (!result.as_int) {
      size_t fsize = header->memory.db_size + PAGE_SIZE;
      size_t grow = ((size+127+grow_size)/grow_size)*grow_size;
      fs::resize_file(file.get_name(), fsize + grow);
      header->memory.grow(grow);
      result = header->memory.allocate(size);
      if (!result.as_int)
        throw std::bad_alloc();  // should never come here
  }
  return result;
}


void Storage::get_stats(Stats& stats) {
  stats.max_db_size = region.get_size();
  stats.burst_size = burst_size;
  stats.grow_size = grow_size;
  //stats.segment_count = storage.segments.size();
  for(size_t i = 0; i < 15; i++) {
    stats.pools[i].node_size = header->pools[i].node_size;
    stats.pools[i].used_nodes = header->pools[i].used_nodes;
    stats.pools[i].free_nodes = header->pools[i].free_nodes;
  }
  stats.free_pages = header->memory.free_count;
  memset(stats.tries_nodes, 0, sizeof(stats.tries_nodes));
  stats.end_nodes = 0;
  stats.intermediate_nodes = 0;
  stats.compressed_nodes = 0;
  Transition::handlers[header->root.resolve().node->type]->report(&header->root, stats);
}


size_t page_count(size_t size) {
  return (size+PAGE_SIZE/2) / PAGE_SIZE;
}

void PageManager::init(size_t db_size_, any_ptr start) {
  db_size = db_size_;
  free_count = 0;
  for(size_t i = 0; i < FREE_POOL_SIZE; i++)
    next_free[i] = offset_ptr();
  next_page = start;
}

any_ptr PageManager::allocate(size_t size) {
  any_ptr result;
  size = page_count(size);

  if (size <= FREE_POOL_SIZE && next_free[size-1]) {
    result = next_free[size-1].resolve();
    next_free[size-1] = *result.next;
    free_count--;
    result.header->inpool = false;
    result.header->pages = size;
    return result;
  }

  result = next_page;
  if (((result.as_int - (size_t)this) + (size_t)(PAGE_SIZE*size)) > db_size)
    return 0;

  next_page += PAGE_SIZE*size;
  result.header->inpool = false;
  result.header->pages = size;
  return result;
}

void PageManager::free(any_ptr ptr) {
  size_t size = ptr.header->pages;
  while(size > 0) {
    free_count++;
    if (size > FREE_POOL_SIZE) {
      *(ptr.next) = next_free[FREE_POOL_SIZE-1];
      next_free[FREE_POOL_SIZE-1] = ptr;
      ptr.as_int += FREE_POOL_SIZE*PAGE_SIZE;
      size -= FREE_POOL_SIZE;
    }
    else {
      *(ptr.next) = next_free[size-1];
      next_free[size-1] = ptr;
      ptr.as_int += size*PAGE_SIZE;
      size = 0;
    }
  }
}

void MemoryPool::init(size_t node_size_, size_t area_size_) {
  used_nodes = 0;
  free_nodes = 0;
  node_size = node_size_;
  area_size = area_size_;
  current_area = this;
  next_free.to_null();
  next_node = (char*)this + area_size;
}

any_ptr MemoryPool::allocate() {
  any_ptr result;

  if (next_free) {
    result = next_free.resolve();
    next_free = *result.next;
    free_nodes--;
  }
  else if ((size_t)(next_node - current_area) >= area_size) {
    return NULL;
  }
  else {
    result = next_node.resolve();
    next_node += node_size;
  }

  used_nodes++;
  return result;
}

void MemoryPool::free(any_ptr ptr) {
  used_nodes--;
  free_nodes++;
  *(ptr.next) = next_free;
  next_free = ptr;
}

void MemoryPool::new_area(any_ptr ptr) {
  current_area = next_node = ptr;
}


} // namespace leaves
