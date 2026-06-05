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

static std::string mkkey(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "key%08d", i);
  return b;
}
static std::string mkval(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "val%08d", i);
  return b;
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

#endif  // LEAVES_SINGLE_PROCESS
