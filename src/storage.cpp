#include <filesystem>
#include <iostream>
#include <cstddef>
#include "storage.hpp"
#include "page.hpp"


using boost::interprocess::create_only_t;
using boost::interprocess::create_only;
using boost::interprocess::open_only_t;
using boost::interprocess::open_only;
using boost::interprocess::read_only;


namespace leaves {

Storage::Storage(const char* path, size_t size, size_t delta) 
      : region(NULL), delta(PAGE_ROUND_UP(delta)) {
  size = PAGE_ROUND_UP(size);
  if (! std::filesystem::is_regular_file(path)) {
    StorageHeader ih;
    strcpy(ih.signature, SIGNATURE);
    ih.version = 0;
    ih.transaction_id = 0;
    ih.freed_head = 0;
    ih.free_block = 2;
    ih.root = location_p::b(1, 0, kNull);
    std::ofstream fhead(path, std::ios::out|std::ios::binary);
    fhead.write((char*)&ih, sizeof(ih));
    fhead.close();

    std::filesystem::resize_file(path, size);
  }

  size = std::filesystem::file_size(path);
  file = file_mapping(path, read_only);
  region = new mapped_region(file, read_only, 0, size);
  start = (Page*)region->get_address();
  block_end = size / PAGE_SIZE;

  if (strcmp(start->header.signature, SIGNATURE))
      throw std::runtime_error("wrong filetype");

  output.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
}  

Storage::~Storage() {
  delete region;
  output.close();
}

void Storage::flush() {
  pagemap_t::iterator i;
  for(i = pages_to_write.begin(); i != pages_to_write.end(); i++) {
    output.seekp(offset(i->first), std::ios_base::beg);
    output.write((const char*)i->second, sizeof(Page));
    tmpmem.free(i->second);
  }
  pages_to_write.clear();
  output.flush();
}

Page *Storage::get_writable(uint64_t pos) {
  auto f = pages_to_write.find(pos);
  if (f != pages_to_write.end()) {
    return f->second;
  }
  Page *result = tmpmem.alloc();
  if (!result) {
    flush();
    result = tmpmem.alloc();
  }
  memcpy(result, page(pos), sizeof(Page));
  pages_to_write[pos] = result;
  return result;
}

void Storage::transaction_inc() {
  size_t trans_id(start->header.transaction_id + 1);
  output.seekp(offsetof(StorageHeader, transaction_id), std::ios_base::beg);
  output.write((char*)&trans_id, sizeof(trans_id));
}

location_p Storage::alloc(size_t size) {
  location_p result = location_p::b(0);
  size = PAGE_ROUND_UP(size);

  if (!start->header.freed_head || size > PAGE_SIZE) {
    size_t count = size/PAGE_SIZE;
    if (start->header.free_block + count >= block_end) {
      increase(std::max(2*size, delta));      
    }

    result.page = start->header.free_block;
    size_t free_block = result.page + count;
    output.seekp(offsetof(StorageHeader, free_block), std::ios_base::beg);
    output.write((char*)&free_block, sizeof(free_block));
    return result;
  }

  assert(size == PAGE_SIZE);
  
  const Page *p = page(start->header.freed_head);
  result = location_p::b(start->header.freed_head);
  size_t freed_head = p->next_storage;
  output.seekp(offsetof(StorageHeader, freed_head), std::ios_base::beg);
  output.write((char*)&freed_head, sizeof(freed_head));
  return result;
}
  
void Storage::free(location_p pos, size_t size) {
  uint64_t off(offset(pos));
  uint64_t first_page = off / PAGE_SIZE;
  size = PAGE_ROUND_UP(size);
  
  for(size_t i = PAGE_SIZE; i < size; i += PAGE_SIZE, off += PAGE_SIZE) {
    uint64_t ploc = (off / PAGE_SIZE) + 1;
    output.seekp(off, std::ios_base::beg);
    output.write((char*)&ploc, sizeof(ploc));
  }
  output.seekp(off, std::ios_base::beg);  // the last page of the block;
  output.write((char*)&start->header.freed_head, sizeof(start->header.freed_head));

  output.seekp(offsetof(StorageHeader, freed_head), std::ios_base::beg);
  output.write((char*)&first_page, sizeof(first_page));
}

void Storage::write_value(location_p pos, const Slice& value) {
  EndLeaf node;
  node.size = value.size();
  output.seekp(offset(pos), std::ios_base::beg);
  output.write((char*)&node, sizeof(node));
  output.write(value.data(), value.size());
}

void Storage::increase(size_t size) {
  size_t new_size = size + region->get_size();
  std::filesystem::resize_file(file.get_name(), new_size);
  delete region;
  region = new mapped_region(file, read_only, 0, new_size);
  start = (Page*)region->get_address();
  block_end = new_size / PAGE_SIZE;
}

} // namespace leaves
