#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE multiprocess
#include <boost/test/included/unit_test.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../include/leaves/intern/multi/_confluence_db.hpp"
#include "../include/leaves/mmap.hpp"
#include "test.hpp"

using namespace leaves;

using StorageImpl = MapStorage::StorageImpl;
using MainDB = _DB<StorageImpl>;
using CDB = _ConfluenceDB<MainDB>;

#if defined(LEAVES_SINGLE_PROCESS)
// Multiprocess tests require cross-process storage primitives; in single-process
// builds the test is a no-op so the suite still reports success.
BOOST_AUTO_TEST_CASE(test_multiprocess_skipped_single_process) {
  BOOST_CHECK(true);
}
#else

static constexpr const char* MP_FILE = "test_mp.lvs";

static Slice mkkey(int i) {
  static char b[32];
  std::snprintf(b, sizeof b, "key%08d", i);
  return Slice(b);
}
static Slice mkval(int i) {
  static char b[32];
  std::snprintf(b, sizeof b, "val%08d", i);
  return Slice(b);
}


struct MpPrep {
  MpPrep() { std::remove(MP_FILE); }
  ~MpPrep() { std::remove(MP_FILE); }
};

// Create the DB file + confluence meta once, in the parent, before forking so
// children only ever reopen (no first-creation race to debug here).
static void create_db() {
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
}

// Fork and run `fn` in the child with a watchdog so a hang FAILS (the child is
// killed by SIGALRM, the parent observes a non-zero exit) rather than blocking
// the whole suite.  Returns the child pid.
static pid_t fork_run(std::function<bool()> fn) {
  std::fflush(nullptr);  // drain inherited stdio buffers so children don't replay them
  pid_t pid = fork();
  if (pid == 0) {
    alarm(30);
    bool ok = false;
    try {
      ok = fn();
    } catch (...) {
      ok = false;
    }
    _exit(ok ? 0 : 1);
  }
  return pid;
}

static bool wait_ok(pid_t pid) {
  int st = 0;
  if (waitpid(pid, &st, 0) != pid) return false;
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return true;
  if (WIFSIGNALED(st))
    std::fprintf(stderr, "[parent] child %d killed by signal %d\n", (int)pid, WTERMSIG(st));
  else if (WIFEXITED(st))
    std::fprintf(stderr, "[parent] child %d exited %d\n", (int)pid, WEXITSTATUS(st));
  return false;
}

// Child body: open the shared DB, write keys [base, base+count) and merge them
// into the main DB.
static bool child_write_range(int base, int count, bool merge) {
  try {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    CDB cdb(*main_db);
    auto cursor = cdb.create_cursor();
    if (!cursor->start_transaction()) return false;
    for (int i = 0; i < count; ++i) {
      cursor->find(Slice(mkkey(base + i)));
      cursor->value(Slice(mkval(base + i)));
    }
    if (!cursor->commit()) return false;
    cursor.reset();
    if (merge) cdb.merge_all_now();
    return true;
  } catch (...) {
    return false;
  }
}

// Verify every key in [base, base+count) is present in the (reopened) DB with
// the expected value.
static void verify_range(CDB& cdb, int base, int count) {
  for (int i = 0; i < count; ++i) {
    auto cursor = cdb.create_cursor();
    cursor->find(Slice(mkkey(base + i)));
    BOOST_REQUIRE_MESSAGE(cursor->is_valid(), "missing key " << mkkey(base + i));
    BOOST_CHECK_EQUAL(cursor->value(), Slice(mkval(base + i)));
  }
}

// ---------------------------------------------------------------------------
// Test 1: N concurrent processes writing disjoint key ranges + merging.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_concurrent_writes) {
  MpPrep p;
  create_db();

  constexpr int kProcs = 4;
  constexpr int kPer = 500;
  std::vector<pid_t> pids;
  for (int c = 0; c < kProcs; ++c)
    pids.push_back(fork_run([c] { return child_write_range(c * kPer, kPer, true); }));

  for (pid_t pid : pids) BOOST_CHECK(wait_ok(pid));

  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
  cdb.merge_all_now();
  verify_range(cdb, 0, kProcs * kPer);
}

// ---------------------------------------------------------------------------
// Test 2: one writer process, one merger process running concurrently.
// Exercises the cross-process merge arbitration without hanging.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_writer_and_merger) {
  MpPrep p;
  create_db();

  constexpr int kKeys = 2000;
  pid_t writer = fork_run([] {
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");
      CDB cdb(*main_db);
      for (int batch = 0; batch < 4; ++batch) {
        auto cursor = cdb.create_cursor();
        if (!cursor->start_transaction()) return false;
        for (int i = 0; i < kKeys / 4; ++i) {
          int idx = batch * (kKeys / 4) + i;
          cursor->find(Slice(mkkey(idx)));
          cursor->value(Slice(mkval(idx)));
        }
        if (!cursor->commit()) return false;
        cursor.reset();
        cdb.merge_all_now();
      }
      return true;
    } catch (...) {
      return false;
    }
  });

  pid_t merger = fork_run([] {
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");
      CDB cdb(*main_db);
      for (int i = 0; i < 200; ++i) cdb.merge_all_now();
      return true;
    } catch (...) {
      return false;
    }
  });

  BOOST_CHECK(wait_ok(writer));
  BOOST_CHECK(wait_ok(merger));

  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
  cdb.merge_all_now();
  verify_range(cdb, 0, kKeys);
}

// ---------------------------------------------------------------------------
// Test 3: a process "crashes" after publishing a MERGING slot. Reopening must
// recover and drain that orphaned slot.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_crash_during_merge) {
  MpPrep p;
  create_db();

  constexpr int kKeys = 300;
  pid_t crasher = fork_run([] {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    auto* cdb = new CDB(*main_db);  // deliberately never destroyed
    auto cursor = cdb->create_cursor();
    if (!cursor->start_transaction()) _exit(1);
    for (int i = 0; i < kKeys; ++i) {
      cursor->find(Slice(mkkey(i)));
      cursor->value(Slice(mkval(i)));
    }
    if (!cursor->commit()) _exit(1);
    // Force the slot that holds the just-committed data into MERGING,
    // simulating a crash after publishing merge work but before the merge job
    // drained it.
    size_t n = cdb->_tributaries_count.load(std::memory_order_acquire);
    bool stamped = false;
    for (size_t i = 0; i < n; ++i) {
      auto* t = cdb->_trib_at(i);
      uint8_t st = t->_header->state.load(std::memory_order_acquire);
      if (st == CDB::Slot::ATTACHED || st == CDB::Slot::WRITING) {
        t->_header->state.store(CDB::Slot::MERGING, std::memory_order_release);
        stamped = true;
      }
    }
    // Crash hard: skip all destructors (no merge, no clean close).
    _exit(stamped ? 0 : 1);
    return true;  // unreachable; satisfies std::function<bool()>
  });

  BOOST_REQUIRE(wait_ok(crasher));

  // Reopen: sanitize()/_merge_unclaimed_tributaries() must drain the orphaned
  // MERGING slot left by the dead process.
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
  cdb.merge_all_now();
  verify_range(cdb, 0, kKeys);
}

// ---------------------------------------------------------------------------
// Test 5: crash during an uncommitted transaction — child writes some keys,
// commits, starts a second transaction, writes more keys, then kills itself
// with SIGINT.  Parent reopens, sanitize() must recover the committed data
// and discard the uncommitted writes.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_sanitize_after_crash_during_transaction) {
  MpPrep p;
  create_db();

  constexpr int kPreCrashKeys = 200;
  constexpr int kCrashKeys = 100;

  pid_t child = fork();
  if (child == 0) {
    alarm(30);
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");
      CDB cdb(*main_db);

      // Transaction 1: committed data that must survive.
      {
        auto cursor = cdb.create_cursor();
        if (!cursor->start_transaction()) _exit(1);
        for (int i = 0; i < kPreCrashKeys; ++i) {
          cursor->find(Slice(mkkey(i)));
          cursor->value(Slice(mkval(i)));
        }
        if (!cursor->commit()) _exit(1);
      }

      // Transaction 2: start writing but crash before commit.
      {
        auto cursor = cdb.create_cursor();
        if (!cursor->start_transaction()) _exit(1);
        for (int i = 0; i < kCrashKeys; ++i) {
          cursor->find(Slice(mkkey(kPreCrashKeys + i)));
          cursor->value(Slice(mkval(kPreCrashKeys + i)));
        }
        // Die mid-transaction — no commit, no rollback.
        std::raise(SIGINT);
      }
      // Should never reach here.
      _exit(1);
    } catch (...) {
      _exit(1);
    }
  }

  // Parent: wait for the child to be killed by SIGINT.
  int st = 0;
  pid_t waited = waitpid(child, &st, 0);
  BOOST_REQUIRE_EQUAL(waited, child);
  BOOST_REQUIRE_MESSAGE(WIFSIGNALED(st),
                        "child should have been killed by a signal");
  BOOST_REQUIRE_EQUAL(WTERMSIG(st), SIGINT);

  // Reopen: sanitize() must recover committed data and discard the
  // uncommitted transaction.
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
  cdb.merge_all_now();

  // All pre-crash keys must be present.
  verify_range(cdb, 0, kPreCrashKeys);

  // Keys written in the interrupted transaction must NOT exist.
  for (int i = 0; i < kCrashKeys; ++i) {
    auto cursor = cdb.create_cursor();
    cursor->find(Slice(mkkey(kPreCrashKeys + i)));
    BOOST_CHECK_MESSAGE(!cursor->is_valid(),
                        "key " << mkkey(kPreCrashKeys + i)
                               << " from interrupted transaction should not exist");
  }
}

// ---------------------------------------------------------------------------
// Test 4: cross-process contention stress — several writers churning slots so
// allocation/recycle is exercised under contention; must make progress without
// deadlock (watchdog-bounded) and end up with every key.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_contention_stress) {
  MpPrep p;
  create_db();

  constexpr int kProcs = 6;
  constexpr int kBatches = 5;
  constexpr int kPer = 200;
  std::vector<pid_t> pids;
  for (int c = 0; c < kProcs; ++c) {
    pids.push_back(fork_run([c] {
      try {
        auto storage = std::make_unique<StorageImpl>(MP_FILE);
        auto* main_db = storage->template open<_DB>("main");
        CDB cdb(*main_db);
        for (int b = 0; b < kBatches; ++b) {
          auto cursor = cdb.create_cursor();
          if (!cursor->start_transaction()) return false;
          int base = (c * kBatches + b) * kPer;
          for (int i = 0; i < kPer; ++i) {
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
    }));
  }

  for (pid_t pid : pids) BOOST_CHECK(wait_ok(pid));

  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");
  CDB cdb(*main_db);
  cdb.merge_all_now();
  verify_range(cdb, 0, kProcs * kBatches * kPer);
}

// ---------------------------------------------------------------------------
// Test 6: crash after corrupting directly-allocated (orphaned) area headers.
// Verifies that recover_areas() re-initialises every orphaned Area, and that
// after recovery the *entire* usable file is exactly partitioned between
// DB-owned areas and the free-area pool — no gaps, no leaks.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_sanitize_recover_lost_areas) {
  MpPrep p;

  // Create the file and a fresh "main" DB entry.
  {
    auto storage = MapStorage::create(MP_FILE);
    auto tdb = storage->open("main");
    auto cursor = tdb.cursor();
    cursor.start_transaction();
    cursor.find(Slice("init"));
    cursor.value(Slice("init"));
    cursor.commit();
  }

  constexpr int kKeys     = 100;   // committed data keys
  constexpr int kCorrupt  = 4;     // number of areas we corrupt

  // -----------------------------------------------------------------------
  //  Child
  // -----------------------------------------------------------------------
  pid_t child = fork();
  if (child == 0) {
    alarm(30);
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");

      // Transaction 1: committed data that must survive.
      {
        auto cursor = main_db->create_cursor();
        if (!cursor->start_transaction()) _exit(1);
        for (int i = 0; i < kKeys; ++i) {
          cursor->find(Slice(mkkey(i)));
          cursor->value(Slice(mkval(i)));
        }
        if (!cursor->commit()) _exit(1);
      }

      // Allocate several areas directly from the storage pool (bypassing
      // the DB cursor).  Their offsets are saved into the DB so the parent
      // can later verify that recover_areas() re-initialised them.
      offset_t corrupt_offsets[kCorrupt];
      {
        auto a1 = storage->alloc_single_area();                      // single
        corrupt_offsets[0] = storage->resolve(a1);
        auto a2 = storage->alloc_single_area();                      // single
        corrupt_offsets[1] = storage->resolve(a2);
        auto a3 = storage->alloc_multi_area(2 * StorageImpl::AREA_SIZE); // 2‑block
        corrupt_offsets[2] = storage->resolve(a3);
        auto a4 = storage->alloc_single_area();                      // single
        corrupt_offsets[3] = storage->resolve(a4);
        if (!a1 || !a2 || !a3 || !a4) _exit(1);
      }

      // Transaction 2: write the corrupt offsets into the DB so the parent
      // can read them back and check that the Area headers were restored.
      {
        auto cursor = main_db->create_cursor();
        if (!cursor->start_transaction()) _exit(1);
        for (int i = 0; i < kCorrupt; ++i) {
          char k[64], v[64];
          std::snprintf(k, sizeof k, "cr_%d_off", i);
          std::snprintf(v, sizeof v, "%llu",
                        (unsigned long long)(uint64_t)corrupt_offsets[i]);
          cursor->find(Slice(k));
          cursor->value(Slice(v));
        }
        if (!cursor->commit()) _exit(1);
      }

      // Corrupt the area headers by zeroing them, then flush so the
      // corrupted state is on disk.
      for (int i = 0; i < kCorrupt; ++i) {
        char* p = (char*)storage->_memory + (uint64_t)corrupt_offsets[i];
        std::memset(p, 0, sizeof(Area));
      }
      storage->flush(true, true);

      // Crash without clean close → sanitize() will see clean_close == 0
      // and call recover_areas().
      std::raise(SIGINT);
      _exit(1);
    } catch (...) {
      _exit(1);
    }
  }

  // -----------------------------------------------------------------------
  //  Parent – wait for child
  // -----------------------------------------------------------------------
  {
    int st = 0;
    pid_t waited = waitpid(child, &st, 0);
    BOOST_REQUIRE_EQUAL(waited, child);
    BOOST_REQUIRE_MESSAGE(WIFSIGNALED(st),
                          "child should have been killed by a signal");
    BOOST_REQUIRE_EQUAL(WTERMSIG(st), SIGINT);
  }

  // -----------------------------------------------------------------------
  //  Reopen – sanitize() → recover_areas() runs here
  // -----------------------------------------------------------------------
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  // ----- 1. Verify committed data survives -------------------------------
  {
    auto cursor = main_db->create_cursor();
    for (int i = 0; i < kKeys; ++i) {
      cursor->find(Slice(mkkey(i)));
      BOOST_REQUIRE_MESSAGE(cursor->is_valid(),
                            "missing committed key " << mkkey(i));
      BOOST_CHECK_EQUAL(cursor->value(), Slice(mkval(i)));
    }
  }

  // ----- 2. Retrieve saved corrupt offsets & verify headers restored -----
  offset_t corrupt_offsets[kCorrupt];
  {
    for (int i = 0; i < kCorrupt; ++i) {
      char k[64];
      std::snprintf(k, sizeof k, "cr_%d_off", i);
      auto cursor = main_db->create_cursor();
      cursor->find(Slice(k));
      BOOST_REQUIRE_MESSAGE(cursor->is_valid(), "missing key " << k);
      corrupt_offsets[i] =
          (offset_t)strtoull(cursor->value().data(), nullptr, 10);
    }
  }

  // ----- 3. Walk *all* areas (DB-owned + free pool) and verify each ------
  //         corrupted offset falls within a recovered area range.
  {
    // Collect [start, end) ranges for every area in the free pool.
    struct Range { uint64_t start, end; };
    std::vector<Range> ranges;

    auto collect = [&](offset_t head) {
      offset_t cur = head;
      while (cur) {
        auto* a = reinterpret_cast<Area*>(
            (char*)storage->_memory + (uint64_t)cur);
        uint64_t s = (uint64_t)cur;
        uint64_t e = s + a->size();
        ranges.push_back({s, e});
        cur = a->next;
      }
    };
    collect(storage->_memory->area_pool.single_areas.get_head());
    collect(storage->_memory->area_pool.multi_areas.get_head());

    for (int i = 0; i < kCorrupt; ++i) {
      uint64_t off = (uint64_t)corrupt_offsets[i];
      bool covered = false;
      for (const auto& r : ranges) {
        if (off >= r.start && off < r.end) {
          covered = true;
          break;
        }
      }
      BOOST_CHECK_MESSAGE(covered,
                          "corrupt#" << i << " offset " << off
                          << " not inside any free-area range"
                          " (recover_areas did not reclaim this block)");
    }
  }

  // ----- 4. Walk *all* areas (DB-owned + free pool) and prove the sum ----
  //         equals the entire usable file (file_size - header region).
  {
    static constexpr uint64_t HEADER_SIZE = 4 * K;
    uint64_t first_area =
        padding(HEADER_SIZE, StorageImpl::AREA_SIZE);
    uint64_t file_sz = storage->_memory->file_size;
    uint64_t usable   = file_sz - first_area;

    BOOST_REQUIRE_MESSAGE(usable % StorageImpl::AREA_SIZE == 0,
                          "usable file space not a multiple of AREA_SIZE");

    // Helper to walk an area chain starting from `head` and sum all sizes.
    auto walk_sum = [&](offset_t head) -> uint64_t {
      uint64_t sum = 0;
      offset_t cur = head;
      while (cur) {
        auto* a = reinterpret_cast<Area*>(
            (char*)storage->_memory + (uint64_t)cur);
        sum += a->size();
        cur = a->next;
      }
      return sum;
    };

    uint64_t db_sum = 0;
    db_sum += walk_sum(main_db->_header->area_list_head_single);
    db_sum += walk_sum(main_db->_header->area_list_head_multi);

    uint64_t free_sum = 0;
    free_sum += walk_sum(storage->_memory->area_pool.single_areas.get_head());
    free_sum += walk_sum(storage->_memory->area_pool.multi_areas.get_head());

    BOOST_CHECK_MESSAGE(db_sum + free_sum == usable,
                        "DB-owned " << db_sum << " + free " << free_sum
                        << " = " << (db_sum + free_sum)
                        << " but usable = " << usable);
  }

  // ----- 5. Write fresh data — final proof the pool is usable ------------
  {
    auto cursor = main_db->create_cursor();
    BOOST_REQUIRE(cursor->start_transaction());
    cursor->find(Slice("recovery_test_key"));
    cursor->value(Slice("recovery_test_value"));
    cursor->commit();
  }
}

// ---------------------------------------------------------------------------
// Test: two-phase commit with crash before commit, recovered via sanitize.
//
// Child: opens a plain _DB (NOT ConfluenceDB), writes keys, calls
//        prepare_commit(true) (synced two-phase prepare), then crashes.
// Parent: reopens, sanitize restores the prepared transaction as active.
//         - Must be inside a transaction state (is_active() == true).
//         - Must NOT be able to start a new transaction (nonblocking fails).
//         - Commits the recovered transaction via DB::commit(0, true).
//         - After commit the data is visible.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_two_phase_commit_crash_before_commit) {
  MpPrep p;

  // Create a plain _DB (no ConfluenceDB).
  {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    // A dummy transaction to properly initialise the DB on disk.
    auto cursor = main_db->create_cursor();
    if (!cursor->start_transaction()) throw std::runtime_error("init txn start");
    cursor->find(Slice("_init"));
    cursor->value(Slice("_init"));
    cursor->commit();
  }
  // storage destroyed → clean_close=1 persisted on disk.

  constexpr int kKeys = 500;

  // -----------------------------------------------------------------------
  //  Child
  // -----------------------------------------------------------------------
  pid_t child = fork();
  if (child == 0) {
    alarm(30);
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");

      auto cursor = main_db->create_cursor();
      if (!cursor->start_transaction()) _exit(1);

      for (int i = 0; i < kKeys; ++i) {
        cursor->find(Slice(mkkey(i)));
        cursor->value(Slice(mkval(i)));
      }

      // Two-phase prepare: sync-flush the prepared transaction to disk,
      // then crash *before* the commit that would advance read_txn.
      cursor->prepare_commit(true);

      // Crash without finalising the commit.
      std::raise(SIGINT);
      _exit(1);
    } catch (...) {
      _exit(1);
    }
  }

  // -----------------------------------------------------------------------
  //  Parent – wait for child
  // -----------------------------------------------------------------------
  {
    int st = 0;
    pid_t waited = waitpid(child, &st, 0);
    BOOST_REQUIRE_EQUAL(waited, child);
    BOOST_REQUIRE_MESSAGE(WIFSIGNALED(st),
                          "child should have been killed by a signal");
    BOOST_REQUIRE_EQUAL(WTERMSIG(st), SIGINT);
  }

  // -----------------------------------------------------------------------
  //  Reopen – sanitize() restores the prepared transaction as active
  // -----------------------------------------------------------------------
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  // ----- 1. Must be inside a transaction state ---------------------------
  BOOST_REQUIRE_MESSAGE(main_db->is_active(),
                        "DB must be in an active transaction state after "
                        "recovering a prepared txn");
  BOOST_REQUIRE_NE(main_db->transaction_active(), tid_t(0));

  // ----- 2. Cannot begin a new transaction (non-blocking) -----------------
  {
    auto cursor = main_db->create_cursor();
    // nonblocking=true → try_lock; the lock is held by the recovered txn so
    // this must fail.
    BOOST_REQUIRE_MESSAGE(!cursor->start_transaction(true),
                          "must NOT be able to start a new transaction while "
                          "the recovered prepared txn is active");
  }

  // ----- 3. Commit the recovered prepared transaction --------------------
  // DB::commit(0, true) uses cursor_id=0 which matches txn_cursor_id=0
  // (set by sanitize).  prepare_commit sees prepared_txn != read_txn and
  // returns the existing txn_id, then commit atomically advances read_txn.
  BOOST_REQUIRE_MESSAGE(main_db->commit(0, true),
                        "commit of the recovered prepared transaction failed");

  // ----- 5. After commit the data must be readable via normal cursors ----
  for (int i = 0; i < kKeys; ++i) {
    auto cursor = main_db->create_cursor();
    cursor->find(Slice(mkkey(i)));
    BOOST_REQUIRE_MESSAGE(cursor->is_valid(),
                          "missing committed key " << mkkey(i));
    BOOST_CHECK_EQUAL(cursor->value(), Slice(mkval(i)));
  }
}

// ---------------------------------------------------------------------------
// Test: WAL crash before commit — child starts a transaction with
//       use_wal=true, writes keys, calls prepare_commit() (which writes a
//       PREPARE record to the WAL and syncs), then crashes before the final
//       COMMIT.  Parent reopens, wal_recover() finds the dangling prepared
//       transaction in the WAL, replays it, and sanitize() restores it as the
//       active write transaction.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_mp_wal_crash_before_commit) {
  MpPrep p;

  // Create a plain _DB (no ConfluenceDB).
  {
    auto storage = std::make_unique<StorageImpl>(MP_FILE);
    auto* main_db = storage->template open<_DB>("main");
    // A dummy transaction to properly initialise the DB on disk.
    auto cursor = main_db->create_cursor();
    if (!cursor->start_transaction()) throw std::runtime_error("init txn start");
    cursor->find(Slice("_init"));
    cursor->value(Slice("_init"));
    cursor->commit();
  }
  // storage destroyed → clean_close=1 persisted on disk.

  constexpr int kKeys = 500;

  // -----------------------------------------------------------------------
  //  Child
  // -----------------------------------------------------------------------
  pid_t child = fork();
  if (child == 0) {
    alarm(30);
    try {
      auto storage = std::make_unique<StorageImpl>(MP_FILE);
      auto* main_db = storage->template open<_DB>("main");

      auto cursor = main_db->create_cursor();
      // use_wal=true — operations go through the write-ahead log.
      if (!cursor->start_transaction(false, true)) _exit(1);

      for (int i = 0; i < kKeys; ++i) {
        cursor->find(Slice(mkkey(i)));
        cursor->value(Slice(mkval(i)));
      }

      // WAL prepare: syncs the PREPARE record to the WAL file. The
      // transaction is now prepared on disk but NOT yet committed.
      cursor->prepare_commit();

      // Crash before the WAL COMMIT record is written.
      std::raise(SIGINT);
      _exit(1);
    } catch (...) {
      _exit(1);
    }
  }

  // -----------------------------------------------------------------------
  //  Parent – wait for child
  // -----------------------------------------------------------------------
  {
    int st = 0;
    pid_t waited = waitpid(child, &st, 0);
    BOOST_REQUIRE_EQUAL(waited, child);
    BOOST_REQUIRE_MESSAGE(WIFSIGNALED(st),
                          "child should have been killed by a signal");
    BOOST_REQUIRE_EQUAL(WTERMSIG(st), SIGINT);
  }

  // -----------------------------------------------------------------------
  //  Reopen – sanitize() → wal_recover() replays the dangling WAL txn
  // -----------------------------------------------------------------------
  auto storage = std::make_unique<StorageImpl>(MP_FILE);
  auto* main_db = storage->template open<_DB>("main");

  // ----- 1. Must be inside a transaction state ---------------------------
  BOOST_REQUIRE_MESSAGE(main_db->is_active(),
                        "DB must be in an active transaction state after "
                        "recovering a prepared WAL transaction");
  BOOST_REQUIRE_NE(main_db->transaction_active(), tid_t(0));

  // ----- 2. Cannot begin a new transaction (non-blocking) -----------------
  {
    auto cursor = main_db->create_cursor();
    // nonblocking=true → try_lock; the lock is held by the recovered txn so
    // this must fail.
    BOOST_REQUIRE_MESSAGE(!cursor->start_transaction(true),
                          "must NOT be able to start a new transaction while "
                          "the recovered prepared txn is active");
  }

  // ----- 3. Commit the recovered prepared transaction --------------------
  // DB::commit(0, true) uses cursor_id=0 which matches txn_cursor_id=0
  // (set by sanitize).  prepare_commit sees prepared_txn != read_txn and
  // returns the existing txn_id, then commit atomically advances read_txn.
  BOOST_REQUIRE_MESSAGE(main_db->commit(0, true),
                        "commit of the recovered prepared transaction failed");

  // ----- 4. After commit the data must be readable via normal cursors ----
  for (int i = 0; i < kKeys; ++i) {
    auto cursor = main_db->create_cursor();
    cursor->find(Slice(mkkey(i)));
    BOOST_REQUIRE_MESSAGE(cursor->is_valid(),
                          "missing committed key " << mkkey(i));
    BOOST_CHECK_EQUAL(cursor->value(), Slice(mkval(i)));
  }
}

#endif  // LEAVES_SINGLE_PROCESS
