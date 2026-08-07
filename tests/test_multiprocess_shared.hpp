#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../include/leaves/intern/multi/_confluence_db.hpp"
#include "../include/leaves/mmap.hpp"

namespace mp_test {

using namespace leaves;

using StorageImpl = MapStorage::StorageImpl;
using MainDB = _DB<StorageImpl>;
using CDB = _ConfluenceDB<MainDB>;

inline constexpr const char* MP_FILE = "test_mp.lvs";
inline constexpr int kCrashExitCode = 99;

[[noreturn]] inline void abrupt_exit(int code) {
  std::_Exit(code);
}

inline Slice mkkey(int i) {
  static thread_local char buffer[32];
  std::snprintf(buffer, sizeof buffer, "key%08d", i);
  return Slice(buffer);
}

inline Slice mkval(int i) {
  static thread_local char buffer[32];
  std::snprintf(buffer, sizeof buffer, "val%08d", i);
  return Slice(buffer);
}

inline void create_db() {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
}

inline bool child_write_range(int base, int count, bool merge) {
  try {
    std::fprintf(stderr, "[diag][helper] child_write_range begin base=%d count=%d merge=%d\n", base, count, merge ? 1 : 0);
    std::fflush(stderr);
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    CDB cdb(*main_db);
    auto cursor = cdb.create_cursor();
    std::fprintf(stderr, "[diag][helper] child_write_range opening txn\n");
    std::fflush(stderr);
    if (!cursor->start_transaction()) {
      std::fprintf(stderr, "[diag][helper] child_write_range start_transaction failed\n");
      std::fflush(stderr);
      return false;
    }
    for (int i = 0; i < count; ++i) {
      cursor->find(Slice(mkkey(base + i)));
      cursor->value(Slice(mkval(base + i)));
    }
    std::fprintf(stderr, "[diag][helper] child_write_range about to commit\n");
    std::fflush(stderr);
    if (!cursor->commit()) {
      std::fprintf(stderr, "[diag][helper] child_write_range commit failed\n");
      std::fflush(stderr);
      return false;
    }
    cursor.reset();
    if (merge) {
      std::fprintf(stderr, "[diag][helper] child_write_range calling merge_all_now\n");
      std::fflush(stderr);
      cdb.merge_all_now();
    }
    std::fprintf(stderr, "[diag][helper] child_write_range success\n");
    std::fflush(stderr);
    return true;
  } catch (...) {
    std::fprintf(stderr, "[diag][helper] child_write_range threw exception\n");
    std::fflush(stderr);
    return false;
  }
}

inline bool writer_batches(int total_keys, int batches) {
  try {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    CDB cdb(*main_db);
    for (int batch = 0; batch < batches; ++batch) {
      auto cursor = cdb.create_cursor();
      if (!cursor->start_transaction()) return false;
      for (int i = 0; i < total_keys / batches; ++i) {
        int index = batch * (total_keys / batches) + i;
        cursor->find(Slice(mkkey(index)));
        cursor->value(Slice(mkval(index)));
      }
      if (!cursor->commit()) return false;
      cursor.reset();
      cdb.merge_all_now();
    }
    return true;
  } catch (...) {
    return false;
  }
}

inline bool merger_loops(int loops) {
  try {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    CDB cdb(*main_db);
    for (int i = 0; i < loops; ++i) cdb.merge_all_now();
    return true;
  } catch (...) {
    return false;
  }
}

inline bool contention_worker(int worker_index, int batches, int per_batch) {
  try {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    CDB cdb(*main_db);
    for (int batch = 0; batch < batches; ++batch) {
      auto cursor = cdb.create_cursor();
      if (!cursor->start_transaction()) return false;
      int base = (worker_index * batches + batch) * per_batch;
      for (int i = 0; i < per_batch; ++i) {
        cursor->find(Slice(mkkey(base + i)));
        cursor->value(Slice(mkval(base + i)));
      }
      if (!cursor->commit()) return false;
      cursor.reset();
      cdb.merge_all_now();
    }
    return true;
  } catch (...) {
    return false;
  }
}

inline void crash_during_merge(int key_count) {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  auto* cdb = new CDB(*main_db);
  auto cursor = cdb->create_cursor();
  if (!cursor->start_transaction()) abrupt_exit(1);
  for (int i = 0; i < key_count; ++i) {
    cursor->find(Slice(mkkey(i)));
    cursor->value(Slice(mkval(i)));
  }
  if (!cursor->commit()) abrupt_exit(1);

  size_t count = cdb->_tributaries_count.load(std::memory_order_acquire);
  bool stamped = false;
  for (size_t i = 0; i < count; ++i) {
    auto* tributary = cdb->_trib_at(i);
    uint8_t state = tributary->_header->state.load(std::memory_order_acquire);
    if (state == CDB::Slot::ATTACHED || state == CDB::Slot::WRITING) {
      tributary->_header->state.store(CDB::Slot::MERGING,
                                      std::memory_order_release);
      stamped = true;
    }
  }

  abrupt_exit(stamped ? 0 : 1);
}

inline void crash_during_transaction(int committed_keys, int interrupted_keys) {
  std::fprintf(stderr,
               "[diag][helper] crash_during_transaction begin committed=%d interrupted=%d\n",
               committed_keys, interrupted_keys);
  std::fflush(stderr);
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);

  {
    auto cursor = cdb.create_cursor();
    if (!cursor->start_transaction()) abrupt_exit(1);
    for (int i = 0; i < committed_keys; ++i) {
      cursor->find(Slice(mkkey(i)));
      cursor->value(Slice(mkval(i)));
    }
    if (!cursor->commit()) abrupt_exit(1);
  }
  std::fprintf(stderr, "[diag][helper] crash_during_transaction committed first batch\n");
  std::fflush(stderr);

  auto cursor = cdb.create_cursor();
  if (!cursor->start_transaction()) abrupt_exit(1);
  for (int i = 0; i < interrupted_keys; ++i) {
    cursor->find(Slice(mkkey(committed_keys + i)));
    cursor->value(Slice(mkval(committed_keys + i)));
  }
  std::fprintf(stderr, "[diag][helper] crash_during_transaction about to crash\n");
  std::fflush(stderr);

  abrupt_exit(kCrashExitCode);
}

inline void recover_lost_areas_crash(int key_count, int corrupt_count) {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  {
    auto cursor = main_db->create_cursor();
    if (!cursor->start_transaction()) abrupt_exit(1);
    for (int i = 0; i < key_count; ++i) {
      cursor->find(Slice(mkkey(i)));
      cursor->value(Slice(mkval(i)));
    }
    if (!cursor->commit()) abrupt_exit(1);
  }

  offset_t corrupt_offsets[8] = {};
  if (corrupt_count > static_cast<int>(std::size(corrupt_offsets))) abrupt_exit(1);

  auto area1 = storage->alloc_single_area();
  auto area2 = storage->alloc_single_area();
  auto area3 = storage->alloc_multi_area(2 * StorageImpl::AREA_SIZE);
  auto area4 = storage->alloc_single_area();
  if (!area1 || !area2 || !area3 || !area4) abrupt_exit(1);

  corrupt_offsets[0] = storage->resolve(area1);
  corrupt_offsets[1] = storage->resolve(area2);
  corrupt_offsets[2] = storage->resolve(area3);
  corrupt_offsets[3] = storage->resolve(area4);

  {
    auto cursor = main_db->create_cursor();
    if (!cursor->start_transaction()) abrupt_exit(1);
    for (int i = 0; i < corrupt_count; ++i) {
      char key[64];
      char value[64];
      std::snprintf(key, sizeof key, "cr_%d_off", i);
      std::snprintf(value, sizeof value, "%llu",
                    static_cast<unsigned long long>(
                        static_cast<uint64_t>(corrupt_offsets[i])));
      cursor->find(Slice(key));
      cursor->value(Slice(value));
    }
    if (!cursor->commit()) abrupt_exit(1);
  }

  for (int i = 0; i < corrupt_count; ++i) {
    char* pointer = reinterpret_cast<char*>(storage->_memory) +
                    static_cast<uint64_t>(corrupt_offsets[i]);
    std::memset(pointer, 0, sizeof(Area));
  }
  storage->flush(true, true);

  abrupt_exit(kCrashExitCode);
}

inline void two_phase_prepare_crash(int key_count) {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  auto cursor = main_db->create_cursor();
  if (!cursor->start_transaction()) abrupt_exit(1);
  for (int i = 0; i < key_count; ++i) {
    cursor->find(Slice(mkkey(i)));
    cursor->value(Slice(mkval(i)));
  }

  cursor->prepare_commit(true);
  abrupt_exit(kCrashExitCode);
}

inline void wal_prepare_crash(int key_count) {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  auto cursor = main_db->create_cursor();
  if (!cursor->start_transaction(false, true)) abrupt_exit(1);
  for (int i = 0; i < key_count; ++i) {
    cursor->find(Slice(mkkey(i)));
    cursor->value(Slice(mkval(i)));
  }

  cursor->prepare_commit();
  abrupt_exit(kCrashExitCode);
}

}  // namespace mp_test