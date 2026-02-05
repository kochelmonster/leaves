#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MerkleSyncTests

#include <boost/test/included/unit_test.hpp>
#include <cstring>
#include <filesystem>
#include <string>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/replicating_mmap.hpp"
#include "leaves/intern/replication/_sync.hpp"

using namespace leaves;

// Type aliases
using Storage = ReplicatingMapStorage;

struct SyncTestFixture {
  SyncTestFixture() {
    tempDir = std::filesystem::temp_directory_path() / "test_sync";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    localDbPath = tempDir / "local.lvs";
    remoteDbPath = tempDir / "remote.lvs";
  }

  ~SyncTestFixture() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
  std::filesystem::path localDbPath;
  std::filesystem::path remoteDbPath;
};

BOOST_FIXTURE_TEST_SUITE(MerkleSyncTests, SyncTestFixture)

BOOST_AUTO_TEST_CASE(test_get_root_hash_empty) {
  auto storage = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage);
  
  auto db = (*storage)["testdb"];
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  uint8_t hash[HASH_SIZE];
  bool has_root = MerkleSync::get_root_hash(db_impl, txn, hash);
  BOOST_CHECK(!has_root);
}

BOOST_AUTO_TEST_CASE(test_get_root_hash_with_data) {
  auto storage = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage);
  
  auto db = (*storage)["testdb"];
  auto cursor = db.cursor();
  
  // Insert a key
  cursor.find(Slice("key1"));
  cursor.value(Slice("value1"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  uint8_t hash[HASH_SIZE];
  bool has_root = MerkleSync::get_root_hash(db_impl, txn, hash);
  BOOST_CHECK(has_root);

  // Hash should be non-zero
  bool all_zero = true;
  for (size_t i = 0; i < HASH_SIZE; ++i) {
    if (hash[i] != 0) all_zero = false;
  }
  BOOST_CHECK(!all_zero);
}

BOOST_AUTO_TEST_CASE(test_get_child_hashes_single_leaf) {
  auto storage = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage);
  
  auto db = (*storage)["testdb"];
  auto cursor = db.cursor();
  
  // Insert a single key
  cursor.find(Slice("hello"));
  cursor.value(Slice("world"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  auto children = MerkleSync::get_child_hashes(db_impl, txn, {});

  // Single leaf at root - no children
  BOOST_CHECK(children.empty());
}

BOOST_AUTO_TEST_CASE(test_get_child_hashes_trie) {
  auto storage = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage);
  
  auto db = (*storage)["testdb"];
  auto cursor = db.cursor();
  
  // Insert keys with different first characters
  cursor.find(Slice("apple"));
  cursor.value(Slice("fruit"));
  cursor.commit();
  
  cursor.find(Slice("banana"));
  cursor.value(Slice("fruit"));
  cursor.commit();
  
  auto* db_impl = db._internal();
  auto txn = db_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  auto children = MerkleSync::get_child_hashes(db_impl, txn, {});

  // Should have children for 'a' and 'b'
  BOOST_CHECK_GE(children.size(), 2);
}

BOOST_AUTO_TEST_CASE(test_identical_databases_have_same_hash) {
  // Create first database
  auto storage1 = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage1);
  
  auto db1 = (*storage1)["testdb"];
  auto cursor1 = db1.cursor();
  
  cursor1.find(Slice("key1"));
  cursor1.value(Slice("value1"));
  cursor1.commit();
  
  cursor1.find(Slice("key2"));
  cursor1.value(Slice("value2"));
  cursor1.commit();
  
  // Create second database with identical data
  auto storage2 = Storage::create(remoteDbPath.c_str());
  BOOST_REQUIRE(storage2);
  
  auto db2 = (*storage2)["testdb"];
  auto cursor2 = db2.cursor();
  
  cursor2.find(Slice("key1"));
  cursor2.value(Slice("value1"));
  cursor2.commit();
  
  cursor2.find(Slice("key2"));
  cursor2.value(Slice("value2"));
  cursor2.commit();
  
  // Get hashes
  auto* db1_impl = db1._internal();
  auto* db2_impl = db2._internal();
  auto txn1 = db1_impl->txn();
  auto txn2 = db2_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db1_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  uint8_t hash1[HASH_SIZE];
  uint8_t hash2[HASH_SIZE];
  MerkleSync::get_root_hash(db1_impl, txn1, hash1);
  MerkleSync::get_root_hash(db2_impl, txn2, hash2);

  // Hashes should be equal
  BOOST_CHECK(MerkleSync::hashes_equal(hash1, hash2));
}

BOOST_AUTO_TEST_CASE(test_different_databases_have_different_hash) {
  // Create first database
  auto storage1 = Storage::create(localDbPath.c_str());
  BOOST_REQUIRE(storage1);
  
  auto db1 = (*storage1)["testdb"];
  auto cursor1 = db1.cursor();
  
  cursor1.find(Slice("key1"));
  cursor1.value(Slice("value1"));
  cursor1.commit();
  
  // Create second database with different data
  auto storage2 = Storage::create(remoteDbPath.c_str());
  BOOST_REQUIRE(storage2);
  
  auto db2 = (*storage2)["testdb"];
  auto cursor2 = db2.cursor();
  
  cursor2.find(Slice("key1"));
  cursor2.value(Slice("value1"));
  cursor2.commit();
  
  cursor2.find(Slice("key2"));  // Extra key
  cursor2.value(Slice("value2"));
  cursor2.commit();
  
  // Get hashes
  auto* db1_impl = db1._internal();
  auto* db2_impl = db2._internal();
  auto txn1 = db1_impl->txn();
  auto txn2 = db2_impl->txn();
  
  using DBImpl = std::remove_pointer_t<decltype(db1_impl)>;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  uint8_t hash1[HASH_SIZE];
  uint8_t hash2[HASH_SIZE];
  MerkleSync::get_root_hash(db1_impl, txn1, hash1);
  MerkleSync::get_root_hash(db2_impl, txn2, hash2);

  // Hashes should differ
  BOOST_CHECK(!MerkleSync::hashes_equal(hash1, hash2));
}

BOOST_AUTO_TEST_CASE(test_find_diff_paths) {
  ChildHash ch1, ch2, ch3, ch4;

  // Same hash
  std::memset(ch1.hash, 0xAA, HASH_SIZE);
  ch1.child_key = 'a';
  std::memset(ch2.hash, 0xAA, HASH_SIZE);
  ch2.child_key = 'a';

  // Different hash
  std::memset(ch3.hash, 0xBB, HASH_SIZE);
  ch3.child_key = 'b';
  std::memset(ch4.hash, 0xCC, HASH_SIZE);
  ch4.child_key = 'b';

  std::vector<ChildHash> local = {ch1, ch3};
  std::vector<ChildHash> remote = {ch2, ch4};

  // Use a dummy DB type - find_diff_paths is static and doesn't need DB
  using Storage = ReplicatingMapStorage;
  using StorageImpl = Storage::StorageImpl;
  using DBImpl = StorageImpl::DB;
  using MerkleSync = _MerkleSync<DBImpl>;
  
  auto diffs = MerkleSync::find_diff_paths(local, remote, {});

  // Only 'b' should differ
  BOOST_CHECK_EQUAL(diffs.size(), 1);
  BOOST_CHECK_EQUAL(diffs[0].size(), 1);
  BOOST_CHECK_EQUAL(diffs[0][0], 'b');
}

BOOST_AUTO_TEST_CASE(test_overwrite_policy_always_accept) {
  AlwaysAcceptRemote policy;
  OverwriteContext ctx;
  ctx.local_value = Slice("old");
  ctx.remote_value = Slice("new");

  BOOST_CHECK(policy.on_conflict(ctx) == OverwriteAction::USE_REMOTE);
}

BOOST_AUTO_TEST_CASE(test_overwrite_policy_last_write_wins) {
  LastWriteWins policy;

  OverwriteContext ctx1;
  ctx1.local_txn_id = 10;
  ctx1.remote_txn_id = 20;
  BOOST_CHECK(policy.on_conflict(ctx1) == OverwriteAction::USE_REMOTE);

  OverwriteContext ctx2;
  ctx2.local_txn_id = 20;
  ctx2.remote_txn_id = 10;
  BOOST_CHECK(policy.on_conflict(ctx2) == OverwriteAction::KEEP_LOCAL);
}

BOOST_AUTO_TEST_SUITE_END()
