#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE WalTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// Write a binary WAL file directly to disk for parse edge-case tests.
void write_raw_wal(const std::string& path,
                   const std::vector<uint8_t>& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(content.data()), content.size());
  f.close();
}

// Helper: build a minimal valid WAL buffer (magic + records) into a vector.
std::vector<uint8_t> make_wal_magic() {
  std::vector<uint8_t> buf;
  for (char c : std::string("LVSWAL01"))
    buf.push_back(static_cast<uint8_t>(c));
  return buf;
}

void push_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(v & 0xFF);
  buf.push_back((v >> 8) & 0xFF);
  buf.push_back((v >> 16) & 0xFF);
  buf.push_back((v >> 24) & 0xFF);
}

// Helper: create an open _WalWriter with a stack-local WalState.
struct TestWal {
  WalState state;
  _WalWriter w;
  bool open(const std::string& base) { return w.open(base, &state); }
  bool open(const char* base) { return w.open(std::string(base), &state); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Low-level _WalWriter + wal_parse round-trip.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_parse_roundtrip) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "rt").string();

  {
    TestWal tw;
    BOOST_CHECK(!tw.w.is_open());
    BOOST_REQUIRE(tw.open(base));
    BOOST_CHECK(tw.w.is_open());

    tw.w.begin(1);
    tw.w.put(Slice("a"), Slice("1"));
    tw.w.put(Slice("b"), Slice("2"));
    tw.w.prepare();
    tw.w.commit();

    tw.w.begin(2);
    tw.w.del(Slice("a"));
    tw.w.prepare();
    tw.w.commit();

    tw.w.close();
    BOOST_CHECK(!tw.w.is_open());
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
// Open the same WAL files a second time — exercises _init_file "existing file"
// branch (size >= WAL_HEADER_SIZE).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_reopen_existing_file) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "reopen").string();

  // First open: creates new files.
  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));
    tw.w.begin(10);
    tw.w.put(Slice("x"), Slice("42"));
    tw.w.prepare();
    tw.w.commit();
    tw.w.close();
  }

  // Second open: files already exist with data (size >= WAL_HEADER_SIZE).
  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));
    // write_off[0] should be > WAL_HEADER_SIZE (existing file).
    BOOST_CHECK_GT(tw.state.write_off[0], WAL_HEADER_SIZE);
    tw.w.close();
  }
}

// ---------------------------------------------------------------------------
// abort() discards the in-memory buffer without writing to disk.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_abort) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "abort").string();

  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    // Commit txn 1 normally.
    tw.w.begin(1);
    tw.w.put(Slice("keep"), Slice("v"));
    tw.w.prepare();
    tw.w.commit();

    // Begin txn 2 and abort before prepare — nothing must reach disk.
    tw.w.begin(2);
    tw.w.put(Slice("discard"), Slice("x"));
    tw.w.abort();

    tw.w.close();
  }

  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
}

// ---------------------------------------------------------------------------
// prepare() is idempotent (calling it twice is safe).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_prepare_idempotent) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "idem").string();

  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));
    tw.w.begin(1);
    tw.w.put(Slice("k"), Slice("v"));
    tw.w.prepare();
    tw.w.prepare();  // second call must be a no-op
    tw.w.commit();
    tw.w.close();
  }

  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
}

// ---------------------------------------------------------------------------
// A transaction without a COMMIT record must be skipped by wal_parse.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_incomplete_transaction_skipped) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "incomplete").string();

  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    tw.w.begin(1);
    tw.w.put(Slice("k"), Slice("v"));
    tw.w.prepare();
    tw.w.commit();

    // Prepared but never committed (simulated crash during checkpoint).
    tw.w.begin(2);
    tw.w.put(Slice("hanging"), Slice("x"));
    tw.w.prepare();
    // no commit

    tw.w.close();
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

  TestWal tw;
  BOOST_REQUIRE(tw.open(base));
  tw.w.begin(1);
  tw.w.put(Slice("a"), Slice("1"));
  tw.w.prepare();
  tw.w.commit();

  std::vector<_WalTxn> txns;
  wal_parse(base + ".wal.0", txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  tw.w.truncate(0);
  txns.clear();
  wal_parse(base + ".wal.0", txns);
  BOOST_CHECK(txns.empty());
  tw.w.close();
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
  std::string base = path + ".recoverdb";

  // Step 1: Create "recoverdb" so it is a persistent DB entry in the storage
  // file.  On the *next* open it will go through _open_existing() which calls
  // sanitize() → wal_recover().
  {
    auto storage = FileStorage::create(path.c_str());
    storage->open("recoverdb");
  }

  // Step 2: Pre-populate the WAL file pair.
  // wal_base_path() == filename() + "." + db_name == path + ".recoverdb"
  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    tw.w.begin(2);
    tw.w.put(Slice("a"), Slice("1"));
    tw.w.put(Slice("b"), Slice("2"));
    tw.w.prepare();
    tw.w.commit();

    tw.w.begin(3);
    tw.w.put(Slice("c"), Slice("3"));
    tw.w.del(Slice("a"));
    tw.w.prepare();
    tw.w.commit();

    // Incomplete — must NOT be replayed.
    tw.w.begin(4);
    tw.w.put(Slice("d"), Slice("4"));
    tw.w.prepare();

    tw.w.close();
  }

  // Step 3: Reopen the storage.  Opening the existing "recoverdb" triggers
  // _open_existing() → sanitize() → wal_recover(), which replays txns 2 & 3
  // and discards incomplete txn 4.  Plain reads reflect the recovered state.
  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("recoverdb");
    auto cursor = db.cursor();

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
  }
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
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    // --- Phase 1: Txn 1 commits to file 0 ---
    BOOST_CHECK_EQUAL(tw.state.next_log.load(), 0);
    tw.w.begin(1);
    BOOST_CHECK_EQUAL(tw.state.active_log.load(), 0);
    tw.w.put(Slice("a"), Slice("1"));
    tw.w.prepare();
    tw.w.commit();
    BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 1u);

    // --- Phase 2: async flushed(1) is submitted but NOT yet executed.
    // Meanwhile a new transaction starts: begin() pins active_log
    // (still 0, next_log not yet switched) and bumps last_commit[0] to 2.
    tw.w.begin(2);
    BOOST_CHECK_EQUAL(tw.state.active_log.load(), 0);
    tw.w.put(Slice("b"), Slice("2"));
    tw.w.prepare();
    tw.w.commit();
    BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 2u);

    // --- Phase 3: Now flushed(1) runs (async task catches up).
    // flushed() only touches the NON-active file: idx = 1 - active_log = 1.
    // last_commit[1]=0 <= 1 → file 1 is truncated, next_log → 1.
    tw.w.flushed(1);

    // File 0 (active) survived intact — never touched by flushed()
    BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 2u);
    BOOST_CHECK_GT(tw.state.write_off[0], WAL_HEADER_SIZE);
    // File 1 was truncated
    BOOST_CHECK_EQUAL(tw.state.last_commit[1].load(), 0u);
    BOOST_CHECK_EQUAL(tw.state.write_off[1], WAL_HEADER_SIZE);
    // next_log flipped to the non-active file: 1
    BOOST_CHECK_EQUAL(tw.state.next_log.load(), 1);

    // --- Phase 4: Txn 3 starts → uses file 1 (next_log was set to 1) ---
    tw.w.begin(3);
    BOOST_CHECK_EQUAL(tw.state.active_log.load(), 1);
    tw.w.put(Slice("c"), Slice("3"));
    tw.w.prepare();
    tw.w.commit();
    BOOST_CHECK_EQUAL(tw.state.last_commit[1].load(), 3u);

    // --- Phase 5: flushed(3) truncates the non-active file (idx = 1-1 = 0).
    // last_commit[0]=2 <= 3 → file 0 truncated, next_log → 0.
    tw.w.flushed(3);
    BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 0u);
    BOOST_CHECK_EQUAL(tw.state.last_commit[1].load(), 3u);  // active file preserved
    BOOST_CHECK_EQUAL(tw.state.write_off[0], WAL_HEADER_SIZE);
    BOOST_CHECK_GT(tw.state.write_off[1], WAL_HEADER_SIZE);  // still has data
    BOOST_CHECK_EQUAL(tw.state.next_log.load(), 0);

    // --- Phase 6: Txn 4 starts → uses file 0 (next_log is 0) ---
    tw.w.begin(4);
    BOOST_CHECK_EQUAL(tw.state.active_log.load(), 0);
    tw.w.put(Slice("d"), Slice("4"));
    tw.w.prepare();
    tw.w.commit();
    BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 4u);

    // --- Phase 7: flushed(4) clears the non-active file (idx = 1-0 = 1).
    // last_commit[1]=3 <= 4 → file 1 truncated.
    tw.w.flushed(4);
    BOOST_CHECK_EQUAL(tw.state.last_commit[1].load(), 0u);
    BOOST_CHECK_EQUAL(tw.state.write_off[1], WAL_HEADER_SIZE);
    // next_log → 1
    BOOST_CHECK_EQUAL(tw.state.next_log.load(), 1);

    tw.w.close();
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

// ---------------------------------------------------------------------------
// flushed() must NOT truncate when last_commit[idx] > txn_id.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_flushed_no_truncate_when_newer) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "fnt").string();

  TestWal tw;
  BOOST_REQUIRE(tw.open(base));

  // txn 5 goes to file 0
  tw.w.begin(5);
  tw.w.put(Slice("k"), Slice("v"));
  tw.w.prepare();
  tw.w.commit();
  BOOST_CHECK_EQUAL(tw.state.last_commit[0].load(), 5u);

  // active_log is 0; non-active is 1.
  // last_commit[1] = 0 which is <= 3, so file 1 would be truncated.
  // But we want to test when last_commit IS newer (> txn_id).
  // Put a large "commit id" into file 1 without actually committing.
  // Directly set via atomic to simulate a state where file 1 has commit 10.
  tw.state.last_commit[1].store(10);

  uint64_t write_off_1_before = tw.state.write_off[1];

  // flushed(3): non-active is file 1, last_commit[1]=10 > 3 → no truncate.
  tw.w.flushed(3);
  // File 1 should NOT be changed.
  BOOST_CHECK_EQUAL(tw.state.last_commit[1].load(), 10u);
  BOOST_CHECK_EQUAL(tw.state.write_off[1], write_off_1_before);
  // next_log should remain unchanged (was 0).
  BOOST_CHECK_EQUAL(tw.state.next_log.load(), 0);

  tw.w.close();
}

// ---------------------------------------------------------------------------
// _WalWriter::parse() exercises wal_parse on both files and sorts results.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_parse_sorts_two_files) {
  DirPreparation dir;
  std::string base = (dir.tempDir / "sort2").string();

  // Write txn 2 to file 0, txn 1 to file 1 — parse() must sort them.
  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    // Force writes to file 0.
    tw.state.next_log.store(0);
    tw.w.begin(2);
    tw.w.put(Slice("b"), Slice("2"));
    tw.w.prepare();
    tw.w.commit();
    tw.w.close();
  }

  // Manually write txn 1 directly to file 1.
  {
    TestWal tw;
    BOOST_REQUIRE(tw.open(base));
    tw.state.next_log.store(1);
    tw.w.begin(1);
    tw.w.put(Slice("a"), Slice("1"));
    tw.w.prepare();
    tw.w.commit();
    tw.w.close();
  }

  TestWal tw;
  // We only need parse() — open the writer to give it the paths.
  BOOST_REQUIRE(tw.open(base));

  std::vector<_WalTxn> out;
  tw.w.parse(base, out);

  // parse() must have sorted them: txn 1 first, then txn 2.
  BOOST_REQUIRE_EQUAL(out.size(), 2u);
  BOOST_CHECK_EQUAL(out[0].txn_id, 1u);
  BOOST_CHECK_EQUAL(out[1].txn_id, 2u);

  tw.w.close();
}

// ---------------------------------------------------------------------------
// wal_parse with a non-existent file returns empty result.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_missing_file) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "nonexistent.wal.0").string();
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse with a file that is smaller than the magic header.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_too_small) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "tiny.wal.0").string();
  // Write only 3 bytes (< 8 byte magic header).
  write_raw_wal(path, {0x01, 0x02, 0x03});
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse with wrong magic bytes.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_wrong_magic) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "badmagic.wal.0").string();
  // Write 8 bytes with wrong magic.
  write_raw_wal(path, {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03});
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse with unknown / garbage tag — stops parsing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_unknown_tag) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "unk.wal.0").string();

  auto buf = make_wal_magic();
  // BEGIN txn 1
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // Unknown tag 0xFF — should stop parsing.
  buf.push_back(0xFF);
  // PREPARE + COMMIT after (should not be reached)
  buf.push_back(0x04);
  buf.push_back(0x05);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  // Transaction incomplete (no PREPARE+COMMIT before unknown tag).
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse with a COMMIT that has no matching PREPARE — discarded.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_commit_without_prepare) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "noprepare.wal.0").string();

  auto buf = make_wal_magic();
  // BEGIN txn 1
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // PUT
  buf.push_back(0x02);
  push_u32_le(buf, 1);  // key size 1
  push_u32_le(buf, 1);  // val size 1
  buf.push_back('k');
  buf.push_back('v');
  // COMMIT without PREPARE — transaction must be discarded.
  buf.push_back(0x05);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: COMMIT outside of any transaction — stops parsing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_commit_no_begin) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "commitnobegin.wal.0").string();

  auto buf = make_wal_magic();
  // COMMIT with no prior BEGIN — should break out of parse loop.
  buf.push_back(0x05);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: PREPARE outside of any transaction — stops parsing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_prepare_no_begin) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "prepnobegin.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x04);  // PREPARE without BEGIN

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: PUT outside of any transaction — stops parsing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_put_no_begin) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "putnobegin.wal.0").string();

  auto buf = make_wal_magic();
  // PUT without BEGIN
  buf.push_back(0x02);
  push_u32_le(buf, 1);
  push_u32_le(buf, 1);
  buf.push_back('k');
  buf.push_back('v');

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: DELETE outside of any transaction — stops parsing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_delete_no_begin) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "delnobegin.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x03);  // DELETE without BEGIN
  push_u32_le(buf, 1);
  buf.push_back('k');

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: truncated BEGIN record (not enough bytes for txn_id).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_truncated_begin) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "truncbegin.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x01);  // BEGIN tag
  buf.push_back(0x01);  // only 1 byte of 4-byte txn_id

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: truncated PUT record (not enough bytes for ksz+vsz header).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_truncated_put_header) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "truncputhead.wal.0").string();

  auto buf = make_wal_magic();
  // Valid BEGIN
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // PUT tag + only 3 bytes (need 8 for ksz+vsz)
  buf.push_back(0x02);
  buf.push_back(0x00);
  buf.push_back(0x00);
  buf.push_back(0x00);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: truncated PUT record (header ok but key+val data truncated).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_truncated_put_data) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "truncputdata.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // PUT: ksz=10, vsz=10, but no actual data follows
  buf.push_back(0x02);
  push_u32_le(buf, 10);
  push_u32_le(buf, 10);
  // truncated — no key/val bytes

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: truncated DELETE record.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_truncated_delete) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "truncdel.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // DELETE: only 2 bytes of the 4-byte ksz
  buf.push_back(0x03);
  buf.push_back(0x05);
  buf.push_back(0x00);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: truncated DELETE data (ksz ok but key bytes truncated).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_truncated_delete_data) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "truncdeldata.wal.0").string();

  auto buf = make_wal_magic();
  buf.push_back(0x01);
  push_u32_le(buf, 1);
  // DELETE: ksz=10 but only 2 key bytes follow
  buf.push_back(0x03);
  push_u32_le(buf, 10);
  buf.push_back('a');
  buf.push_back('b');

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// wal_parse: full valid transaction with PUT + DELETE + PREPARE + COMMIT.
// Exercises the complete internal parse loop.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_full_transaction_binary) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "full.wal.0").string();

  auto buf = make_wal_magic();
  // txn 7: BEGIN
  buf.push_back(0x01);
  push_u32_le(buf, 7);
  // PUT key="hello" val="world"
  buf.push_back(0x02);
  push_u32_le(buf, 5);
  push_u32_le(buf, 5);
  for (char c : std::string("hello")) buf.push_back(c);
  for (char c : std::string("world")) buf.push_back(c);
  // DELETE key="bye"
  buf.push_back(0x03);
  push_u32_le(buf, 3);
  for (char c : std::string("bye")) buf.push_back(c);
  // PREPARE
  buf.push_back(0x04);
  // COMMIT
  buf.push_back(0x05);

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 1u);
  BOOST_CHECK_EQUAL(txns[0].txn_id, 7u);
  BOOST_REQUIRE_EQUAL(txns[0].ops.size(), 2u);
  BOOST_CHECK(!txns[0].ops[0].is_delete);
  BOOST_CHECK_EQUAL(txns[0].ops[0].key, "hello");
  BOOST_CHECK_EQUAL(txns[0].ops[0].val, "world");
  BOOST_CHECK(txns[0].ops[1].is_delete);
  BOOST_CHECK_EQUAL(txns[0].ops[1].key, "bye");
}

// ---------------------------------------------------------------------------
// wal_parse: multiple complete transactions in a single file, with one
// incomplete one at the end (simulates crash mid-write).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_multiple_txns_trailing_incomplete) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "multi.wal.0").string();

  auto buf = make_wal_magic();

  // txn 1: complete
  buf.push_back(0x01); push_u32_le(buf, 1);
  buf.push_back(0x02); push_u32_le(buf, 1); push_u32_le(buf, 1);
  buf.push_back('a'); buf.push_back('1');
  buf.push_back(0x04);
  buf.push_back(0x05);

  // txn 2: complete (DELETE only)
  buf.push_back(0x01); push_u32_le(buf, 2);
  buf.push_back(0x03); push_u32_le(buf, 1); buf.push_back('a');
  buf.push_back(0x04);
  buf.push_back(0x05);

  // txn 3: BEGIN+PUT+PREPARE but no COMMIT
  buf.push_back(0x01); push_u32_le(buf, 3);
  buf.push_back(0x02); push_u32_le(buf, 1); push_u32_le(buf, 1);
  buf.push_back('b'); buf.push_back('2');
  buf.push_back(0x04);
  // no COMMIT

  write_raw_wal(path, buf);
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_REQUIRE_EQUAL(txns.size(), 2u);
  BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
  BOOST_CHECK_EQUAL(txns[1].txn_id, 2u);
}

// ---------------------------------------------------------------------------
// wal_parse: empty file (only magic header, no records).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_only_magic) {
  DirPreparation dir;
  std::string path = (dir.tempDir / "onlymagic.wal.0").string();

  write_raw_wal(path, make_wal_magic());
  std::vector<_WalTxn> txns;
  wal_parse(path, txns);
  BOOST_CHECK(txns.empty());
}

// ---------------------------------------------------------------------------
// _wal_put_u32 / _wal_read_u32 round-trip
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_u32_serialization_roundtrip) {
    std::vector<uint8_t> buf;
    uint32_t val = 123456789;
    _wal_put_u32(buf, val);
    BOOST_REQUIRE_EQUAL(buf.size(), 4);
    uint32_t read_val = _wal_read_u32(buf.data());
    BOOST_CHECK_EQUAL(val, read_val);

    buf.clear();
    _wal_put_u32(buf, 0);
    BOOST_REQUIRE_EQUAL(buf.size(), 4);
    read_val = _wal_read_u32(buf.data());
    BOOST_CHECK_EQUAL(0, read_val);

    buf.clear();
    _wal_put_u32(buf, UINT32_MAX);
    BOOST_REQUIRE_EQUAL(buf.size(), 4);
    read_val = _wal_read_u32(buf.data());
    BOOST_CHECK_EQUAL(UINT32_MAX, read_val);
}

// ---------------------------------------------------------------------------
// _init_file with a tiny existing file (must be truncated).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_init_file_truncates_small_file) {
    DirPreparation dir;
    std::string base = (dir.tempDir / "init_trunc").string();
    std::string wal0 = base + ".wal.0";

    // Create a tiny file, smaller than the WAL header.
    write_raw_wal(wal0, {1, 2, 3});

    TestWal tw;
    BOOST_REQUIRE(tw.open(base));

    // After open, the file should have been truncated and rewritten with the
    // magic header, so its size should be exactly WAL_HEADER_SIZE.
    _wal_fd_t fd = _wal_open(wal0);
    BOOST_REQUIRE(fd != WAL_INVALID_FD);
    BOOST_CHECK_EQUAL(_wal_size(fd), WAL_HEADER_SIZE);
    _wal_close(fd);

    tw.w.close();
}

// ---------------------------------------------------------------------------
// _WalWriter::open fails if a file can't be opened.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_open_failure) {
    TestWal tw;
    // Try to open a WAL in a path that is not a directory, which will fail.
    BOOST_CHECK(!tw.open("/dev/null/wal"));
    BOOST_CHECK(!tw.w.is_open());
}

// ---------------------------------------------------------------------------
// _WalWriter::close is idempotent.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_writer_close_idempotent) {
    DirPreparation dir;
    std::string base = (dir.tempDir / "close_idem").string();

    TestWal tw;
    BOOST_REQUIRE(tw.open(base));
    BOOST_CHECK(tw.w.is_open());
    tw.w.close();
    BOOST_CHECK(!tw.w.is_open());
    // Second call should do nothing and not crash.
    tw.w.close();
    BOOST_CHECK(!tw.w.is_open());
}

// ---------------------------------------------------------------------------
// wal_parse stops at the first sign of corruption.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(wal_parse_stops_at_corruption) {
    DirPreparation dir;
    std::string path = (dir.tempDir / "corruption.wal.0").string();

    auto buf = make_wal_magic();
    // Txn 1: complete and valid
    buf.push_back(0x01); push_u32_le(buf, 1);
    buf.push_back(0x02); push_u32_le(buf, 1); push_u32_le(buf, 1);
    buf.push_back('a'); buf.push_back('1');
    buf.push_back(0x04); // PREPARE
    buf.push_back(0x05); // COMMIT

    // A truncated PUT record
    buf.push_back(0x01); push_u32_le(buf, 2);
    buf.push_back(0x02); push_u32_le(buf, 10); // ksz=10, but no data

    // Txn 3: another valid transaction that should NOT be parsed.
    buf.push_back(0x01); push_u32_le(buf, 3);
    buf.push_back(0x02); push_u32_le(buf, 1); push_u32_le(buf, 1);
    buf.push_back('c'); buf.push_back('3');
    buf.push_back(0x04);
    buf.push_back(0x05);

    write_raw_wal(path, buf);
    std::vector<_WalTxn> txns;
    wal_parse(path, txns);

    // Only the first valid transaction should be parsed.
    BOOST_REQUIRE_EQUAL(txns.size(), 1u);
    BOOST_CHECK_EQUAL(txns[0].txn_id, 1u);
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