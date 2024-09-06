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

INLINE Storage::Storage(const char* path) {
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
  return true;
}

INLINE void Storage::rollback() {
  memory->end_transaction();
  shared->transaction_active = 0;
}

INLINE void Storage::prepare_commit() {
  memory->write_transaction();
  memory->end_transaction();
}

INLINE void Storage::commit() {
  memory->commit_transaction();
  shared->transaction_active = 0;
}

INLINE int Storage::alloc_cursor() {
  uint32_t last_index = shared->last_index;

  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
    ReadCursor& cursor = shared->readers[
        (i + last_index) % SharedMem::READER_COUNT];
    if (!cursor.txn_id) {
      if (ipcdetail::atomic_cas32(&shared->last_index, last_index, i+1) == last_index) {
        cursor.txn_id = memory->get_active_head()->txn_id;
        return i;
      }
      else {
        return alloc_cursor();
      }
    }
  }
  throw std::runtime_error("Cannot allocate cursor");
}

INLINE offset_ptr Storage::update_cursor(int id) {
  ReadCursor& cursor = shared->readers[id];
  const DBMeta* head = memory->get_active_head();
  tid_t txn_id = head->txn_id;
  if (txn_id != cursor.txn_id) {
    cursor.txn_id = txn_id;
    shared->max_free_transaction = 0;
  }
  return head->root;
}


INLINE void Storage::free_cursor(int id) {
  shared->readers[id].txn_id = 0;
  shared->max_free_transaction = 0; // invalidate the transaction
}

INLINE tid_t Storage::get_max_transaction() {
  if (shared->max_free_transaction)
    return shared->max_free_transaction;

  tid_t min_trans = memory->get_active_head()->txn_id;
  for(int i = 0; i < SharedMem::READER_COUNT; i++) {
      ReadCursor& cursor = shared->readers[i];
      if (cursor.txn_id)
        min_trans = std::min(min_trans, cursor.txn_id);
  }

  shared->max_free_transaction = min_trans;
  return min_trans;
}


}  // namespace leaves
