#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE hash_lookup
#include <boost/test/included/unit_test.hpp>

#include <blake3.h>
#include <cstring>

#include "../include/leaves/intern/replication/_hash.hpp"
#include "../include/leaves/intern/replication/_replication_db.hpp"
#include "../include/leaves/mmap.hpp"
#include "../include/leaves/replication.hpp"
#include "../include/leaves/intern/replication/_replication_db.hpp"

using namespace leaves;

#define TEST_FILE "test_hash_lookup.lvs"

struct HashLookupPreparation {
  HashLookupPreparation() { std::remove(TEST_FILE); }
  ~HashLookupPreparation() { std::remove(TEST_FILE); }
};

BOOST_GLOBAL_FIXTURE(HashLookupPreparation);

// Per-test fixture to ensure each test starts with a fresh file
struct FreshFile {
  FreshFile() { std::remove(TEST_FILE); }
};

typedef MapStorage Storage;
typedef _ReplicationDB<Storage::StorageImpl> InternalDB;
typedef InternalDB::CursorTraits CursorTraits;
typedef InternalDB::Traits Traits;
typedef _TransactionalCursor<CursorTraits> InternalCursor;

// Hash trie types
typedef HashTrieTraits<CursorTraits> HTraits;
typedef _TrieNode<HTraits> HTrieNode;
typedef _LeafNode<HTraits> HLeafNode;
typedef _TrieNode<CursorTraits> DTrieNode;
typedef _LeafNode<CursorTraits> DLeafNode;

/**
 * Helper: run _HashUpdater within a transaction context.
 */
template <typename DB>
void build_hash_trie(DB* idb, typename DB::offset_e data_root,
                     typename DB::offset_e* hash_root_ptr) {
  InternalCursor cursor(idb, hash_root_ptr);
  cursor.start_transaction();
  std::string key_path;
  key_path.reserve(255);
  _HashUpdater<DB, DB> updater(idb, idb);
  updater.sync_nodes(key_path, data_root, hash_root_ptr);
  cursor.commit(idb->new_cursor_id());
}

/**
 * Helper: insert key/value and commit.
 */
void insert(TDB<Storage, _ReplicationDB>& db, const std::string& key,
            const std::string& value) {
  auto cursor = db.cursor();
  cursor.find(key);
  cursor.value(value);
  cursor.commit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Recursive verifier: walks data+hash tries in parallel, building the path
// by only concatenating compressed prefixes and the leaf key.
//
// Branch bytes are NOT pushed separately — every child's first compressed
// byte (for tries) or leaf key[0] (for leaves) IS the branch byte.
// Path = join(compressed_1, compressed_2, ..., leaf_key).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Walk data/hash trie in parallel, verify _HashLookup at every node.
 *
 * Path is built by concatenating compressed prefixes from each trie level.
 * At a leaf, the hash leaf key (0 or 1 bytes) is appended — it contains the
 * branch byte (or nothing for NONE).  No separate branch-byte push needed.
 */
template <typename DB>
void verify_lookup_recursive(DB* db,
                             typename DB::offset_e data_offset,
                             typename DB::offset_e hash_offset,
                             _HashLookup<DB>& lookup,
                             std::string& path,
                             int& checked) {
  if (!data_offset) return;

  if (data_offset.type() == LEAF) {
    // Append hash leaf key (0 bytes for NONE, 1 byte = branch char otherwise).
    // This completes the path: concat(compressed parts) + leaf_key.
    auto hash_leaf = db->template resolve<HLeafNode>(&hash_offset);
    size_t saved = path.size();
    path.append((const char*)hash_leaf->data, hash_leaf->key_size);

    const uint8_t* h = lookup.find(path, LEAF);
    BOOST_CHECK_MESSAGE(h != nullptr,
                        "Leaf hash not found at path '" << path << "'");
    if (h) {
      BOOST_CHECK_MESSAGE(memcmp(h, hash_leaf->hash, HASH_SIZE) == 0,
                          "Hash mismatch at path '" << path << "'");
    }
    ++checked;
    path.resize(saved);
    return;
  }

  // Trie node: append compressed prefix (first byte IS the branch byte
  // from the parent, so it's implicitly included), then recurse.
  auto data_trie = db->template resolve<DTrieNode>(&data_offset);
  auto hash_trie = db->template resolve<HTrieNode>(&hash_offset);

  size_t saved = path.size();
  path.append((const char*)data_trie->compressed(), data_trie->len());

  // Verify TRIE hash lookup: path is now the full accumulated compressed
  // from root to this trie node.
  {
    const uint8_t* h = lookup.find(path, TRIE);
    BOOST_CHECK_MESSAGE(h != nullptr,
                        "Trie hash not found at path '" << path << "'");
    if (h) {
      BOOST_CHECK_MESSAGE(memcmp(h, hash_trie->hash, HASH_SIZE) == 0,
                          "Trie hash mismatch at path '" << path << "'");
    }
  }

  data_trie->for_each_branch([&](int k, auto* off) {
    auto data_child = *off;
    auto hash_child = *hash_trie->offset(k);

    // No branch byte push — child's compressed[0] or leaf key[0] IS it
    verify_lookup_recursive(db, data_child, hash_child, lookup, path, checked);
  });

  path.resize(saved);
}

/**
 * @brief Top-level verifier: handles root-is-leaf case.
 */
template <typename DB>
int verify_all_lookups(DB* db,
                       typename DB::offset_e data_root,
                       typename DB::offset_e hash_root,
                       _HashLookup<DB>& lookup) {
  std::string path;
  int checked = 0;

  if (!data_root) return 0;

  if (data_root.type() == LEAF) {
    // Root is a leaf — path is just the hash leaf key (branch byte or empty).
    auto hash_leaf = db->template resolve<HLeafNode>(&hash_root);
    path.append((const char*)hash_leaf->data, hash_leaf->key_size);

    const uint8_t* h = lookup.find(path, LEAF);
    BOOST_CHECK_MESSAGE(h != nullptr, "Root leaf hash not found");
    if (h) {
      BOOST_CHECK(memcmp(h, hash_leaf->hash, HASH_SIZE) == 0);
    }
    ++checked;
  } else {
    // Root is a trie — start with empty path; compressed prefix of root
    // is appended inside verify_lookup_recursive.
    verify_lookup_recursive(db, data_root, hash_root, lookup, path, checked);
  }

  return checked;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(lookup_empty_hash_trie, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  auto* idb = db._internal();

  offset_t hash_root{};
  _HashLookup<InternalDB> lookup(idb, &hash_root);

  BOOST_CHECK(lookup.find("anything", LEAF) == nullptr);
  BOOST_CHECK(lookup.find("anything", TRIE) == nullptr);
  BOOST_CHECK(lookup.find("", LEAF) == nullptr);
  BOOST_CHECK(lookup.find("", TRIE) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(lookup_single_leaf, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "hello", "world");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 1);
}

BOOST_FIXTURE_TEST_CASE(lookup_two_divergent_keys, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "aaa", "v1");
  insert(db, "bbb", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);
}

BOOST_FIXTURE_TEST_CASE(lookup_shared_prefix, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "apple", "v1");
  insert(db, "apply", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);
}

BOOST_FIXTURE_TEST_CASE(lookup_many_keys, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");

  std::vector<std::string> keys = {"apple", "application", "apply",
                                    "banana", "bandana", "cat",
                                    "car", "card"};
  for (auto& k : keys)
    insert(db, k, k + "_val");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, (int)keys.size());

  // Non-existent
  BOOST_CHECK(lookup.find("dog", LEAF) == nullptr);
  BOOST_CHECK(lookup.find("app", LEAF) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(lookup_exact_match_none_branch, FreshFile) {
  // Create a structure where one key is a prefix of another,
  // producing a NONE-branch leaf.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "hello", "v1");
  insert(db, "hellox", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);
}

BOOST_FIXTURE_TEST_CASE(lookup_deterministic_hash, FreshFile) {
  uint8_t hash1[HASH_SIZE], hash2[HASH_SIZE];

  std::remove(TEST_FILE);
  {
    auto storage = Storage::create(TEST_FILE);
    auto db = storage->open<Storage::ReplicationDB>("test");
    insert(db, "testkey", "testvalue");

    auto* idb = db._internal();
    offset_t hash_root{};
    build_hash_trie(idb, idb->txn()->root, &hash_root);

    _HashLookup<InternalDB> lookup(idb, &hash_root);

    // Root is single leaf — hash leaf key = first byte of data key
    auto hash_leaf = idb->template resolve<HLeafNode>(&hash_root);
    std::string path((const char*)hash_leaf->data, hash_leaf->key_size);
    auto* h = lookup.find(path, LEAF);
    BOOST_REQUIRE(h);
    memcpy(hash1, h, HASH_SIZE);
  }

  std::remove(TEST_FILE);
  {
    auto storage = Storage::create(TEST_FILE);
    auto db = storage->open<Storage::ReplicationDB>("test");
    insert(db, "testkey", "testvalue");

    auto* idb = db._internal();
    offset_t hash_root{};
    build_hash_trie(idb, idb->txn()->root, &hash_root);

    _HashLookup<InternalDB> lookup(idb, &hash_root);
    auto hash_leaf = idb->template resolve<HLeafNode>(&hash_root);
    std::string path((const char*)hash_leaf->data, hash_leaf->key_size);
    auto* h = lookup.find(path, LEAF);
    BOOST_REQUIRE(h);
    memcpy(hash2, h, HASH_SIZE);
  }

  BOOST_CHECK_EQUAL_COLLECTIONS(hash1, hash1 + HASH_SIZE,
                                hash2, hash2 + HASH_SIZE);
}

BOOST_FIXTURE_TEST_CASE(lookup_different_values_different_hashes, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "key_a", "value_A");
  insert(db, "key_b", "value_B");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);

  // Walk the trie to find leaf paths and verify hashes differ
  auto txn = idb->txn();
  if (txn->root.type() == TRIE) {
    auto trie = idb->template resolve<DTrieNode>(&txn->root);
    auto htrie = idb->template resolve<HTrieNode>(&hash_root);
    std::string base((const char*)trie->compressed(), trie->len());

    std::vector<const uint8_t*> hashes;
    trie->for_each_branch([&](int k, auto* off) {
      auto data_child = *off;
      auto hash_child = *htrie->offset(k);
      if (data_child.type() == LEAF) {
        // Build path: compressed + hash leaf key
        auto hleaf = idb->template resolve<HLeafNode>(&hash_child);
        std::string p = base;
        p.append((const char*)hleaf->data, hleaf->key_size);
        auto* h = lookup.find(p, LEAF);
        if (h) hashes.push_back(h);
      }
    });
    if (hashes.size() >= 2) {
      BOOST_CHECK(memcmp(hashes[0], hashes[1], HASH_SIZE) != 0);
    }
  }
}

BOOST_FIXTURE_TEST_CASE(lookup_after_update, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "alpha", "v1");
  insert(db, "beta", "v2");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);
  int n1 = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n1, 2);

  // Insert a 3rd key, rebuild hash trie
  insert(db, "gamma", "v3");
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  // Same lookup object should work (root pointer updated in-place)
  int n2 = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n2, 3);
}

BOOST_FIXTURE_TEST_CASE(lookup_after_delete, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "keep", "v1");
  insert(db, "remove_me", "v2");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);
  int n1 = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n1, 2);

  // Delete one key, rebuild
  {
    auto cursor = db.cursor();
    cursor.find("remove_me");
    BOOST_REQUIRE(cursor.is_valid());
    cursor.remove();
    cursor.commit();
  }
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  int n2 = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n2, 1);
}

BOOST_FIXTURE_TEST_CASE(lookup_set_root, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db1 = storage->open<Storage::ReplicationDB>("db1");
  auto db2 = storage->open<Storage::ReplicationDB>("db2");

  insert(db1, "only_in_db1", "v1");
  insert(db2, "only_in_db2", "v2");

  auto* idb1 = db1._internal();
  auto* idb2 = db2._internal();

  offset_t hash_root1{}, hash_root2{};
  build_hash_trie(idb1, idb1->txn()->root, &hash_root1);
  build_hash_trie(idb2, idb2->txn()->root, &hash_root2);

  _HashLookup<InternalDB> lookup(idb1, &hash_root1);

  int n1 = verify_all_lookups(idb1, idb1->txn()->root, hash_root1, lookup);
  BOOST_CHECK_EQUAL(n1, 1);

  // Switch to db2's hash trie
  lookup.set_root(&hash_root2);
  int n2 = verify_all_lookups(idb2, idb2->txn()->root, hash_root2, lookup);
  BOOST_CHECK_EQUAL(n2, 1);

  // Switch back
  lookup.set_root(&hash_root1);
  int n3 = verify_all_lookups(idb1, idb1->txn()->root, hash_root1, lookup);
  BOOST_CHECK_EQUAL(n3, 1);
}

BOOST_FIXTURE_TEST_CASE(lookup_long_shared_prefix, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");

  insert(db, "prefix_shared_abc", "v1");
  insert(db, "prefix_shared_def", "v2");
  insert(db, "prefix_shared_abc_more", "v3");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 3);
}

BOOST_FIXTURE_TEST_CASE(lookup_repeated_calls, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "aaa", "v1");
  insert(db, "bbb", "v2");
  insert(db, "ccc", "v3");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  for (int round = 0; round < 3; ++round) {
    int n = verify_all_lookups(idb, idb->txn()->root, hash_root, lookup);
    BOOST_CHECK_EQUAL(n, 3);
  }

  // Non-existent
  BOOST_CHECK(lookup.find("ddd", LEAF) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(lookup_trie_root_hash, FreshFile) {
  // When the root trie has empty compressed prefix, find("", TRIE)
  // should return its hash.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");

  // Two keys with different first bytes → root trie has empty compressed
  insert(db, "aaa", "v1");
  insert(db, "bbb", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);
  BOOST_REQUIRE(hash_root.type() == TRIE);

  _HashLookup<InternalDB> lookup(idb, &hash_root);
  auto htrie = idb->template resolve<HTrieNode>(&hash_root);

  // Root trie with empty compressed: find("", TRIE) should work
  if (htrie->len() == 0) {
    const uint8_t* h = lookup.find("", TRIE);
    BOOST_CHECK_MESSAGE(h != nullptr,
                        "Root trie hash with empty compressed not found");
    if (h) {
      BOOST_CHECK(memcmp(h, htrie->hash, HASH_SIZE) == 0);
    }
  }
}

BOOST_FIXTURE_TEST_CASE(lookup_leaf_key_longer_than_one, FreshFile) {
  // Regression: when a data leaf has key_size > 1 (e.g. "ication" after
  // splitting "application"), the hash leaf key is still only 1 byte
  // (the branch byte 'i'). The lookup path must use that 1 byte, not
  // the full data leaf key.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");

  // "apple" and "application" share prefix "appl", branch 'e' vs 'i'.
  // Leaf for "application" has data key "ication" (7 bytes) in data trie
  // but hash leaf key is just "i" (1 byte).
  insert(db, "apple", "v1");
  insert(db, "application", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);
}

BOOST_FIXTURE_TEST_CASE(lookup_trie_with_shared_prefix, FreshFile) {
  // Two keys sharing a prefix produce a trie node with multi-byte compressed.
  // find(accumulated_compressed, TRIE) must return that trie's hash.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");

  // "apple", "apply" → trie compressed="appl" with branches 'e' and 'y'
  insert(db, "apple", "v1");
  insert(db, "apply", "v2");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);
  BOOST_REQUIRE(hash_root.type() == TRIE);

  auto htrie = idb->template resolve<HTrieNode>(&hash_root);
  std::string root_compressed((const char*)htrie->compressed(), htrie->len());

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  // find(full_compressed, TRIE) should return the root trie hash
  const uint8_t* h = lookup.find(root_compressed, TRIE);
  BOOST_CHECK_MESSAGE(h != nullptr,
                      "Trie hash not found for compressed='" << root_compressed << "'");
  if (h) {
    BOOST_CHECK(memcmp(h, htrie->hash, HASH_SIZE) == 0);
  }
}

BOOST_FIXTURE_TEST_CASE(lookup_nested_trie_hashes, FreshFile) {
  // Multi-level trie: verify that find(path, TRIE) works at every level.
  // Keys: "abc", "abd", "xyz" → root trie (compressed="") with branches
  //   'a' → sub-trie (compressed="ab") with branches 'c','d'
  //   'x' → leaf "xyz"
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "abc", "v1");
  insert(db, "abd", "v2");
  insert(db, "xyz", "v3");

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);
  BOOST_REQUIRE(hash_root.type() == TRIE);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  // Root trie
  auto htrie = idb->template resolve<HTrieNode>(&hash_root);
  std::string root_comp((const char*)htrie->compressed(), htrie->len());

  const uint8_t* h = lookup.find(root_comp, TRIE);
  BOOST_REQUIRE_MESSAGE(h != nullptr, "Root trie hash not found");
  BOOST_CHECK(memcmp(h, htrie->hash, HASH_SIZE) == 0);

  // Walk to the 'a' branch sub-trie (should be compressed "ab")
  if (htrie->isset('a')) {
    auto child_off = *htrie->offset('a');
    if (child_off.type() == TRIE) {
      auto sub_trie = idb->template resolve<HTrieNode>(&child_off);
      std::string sub_path = root_comp;
      sub_path.append((const char*)sub_trie->compressed(), sub_trie->len());

      const uint8_t* h2 = lookup.find(sub_path, TRIE);
      BOOST_CHECK_MESSAGE(h2 != nullptr,
                          "Sub-trie hash not found at path '" << sub_path << "'");
      if (h2) {
        BOOST_CHECK(memcmp(h2, sub_trie->hash, HASH_SIZE) == 0);
      }
    }
  }

  // verify_all_lookups now checks all trie AND leaf hashes
  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 3);
}

BOOST_FIXTURE_TEST_CASE(lookup_trie_with_none_branch, FreshFile) {
  // When a trie has a NONE-branch leaf (key is prefix of another key),
  // find(path, TRIE) should still work — it finds the trie via the
  // parent frame after cursor follows the NONE branch into the leaf.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "ab", "v1");     // NONE-branch leaf under trie "ab"
  insert(db, "abc", "v2");    // branch 'c' under trie "ab"

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  // The root trie compressed should be "ab" with a NONE-branch leaf.
  // find("ab", TRIE) must return the trie hash via Case 2 (NONE→leaf→parent).
  auto htrie = idb->template resolve<HTrieNode>(&hash_root);
  std::string comp((const char*)htrie->compressed(), htrie->len());
  BOOST_REQUIRE_EQUAL(comp, "ab");

  const uint8_t* h = lookup.find("ab", TRIE);
  BOOST_CHECK_MESSAGE(h != nullptr,
                      "Trie hash with NONE branch not found at 'ab'");
  if (h) {
    BOOST_CHECK(memcmp(h, htrie->hash, HASH_SIZE) == 0);
  }

  // Full walk should verify both leaves and the trie
  int n = verify_all_lookups(idb, txn->root, hash_root, lookup);
  BOOST_CHECK_EQUAL(n, 2);
}

BOOST_FIXTURE_TEST_CASE(lookup_trie_nonexistent_path, FreshFile) {
  // find(path, TRIE) for a path that doesn't correspond to any trie
  // boundary may return a non-matching hash (harmlessly rejected by the
  // caller's hash comparison) or nullptr.  Either way, the caller won't
  // incorrectly prune because the hash won't match.
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "apple", "v1");
  insert(db, "apply", "v2");

  auto* idb = db._internal();
  offset_t hash_root{};
  build_hash_trie(idb, idb->txn()->root, &hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  // Compute the actual trie hash at "appl" for comparison
  auto htrie = idb->template resolve<HTrieNode>(&hash_root);
  const uint8_t* trie_hash = htrie->hash;

  // "ap" is mid-compressed of "appl" → not a trie boundary.
  // Returns the "appl" trie hash (nearest trie), which won't match any
  // wire hash for path "ap".
  const uint8_t* h_ap = lookup.find("ap", TRIE);
  if (h_ap) {
    BOOST_CHECK(memcmp(h_ap, trie_hash, HASH_SIZE) == 0);
  }
  // "appli" is past the trie into a non-existent branch
  const uint8_t* h_appli = lookup.find("appli", TRIE);
  if (h_appli) {
    BOOST_CHECK(memcmp(h_appli, trie_hash, HASH_SIZE) == 0);
  }
  // "xyz" doesn't exist at all
  const uint8_t* h_xyz = lookup.find("xyz", TRIE);
  if (h_xyz) {
    BOOST_CHECK(memcmp(h_xyz, trie_hash, HASH_SIZE) == 0);
  }
}

/**
 * Test that _HashLookup::find(path, LEAF) works when the remaining search
 * key is longer than the hash leaf's 1-byte key.
 *
 * Hash leaves store only the branch char (data[0], key_size=1) for memory
 * efficiency.  When the cursor reaches such a leaf, _Transition::find()
 * calls get_prefix(remaining_key, leaf.data, remaining_size, 1, cmp).
 * Since remaining_size > 1, cmp != 0 — so back.success() would wrongly
 * return false.  back.is_leaf() is the correct check.
 *
 * Scenario:
 *   Keys "abc" (NONE leaf) and "abcXYZ" (branch 'X' leaf with key_size=1)
 *   → hash trie: root compressed="abc", branches NONE and 'X'
 *   → find("abcXYZ", LEAF) navigates "abc" (3 bytes consumed), then branch 'X'
 *     → hash leaf has data[0]='X', key_size=1
 *     → remaining key "XYZ" (3 bytes) vs leaf key "X" (1 byte) → cmp=1
 *     → back.success() == false, but cursor correctly reached the leaf
 */
BOOST_FIXTURE_TEST_CASE(lookup_leaf_with_multi_byte_suffix, FreshFile) {
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open<Storage::ReplicationDB>("test");
  insert(db, "abc", "v1");     // NONE-branch leaf under "abc"
  insert(db, "abcXYZ", "v2");  // branch 'X' leaf under "abc"

  auto* idb = db._internal();
  auto txn = idb->txn();
  offset_t hash_root{};
  build_hash_trie(idb, txn->root, &hash_root);
  BOOST_REQUIRE(hash_root);

  _HashLookup<InternalDB> lookup(idb, &hash_root);

  // Look up "abcXYZ" — the remaining key "XYZ" is longer than the hash
  // leaf's key_size=1 (data[0]='X').  With the old back.success() check
  // this would return nullptr.  With back.is_leaf() it returns the hash.
  auto* abcxyz_hash = lookup.find("abcXYZ", LEAF);
  BOOST_REQUIRE_MESSAGE(abcxyz_hash != nullptr,
                        "Leaf hash for 'abcXYZ' found via is_leaf()");

  // Verify the returned hash is non-zero
  bool non_zero = false;
  for (size_t i = 0; i < HASH_SIZE; ++i) {
    if (abcxyz_hash[i] != 0) { non_zero = true; break; }
  }
  BOOST_CHECK(non_zero);

  // NONE-branch leaf for "abc" should still work
  auto* abc_hash = lookup.find("abc", LEAF);
  BOOST_REQUIRE_MESSAGE(abc_hash != nullptr,
                        "Leaf hash for 'abc' (NONE-branch) found");
  BOOST_CHECK(abc_hash != abcxyz_hash);  // different keys → different hashes
}
