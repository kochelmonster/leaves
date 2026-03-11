#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE hash_updater
#include <boost/test/included/unit_test.hpp>

#include <blake3.h>
#include <set>

#include "../include/leaves/intern/replication/_hash.hpp"
#include "../include/leaves/intern/util/_threadpool.hpp"
#include "../include/leaves/mmap.hpp"

using namespace leaves;

#define TEST_FILE "test_hash_updater.lvs"

struct HashUpdaterPreparation {
  HashUpdaterPreparation() { std::remove(TEST_FILE); }
  ~HashUpdaterPreparation() { std::remove(TEST_FILE); }
};

BOOST_GLOBAL_FIXTURE(HashUpdaterPreparation);

// For testing, we use the same DB for both data and hash tries.
// The hash trie is stored in a separate root offset that we manage manually.
// In production, the hash trie root would be in the DB header.

typedef MapStorage Storage;
typedef Storage::StorageImpl::DB InternalDB;
typedef InternalDB::CursorTraits CursorTraits;
typedef InternalDB::Traits Traits;
typedef _TransactionalCursor<CursorTraits> InternalCursor;

/**
 * Helper to run _HashUpdater within a transaction context.
 * The new _HashUpdater uses txn_id comparison to skip unchanged subtrees.
 */
template <typename DB>
void run_hash_update(DB* internal_db, typename DB::offset_e data_root,
                     typename DB::offset_e* hash_root_ptr, tid_t /*last_hashed - unused*/) {
  // Start a transaction for allocations
  InternalCursor cursor(internal_db, hash_root_ptr);
  cursor.start_transaction();

  _HashUpdater<DB, DB> updater(internal_db, internal_db);
  updater.exec(data_root, hash_root_ptr);

  cursor.commit(internal_db->new_cursor_id());
}

/**
 * Helper to count data trie nodes (for verification)
 */
template <typename DB>
int count_data_nodes(DB* db, typename DB::offset_e offset) {
  using TrieNode = _TrieNode<CursorTraits>;

  if (!offset) return 0;

  if (offset.type() == LEAF) {
    return 1;
  }

  auto trie = db->template resolve<TrieNode>(&offset);
  int count = 1;  // This trie node
  for (int k = trie->first(); k != TrieNode::OUT_OF_RANGE; k = trie->next(k)) {
    count += count_data_nodes(db, *trie->offset(k));
  }
  return count;
}

/**
 * Helper to count hash trie nodes (for verification)
 */
template <typename DB>
int count_hash_nodes(DB* db, typename DB::offset_e offset) {
  using HashTraits = HashTrieTraits<CursorTraits>;
  using TrieNode = _TrieNode<HashTraits>;

  if (!offset) return 0;

  if (offset.type() == LEAF) {
    return 1;
  }

  auto trie = db->template resolve<TrieNode>(&offset);
  int count = 1;  // This trie node
  for (int k = trie->first(); k != TrieNode::OUT_OF_RANGE; k = trie->next(k)) {
    count += count_hash_nodes(db, *trie->offset(k));
  }
  return count;
}

/**
 * Helper to collect all hash node offsets into a set
 */
template <typename DB>
void collect_hash_offsets(DB* db, typename DB::offset_e offset,
                          std::set<uint64_t>& offsets) {
  using HashTraits = HashTrieTraits<CursorTraits>;
  using TrieNode = _TrieNode<HashTraits>;

  if (!offset) return;

  offsets.insert(offset._offset);

  if (offset.type() == TRIE) {
    auto trie = db->template resolve<TrieNode>(&offset);
    for (int k = trie->first(); k != TrieNode::OUT_OF_RANGE; k = trie->next(k)) {
      collect_hash_offsets(db, *trie->offset(k), offsets);
    }
  }
}

/**
 * Count how many offsets from 'before' are still present in 'after'
 */
inline int count_preserved_offsets(const std::set<uint64_t>& before,
                                   const std::set<uint64_t>& after) {
  int count = 0;
  for (uint64_t off : before) {
    if (after.count(off)) {
      count++;
    }
  }
  return count;
}

/**
 * Helper to verify hash trie structure matches data trie
 */
template <typename DataDB, typename HashDB>
bool verify_structure(DataDB* data_db, HashDB* hash_db,
                      typename DataDB::offset_e data_offset,
                      typename HashDB::offset_e hash_offset) {
  using DataTrieNode = _TrieNode<typename DataDB::CursorTraits>;
  using DataLeafNode = _LeafNode<typename DataDB::CursorTraits>;
  // Hash trie uses HashTrieTraits which includes 32-byte hash in nodes
  using HashTraits = HashTrieTraits<typename HashDB::CursorTraits>;
  using HashTrieNode = _TrieNode<HashTraits>;
  using HashLeafNode = _LeafNode<HashTraits>;

  if (!data_offset && !hash_offset) return true;
  if (!data_offset || !hash_offset) return false;

  if (data_offset.type() != hash_offset.type()) return false;

  if (data_offset.type() == LEAF) {
    // Both are leaves - check hash is non-zero
    auto hash_leaf = hash_db->template resolve<HashLeafNode>(&hash_offset);
    // Hash should be computed (non-zero)
    bool has_hash = false;
    for (size_t i = 0; i < sizeof(hash_leaf->hash); ++i) {
      if (hash_leaf->hash[i] != 0) {
        has_hash = true;
        break;
      }
    }
    return has_hash;
  }

  // Both are tries - verify same branches
  auto data_trie = data_db->template resolve<DataTrieNode>(&data_offset);
  auto hash_trie = hash_db->template resolve<HashTrieNode>(&hash_offset);

  // Check branch count matches
  if (data_trie->count() != hash_trie->count()) return false;

  // Check each branch exists in both and recurse
  for (int k = data_trie->first(); k != DataTrieNode::OUT_OF_RANGE;
       k = data_trie->next(k)) {
    if (!hash_trie->isset(k)) return false;
    if (!verify_structure(data_db, hash_db, *data_trie->offset(k),
                          *hash_trie->offset(k))) {
      return false;
    }
  }

  // Check hash trie node has non-zero hash
  bool has_hash = false;
  for (size_t i = 0; i < sizeof(hash_trie->hash); ++i) {
    if (hash_trie->hash[i] != 0) {
      has_hash = true;
      break;
    }
  }

  return has_hash;
}

BOOST_AUTO_TEST_CASE(empty_data_trie) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Empty data trie
  offset_t hash_root{};

  // Empty data trie - nothing to update
  auto txn = internal_db->txn();
  run_hash_update(internal_db, txn->root, &hash_root, tid_t(0));

  // Hash root should remain empty (data trie is empty)
  BOOST_CHECK(!hash_root);
}

BOOST_AUTO_TEST_CASE(single_leaf) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];

  // Insert a single key
  auto cursor = db.cursor();
  cursor.find("hello");
  cursor.value("world");
  cursor.commit();

  auto* internal_db = db._internal();
  auto txn = internal_db->txn();

  offset_t hash_root{};
  run_hash_update(internal_db, txn->root, &hash_root, tid_t(0));

  // Hash root should now point to a leaf
  BOOST_REQUIRE(hash_root);
  BOOST_CHECK_EQUAL(hash_root.type(), LEAF);

  // Verify hash is non-zero - use HashTrieTraits to access 32-byte hash
  using HashLeafNode = _LeafNode<HashTrieTraits<Traits>>;
  auto hash_leaf = internal_db->template resolve<HashLeafNode>(&hash_root);
  bool has_hash = false;
  for (size_t i = 0; i < HASH_SIZE; ++i) {
    if (hash_leaf->hash[i] != 0) {
      has_hash = true;
      break;
    }
  }
  BOOST_CHECK(has_hash);
}

BOOST_AUTO_TEST_CASE(multiple_keys_structure_match) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];

  // Insert multiple keys to create a trie structure
  std::vector<std::string> keys = {"apple", "application", "apply", "banana",
                                   "bandana", "cat"};

  for (const auto& key : keys) {
    auto cursor = db.cursor();
    cursor.find(key);
    cursor.value(key + "_value");
    cursor.commit();
  }

  auto* internal_db = db._internal();
  auto txn = internal_db->txn();

  offset_t hash_root{};
  run_hash_update(internal_db, txn->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn->root, hash_root));

  // Count nodes should match (roughly - hash trie mirrors data trie)
  int data_nodes = count_data_nodes(internal_db, txn->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

BOOST_AUTO_TEST_CASE(incremental_update) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Insert initial keys
  {
    auto cursor = db.cursor();
    cursor.find("key1");
    cursor.value("value1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("key2");
    cursor.value("value2");
    cursor.commit();
  }

  auto txn1 = internal_db->txn();
  tid_t first_txn_id = txn1->txn_id;

  // Initial hash update
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Get hash of a known node before modification (use HashTrieTraits)
  using HashTrieNode = _TrieNode<HashTrieTraits<Traits>>;
  uint8_t old_root_hash[HASH_SIZE];
  {
    auto hash_trie = internal_db->template resolve<HashTrieNode>(&hash_root);
    memcpy(old_root_hash, hash_trie->hash, HASH_SIZE);
  }

  // Add another key
  {
    auto cursor = db.cursor();
    cursor.find("key3");
    cursor.value("value3");
    cursor.commit();
  }

  auto txn2 = internal_db->txn();

  // Incremental update - only hash nodes newer than first_txn_id
  run_hash_update(internal_db, txn2->root, &hash_root, first_txn_id);

  // Structure should still match
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  // Root hash should have changed (new child added)
  {
    auto hash_trie = internal_db->template resolve<HashTrieNode>(&hash_root);
    bool hash_changed = false;
    for (size_t i = 0; i < HASH_SIZE; ++i) {
      if (hash_trie->hash[i] != old_root_hash[i]) {
        hash_changed = true;
        break;
      }
    }
    BOOST_CHECK(hash_changed);
  }
}

BOOST_AUTO_TEST_CASE(deterministic_hashes) {
  // Same data should produce same hashes
  std::remove(TEST_FILE);  // Clean slate for this test
  using HashLeafNode = _LeafNode<HashTrieTraits<Traits>>;
  uint8_t hash1[HASH_SIZE];
  
  {
    auto storage1 = MapStorage::create(TEST_FILE);
    {
      auto db = (*storage1)["test"];
      auto cursor = db.cursor();
      cursor.find("testkey");
      cursor.value("testvalue");
      cursor.commit();
    }

    auto db1 = (*storage1)["test"];
    auto* internal_db1 = db1._internal();
    auto txn1 = internal_db1->txn();

    offset_t hash_root1{};
    run_hash_update(internal_db1, txn1->root, &hash_root1, tid_t(0));

    // Get hash - use HashTrieTraits to access 32-byte hash
    auto hash_leaf = internal_db1->template resolve<HashLeafNode>(&hash_root1);
    memcpy(hash1, hash_leaf->hash, HASH_SIZE);
  }
  // storage1 is now closed

  // Create another storage with same data
  std::remove(TEST_FILE);
  uint8_t hash2[HASH_SIZE];
  
  {
    auto storage2 = MapStorage::create(TEST_FILE);
    {
      auto db = (*storage2)["test"];
      auto cursor = db.cursor();
      cursor.find("testkey");
      cursor.value("testvalue");
      cursor.commit();
    }

    auto db2 = (*storage2)["test"];
    auto* internal_db2 = db2._internal();
    auto txn2 = internal_db2->txn();

    offset_t hash_root2{};
    run_hash_update(internal_db2, txn2->root, &hash_root2, tid_t(0));

    // Get hash
    auto hash_leaf = internal_db2->template resolve<HashLeafNode>(&hash_root2);
    memcpy(hash2, hash_leaf->hash, HASH_SIZE);
  }

  // Hashes should be identical
  BOOST_CHECK_EQUAL_COLLECTIONS(hash1, hash1 + HASH_SIZE, hash2, hash2 + HASH_SIZE);
}

BOOST_AUTO_TEST_CASE(prune_deleted_branches) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Insert keys to create structure
  {
    auto cursor = db.cursor();
    cursor.find("keep1");
    cursor.value("value1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("keep2");
    cursor.value("value2");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("delete_me");
    cursor.value("to_be_deleted");
    cursor.commit();
  }

  // Initial hash
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  int initial_hash_nodes = count_hash_nodes(internal_db, hash_root);

  // Delete a key
  {
    auto cursor = db.cursor();
    cursor.find("delete_me");
    BOOST_REQUIRE(cursor.is_valid());
    cursor.remove();
    cursor.commit();
  }

  // Update hash trie - should prune the deleted branch
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Hash trie should have fewer nodes now
  int final_hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_LT(final_hash_nodes, initial_hash_nodes);

  // Structure should still match
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));
}

/**
 * Test prefix alignment: hash prefix longer than data prefix.
 *
 * Scenario:
 * 1. Create data with long common prefix: "zzzzz_abc" and "zzzzz_def"
 *    → data trie: prefix="zzzzz_", branches 'a' and 'd'
 * 2. Generate hash trie (mirrors data)
 * 3. Add key "zzzzz_aXX" which splits the 'a' subtree
 *    → 'a' child now has prefix="" with branches 'b' and 'X'
 * 4. Hash update should handle: hash child has prefix="bc" but data child has prefix=""
 *    This triggers common < effective_hash_len
 */
BOOST_AUTO_TEST_CASE(prefix_alignment_hash_longer) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Step 1: Create initial structure with long prefixes
  {
    auto cursor = db.cursor();
    cursor.find("zzzzz_abc");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("zzzzz_def");
    cursor.value("v2");
    cursor.commit();
  }

  // Step 2: Generate initial hash trie
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Step 3: Add key that splits a subtree, creating shorter prefix in data
  {
    auto cursor = db.cursor();
    cursor.find("zzzzz_aXX");  // Splits the "abc" → now "a" branches to "bc" and "XX"
    cursor.value("v3");
    cursor.commit();
  }

  // Step 4: Update hash trie - this should trigger hash prefix longer case
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  // Count should match
  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test prefix alignment: data prefix longer than hash prefix.
 *
 * Scenario:
 * 1. Create data with short common prefix: "pre_a" and "pre_b"
 *    → data trie: prefix="pre_", branches 'a' and 'b'
 * 2. Generate hash trie (mirrors data)
 * 3. Delete "pre_b" and merge "pre_a" into longer prefix
 *    → data might now be single leaf or have different structure
 * 4. Add "pre_abc" and "pre_abd" to create: prefix="pre_ab", branches 'c' and 'd'
 *    → data prefix at that level is now longer than hash child's prefix
 *    This triggers common < effective_data_len
 */
BOOST_AUTO_TEST_CASE(prefix_alignment_data_longer) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Step 1: Create initial structure
  {
    auto cursor = db.cursor();
    cursor.find("pre_a");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("pre_b");
    cursor.value("v2");
    cursor.commit();
  }

  // Step 2: Generate initial hash trie
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Step 3: Remove "pre_b" and add keys that create longer prefix
  {
    auto cursor = db.cursor();
    cursor.find("pre_b");
    BOOST_REQUIRE(cursor.is_valid());
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("pre_abc");
    cursor.value("v3");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("pre_abd");
    cursor.value("v4");
    cursor.commit();
  }
  // Also remove the original pre_a to force restructuring
  {
    auto cursor = db.cursor();
    cursor.find("pre_a");
    if (cursor.is_valid()) {
      cursor.remove();
      cursor.commit();
    }
  }

  // Step 4: Update hash trie - should trigger data prefix longer case
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  // Count should match
  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test deep prefix mismatch with multiple levels of skip.
 *
 * Scenario: Create a deep trie structure where updates require following
 * branches multiple levels deep with prefix skipping.
 */
BOOST_AUTO_TEST_CASE(deep_prefix_mismatch) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Create a deep structure with long prefixes
  std::vector<std::string> keys = {
    "aaaa_bbbb_cccc_1",
    "aaaa_bbbb_cccc_2",
    "aaaa_bbbb_dddd_1",
    "aaaa_eeee_ffff_1"
  };

  for (const auto& key : keys) {
    auto cursor = db.cursor();
    cursor.find(key);
    cursor.value("v");
    cursor.commit();
  }

  // Initial hash
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Add a key that requires restructuring deep in the trie
  {
    auto cursor = db.cursor();
    cursor.find("aaaa_bbbb_cccc_1_extra");  // Extends existing leaf
    cursor.value("v_new");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("aaaa_bbbb_cXXX");  // Splits at different level
    cursor.value("v_split");
    cursor.commit();
  }

  // Update and verify
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test divergent prefixes (no common match).
 *
 * Ensure that when prefixes diverge completely, we properly
 * replace the hash subtree.
 */
BOOST_AUTO_TEST_CASE(divergent_prefixes) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Create initial structure
  {
    auto cursor = db.cursor();
    cursor.find("alpha_one");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("alpha_two");
    cursor.value("v2");
    cursor.commit();
  }

  // Generate hash
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Delete all and create completely different structure
  {
    auto cursor = db.cursor();
    cursor.find("alpha_one");
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("alpha_two");
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("beta_xyz");
    cursor.value("new_v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("beta_abc");
    cursor.value("new_v2");
    cursor.commit();
  }

  // Update hash - prefixes will diverge ("alpha" vs "beta")
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test hash_prefix_skip: hash prefix is longer than data prefix at root.
 *
 * This test specifically triggers the `common < effective_hash_len` code path
 * where we need to recursively call sync_nodes with hash_prefix_skip > 0.
 *
 * Scenario:
 * 1. Create "abcdef1" and "abcdef2"
 *    → root trie: prefix="abcdef", branches '1' and '2' (both leaves)
 * 2. Generate hash trie (mirrors structure)
 * 3. Add "abcXYZ"
 *    → root trie: prefix="abc", branches 'd' and 'X'
 *       - 'd' branch: trie with prefix="ef", branches '1', '2'
 *       - 'X' branch: leaf "YZ"
 * 4. Update hash:
 *    - data prefix="abc" (len=3), hash prefix="abcdef" (len=6)
 *    - common=3, effective_hash_len=6
 *    - common < effective_hash_len → YES!
 *    - diverge_byte = 'd', data.isset('d') → YES
 *    - Recurse with hash_prefix_skip = 4
 */
BOOST_AUTO_TEST_CASE(hash_prefix_skip_root_level) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Step 1: Create structure with long common prefix
  {
    auto cursor = db.cursor();
    cursor.find("abcdef1");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abcdef2");
    cursor.value("v2");
    cursor.commit();
  }

  // Step 2: Generate initial hash trie
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Step 3: Add key that shortens root prefix
  {
    auto cursor = db.cursor();
    cursor.find("abcXYZ");
    cursor.value("v3");
    cursor.commit();
  }

  // Step 4: Update hash with prefix_skip logic
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test data_prefix_skip: data prefix is longer than hash prefix at root.
 *
 * This test specifically triggers the `common < effective_data_len` code path
 * where we need to recursively call sync_nodes with data_prefix_skip > 0.
 *
 * Scenario:
 * 1. Create "abc1" and "abc2"
 *    → root trie: prefix="abc", branches '1' and '2'
 * 2. Generate hash trie
 * 3. Delete "abc2" and add "abc1XYZ" and "abc1ABC"
 *    → root trie: prefix="abc1", branches 'A', 'X' (longer prefix!)
 *       - The root prefix grew from "abc" to "abc1"
 * 4. Update hash:
 *    - data prefix="abc1" (len=4), hash prefix="abc" (len=3)
 *    - common=3, effective_hash_len=3, effective_data_len=4
 *    - common < effective_data_len → YES!
 *    - next_byte = '1', hash.isset('1') → YES
 *    - Recurse with data_prefix_skip = 4
 */
BOOST_AUTO_TEST_CASE(data_prefix_skip_root_level) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Step 1: Create structure with short common prefix
  {
    auto cursor = db.cursor();
    cursor.find("abc1");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc2");
    cursor.value("v2");
    cursor.commit();
  }

  // Step 2: Generate initial hash trie
  // Hash: prefix="abc", branches '1' and '2'
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn1->root, hash_root));

  // Step 3: Delete one key and add keys that extend the prefix
  {
    auto cursor = db.cursor();
    cursor.find("abc2");
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1");  // Remove original too
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1XYZ");
    cursor.value("v3");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1ABC");
    cursor.value("v4");
    cursor.commit();
  }

  // Now data has: prefix="abc1", branches 'A' and 'X'
  // Hash has: prefix="abc", branches '1' and '2'

  // Step 4: Update hash with data_prefix_skip logic
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  int data_nodes = count_data_nodes(internal_db, txn2->root);
  int hash_nodes = count_hash_nodes(internal_db, hash_root);
  BOOST_CHECK_EQUAL(data_nodes, hash_nodes);
}

/**
 * Test hash_prefix_skip REUSE: verify unchanged subtrees are reused, not deep-copied.
 *
 * This test verifies that when hash_prefix_skip is used correctly, unchanged
 * hash nodes are preserved (same offsets) rather than being recreated.
 *
 * Scenario:
 * 1. Create "abcdef1" and "abcdef2"
 *    → data: prefix="abcdef", branches '1' and '2' (both leaves)
 *    → hash: prefix="abcdef", branches '1' (hash_leaf1), '2' (hash_leaf2)
 *
 * 2. Add "abcXYZ" which restructures:
 *    → data: prefix="abc", branches 'd' (trie["ef"] with '1','2'), 'X' (leaf)
 *    → The leaves for '1' and '2' are UNCHANGED (same data offsets, same txn_id)
 *
 * 3. Update hash:
 *    - hash prefix="abcdef" (len=6), data prefix="abc" (len=3)
 *    - common=3 < effective_hash_len=6 → triggers hash_prefix_skip
 *    - With correct skip, hash_leaf1 and hash_leaf2 should be REUSED (same offsets)
 *    - Without correct skip, they would be deep-copied (new offsets)
 */
BOOST_AUTO_TEST_CASE(hash_prefix_skip_reuse_verification) {
  std::remove(TEST_FILE);  // Start with clean slate
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  using HashTraits = HashTrieTraits<Traits>;
  using HashTrieNode = _TrieNode<HashTraits>;

  // Step 1: Create initial structure with long common prefix
  {
    auto cursor = db.cursor();
    cursor.find("abcdef1");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abcdef2");
    cursor.value("v2");
    cursor.commit();
  }

  // Get data leaf offsets BEFORE modification (to verify COW preserves them)
  using DataTrieNode = _TrieNode<CursorTraits>;
  auto txn_before = internal_db->txn();
  auto data_trie_before = internal_db->template resolve<DataTrieNode>(&txn_before->root);
  int data_branch1 = data_trie_before->first();
  int data_branch2 = data_trie_before->next(data_branch1);
  uint64_t data_leaf1_before = data_trie_before->offset(data_branch1)->_offset;
  uint64_t data_leaf2_before = data_trie_before->offset(data_branch2)->_offset;
  BOOST_TEST_MESSAGE("Data leaf offsets before: " << data_leaf1_before << ", " << data_leaf2_before);

  // Generate initial hash trie
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  BOOST_REQUIRE_EQUAL(hash_root.type(), TRIE);

  // Collect all hash offsets BEFORE modification
  std::set<uint64_t> offsets_before;
  collect_hash_offsets(internal_db, hash_root, offsets_before);

  // Get the specific leaf offsets we expect to be preserved
  auto hash_trie = internal_db->template resolve<HashTrieNode>(&hash_root);
  
  // Debug: print what branches exist
  BOOST_TEST_MESSAGE("Initial hash trie branches:");
  for (int k = hash_trie->first(); k != HashTrieNode::OUT_OF_RANGE; k = hash_trie->next(k)) {
    BOOST_TEST_MESSAGE("  Branch: " << k << " (char: '" << (char)(k >= 0 ? k : '?') << "')");
  }
  
  // Get first two branches (whatever they are)
  int branch1 = hash_trie->first();
  int branch2 = hash_trie->next(branch1);
  BOOST_REQUIRE_NE(branch1, HashTrieNode::OUT_OF_RANGE);
  BOOST_REQUIRE_NE(branch2, HashTrieNode::OUT_OF_RANGE);
  
  uint64_t leaf1_offset_before = hash_trie->offset(branch1)->_offset;
  uint64_t leaf2_offset_before = hash_trie->offset(branch2)->_offset;

  // Step 2: Add key that shortens root prefix but preserves subtree
  {
    auto cursor = db.cursor();
    cursor.find("abcXYZ");
    cursor.value("v3");
    cursor.commit();
  }

  // Verify data leaf offsets are PRESERVED (COW working)
  auto txn2 = internal_db->txn();
  BOOST_REQUIRE_EQUAL(txn2->root.type(), TRIE);
  auto data_root_after = internal_db->template resolve<DataTrieNode>(&txn2->root);
  // After adding "abcXYZ", root should have branches 'X' and 'd'
  BOOST_REQUIRE(data_root_after->isset('d'));
  offset_t data_d_branch = *data_root_after->offset('d');
  BOOST_REQUIRE_EQUAL(data_d_branch.type(), TRIE);
  auto data_d_trie = internal_db->template resolve<DataTrieNode>(&data_d_branch);
  
  // The 'd' subtrie should have branches '1' and '2' with original leaf offsets
  int d_data_branch1 = data_d_trie->first();
  int d_data_branch2 = data_d_trie->next(d_data_branch1);
  uint64_t data_leaf1_after = data_d_trie->offset(d_data_branch1)->_offset;
  uint64_t data_leaf2_after = data_d_trie->offset(d_data_branch2)->_offset;
  BOOST_TEST_MESSAGE("Data leaf offsets after: " << data_leaf1_after << ", " << data_leaf2_after);
  
  // Verify data COW: leaf offsets should be PRESERVED
  bool data_cow_works = (data_leaf1_before == data_leaf1_after && data_leaf2_before == data_leaf2_after) ||
                        (data_leaf1_before == data_leaf2_after && data_leaf2_before == data_leaf1_after);
  BOOST_CHECK_MESSAGE(data_cow_works, "Data COW should preserve leaf offsets");

  // Step 3: Update hash - should trigger hash_prefix_skip
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure is correct
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  // Collect all hash offsets AFTER modification
  std::set<uint64_t> offsets_after;
  collect_hash_offsets(internal_db, hash_root, offsets_after);

  // The leaf offsets should be PRESERVED (same offsets still present)
  // Navigate to find the subtrie (branch 'd' for "def...")
  BOOST_REQUIRE_EQUAL(hash_root.type(), TRIE);
  auto new_hash_root = internal_db->template resolve<HashTrieNode>(&hash_root);
  
  // Debug: print new hash trie branches
  BOOST_TEST_MESSAGE("New hash trie branches after update:");
  for (int k = new_hash_root->first(); k != HashTrieNode::OUT_OF_RANGE; k = new_hash_root->next(k)) {
    BOOST_TEST_MESSAGE("  Branch: " << k << " (char: '" << (char)(k >= 0 ? k : '?') << "')");
  }
  
  // Find the 'd' branch which should contain the preserved subtree
  BOOST_REQUIRE(new_hash_root->isset('d'));

  offset_t d_branch = *new_hash_root->offset('d');
  BOOST_REQUIRE_EQUAL(d_branch.type(), TRIE);
  auto d_trie = internal_db->template resolve<HashTrieNode>(&d_branch);
  
  // Debug: print d subtrie branches
  BOOST_TEST_MESSAGE("Subtrie 'd' branches:");
  for (int k = d_trie->first(); k != HashTrieNode::OUT_OF_RANGE; k = d_trie->next(k)) {
    BOOST_TEST_MESSAGE("  Branch: " << k << " (char: '" << (char)(k >= 0 ? k : '?') << "')");
  }
  
  // The subtrie should have two branches (our original leaf branches)
  int d_branch1 = d_trie->first();
  int d_branch2 = d_trie->next(d_branch1);
  BOOST_REQUIRE_NE(d_branch1, HashTrieNode::OUT_OF_RANGE);
  BOOST_REQUIRE_NE(d_branch2, HashTrieNode::OUT_OF_RANGE);

  uint64_t leaf1_offset_after = d_trie->offset(d_branch1)->_offset;
  uint64_t leaf2_offset_after = d_trie->offset(d_branch2)->_offset;

  // KEY ASSERTION: leaf offsets should be PRESERVED (reused, not deep-copied)
  // Note: branch ordering may differ, so check if EITHER matches
  bool leaf1_preserved = (leaf1_offset_before == leaf1_offset_after || leaf1_offset_before == leaf2_offset_after);
  bool leaf2_preserved = (leaf2_offset_before == leaf1_offset_after || leaf2_offset_before == leaf2_offset_after);
  
  BOOST_TEST_MESSAGE("leaf1_offset_before: " << leaf1_offset_before);
  BOOST_TEST_MESSAGE("leaf2_offset_before: " << leaf2_offset_before);
  BOOST_TEST_MESSAGE("leaf1_offset_after: " << leaf1_offset_after);
  BOOST_TEST_MESSAGE("leaf2_offset_after: " << leaf2_offset_after);
  
  BOOST_CHECK(leaf1_preserved);
  BOOST_CHECK(leaf2_preserved);

  // Count preserved offsets - at least the leaf nodes should be preserved
  int preserved = count_preserved_offsets(offsets_before, offsets_after);
  BOOST_CHECK_GE(preserved, 2);  // At least both leaf hashes preserved
}

/**
 * Test data_prefix_skip REUSE: verify unchanged subtrees are reused.
 *
 * Scenario:
 * 1. Create "abc1", "abc2" → hash: prefix="abc", branches '1','2' (both leaves)
 * 2. Modify to "abc1XYZ", "abc1ABC"
 *    → data: prefix="abc1", branches 'A','X' (data prefix grew longer)
 *    → hash still has: prefix="abc", branches '1','2'
 *
 * 3. Update hash:
 *    - data prefix="abc1" (len=4), hash prefix="abc" (len=3)
 *    - common=3 < effective_data_len=4 → triggers data_prefix_skip
 *    - next_byte='1', hash.isset('1')=YES
 *    - Recurse with data_prefix_skip=4
 *
 * Note: In this case, the original leaves under '1' won't be reused because
 * we deleted "abc1" and added different keys. But the recursive logic with
 * data_prefix_skip should still work correctly.
 */
BOOST_AUTO_TEST_CASE(data_prefix_skip_structural_verification) {
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Step 1: Create initial structure
  {
    auto cursor = db.cursor();
    cursor.find("abc1");
    cursor.value("v1");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc2");
    cursor.value("v2");
    cursor.commit();
  }

  // Generate initial hash trie
  auto txn1 = internal_db->txn();
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  BOOST_REQUIRE(hash_root);
  int initial_hash_nodes = count_hash_nodes(internal_db, hash_root);

  // Step 2: Delete and add to create longer data prefix
  {
    auto cursor = db.cursor();
    cursor.find("abc2");
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1");
    cursor.remove();
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1XYZ");
    cursor.value("v3");
    cursor.commit();
  }
  {
    auto cursor = db.cursor();
    cursor.find("abc1ABC");
    cursor.value("v4");
    cursor.commit();
  }

  // Step 3: Update hash with data_prefix_skip logic
  auto txn2 = internal_db->txn();
  run_hash_update(internal_db, txn2->root, &hash_root, tid_t(0));

  // Verify structure matches
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn2->root, hash_root));

  int final_hash_nodes = count_hash_nodes(internal_db, hash_root);
  int data_nodes = count_data_nodes(internal_db, txn2->root);
  BOOST_CHECK_EQUAL(data_nodes, final_hash_nodes);
}

// =============================================================================
// Parallel execution tests (Phase 4)
// =============================================================================

#if LEAVES_HAS_THREADS

/**
 * Collect all leaf hashes from a hash trie into a map keyed by branch path.
 * Used to compare parallel vs inline hash results.
 */
template <typename DB>
void collect_leaf_hashes(DB* db, typename DB::offset_e offset,
                         std::string path,
                         std::map<std::string, std::vector<uint8_t>>& result) {
  using HashTraits = HashTrieTraits<CursorTraits>;
  using TrieNode = _TrieNode<HashTraits>;
  using LeafNode = _LeafNode<HashTraits>;

  if (!offset) return;

  if (offset.type() == LEAF) {
    auto leaf = db->template resolve<LeafNode>(&offset);
    result[path] = std::vector<uint8_t>(leaf->hash, leaf->hash + HASH_SIZE);
    return;
  }

  auto trie = db->template resolve<TrieNode>(&offset);
  path.append((const char*)trie->compressed(), trie->len());
  for (int k = trie->first(); k != TrieNode::OUT_OF_RANGE;
       k = trie->next(k)) {
    std::string child_path = path;
    if (k != TrieNode::NONE) child_path.push_back((char)k);
    collect_leaf_hashes(db, *trie->offset(k), child_path, result);
  }
}

struct TestPool : _ThreadPoolMixin<TestPool> {
  TestPool(size_t n = 4) : _ThreadPoolMixin(n) {}
};

template <typename DB>
void run_hash_update_parallel(DB* internal_db, _PoolExecutor& exec,
                              typename DB::offset_e data_root,
                              typename DB::offset_e* hash_root_ptr) {
  InternalCursor cursor(internal_db, hash_root_ptr);
  cursor.start_transaction();

  update_hash_trie(exec, internal_db, internal_db, data_root, hash_root_ptr);

  cursor.commit(internal_db->new_cursor_id());
}

BOOST_AUTO_TEST_CASE(parallel_matches_inline_wide_trie) {
  // Create a wide trie (many branches at root) and verify parallel hashes
  // match inline hashes
  std::remove(TEST_FILE);
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Insert keys that create many branches at the root level
  for (int i = 0; i < 26; i++) {
    auto cursor = db.cursor();
    std::string key(1, 'a' + i);
    key += "_value_key";
    cursor.find(key);
    cursor.value("value_" + std::to_string(i));
    cursor.commit();
  }

  auto txn = internal_db->txn();

  // Run inline hash update
  offset_t hash_root_inline{};
  run_hash_update(internal_db, txn->root, &hash_root_inline, tid_t(0));

  // Collect inline hashes
  std::map<std::string, std::vector<uint8_t>> inline_hashes;
  collect_leaf_hashes(internal_db, hash_root_inline, "", inline_hashes);

  // Run parallel hash update
  TestPool pool(4);
  _PoolExecutor exec(pool);
  offset_t hash_root_parallel{};
  run_hash_update_parallel(internal_db, exec, txn->root, &hash_root_parallel);
  pool.wait_all();

  // Collect parallel hashes
  std::map<std::string, std::vector<uint8_t>> parallel_hashes;
  collect_leaf_hashes(internal_db, hash_root_parallel, "", parallel_hashes);

  // Verify same number of leaves
  BOOST_CHECK_EQUAL(inline_hashes.size(), parallel_hashes.size());

  // Verify all hashes match
  for (auto& [path, hash] : inline_hashes) {
    auto it = parallel_hashes.find(path);
    BOOST_REQUIRE_MESSAGE(it != parallel_hashes.end(),
                          "Missing parallel hash for path: " + path);
    BOOST_CHECK_EQUAL_COLLECTIONS(hash.begin(), hash.end(),
                                  it->second.begin(), it->second.end());
  }

  // Verify root hashes match
  using HashTrieNode = _TrieNode<HashTrieTraits<Traits>>;
  auto inline_root = internal_db->template resolve<HashTrieNode>(&hash_root_inline);
  auto parallel_root = internal_db->template resolve<HashTrieNode>(&hash_root_parallel);
  BOOST_CHECK_EQUAL_COLLECTIONS(
      inline_root->hash, inline_root->hash + HASH_SIZE,
      parallel_root->hash, parallel_root->hash + HASH_SIZE);

  // Verify structure
  BOOST_CHECK(verify_structure(internal_db, internal_db, txn->root, hash_root_parallel));
}

BOOST_AUTO_TEST_CASE(parallel_matches_inline_deep_trie) {
  // Create a deep trie with moderate branching at multiple levels
  std::remove(TEST_FILE);
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  std::vector<std::string> keys = {
    "aaa_111_x", "aaa_111_y", "aaa_222_x", "aaa_222_y",
    "aaa_333_x", "aaa_333_y", "bbb_111_x", "bbb_111_y",
    "bbb_222_x", "bbb_222_y", "ccc_111_x", "ccc_111_y",
    "ddd_111_x", "ddd_111_y", "eee_111_x", "eee_111_y"
  };

  for (const auto& key : keys) {
    auto cursor = db.cursor();
    cursor.find(key);
    cursor.value("v_" + key);
    cursor.commit();
  }

  auto txn = internal_db->txn();

  // Inline hashes
  offset_t hash_root_inline{};
  run_hash_update(internal_db, txn->root, &hash_root_inline, tid_t(0));

  std::map<std::string, std::vector<uint8_t>> inline_hashes;
  collect_leaf_hashes(internal_db, hash_root_inline, "", inline_hashes);

  // Parallel hashes
  TestPool pool(4);
  _PoolExecutor exec(pool);
  offset_t hash_root_parallel{};
  run_hash_update_parallel(internal_db, exec, txn->root, &hash_root_parallel);
  pool.wait_all();

  std::map<std::string, std::vector<uint8_t>> parallel_hashes;
  collect_leaf_hashes(internal_db, hash_root_parallel, "", parallel_hashes);

  BOOST_CHECK_EQUAL(inline_hashes.size(), parallel_hashes.size());
  for (auto& [path, hash] : inline_hashes) {
    auto it = parallel_hashes.find(path);
    BOOST_REQUIRE_MESSAGE(it != parallel_hashes.end(),
                          "Missing parallel hash for path: " + path);
    BOOST_CHECK_EQUAL_COLLECTIONS(hash.begin(), hash.end(),
                                  it->second.begin(), it->second.end());
  }
}

BOOST_AUTO_TEST_CASE(parallel_incremental_update) {
  // Test parallel incremental update after modifications
  std::remove(TEST_FILE);
  auto storage = MapStorage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto* internal_db = db._internal();

  // Create initial wide trie
  for (int i = 0; i < 20; i++) {
    auto cursor = db.cursor();
    std::string key(1, 'a' + i);
    key += "_data";
    cursor.find(key);
    cursor.value("initial_" + std::to_string(i));
    cursor.commit();
  }

  auto txn1 = internal_db->txn();

  // Initial inline hash
  offset_t hash_root{};
  run_hash_update(internal_db, txn1->root, &hash_root, tid_t(0));

  // Modify some keys
  for (int i = 0; i < 5; i++) {
    auto cursor = db.cursor();
    std::string key(1, 'a' + i);
    key += "_data";
    cursor.find(key);
    cursor.value("modified_" + std::to_string(i));
    cursor.commit();
  }
  // Add new keys
  for (int i = 20; i < 26; i++) {
    auto cursor = db.cursor();
    std::string key(1, 'a' + i);
    key += "_data";
    cursor.find(key);
    cursor.value("new_" + std::to_string(i));
    cursor.commit();
  }

  auto txn2 = internal_db->txn();

  // Inline incremental update
  offset_t hash_root_inline = hash_root;
  run_hash_update(internal_db, txn2->root, &hash_root_inline, tid_t(0));

  std::map<std::string, std::vector<uint8_t>> inline_hashes;
  collect_leaf_hashes(internal_db, hash_root_inline, "", inline_hashes);

  // Parallel incremental update (from same starting hash trie)
  TestPool pool(4);
  _PoolExecutor exec(pool);
  offset_t hash_root_parallel = hash_root;
  run_hash_update_parallel(internal_db, exec, txn2->root, &hash_root_parallel);
  pool.wait_all();

  std::map<std::string, std::vector<uint8_t>> parallel_hashes;
  collect_leaf_hashes(internal_db, hash_root_parallel, "", parallel_hashes);

  BOOST_CHECK_EQUAL(inline_hashes.size(), parallel_hashes.size());
  for (auto& [path, hash] : inline_hashes) {
    auto it = parallel_hashes.find(path);
    BOOST_REQUIRE_MESSAGE(it != parallel_hashes.end(),
                          "Missing parallel hash for path: " + path);
    BOOST_CHECK_EQUAL_COLLECTIONS(hash.begin(), hash.end(),
                                  it->second.begin(), it->second.end());
  }
}

#endif  // LEAVES_HAS_THREADS
