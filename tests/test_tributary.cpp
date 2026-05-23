#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE tributary
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <vector>

#include "../include/leaves/intern/multi/_confluence_db.hpp"
#include "test.hpp"

using namespace leaves;

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------
using StorageImpl  = MapStorage::StorageImpl;
using MainDB       = _DB<StorageImpl>;
using CDB          = _ConfluenceDB<MainDB>;
using TDB          = _TributaryDB<StorageImpl>;

struct TributaryPreparation {
  TributaryPreparation() { std::remove(TEST_FILE); }
  ~TributaryPreparation() {
    std::remove(TEST_FILE);
  }
};

// Helper: create a ConfluenceDB (no background monitor in tests)
using TestHandle = std::tuple<std::unique_ptr<StorageImpl>,
                              std::unique_ptr<MainDB>, CDB*>;
static TestHandle make_cdb(const char* path) {
  auto storage = std::make_unique<StorageImpl>(path);
  offset_t header{0};
  auto main_db = std::make_unique<MainDB>(*storage, &header, "main");
  auto* cdb = new CDB(*main_db, false, false);
  return {std::move(storage), std::move(main_db), cdb};
}

// Helper: open an existing ConfluenceDB
static TestHandle open_cdb(const char* path, offset_t header) {
  auto storage = std::make_unique<StorageImpl>(path);
  auto main_db = std::make_unique<MainDB>(*storage, header, "main");
  auto* cdb = new CDB(*main_db, false);
  return {std::move(storage), std::move(main_db), cdb};
}

// Helper: write a key/value pair via a ConfluenceCursor
static void write_kv(CDB& cdb, const std::string& key,
                     const std::string& value) {
  auto cursor = cdb.create_cursor();
  BOOST_REQUIRE(cursor->start_transaction());
  cursor->find(Slice(key));
  cursor->value(Slice(value));
  BOOST_REQUIRE(cursor->commit());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_tributary_basic_write_read) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "hello", "world");

  // Read back without merging
  auto cursor = cdb->create_cursor();
  cursor->find(Slice("hello"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("world"));
}

BOOST_AUTO_TEST_CASE(test_tributary_two_producers) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Producer A
  write_kv(*cdb, "key_a", "value_a");
  // Producer B
  write_kv(*cdb, "key_b", "value_b");

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key_a"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("value_a"));
  cursor->find(Slice("key_b"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("value_b"));
}

BOOST_AUTO_TEST_CASE(test_tributary_overwrite_latest_wins) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Write first value (lower txn_id)
  write_kv(*cdb, "key", "old");
  // Write second value (higher txn_id)
  write_kv(*cdb, "key", "new");

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("new"));
}

BOOST_AUTO_TEST_CASE(test_tributary_delete_propagates) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "key", "value");

  // Delete the key
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("key"));
    BOOST_REQUIRE(cursor->is_valid());
    cursor->remove();
    BOOST_REQUIRE(cursor->commit());
  }

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_tributary_merge_into_main) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "alpha", "1");
  write_kv(*cdb, "beta",  "2");

  // Force merge all tributaries (set idle_timeout=0 so any written slot qualifies)
  cdb->set_idle_timeout_seconds(0);
  cdb->merge_eligible_tributaries();

  // After merge the data should be in the main DB
  auto cursor = cdb->create_cursor();
  cursor->find(Slice("alpha"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("1"));
  cursor->find(Slice("beta"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("2"));
}

BOOST_AUTO_TEST_CASE(test_tributary_delete_then_merge) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "k", "v");

  // Delete
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("k"));
    BOOST_REQUIRE(cursor->is_valid());
    cursor->remove();
    BOOST_REQUIRE(cursor->commit());
  }

  // Force merge — deletion should propagate to main DB
  cdb->set_idle_timeout_seconds(0);
  cdb->merge_eligible_tributaries();

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("k"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_tributary_iteration_forward) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "a", "1");
  write_kv(*cdb, "b", "2");
  write_kv(*cdb, "c", "3");

  auto cursor = cdb->create_cursor();
  cursor->first();
  BOOST_REQUIRE(cursor->is_valid());

  std::vector<std::string> keys;
  while (cursor->is_valid()) {
    keys.push_back(cursor->key().string());
    cursor->next();
  }

  BOOST_REQUIRE_EQUAL(keys.size(), 3u);
  BOOST_CHECK_EQUAL(keys[0], "a");
  BOOST_CHECK_EQUAL(keys[1], "b");
  BOOST_CHECK_EQUAL(keys[2], "c");
}

BOOST_AUTO_TEST_CASE(test_tributary_rollback) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Write something and rollback
  {
    auto cursor = cdb->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("x"));
    cursor->value(Slice("y"));
    cursor->rollback();
  }

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("x"));
  BOOST_CHECK(!cursor->is_valid());
}

// ---------------------------------------------------------------------------
// Bug-regression tests
// ---------------------------------------------------------------------------

// Bug 1: _merge_unclaimed_tributaries() in the original code silently skips
// tributaries in WRITING or MERGING state.  If a process crashes while a
// tributary is in WRITING state (e.g. while holding txn_ref_lock inside
// start_transaction()), the spinlock is left at 1 in the mmap file.
// On re-open, sanitize() -> _merge_unclaimed_tributaries() ignores WRITING
// tributaries, leaving the stuck lock unreset.  Any subsequent txn_ref() on
// that tributary spins forever (deadlock).
//
// This test forces a tributary into WRITING state with txn_ref_lock=1,
// calls sanitize(), then asserts the lock is 0.
// Expected result with the *original* code: FAIL (lock stays 1).
BOOST_AUTO_TEST_CASE(test_tributary_writing_state_not_sanitized) {
  TributaryPreparation p;
  auto [storage_uptr, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Allocate at least one tributary slot.
  write_kv(*cdb, "key1", "value1");

  // Simulate crash: force WRITING state + stuck spinlock on every live tributary.
  using Slot = CDB::Slot;
  size_t n = cdb->_tributaries_count.load(std::memory_order_acquire);
  BOOST_REQUIRE_GT(n, 0u);
  for (size_t i = 0; i < n; ++i) {
    _TributaryDB<StorageImpl>* t = cdb->_trib_at(i);
    Slot* slot = t->_header;
    if (slot->state.load(std::memory_order_relaxed) != Slot::MERGED) {
      slot->state.store(Slot::WRITING, std::memory_order_relaxed);
      slot->txn_ref_lock._flag.store(1, std::memory_order_relaxed);
    }
  }

  // Crash-recovery path (normally called in the constructor on reopen).
  cdb->sanitize();

  // With original code _merge_unclaimed_tributaries() skips WRITING =>
  // txn_ref_lock stays at 1 => fill_sources_lockfree() deadlocks on txn_ref().
  n = cdb->_tributaries_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    _TributaryDB<StorageImpl>* t = cdb->_trib_at(i);
    uint32_t flag = t->_header->txn_ref_lock._flag.load(std::memory_order_relaxed);
    BOOST_CHECK_MESSAGE(
        flag == 0u,
        "tributary[" << i << "]: txn_ref_lock._flag=" << flag
        << " after sanitize() — _merge_unclaimed_tributaries() ignored WRITING state");
  }
}

// Bug 2: Verify that a tributary's header area (the first Area that contains
// the _TributaryHeader) is never returned to the free-area pool after
// merge/recycle cycles.  If reset_in_place() ever frees the header area, the
// mmap cell containing txn_ref_lock gets reallocated for B-tree user data.
// A user-data byte of 0x01 at that offset then causes every txn_ref() caller
// to spin forever — the same deadlock observed at YCSB op 5391.
//
// This test runs 30 write/merge/recycle cycles and after each cycle scans the
// free single-area pool to verify the header area was never leaked into it.
BOOST_AUTO_TEST_CASE(test_tributary_header_area_not_freed) {
  TributaryPreparation p;
  auto [storage_uptr, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);
  StorageImpl& storage = *storage_uptr;

  auto check_pool_integrity = [&](int cycle) {
    // Collect file offsets of the Area structs that host tributary headers.
    std::vector<uint64_t> header_area_offsets;
    size_t n = cdb->_tributaries_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
      _TributaryDB<StorageImpl>* t = cdb->_trib_at(i);
      uint64_t hdr_va   = (uint64_t)t->_header;
      uint64_t base_va  = reinterpret_cast<uint64_t>(storage._memory);
      uint64_t hdr_off  = hdr_va - base_va;
      uint64_t area_off = hdr_off - sizeof(Area);
      header_area_offsets.push_back(area_off);
    }

    // Walk the free single-area pool and check for any header area.
    auto& pool = storage._memory->area_pool.single_areas;
    offset_t cur = pool.head[pool.active];
    int steps = 0;
    while (cur && steps < 100000) {
      uint64_t off = (uint64_t)cur;
      bool is_header =
          std::find(header_area_offsets.begin(), header_area_offsets.end(), off)
          != header_area_offsets.end();
      BOOST_CHECK_MESSAGE(
          !is_header,
          "cycle " << cycle
          << ": tributary header area (file offset " << off
          << ") found in free area pool — header area was incorrectly freed");
      Area* area =
          reinterpret_cast<Area*>(reinterpret_cast<char*>(storage._memory) + off);
      cur = area->next;
      ++steps;
    }
  };

  cdb->set_idle_timeout_seconds(0);
  for (int cycle = 0; cycle < 30; ++cycle) {
    // Large values force extra area allocations inside the tributary so that
    // reset_in_place() must actually return areas, exercising the code path.
    for (int i = 0; i < 5; ++i) {
      write_kv(*cdb, "k" + std::to_string(cycle * 5 + i),
               std::string(200, 'x'));
    }
    cdb->merge_eligible_tributaries();
    check_pool_integrity(cycle);
  }
}
