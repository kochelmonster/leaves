#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE tributary
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
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
                              MainDB*, CDB*>;
static TestHandle make_cdb(const char* path) {
  auto storage = std::make_unique<StorageImpl>(path);
  auto* main_db = storage->template open<_DB>("main");
  auto* cdb = new CDB(*main_db);
  return {std::move(storage), main_db, cdb};
}

// Helper: open an existing ConfluenceDB
static TestHandle open_cdb(const char* path, offset_t /*header*/) {
  auto storage = std::make_unique<StorageImpl>(path);
  auto* main_db = storage->template open<_DB>("main");
  auto* cdb = new CDB(*main_db);
  return {std::move(storage), main_db, cdb};
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

  // "Latest wins" is only guaranteed for a single producer (one cursor reusing
  // its sticky tributary slot).  Across separate tributaries there is no global
  // commit order, so the winner of concurrent overwrites is unspecified.
  auto writer = cdb->create_cursor();

  // First overwrite (older)
  BOOST_REQUIRE(writer->start_transaction());
  writer->find(Slice("key"));
  writer->value(Slice("old"));
  BOOST_REQUIRE(writer->commit());

  // Second overwrite (newer) — same producer/slot, so this must win.
  BOOST_REQUIRE(writer->start_transaction());
  writer->find(Slice("key"));
  writer->value(Slice("new"));
  BOOST_REQUIRE(writer->commit());

  auto cursor = cdb->create_cursor();
  cursor->find(Slice("key"));
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("new"));
}

BOOST_AUTO_TEST_CASE(test_tributary_delete_propagates) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Read-your-own-delete is only guaranteed within a single producer (one
  // cursor reusing its sticky tributary slot).  Across separate tributaries
  // there is no global commit order, so a tombstone in one tributary is not
  // guaranteed to shadow a value in another until both are merged
  // (see test_tributary_delete_then_merge).
  auto cursor = cdb->create_cursor();

  BOOST_REQUIRE(cursor->start_transaction());
  cursor->find(Slice("key"));
  cursor->value(Slice("value"));
  BOOST_REQUIRE(cursor->commit());

  // Delete the key via the same producer/slot.
  BOOST_REQUIRE(cursor->start_transaction());
  cursor->find(Slice("key"));
  BOOST_REQUIRE(cursor->is_valid());
  cursor->remove();
  BOOST_REQUIRE(cursor->commit());

  cursor->find(Slice("key"));
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_tributary_merge_into_main) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "alpha", "1");
  write_kv(*cdb, "beta",  "2");

  // Force merge all tributaries (set max age=0 so any written slot qualifies)
  cdb->set_max_attached_age_ms(0);
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

// ---------------------------------------------------------------------------
// Regression: an ATTACHED slot that has aged past max_attached_age_seconds must
// be merged by the periodic age sweep EVEN under continuous writes to other
// slots.  The previous debounce design re-armed (cancel+reschedule) the timer
// on every transaction end, so sustained activity on slot B perpetually
// deferred slot A's age-out and A was never merged by the age path.  With the
// arm-once sweep, A's first deadline stands and A reaches main within ~2x the
// age limit.
//
// Cursor A is kept ALIVE for the whole test so its slot stays ATTACHED: if A
// were destroyed, ~_ConfluenceCursor would mark the slot MERGING and merge it
// directly, bypassing (and hiding) the age path we are testing.  Merge is
// observed by reading the MAIN DB directly (a main-only cursor does not see
// tributaries), so a hit proves the age sweep actually merged A.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_tributary_age_merge_not_starved_by_other_slots) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // High write threshold so commits stay ATTACHED: only the age sweep (not the
  // write-count threshold) may merge these slots.
  cdb->set_merge_write_threshold(1000000);
  // Short max attached age so the sweep fires quickly.
  cdb->set_max_attached_age_ms(1000);

  // Producer A: write a single key and KEEP the cursor alive so its sticky slot
  // stays ATTACHED and begins to age (only the age sweep may merge it).
  auto a = cdb->create_cursor();
  BOOST_REQUIRE(a->start_transaction());
  a->find(Slice("A_key"));
  a->value(Slice("A_val"));
  BOOST_REQUIRE(a->commit());

  // Producer B: a separate cursor that keeps committing.  Each commit re-enters
  // the age-sweep arming path; under the old debounce this perpetually cancelled
  // A's pending sweep so A never aged out.
  std::atomic<bool> stop{false};
  std::thread b([&] {
    auto bc = cdb->create_cursor();
    int i = 0;
    while (!stop.load(std::memory_order_acquire)) {
      if (bc->start_transaction()) {
        std::string k = "B_" + std::to_string(i++);
        bc->find(Slice(k));
        bc->value(Slice("v"));
        bc->commit();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  // Within ~2x the age limit A's slot must have been merged into main.
  bool merged = false;
  for (int waited = 0; waited < 4000 && !merged; waited += 50) {
    auto m = main_db->create_cursor();
    m->find(Slice("A_key"));
    if (m->is_valid() && m->value() == Slice("A_val"))
      merged = true;
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  stop.store(true, std::memory_order_release);
  b.join();

  BOOST_CHECK_MESSAGE(merged,
      "ATTACHED slot A was never merged into main within 2x the age limit "
      "despite continuous writes to other slots (age-sweep starvation)");
}

// Regression: a long-idle sticky cursor whose ATTACHED slot was age-merged to
// MERGED must NOT have its live pin stolen and the slot recycled out from under
// it.  The old _recover_merged_slots_locked() "steal" did exactly that after a
// 2s grace, freeing the slot to EMPTY while cursor A still cached its pointer.
// A second cursor C would then reclaim the SAME slot, and when A woke its
// CAS(ATTACHED→WRITING) would succeed on C's incarnation — an ABA that let A
// scribble into C's tributary and corrupt C's refcount (lost data / underflow).
//
// With owner-only-unpin, A keeps its pin through the age-merge (slot stays
// MERGED, refs>=1, never recycled), C claims a DIFFERENT slot, and when A wakes
// it reclaims its own slot in place.  This test drives that exact sequence and
// asserts all three keys survive a final flush to main with correct values.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_tributary_idle_owner_pin_not_stolen) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // Stay-attached on commit (only the age sweep may merge), short age limit.
  cdb->set_merge_write_threshold(1000000);
  cdb->set_max_attached_age_ms(1000);

  // Cursor A writes one key and stays ALIVE + idle so its slot stays ATTACHED
  // and ages out.  A keeps its sticky pin the whole time.
  auto a = cdb->create_cursor();
  BOOST_REQUIRE(a->start_transaction());
  a->find(Slice("A_key"));
  a->value(Slice("A_val"));
  BOOST_REQUIRE(a->commit());

  // Let A's slot age past the limit, then force the age-merge: A's slot goes
  // MERGED (data flushed to main) while A still holds its pin.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  cdb->merge_now();

  // Keep the slot MERGED for >2s — the window in which the old steal would have
  // recycled A's slot — and drive merge passes the way steady-state activity
  // would have, so the old _recover_merged_slots_locked() path would fire.
  for (int i = 0; i < 6; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cdb->merge_now();
  }

  // Cursor C claims a slot and commits.  Under the old code A's slot is now
  // EMPTY and C would reuse it; under the fix A's slot is still reserved so C
  // takes a different one.  Either way C's data must survive.
  auto c = cdb->create_cursor();
  BOOST_REQUIRE(c->start_transaction());
  c->find(Slice("C_key"));
  c->value(Slice("C_val"));
  BOOST_REQUIRE(c->commit());

  // A wakes and writes again.  With the fix it reclaims its own (MERGED) slot
  // in place; under the old code its cached pin now aliases C's slot (ABA).
  BOOST_REQUIRE(a->start_transaction());
  a->find(Slice("A_key2"));
  a->value(Slice("A_val2"));
  BOOST_REQUIRE(a->commit());

  // Flush everything to main and verify NO data was lost or corrupted.
  cdb->merge_all_now();
  auto m1 = main_db->create_cursor();
  m1->find(Slice("A_key"));
  BOOST_CHECK_MESSAGE(m1->is_valid() && m1->value() == Slice("A_val"),
                      "A_key/A_val lost after age-merge + reclaim");
  auto m2 = main_db->create_cursor();
  m2->find(Slice("C_key"));
  BOOST_CHECK_MESSAGE(m2->is_valid() && m2->value() == Slice("C_val"),
                      "C_key/C_val lost — A's stale pin corrupted C's slot");
  auto m3 = main_db->create_cursor();
  m3->find(Slice("A_key2"));
  BOOST_CHECK_MESSAGE(m3->is_valid() && m3->value() == Slice("A_val2"),
                      "A_key2/A_val2 lost after in-place slot reclaim");
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
  cdb->set_max_attached_age_ms(0);
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

  // Background merging is always on: when the write cursor above released its
  // slot it transitioned ATTACHED→MERGING and scheduled a merge task on the
  // storage pool.  Quiesce that task before the white-box state surgery below,
  // otherwise the background merge holds _meta->chain_lock while sanitize()
  // re-initialises it via placement-new — a data race that aborts.
  cdb->_main_db._storage.wait_idle();

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
  // sanitize() runs recovery once per storage generation (so concurrent openers
  // don't double-recover); force it to treat the current generation as not-yet
  // recovered, simulating a fresh crash/reopen cycle for this white-box test.
  cdb->_meta->recovered_generation.store(~0ull, std::memory_order_relaxed);
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

  cdb->set_max_attached_age_ms(0);
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

// Bug 2 (concurrent reproducer): the single-threaded
// test_tributary_header_area_not_freed passes, but the YCSB benchmark
// deadlocks at ~op 393 with 8 concurrent writers.  The crucial difference
// from the single-threaded test is that YCSB performs a LOAD phase, then
// CLOSES the DB, then REOPENS it and runs the RUN phase.  This test
// mirrors that pattern: bulk-load with concurrent writers, close, reopen
// (exercising the sanitize() path), then run a mixed concurrent workload.
// A watchdog converts a hang into a test failure so we do not block the
// CI.
//
// Expected behaviour with current code: HANG -> watchdog fails the test.
// Expected behaviour with fix: completes within the budget.
BOOST_AUTO_TEST_CASE(test_tributary_concurrent_writers_no_deadlock) {
  TributaryPreparation p;

  constexpr int kThreads          = 8;
  constexpr int kLoadOpsPerThread = 5000;          // ~40K rows = LOAD phase
  constexpr int kRunOpsPerThread  = 5000;          // RUN phase ops
  constexpr int kValueSize        = 1024;          // 1 KB — YCSB record size
  constexpr int kKeyspace         = 40000;
  constexpr int kStaleSeconds     = 20;            // hang detection window
  constexpr bool kEnableMerger    = false;         // diag: skip merger to isolate

  auto run_phase = [&](CDB& cdb, int ops_per_thread, bool inserts_only,
                       const char* phase_name) -> bool {
    std::atomic<bool> stop{false};
    std::atomic<bool> deadlock_detected{false};
    std::vector<std::atomic<uint64_t>> progress(kThreads);
    for (auto& p : progress) p.store(0, std::memory_order_relaxed);

    // Diagnostic: capture the SpinLock address up-front.
    char* mem_base = (char*)cdb._main_db._storage._memory;
    char* hdr_addr = (char*)&*cdb._main_db._header;
    void* lock_addr = (void*)&cdb._main_db._header->txn_ref_lock;
    std::fprintf(stderr,
        "[%s] mem_base=%p hdr_addr=%p hdr_off=0x%lx lock_addr=%p "
        "lock_off=0x%lx file_size=0x%lx\n",
        phase_name, (void*)mem_base, (void*)hdr_addr,
        (uint64_t)(hdr_addr - mem_base), lock_addr,
        (uint64_t)((char*)lock_addr - mem_base),
        (uint64_t)cdb._main_db._storage._memory->file_size);
    {
      uint8_t* lp = (uint8_t*)lock_addr;
      std::fprintf(stderr, "[%s] PRE-RUN bytes at hdr:\n", phase_name);
      uint8_t* b = (uint8_t*)hdr_addr;
      for (int row = 0; row < 8; ++row) {
        std::fprintf(stderr, "  %p:", (void*)(b + row * 16));
        for (int col = 0; col < 16; ++col)
          std::fprintf(stderr, " %02x", b[row * 16 + col]);
        std::fprintf(stderr, "\n");
      }
      (void)lp;
    }
    std::fflush(stderr);

    auto worker = [&](int tid) {
      std::mt19937 rng(static_cast<uint32_t>(tid * 2654435761u + 0x9e3779b9));
      std::uniform_int_distribution<int> key_dist(0, kKeyspace - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      std::string val(kValueSize, 'A' + (tid % 26));
      for (int i = 0;
           i < ops_per_thread && !stop.load(std::memory_order_relaxed); ++i) {
        int k = key_dist(rng);
        std::string key = "user" + std::to_string(k);
        int op = inserts_only ? 0 : op_dist(rng);
        try {
          if (op < 50 || inserts_only) {
            auto cursor = cdb.create_cursor();
            if (!cursor->start_transaction()) continue;
            cursor->find(Slice(key));
            cursor->value(Slice(val));
            cursor->commit();
          } else if (op < 95) {
            auto cursor = cdb.create_cursor();
            cursor->find(Slice(key));
            (void)cursor->is_valid();
          } else {
            auto cursor = cdb.create_cursor();
            if (!cursor->start_transaction()) continue;
            cursor->find(Slice(key));
            if (cursor->is_valid()) cursor->remove();
            cursor->commit();
          }
        } catch (...) {
        }
        progress[tid].fetch_add(1, std::memory_order_relaxed);
      }
    };

    auto merger = [&]() {
      if (!kEnableMerger) return;
      while (!stop.load(std::memory_order_relaxed)) {
        try {
          cdb.merge_eligible_tributaries();
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) workers.emplace_back(worker, i);
    std::thread merge_thread(merger);

    std::thread watchdog([&]() {
      std::vector<uint64_t> last(kThreads, 0);
      auto last_progress_time = std::chrono::steady_clock::now();
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto now = std::chrono::steady_clock::now();
        bool any_progress = false;
        for (int i = 0; i < kThreads; ++i) {
          uint64_t cur = progress[i].load(std::memory_order_relaxed);
          if (cur != last[i]) {
            last[i] = cur;
            any_progress = true;
          }
        }
        if (any_progress) last_progress_time = now;
        if (now - last_progress_time > std::chrono::seconds(kStaleSeconds)) {
          deadlock_detected.store(true, std::memory_order_relaxed);
          stop.store(true, std::memory_order_relaxed);
          break;
        }
        bool all_done = true;
        for (int i = 0; i < kThreads; ++i) {
          if (progress[i].load(std::memory_order_relaxed) <
              (uint64_t)ops_per_thread) {
            all_done = false;
            break;
          }
        }
        if (all_done) {
          stop.store(true, std::memory_order_relaxed);
          break;
        }
      }
    });

    watchdog.join();

    if (deadlock_detected.load(std::memory_order_relaxed)) {
      std::string detail;
      for (int i = 0; i < kThreads; ++i) {
        detail += " t" + std::to_string(i) + "=" +
                  std::to_string(progress[i].load(std::memory_order_relaxed));
      }
      BOOST_TEST_MESSAGE("Deadlock in phase '" << phase_name << "':" << detail);
      // Dump the SpinLock value and surrounding bytes to detect overwrite.
      {
        uint8_t* lp = (uint8_t*)&cdb._main_db._header->txn_ref_lock;
        uint8_t* base = (uint8_t*)hdr_addr;
        std::fprintf(stderr, "[%s] POST bytes around hdr:\n", phase_name);
        for (int row = 0; row < 8; ++row) {
          std::fprintf(stderr, "  %p:", (void*)(base + row * 16));
          for (int col = 0; col < 16; ++col)
            std::fprintf(stderr, " %02x", base[row * 16 + col]);
          std::fprintf(stderr, "\n");
        }
        (void)lp;
        std::fflush(stderr);
      }
      const char* dump = std::getenv("LEAVES_DEADLOCK_GDB");
      if (dump && *dump && *dump != '0') {
        char cmd[1024];
        std::snprintf(cmd, sizeof(cmd),
            "gdb -batch -p %d "
            "-ex 'set pagination off' "
            "-ex 'thread apply all bt 25' "
            "-ex 'thread 2' "
            "-ex 'frame 3' "
            "-ex 'print/x *_lock' "
            "-ex 'print _lock' "
            "-ex detach 2>&1 | tee /tmp/leaves_deadlock_stacks.txt",
            (int)getpid());
        std::system(cmd);
      }
      const char* abrt = std::getenv("LEAVES_DEADLOCK_ABORT");
      if (abrt && *abrt && *abrt != '0') std::abort();
      for (auto& w : workers) w.detach();
      merge_thread.detach();
      return false;
    }

    for (auto& w : workers) w.join();
    merge_thread.join();
    return true;
  };

  offset_t saved_header{0};
  // ---- LOAD phase: concurrent inserts only, mimics YCSB load.
  {
    auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
    std::unique_ptr<CDB> cdb(cdb_ptr);
    cdb->set_max_attached_age_ms(0);

    bool ok = run_phase(*cdb, kLoadOpsPerThread, /*inserts_only=*/true, "LOAD");
    BOOST_REQUIRE_MESSAGE(ok, "Deadlock during LOAD phase");

    // Capture the file offset of the main DB header before tearing down.
    uint64_t hdr_va  = (uint64_t)main_db->_header;
    uint64_t base_va = (uint64_t)storage->_memory;
    saved_header     = offset_t(hdr_va - base_va);
    // Destructors fire here, closing the DB / file.
  }

  // ---- Reopen and run a mixed concurrent workload (RUN phase).
  {
    auto [storage, main_db, cdb_ptr] = open_cdb(TEST_FILE, saved_header);
    std::unique_ptr<CDB> cdb(cdb_ptr);
    cdb->set_max_attached_age_ms(0);

    bool ok = run_phase(*cdb, kRunOpsPerThread, /*inserts_only=*/false, "RUN");
    BOOST_CHECK_MESSAGE(ok,
        "Deadlock during RUN phase after reopen — Bug 2 reproduced");
  }
}

// ---------------------------------------------------------------------------
// Async merge error propagation
// ---------------------------------------------------------------------------

// Simulates a failed async merge by injecting an exception directly into
// _ConfluenceDB's error slot (as merge_tributary()'s catch block would do),
// then verifies:
//   1. Cursor operations (first(), is_valid(), start_transaction()) do NOT
//      throw; the error is surfaced by explicitly calling get_merge_error().
//   2. get_merge_error() returns and clears the stored exception (one-shot).
//   3. After the error is drained, subsequent cursor accesses succeed normally.
BOOST_AUTO_TEST_CASE(test_merge_error_propagated_to_cursor) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  write_kv(*cdb, "hello", "world");

  auto inject = [&] {
    std::lock_guard<std::mutex> lk(cdb->_merge_error_mutex);
    cdb->_pending_merge_error =
        std::make_exception_ptr(std::runtime_error("StorageFull (injected)"));
    cdb->_has_merge_error.store(true, std::memory_order_release);
  };

  // --- Part 1: error retrievable via get_merge_error() after first() -------
  {
    inject();
    auto cursor = cdb->create_cursor();
    // first() does NOT throw; error is polled separately.
    cursor->first();
    // get_merge_error() returns the pending exception and clears it (one-shot).
    auto e = cdb->get_merge_error();
    BOOST_REQUIRE(e != nullptr);
    BOOST_CHECK_THROW(std::rethrow_exception(e), std::runtime_error);
    // Error is cleared: next first() succeeds and positions on the key.
    cursor->first();
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->key().string(), "hello");
  }

  // --- Part 2: error retrievable after find() + is_valid() ----------------
  {
    inject();
    auto cursor = cdb->create_cursor();
    // find() is lazy; is_valid() triggers materialization — neither throws.
    cursor->find(Slice("hello"));
    cursor->is_valid();
    // get_merge_error() returns and clears the error.
    auto e = cdb->get_merge_error();
    BOOST_REQUIRE(e != nullptr);
    BOOST_CHECK_THROW(std::rethrow_exception(e), std::runtime_error);
    // Error cleared: re-issue find and check it works.
    cursor->find(Slice("hello"));
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value().string(), "world");
  }

  // --- Part 3: error retrievable after start_transaction() ----------------
  {
    inject();
    auto cursor = cdb->create_cursor();
    // start_transaction() does NOT throw; poll the error separately.
    if (cursor->start_transaction()) cursor->rollback();
    auto e = cdb->get_merge_error();
    BOOST_REQUIRE(e != nullptr);
    BOOST_CHECK_THROW(std::rethrow_exception(e), std::runtime_error);
    // Error cleared: start_transaction() succeeds on the next call.
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->rollback();
  }
}

// ---------------------------------------------------------------------------
// Retire-race stress: writers transition slots to MERGING right as the merge
// job retires.  With merge_write_threshold==1 every commit flips its slot to
// MERGING and calls _schedule_merge(), maximally exercising the Dekker
// handshake between a producer's signal+CAS (W1/W2) and the merge job's
// J1 clear / J2 re-acquire.  The invariant under test: no MERGING tributary is
// ever orphaned — after a final drain every committed key is visible in main.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_tributary_retire_race_no_orphan_merging) {
  constexpr int kThreads     = 8;
  constexpr int kOpsPerThread = 4000;
  constexpr int kRounds      = 3;

  for (int round = 0; round < kRounds; ++round) {
    TributaryPreparation p;
    auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
    std::unique_ptr<CDB> cdb(cdb_ptr);
    // Threshold 1: every commit releases its slot into MERGING immediately,
    // so the merge job is constantly retiring while writers re-arm it.
    cdb->set_merge_write_threshold(1);

    std::atomic<bool> go{false};
    // Worker threads MUST NOT call BOOST_* assertion macros: Boost.Test's
    // logger/framework state is not thread-safe and would itself race.  Record
    // failures into atomics and assert on the main thread after join().
    std::atomic<int> start_failures{0};
    std::atomic<int> commit_failures{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
      writers.emplace_back([&, t] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        auto cursor = cdb->create_cursor();
        for (int i = 0; i < kOpsPerThread; ++i) {
          std::string key = std::to_string(t) + "_" + std::to_string(i);
          std::string val = "v" + key;
          if (!cursor->start_transaction()) {
            start_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          cursor->find(Slice(key));
          cursor->value(Slice(val));
          if (!cursor->commit())
            commit_failures.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    go.store(true, std::memory_order_release);
    for (auto& w : writers) w.join();

    BOOST_REQUIRE_EQUAL(start_failures.load(), 0);
    BOOST_REQUIRE_EQUAL(commit_failures.load(), 0);

    // Final drain: every MERGING/ATTACHED slot must be picked up.  If any
    // wakeup was lost, an orphaned MERGING slot would leave its keys invisible.
    cdb->merge_all_now();

    // Verify every committed key is visible in the merged main DB.
    auto cursor = cdb->create_cursor();
    for (int t = 0; t < kThreads; ++t) {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = std::to_string(t) + "_" + std::to_string(i);
        cursor->find(Slice(key));
        BOOST_REQUIRE_MESSAGE(cursor->is_valid(),
            "round " << round << " missing key " << key
                     << " (orphaned MERGING slot?)");
        BOOST_CHECK_EQUAL(cursor->value(), Slice("v" + key));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Read/merge visibility race: a reader that refreshes its snapshot while a
// background merge is committing must never lose a committed key.
//
// The hazard (TOCTOU in _ensure_sources): the reader refreshes its MAIN-DB
// snapshot first, then scans per-slot states.  If a merge commits its data to
// main and flips the slot ATTACHED/MERGING→MERGED in that window, the reader
// skips the tributary (state==MERGED) but its main snapshot predates the merge
// commit — so the just-committed key briefly vanishes from the merged view.
//
// A writer commits keys with threshold=1 (every commit immediately schedules a
// merge), publishing the highest committed index.  A reader continuously looks
// up the latest published key; it must ALWAYS find it.  BOOST_* is not called
// from the reader thread (records the first lost key into atomics instead).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_tributary_read_during_merge_no_loss) {
  TributaryPreparation p;
  auto [storage, main_db, cdb_ptr] = make_cdb(TEST_FILE);
  std::unique_ptr<CDB> cdb(cdb_ptr);

  // threshold=1: each commit transitions its slot to MERGING and schedules a
  // merge, maximizing the read-vs-merge race window.
  cdb->set_merge_write_threshold(1);

  constexpr int kKeys = 20000;
  std::atomic<int> last_committed{-1};
  std::atomic<bool> done{false};
  std::atomic<int> lost_key{-1};  // first key index the reader failed to see

  std::thread reader([&] {
    auto rcursor = cdb->create_cursor();  // long-lived: relies on epoch refresh
    while (!done.load(std::memory_order_acquire)) {
      int j = last_committed.load(std::memory_order_acquire);
      if (j < 0) { std::this_thread::yield(); continue; }
      std::string key = "k" + std::to_string(j);
      rcursor->find(Slice(key));
      if (!rcursor->is_valid()) {
        int expected = -1;
        lost_key.compare_exchange_strong(expected, j);
        break;
      }
    }
  });

  auto wcursor = cdb->create_cursor();
  for (int i = 0; i < kKeys; ++i) {
    std::string key = "k" + std::to_string(i);
    BOOST_REQUIRE(wcursor->start_transaction());
    wcursor->find(Slice(key));
    wcursor->value(Slice("v" + std::to_string(i)));
    BOOST_REQUIRE(wcursor->commit());
    last_committed.store(i, std::memory_order_release);
    if (lost_key.load(std::memory_order_acquire) >= 0) break;
  }
  done.store(true, std::memory_order_release);
  reader.join();

  BOOST_CHECK_MESSAGE(lost_key.load() < 0,
      "reader lost committed key k" << lost_key.load()
          << " during a concurrent merge (snapshot/slot-state TOCTOU)");
}

