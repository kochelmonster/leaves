#include "storage.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <cstddef>
#include <filesystem>
#include <iostream>

#include "page.hpp"

using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using namespace boost::interprocess;

namespace leaves {

MemoryView::MemoryView(const char* path) {
  size_t size = std::filesystem::file_size(path);
  file = file_mapping(path, read_only);
  region = mapped_region(file, read_only, 0, size);
  start = (char*)region.get_address();
}

Storage::Storage(const char* path) : active_transaction(0) {
  if (!std::filesystem::is_regular_file(path)) {
    memset(&header, 0, sizeof(header));
    strcpy(header.signature, SIGNATURE);
    header.db_version = 0;
    header.transaction_id = 1;
    header.freed_head = 0;
    header.free_block = 2;
    header.txn[0].id = 1;  // to avoid wrong ref counting in trace
    header.txn[1].id = header.transaction_id;
    header.txn[1].root.offset = PAGE_SIZE;
    header.pools[stored_ptr::PAGE_POOL].scurrent =
        sizeof(header) + PAGE_SIZE;  // allocate the first page!
    header.pools[stored_ptr::PAGE_POOL].slast = sizeof(header) + BLOCK_SIZE2;

    std::ofstream fhead(path, std::ios::out | std::ios::binary);
    fhead.write((char*)&header, sizeof(header));
    assert(sizeof(Header) == PAGE_SIZE);
    fhead.close();
  }

  view.reset(new MemoryView(path));
  memcpy(&header, get_header(), sizeof(header));

  if (strcmp(get_header()->signature, SIGNATURE))
    throw std::runtime_error("wrong filetype");

  offset_t last = 0;
  for (int i = 0; i < STORAGE_POOL_COUNT; i++) {
    last = std::max(header.pools[i].slast, last);
  }

  if (view->get_size() < last) {
    // A missed resize
    std::filesystem::resize_file(path, last);
    view.reset(new MemoryView(path));
  }

  output.open(path,
              std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);

  init_shared();
}

Storage::~Storage() {
  output.close();
  shared_memory_object::remove(shared_object.get_name());
}

void Storage::init_shared() {
  std::vector<std::string> parts;
  boost::split(parts, view->get_filename(), boost::is_any_of("/\\"));
  std::string name = *parts.rbegin();
  name.insert(0, "shared-");
  try {
    shared_object = shared_memory_object(create_only, name.c_str(), read_write);
    shared_object.truncate(sizeof(SharedMem));
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
    memset(shared, 0, sizeof(SharedMem));
    shared->size = view->get_size();
  } catch (...) {
    shared_object = shared_memory_object(open_only, name.c_str(), read_write);
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
  }
}

void Storage::check_size() {
  if (view->get_size() != shared->size) {
    view.reset(new MemoryView(view->get_filename()));
  }
}

Page* Storage::get_writable_page(stored_ptr ploc) {
  Page* page = alloc_new_page();
  memcpy(page, ploc.get<Page>(view.get()), PAGE_SIZE);
  add_page_to_copied(ploc);
  return page;
}

void Storage::add_to_marked(stored_ptr value, int pool_id,
                            MarkedBlocks* marker_storage[]) {
  MarkedBlocks* page = marker_storage[pool_id];
  if (!page) {
    marker_storage[pool_id] = page = &page_pool.malloc()->marker;
    if (!page) throw std::bad_alloc();
    page->count = 0;
    page->pool_id = pool_id;
    page->next_mem = nullptr;
  }

  if (page->count >= page->REF_COUNT) {
    MarkedBlocks* new_page = &page_pool.malloc()->marker;
    if (!new_page) throw std::bad_alloc();
    new_page->count = 0;
    new_page->pool_id = pool_id;
    new_page->next_mem = page;
    marker_storage[pool_id] = page = new_page;
  }

  page->refs[page->count++] = value.offset;
}

void Storage::merge_marked_to_free(offset_t head) {
  if (head) {
    MarkedBlocks* blocks = view->get_blocks(head);
    merge_marked_to_free(blocks->next);
    merge_to_free(blocks, blocks->pool_id);
  }
}

void Storage::free(MarkedBlocks* page) {
  while (page) {
    MarkedBlocks* next = page->next_mem;
    page_pool.free((PageMemory*)page);
    page = next;
  }
}

void Storage::free_marked(MarkedBlocks* marker_storage[]) {
  for (int i = 0; i < STORAGE_POOL_COUNT; i++) {
    free(marker_storage[i]);
    marker_storage[i] = nullptr;
  }
}

void Storage::free_marked(offset_t head) {
  if (head) {
    MarkedBlocks* blocks = view->get_blocks(head);
    free_marked(blocks->next);
    free(head, stored_ptr::PAGE_POOL);
  }
}

offset_t Storage::write_marked(MarkedBlocks* marker_storage[]) {
  offset_t next = 0;
  for (int i = 0; i < STORAGE_POOL_COUNT; i++) {
    MarkedBlocks* j = marker_storage[i];
    if (j) next = write_marked(j, next);
  }
  return next;
}

offset_t Storage::write_marked(MarkedBlocks* blocks, offset_t next) {
  MarkedBlocks* next_mem = blocks->next_mem;
  if (next_mem) {
    blocks->next = write_marked(blocks->next_mem, next);
  } else {
    blocks->next = next;
  }

  assert(blocks->count <= MarkedBlocks::REF_COUNT);
  offset_t result = alloc_storage_block(stored_ptr::PAGE_POOL);
  output.seekp(result, std::ios_base::beg);
  output.write((char*)blocks, sizeof(MarkedBlocks));
  blocks->next_mem = next_mem;
  return result;
}

stored_ptr Storage::new_value(const Slice& value) {
  stored_ptr result;
  uint32_t storage_size = 0;
  if (value.size() > 2048) {
    // value size are the first bytes of value block
    if (value.size() < (64 * 1024) - sizeof(uint16_t))
      storage_size = sizeof(uint16_t);
    else
      storage_size = sizeof(uint32_t);
  } else {
    result.size = value.size() + 20;
  }

  uint32_t sum_size = storage_size + value.size();
  int pool_id = 31 - clz(sum_size | 1);
  if ((1 << pool_id) < sum_size && pool_id < 32) pool_id++;

  pool_id = std::max(pool_id, 4) - 4;

  offset_t offset = alloc_storage_block(pool_id);
  output.seekp(offset, std::ios_base::beg);

  if (storage_size == sizeof(uint16_t)) {
    uint16_t size = value.size();
    output.write((char*)&size, sizeof(size));
    result.size = pool_id - 8;
  } else if (storage_size == sizeof(uint32_t)) {
    uint32_t size = value.size();
    output.write((char*)&size, sizeof(size));
    result.size = pool_id - 8;
  }
  output.write(value.data(), value.size());
  output.flush();

  result.offset = offset;
  add_to_marked(result, pool_id, txn_heads);
  return result;
}

MarkedBlocks* Storage::get_free_blocks(int pool_id) {
  StoragePool& pool = header.pools[pool_id];
  if (pool.mfree) return pool.mfree;

  if (pool.sfree) {
    offset_t sfree = pool.sfree;
    pool.mfree = &page_pool.malloc()->marker;
    memcpy(pool.mfree, view->get_blocks(pool.sfree), sizeof(MarkedBlocks));
    pool.sfree = pool.mfree->next;
    pool.mfree->next_mem = nullptr;
    free(sfree, stored_ptr::PAGE_POOL);
    assert(pool.mfree->count <= MarkedBlocks::REF_COUNT);
    return pool.mfree;
  }
  return nullptr;
}

offset_t Storage::alloc_storage_block(int pool_id) {
  StoragePool& pool = header.pools[pool_id];
  MarkedBlocks* free_blocks = get_free_blocks(pool_id);
  offset_t result;

  if (free_blocks) {
    result = free_blocks->refs[--free_blocks->count];
    if (!free_blocks->count) {
      pool.mfree = free_blocks->next_mem;
      free_blocks->next_mem = nullptr;
      free(free_blocks);
    }
    return result;
  }

  if (pool.scurrent == pool.slast) {
    size_t old_size = view->get_size();
    size_t new_size = old_size + block_size_per_pool[pool_id];
    pool.scurrent = old_size;
    pool.slast = new_size;
    output.seekp(offsetof(Header, pools) + sizeof(StoragePool) * pool_id,
                 std::ios_base::beg);
    output.write((char*)&pool.scurrent, sizeof(pool.scurrent));
    output.write((char*)&pool.slast, sizeof(pool.slast));
    output.flush();
    std::filesystem::resize_file(view->get_filename(), new_size);
    view.reset(new MemoryView(view->get_filename()));
    shared->size = view->get_size();
  }

  result = pool.scurrent;
  pool.scurrent += 1 << (pool_id + 4);
  return result;
}

MarkedBlocks* Storage::new_free_blocks(int pool_id) {
  StoragePool& pool = header.pools[pool_id];
  assert(!pool.mfree);
  pool.mfree = &page_pool.malloc()->marker;
  pool.mfree->next_mem = nullptr;
  pool.mfree->count = 0;
  pool.mfree->pool_id = pool_id;
  return pool.mfree;
}

void Storage::free(offset_t ptr, int pool_id) {
  MarkedBlocks* free_blocks = get_free_blocks(pool_id);
  if (!free_blocks) free_blocks = new_free_blocks(pool_id);

  if (free_blocks->count >= free_blocks->REF_COUNT) {
    StoragePool& pool = header.pools[pool_id];
    pool.mfree = &page_pool.malloc()->marker;
    pool.mfree->pool_id = pool_id;
    pool.mfree->next_mem = free_blocks;
    free_blocks = pool.mfree;
    free_blocks->count = 0;
  }
  free_blocks->refs[free_blocks->count++] = ptr;
}

void Storage::merge_to_free(MarkedBlocks* src, int pool_id) {
  MarkedBlocks* free_blocks = get_free_blocks(pool_id);
  if (!free_blocks) free_blocks = new_free_blocks(pool_id);

  int add_count =
      std::min(src->count, (uint16_t)(src->REF_COUNT - free_blocks->count));
  memcpy(&free_blocks->refs[free_blocks->count], src->refs,
         sizeof(offset_t) * add_count);
  free_blocks->count += add_count;

  if (add_count < src->count) {
    StoragePool& pool = header.pools[pool_id];
    pool.mfree = &page_pool.malloc()->marker;
    pool.mfree->pool_id = pool_id;
    pool.mfree->next_mem = free_blocks;
    free_blocks = pool.mfree;
    free_blocks->count = src->count - add_count;
    memcpy(free_blocks->refs, &src->refs[add_count],
           sizeof(offset_t) * free_blocks->count);
  }
}

stored_ptr Storage::write_page(Page* copy) {
  stored_ptr result;
  result.size = copy->size;
  result.offset = alloc_storage_block(stored_ptr::PAGE_POOL);
  output.seekp(result.offset, std::ios_base::beg);
  output.write((char*)copy, PAGE_SIZE);
  add_to_marked(result, stored_ptr::PAGE_POOL, txn_heads);
  free(copy);
  return result;
}

bool Storage::start_transaction() {
  if (active_transaction) return false;

  if (ipcdetail::atomic_cas32(&shared->transaction, 1, 0) == 1) {
    throw TransactionActive();
  }

  uint64_t next_txn = transaction_id();

  /* TRANSACTION_COUNT-1: don't use the current transaction view for the new
     transaction */
  for (int i = 1; i < TRANSACTION_COUNT - 2; i++) {
    if (!shared->txn_ref_count[++next_txn % TRANSACTION_COUNT]) {
      // this transaction is free
      break;
    }
  }

  int idx = next_txn % TRANSACTION_COUNT;
  if (shared->txn_ref_count[idx]) {
    shared->transaction = 0;
    throw NoTransactionFree();
  }

  active_transaction = next_txn;
  memset(txn_heads, 0, sizeof(txn_heads));
  memset(copied_heads, 0, sizeof(copied_heads));
  return true;
}

void Storage::rollback() {
  int idx = active_transaction % TRANSACTION_COUNT;
  Transaction& txn = header.txn[idx];

  // Free heap memory
  free_marked(copied_heads);
  free_marked(txn_heads);

  for (int i = 0; i < STORAGE_POOL_COUNT; i++) {
    free(header.pools[i].mfree);
    header.pools[i].mfree = nullptr;
  }

  // free storage memory
  if (memcmp(&header, view->get_header(), sizeof(header))) {
    // before prepare commit
    memcpy(&header, view->start, sizeof(header));
  } else {
    merge_marked_to_free(txn.txn_head);
    free_marked(txn.copied_head);
    free_marked(txn.txn_head);
    txn.copied_head = txn.txn_head = 0;
    write_pools();
    output.flush();
  }

  shared->transaction = 0;
  active_transaction = 0;
}

void Storage::write_pools() {
  for (int i = 0; i < STORAGE_POOL_COUNT; i++) {
    if (header.pools[i].mfree && i != stored_ptr::PAGE_POOL) {
      header.pools[i].sfree =
          write_marked(header.pools[i].mfree, header.pools[i].sfree);
      free(header.pools[i].mfree);
      header.pools[i].mfree = nullptr;
    }
  }

  // special handling for page page pool
  StoragePool& pp = header.pools[stored_ptr::PAGE_POOL];
  if (pp.mfree) {
    // count the free marker pages
    int free_markers_count = 0;
    MarkedBlocks *fm = pp.mfree;
    while(fm) {
      free_markers_count++;
      fm = fm->next_mem;
    }

    while (pp.mfree->count <= free_markers_count) {
      /* Special Page hack:
         write_marked allocates pages: i.e. it reduces the pp.mfree->count.
         if pp.mfree->count == free_markers_count, pm.free will be released
         while write_page. This leads to a segmentation fault.

         Add an extra free page to avoid releasing pp.mfree
      */
      offset_t sfree = pp.sfree;
      MarkedBlocks* mfree = pp.mfree;
      pp.sfree = 0;      // alloc shall use the main mem block
      pp.mfree = nullptr;
      offset_t block = alloc_storage_block(stored_ptr::PAGE_POOL);
      pp.sfree = sfree;
      pp.mfree = mfree;
      free(block, stored_ptr::PAGE_POOL);
    }
    pp.sfree = write_marked(pp.mfree, pp.sfree);
    free(pp.mfree);
    pp.mfree = nullptr;
  }
}

void Storage::prepare_commit(stored_ptr root) {
  int idx = active_transaction % TRANSACTION_COUNT;
  Transaction& txn = header.txn[idx];

  // Free old transaction data
  merge_marked_to_free(txn.copied_head);
  free_marked(txn.copied_head);
  free_marked(txn.txn_head);

  txn.id = active_transaction;
  txn.root = root;
  txn.copied_head = write_marked(copied_heads);
  txn.txn_head = write_marked(txn_heads);
  write_pools();

  output.seekp(0, std::ios_base::beg);
  output.write((char*)&header, sizeof(header));
  output.flush();

  free_marked(copied_heads);
  free_marked(txn_heads);
}

void Storage::commit() {
  output.seekp(offsetof(Header, transaction_id), std::ios_base::beg);
  output.write((char*)&active_transaction, sizeof(active_transaction));
  output.flush();
  header.transaction_id = active_transaction;
  active_transaction = shared->transaction = 0;
}

}  // namespace leaves
