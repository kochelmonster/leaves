#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE WalTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "leaves/fstore.hpp"
#include "leaves/intern/storage/_wal.hpp"

using namespace leaves;

namespace {

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_wal";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }
  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;

  std::string db_path() const { return (tempDir / "wal_test.lvs").string(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Low-level _WalWriter + wal_parse round-trip.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_parse_roundtrip) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "rt").string();

  {
    _WalWriter w;
    BOOST_REQUIRE(w.open(base));

    w.begin(1);
    w.put(Slice("a"), Slice("1"));
    w.put(Slice("b"), Slice("2"));
    w.prepare();
    w.commit();

    w.begin(2);
    w.del(Slice("a"));
    w.prepare();
    w.commit();

    w.close();
  }

  // Everything was written to file index 0 (next_log starts at 0).
  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 2u);

  BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
  BOOST_REQUIRE_EQUAL(txns[0].ops.size(), 2u);
  BOOST_CHECK(!txns[0].ops[0].is_delete);
  BOOST_CHECK_EQUAL(txns[0].ops[0].key, "a");
  BOOST_CHECK_EQUAL(txns[0].ops[0].val, "1");
  BOOST_CHECK_EQUAL(txns[0].ops[1].key, "b");
  BOOST_CHECK_EQUAL(txns[0].ops[1].val, "2");

  BOOST_CHECK_EQUAL(txns[1].txn_id, 2u);
  BOOST_REQUIRE_EQUAL(txns[1].ops.size(), 1u);
  BOOST_CHECK(txns[1].ops[0].is_delete);
  BOOST_CHECK_EQUAL(txns[1].ops[0].key, "a");

  // The other file must be empty (only the magic header).
  std::vector<_WalTxn> other;
  wal_parse(base + ".wal.1", other);
  BOOST_CHECK(other.empty());
}

// ---------------------------------------------------------------------------
// A transaction without a COMMIT record must be skipped by wal_parse.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_incomplete_transaction_skipped) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "incomplete").string();

  {
    _WalWriter w;
    BOOST_REQUIRE(w.open(base));

    w.begin(1);
    w.put(Slice("k"), Slice("v"));
    w.prepare();
    w.commit();

    // Prepared but never committed (simulated crash during checkpoint).
    w.begin(2);
    w.put(Slice("hanging"), Slice("x"));
    w.prepare();
    // no commit

    w.close();
  }

  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
}

// ---------------------------------------------------------------------------
// truncate() resets a file back to just the magic header.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_truncate_clears_file) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "trunc").string();

  _WalWriter w;
  BOOST_REQUIRE(w.open(base));
  w.begin(1);
  w.put(Slice("a"), Slice("1"));
  w.prepare();
  w.commit();

  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  w.truncate(0);
  txns.clear();
  wal_parse(base + ".wal.0", txns);
  BOOST_CHECK(txns.empty());
  w.close();
}

// ---------------------------------------------------------------------------
// End-to-end: a WAL-enabled transaction is durable and readable.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_commit_and_read_back) {
  DirPreparation dir;
  std::string path = dir.db_path();

  auto storage = FileStorage::create(path.c_str());
  auto db = storage->open("kv");
  auto cursor = db.cursor();

  BOOST_REQUIRE(cursor.start_transaction(false, true));  // use_wal = true
  cursor.find("key1");
  cursor.value("val1");
  cursor.find("key2");
  cursor.value("val2");
  cursor.commit();

  cursor.find("key1");
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value().string(), "val1");
  cursor.find("key2");
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value().string(), "val2");
}

// ---------------------------------------------------------------------------
// Recovery: committed transactions found in the WAL files at open time are
// replayed into the main DB; incomplete transactions are ignored.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_recovery_replays_committed) {
  DirPreparation dir;
  std::string path = dir.db_path();

  // Pre-populate the WAL file pair that the DB "recoverdb" will look for.
  // wal_base_path() == filename() + "." + db_name == path + ".recoverdb".
  std::string base = path + ".recoverdb";
  {
    _WalWriter w;
    BOOST_REQUIRE(w.open(base));

    w.begin(1);
    w.put(Slice("a"), Slice("1"));
    w.put(Slice("b"), Slice("2"));
    w.prepare();
    w.commit();

    w.begin(2);
    w.put(Slice("c"), Slice("3"));
    w.del(Slice("a"));
    w.prepare();
    w.commit();

    // Incomplete — must NOT be replayed.
    w.begin(3);
    w.put(Slice("d"), Slice("4"));
    w.prepare();

    w.close();
  }

  auto storage = FileStorage::create(path.c_str());
  auto db = storage->open("recoverdb");
  auto cursor = db.cursor();

  // Enabling WAL triggers open_wal() -> recover_wal().  Roll the (empty) user
  // transaction back so it doesn't append fresh records to the WAL files,
  // leaving them in the truncated state recovery produced.
  BOOST_REQUIRE(cursor.start_transaction(false, true));
  cursor.rollback();

  cursor.find("a");
  BOOST_CHECK(!cursor.is_valid());  // inserted in txn1, deleted in txn2
  cursor.find("b");
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value().string(), "2");
  cursor.find("c");
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value().string(), "3");
  cursor.find("d");
  BOOST_CHECK(!cursor.is_valid());  // incomplete txn3 skipped

  // After recovery both WAL files are reset to empty.
  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_CHECK(txns.empty());
  wal_parse(base + ".wal.0", txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// The storage-wide checkpoint thread flushes the main DB and eventually
// truncates the WAL files (ping-pong), leaving committed data in the DB.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// flushed() truncates the files whose last_commit <= txn_id, resetting
// _next_log (ping-pong buffer switch).  Verify both files are emptied and
// the next transaction lands in the other file.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_flushed_buffer_switch) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "fs").string();

  {
    _WalWriter w;
    BOOST_REQUIRE(w.open(base));

    // --- Phase 1: Txn 1 commits to file 0 ---
    BOOST_CHECK_EQUAL(w._next_log.load(), 0);
    w.begin(1);
    BOOST_CHECK_EQUAL(w._active_log.load(), 0);
    w.put(Slice("a"), Slice("1"));
    w.prepare();
    w.commit();
    BOOST_CHECK_EQUAL(w._last_commit[0].load(), 1u);

    // --- Phase 2: async flushed(1) is submitted but NOT yet executed.
    // Meanwhile a new transaction starts: begin() pins _active_log
    // (still 0, next_log not yet switched) and bumps _last_commit[0] to 2.
    w.begin(2);
    BOOST_CHECK_EQUAL(w._active_log.load(), 0);
    w.put(Slice("b"), Slice("2"));
    w.prepare();
    w.commit();
    BOOST_CHECK_EQUAL(w._last_commit[0].load(), 2u);

    // --- Phase 3: Now flushed(1) runs (async task catches up).
    // flushed() only touches the NON-active file: idx = 1 - _active_log = 1.
    // _last_commit[1]=0 <= 1 → file 1 is truncated, _next_log → 1.
    w.flushed(1);

    // File 0 (active) survived intact — never touched by flushed()
    BOOST_CHECK_EQUAL(w._last_commit[0].load(), 2u);
    BOOST_CHECK_GT(w._write_off[0], WAL_HEADER_SIZE);
    // File 1 was truncated
    BOOST_CHECK_EQUAL(w._last_commit[1].load(), 0u);
    BOOST_CHECK_EQUAL(w._write_off[1], WAL_HEADER_SIZE);
    // _next_log flipped to the non-active file: 1
    BOOST_CHECK_EQUAL(w._next_log.load(), 1);

    // --- Phase 4: Txn 3 starts → uses file 1 (next_log was set to 1) ---
    w.begin(3);
    BOOST_CHECK_EQUAL(w._active_log.load(), 1);
    w.put(Slice("c"), Slice("3"));
    w.prepare();
    w.commit();
    BOOST_CHECK_EQUAL(w._last_commit[1].load(), 3u);

    // --- Phase 5: flushed(3) truncates the non-active file (idx = 1-1 = 0).
    // _last_commit[0]=2 <= 3 → file 0 truncated, _next_log → 0.
    w.flushed(3);
    BOOST_CHECK_EQUAL(w._last_commit[0].load(), 0u);
    BOOST_CHECK_EQUAL(w._last_commit[1].load(), 3u);  // active file preserved
    BOOST_CHECK_EQUAL(w._write_off[0], WAL_HEADER_SIZE);
    BOOST_CHECK_GT(w._write_off[1], WAL_HEADER_SIZE);  // still has data
    BOOST_CHECK_EQUAL(w._next_log.load(), 0);

    // --- Phase 6: Txn 4 starts → uses file 0 (next_log is 0) ---
    w.begin(4);
    BOOST_CHECK_EQUAL(w._active_log.load(), 0);
    w.put(Slice("d"), Slice("4"));
    w.prepare();
    w.commit();
    BOOST_CHECK_EQUAL(w._last_commit[0].load(), 4u);

    // --- Phase 7: flushed(4) clears the non-active file (idx = 1-0 = 1).
    // _last_commit[1]=3 <= 4 → file 1 truncated.
    w.flushed(4);
    BOOST_CHECK_EQUAL(w._last_commit[1].load(), 0u);
    BOOST_CHECK_EQUAL(w._write_off[1], WAL_HEADER_SIZE);
    // _next_log → 1
    BOOST_CHECK_EQUAL(w._next_log.load(), 1);

    w.close();
  }

  // After final flushed(4): file 0 still has active txn 4, file 1 is empty
  std::vector<_WalTxn> txn0;
  std::vector<_WalTxn> txn1;
  wal_parse(base + ".wal.0", txn0);
  wal_parse(base + ".wal.1", txn1);
  BOOST_REQUIRE_EQUAL(txn0.size(), 1u);
  BOOST_CHECK_EQUAL(txn0[0].txn_id, 4u);
  BOOST_CHECK(txn1.empty());
}

BOOST_AUTO_TEST_CASE(wal_background_checkpoint_truncates) {
  DirPreparation dir;
  std::string path = dir.db_path();
  std::string base = path + ".cp";

  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("cp");
    auto cursor = db.cursor();

    BOOST_REQUIRE(cursor.start_transaction(false, true));
    cursor.find("x");
    cursor.value("y");
    cursor.commit();

    // Give the background checkpoint thread (interval ~200ms) time to run.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    cursor.find("x");
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value().string(), "y");
  }

  // After a clean close the committed value survives a reopen.
  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("cp");
    auto cursor = db.cursor();
    cursor.find("x");
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value().string(), "y");
  }
}
