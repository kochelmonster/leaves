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
  auto* cdb = new CDB(*main_db, false);
  return {std::move(storage), main_db, cdb};
}

// Helper: open an existing ConfluenceDB
static TestHandle open_cdb(const char* path, offset_t /*header*/) {
  auto storage = std::make_unique<StorageImpl>(path);
  auto* main_db = storage->template open<_DB>("main");
  auto* cdb = new CDB(*main_db, false);
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
    cdb->set_idle_timeout_seconds(0);

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
    cdb->set_idle_timeout_seconds(0);

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
//   1. first() rethrows the exception (eager: calls _ensure_sources() directly).
//   2. find() + is_valid() rethrows the exception (lazy materialization path).
//   3. start_transaction() also rethrows before the error is drained.
//   4. Delivery is one-shot: subsequent accesses succeed normally.
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

  // --- Part 1: first() rethrows then clears --------------------------------
  {
    inject();
    auto cursor = cdb->create_cursor();
    // first() calls _ensure_sources() eagerly → must throw.
    BOOST_CHECK_THROW(cursor->first(), std::runtime_error);
    // Error is cleared: next first() succeeds and positions on the key.
    cursor->first();
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->key().string(), "hello");
  }

  // --- Part 2: find() + is_valid() rethrows then clears -------------------
  {
    inject();
    auto cursor = cdb->create_cursor();
    // find() is lazy (no _ensure_sources()); is_valid() triggers materialization.
    cursor->find(Slice("hello"));
    BOOST_CHECK_THROW(cursor->is_valid(), std::runtime_error);
    // Error cleared: re-issue find and check it works.
    cursor->find(Slice("hello"));
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value().string(), "world");
  }

  // --- Part 3: start_transaction() rethrows then clears -------------------
  {
    inject();
    auto cursor = cdb->create_cursor();
    BOOST_CHECK_THROW(cursor->start_transaction(), std::runtime_error);
    // Error cleared: start_transaction() succeeds on the next call.
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->rollback();
  }
}
