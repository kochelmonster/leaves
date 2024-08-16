#include "storage.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <filesystem>
#include <iostream>


using boost::interprocess::create_only;
using boost::interprocess::create_only_t;
using boost::interprocess::open_only;
using boost::interprocess::open_only_t;
using boost::interprocess::read_only;
using boost::interprocess::read_write;
using namespace boost::interprocess;

namespace leaves {

INLINE Storage::Storage(const char* path) : active_transaction(0) {
  DBMemory::init(path);
  memory.reset(new DBMemory(path));
  init_shared();
}

INLINE Storage::~Storage() {
  shared_memory_object::remove(shared_object.get_name());
}

INLINE void Storage::init_shared() {
  std::vector<std::string> parts;
  boost::split(parts, memory->get_filename(), boost::is_any_of("/\\"));
  std::string name = *parts.rbegin();
  name.insert(0, "shared-");
  try {
    shared_object = shared_memory_object(create_only, name.c_str(), read_write);
    shared_object.truncate(sizeof(SharedMem));
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
    memset(shared, 0, sizeof(SharedMem));
  } catch (...) {
    shared_object = shared_memory_object(open_only, name.c_str(), read_write);
    shared_region = mapped_region(shared_object, read_write);
    shared = (SharedMem*)shared_region.get_address();
  }
}

INLINE bool Storage::start_transaction() {
  if (ipcdetail::atomic_cas32(&shared->transaction_active, 1, 0) == 1) {
    return false;
  }

  memory->prepare_transaction();
  const PersistBlock& root = memory->get_root()->block;
  active_transaction = root.transaction + 1;
  return true;
}

INLINE void Storage::rollback() {
  memory->end_transaction();
  shared->transaction_active = 0;
  active_transaction = 0;
}

INLINE void Storage::prepare_commit() {
  int active = (memory->db->active + 1) & 1;
  memory->write_transaction(active);
  memory->end_transaction();
  memory->output.flush();
}

INLINE void Storage::commit() {
  int active = (memory->db->active + 1) & 1;
  memory->write_active(active);
  memory->output.flush();
}

INLINE int Storage::alloc_cursor(offset_ptr root) {
  uint32_t last_index = shared->last_index;

  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
    ReadCursor& cursor = shared->readers[
        (i + last_index) % SharedMem::READER_COUNT];
    if (!cursor.transaction) {
      if (ipcdetail::atomic_cas32(&shared->last_index, last_index, i+1) == last_index) {
        const PersistBlock& root_block = memory->get_block(root)->block;
        cursor.transaction = root_block.transaction;
        return i;
      }
      else {
        return alloc_cursor(root);
      }
    }
  }
  throw std::runtime_error("Cannot allocate cursor");
}

INLINE void Storage::free_cursor(int id) {
  shared->readers[id].transaction = 0;
  shared->max_free_transaction = 0; // invalidate the transaction
}

INLINE tid_t Storage::get_max_transaction() {
  if (shared->max_free_transaction)
    return shared->max_free_transaction;

  tid_t min_trans = active_transaction - 1;
  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
      ReadCursor& cursor = shared->readers[i];
      if (cursor.transaction)
        min_trans = std::min(min_trans, cursor.transaction);
  }

  shared->max_free_transaction = min_trans;
  return min_trans;
}


}  // namespace leaves
