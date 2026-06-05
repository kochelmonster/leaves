#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE merger
// #define GENERATE
#include <boost/test/included/unit_test.hpp>

#include <random>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../include/leaves/intern/db/_cursor.hpp"
#include "../include/leaves/intern/util/_merger.hpp"
#include "../include/leaves/confluence.hpp"
#include "test.hpp"

using namespace leaves;

typedef MapStorage Storage;
typedef _DB<Storage::StorageImpl> InternalDB;
typedef InternalDB::CursorTraits CursorTraits;
typedef _TransactionalCursor<CursorTraits> InternalCursor;

// Version with dumper
template <typename DBDst, typename DBSrc, typename MergePolicy>
void exec_merger(DBDst& dst_db, DBSrc& src_db, MergePolicy& handler,
                 const std::string& dump_filename = "") {
  using DstCursorTraits = typename DBDst::CursorTraits;
  using SrcCursorTraits = typename DBSrc::CursorTraits;
  using DstCursor = _TransactionalCursor<DstCursorTraits>;
  using SrcCursor = _Cursor<SrcCursorTraits>;

  auto src_root = &src_db.txn()->root;
  auto dst_root = &dst_db.txn()->root;

  uint64_t cursor_id = dst_db.new_cursor_id();

  // Create cursors
  DstCursor dst_cursor(&dst_db, dst_root);
  SrcCursor src_cursor(&src_db, src_root);

  // Initialize source cursor to start of tree
  src_cursor.clear();
  dst_cursor.start_transaction();

  // Note: Dumper functionality may need to be reimplemented for cursor-based
  // API
  _Merger<DstCursor, SrcCursor, MergePolicy> merger(dst_cursor, src_cursor,
                                                    handler);
  merger.exec();
  dst_cursor.commit(cursor_id);

  if (!dump_filename.empty()) {
    std::ofstream out(dump_filename);
    _Dumper(dst_db, &dst_db.txn()->root, false).dump(out);
    out.close();
    std::cout << "Final tree dumped to: " << dump_filename << std::endl;
  }
}

#if LEAVES_HAS_THREADS
template <typename DBDst, typename DBSrc, typename MergePolicy>
void exec_merger_threaded_path(DBDst& dst_db, DBSrc& src_db, MergePolicy& handler) {
  exec_merger(dst_db, src_db, handler);
}
#endif  // LEAVES_HAS_THREADS

// Extended preparation that also cleans up the second test file
struct MergerPreparation {
  MergerPreparation() {
    std::remove(TEST_FILE);
    std::remove(TEST_FILE "2");
  }
  ~MergerPreparation() {
    std::remove(TEST_FILE);
    std::remove(TEST_FILE "2");
  }
};

// Simple handler for testing - always overwrites
struct OverwritePolicy {
  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return true;  // Always overwrite
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return true;  // Always add
  }

  bool may_add_trie(const std::string& key) {
    return true;  // Always recurse
  }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {
    // No-op for testing - in real usage would free big value memory
  }

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    dst_cursor.get_bigmemory().alloc(value_size, &_big_value_storage);
    offset_t dst_offset(_big_value_storage.chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

// Handler that keeps destination values
struct KeepDestPolicy {
  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return false;  // Never overwrite
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return true;  // Always add
  }

  bool may_add_trie(const std::string& key) {
    return true;  // Always recurse
  }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {
    // No-op for testing
  }

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    dst_cursor.get_bigmemory().alloc(value_size, &_big_value_storage);
    offset_t dst_offset(_big_value_storage.chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

// Test actual _Merger functionality

BOOST_AUTO_TEST_CASE(test_merger_empty_to_empty) {
  // Merge empty source into empty destination
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_internal = src_storage->open("test")._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  // Both are empty - merger should handle this
  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Both should still be empty
  auto dst_cursor = dest_storage->open("test").cursor();
  dst_cursor.first();
  BOOST_CHECK(!dst_cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_single_leaf_to_empty) {
  // Merge single leaf into empty destination
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("value1");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify destination has the value
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find("key1");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("value1"));
}

BOOST_AUTO_TEST_CASE(test_merger_single_leaf_same_key) {
  // Merge single leaf into destination with same key (overwrite scenario)
  // This tests the may_overwrite path in split_trie when both src and dst
  // are single-node tries with identical keys
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create source with single key
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("samekey");
  src_cursor_pub.value("src_value");
  src_cursor_pub.commit();

  // Create destination with same key but different value
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("samekey");
  dst_cursor_pub.value("dst_value");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify value was overwritten
  auto dst_cursor_pub2 = dest_storage->open("test").cursor();
  dst_cursor_pub2.find("samekey");
  BOOST_CHECK(dst_cursor_pub2.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub2.value(), Slice("src_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_multiple_leaves_to_empty) {
  // Merge multiple leaves into empty destination
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();

  src_cursor_pub.find("apple");
  src_cursor_pub.value("fruit1");
  src_cursor_pub.find("banana");
  src_cursor_pub.value("fruit2");
  src_cursor_pub.find("cherry");
  src_cursor_pub.value("fruit3");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all values in destination
  auto dst_cursor_pub = dest_storage->open("test").cursor();

  dst_cursor_pub.find("apple");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("fruit1"));

  dst_cursor_pub.find("banana");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("fruit2"));

  dst_cursor_pub.find("cherry");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("fruit3"));
}

BOOST_AUTO_TEST_CASE(test_merger_overwrite_existing) {
  // Test overwriting existing values
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("new_value");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key1");
  dst_cursor_pub.value("old_value");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify value was overwritten - need fresh cursor to see committed changes
  auto dst_cursor_pub2 = dest_storage->open("test").cursor();
  dst_cursor_pub2.find("key1");
  BOOST_CHECK(dst_cursor_pub2.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub2.value(), Slice("new_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_keep_destination) {
  // Test keeping destination values
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("new_value");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key1");
  dst_cursor_pub.value("old_value");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify value was NOT overwritten
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage->open("test").cursor();
  dst_cursor_verify.find("key1");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("old_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_disjoint_keys) {
  // Merge with disjoint key sets
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("value1");
  src_cursor_pub.find("key2");
  src_cursor_pub.value("value2");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key3");
  dst_cursor_pub.value("value3");
  dst_cursor_pub.find("key4");
  dst_cursor_pub.value("value4");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all four keys exist
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage->open("test").cursor();
  dst_cursor_verify.find("key1");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("value1"));

  dst_cursor_verify.find("key2");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("value2"));

  dst_cursor_verify.find("key3");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("value3"));

  dst_cursor_verify.find("key4");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("value4"));
}

BOOST_AUTO_TEST_CASE(test_merger_prefix_keys) {
  // Test merging keys with common prefixes
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("prefix_1");
  src_cursor_pub.value("val1");
  src_cursor_pub.find("prefix_2");
  src_cursor_pub.value("val2");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("prefix_3");
  dst_cursor_pub.value("val3");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all values
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage->open("test").cursor();

  dst_cursor_verify.find("prefix_1");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("val1"));

  dst_cursor_verify.find("prefix_2");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("val2"));

  dst_cursor_verify.find("prefix_3");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("val3"));
}

BOOST_AUTO_TEST_CASE(test_merger_big_values) {
  // Test merging with large values
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  std::string big_value(5000, 'X');

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("bigkey");
  src_cursor_pub.value(big_value);
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify big value was copied
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find("bigkey");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice(big_value));
}

BOOST_AUTO_TEST_CASE(test_merger_big_keys) {
  // Test merging with long keys (> 255 bytes)
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  std::string big_key(260, 'K');

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find(big_key);
  src_cursor_pub.value("bigkey_value");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify big key was copied
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find(big_key);
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("bigkey_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_compressed_trie_nodes) {
  // Test merging with compressed trie nodes
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();

  // Create keys that will form compressed tries
  src_cursor_pub.find("abcdefghi");
  src_cursor_pub.value("val1");
  src_cursor_pub.find("abcdefghj");
  src_cursor_pub.value("val2");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify values
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find("abcdefghi");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("val1"));

  dst_cursor_pub.find("abcdefghj");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("val2"));
}

BOOST_AUTO_TEST_CASE(test_merger_split_scenarios) {
  // Test merge with split scenarios (different prefix lengths)
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abc");
  src_cursor_pub.value("src_abc");
  src_cursor_pub.find("abcdef");
  src_cursor_pub.value("src_abcdef");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abd");
  dst_cursor_pub.value("dst_abd");
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all values
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage->open("test").cursor();
  dst_cursor_verify.find("abc");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("src_abc"));

  dst_cursor_verify.find("abcdef");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("src_abcdef"));

  dst_cursor_verify.find("abd");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("dst_abd"));
}

BOOST_AUTO_TEST_CASE(test_merger_minimal_debug) {
  // Minimal test to debug the suffix issue
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();

  // Insert just 3 keys that should split
  src_cursor_pub.find("key_0");
  src_cursor_pub.value("value_0");
  src_cursor_pub.find("key_1");
  src_cursor_pub.value("value_1");
  src_cursor_pub.find("key_10");
  src_cursor_pub.value("value_10");
  src_cursor_pub.commit();

  std::cout << "\n=== Source keys ===" << std::endl;
  src_cursor_pub.first();
  while (src_cursor_pub.is_valid()) {
    std::string key(src_cursor_pub.key().data(), src_cursor_pub.key().size());
    std::string value(src_cursor_pub.value().data(),
                      src_cursor_pub.value().size());
    std::cout << "  " << key << " = " << value << std::endl;
    src_cursor_pub.next();
  }

  auto src_internal = src_db._internal();
  auto dst_db = dest_storage->open("test");
  auto dst_internal = dst_db._internal();

  // Create dumper for detailed merge analysis
  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  std::cout << "\n=== Merge dumps written to: test_merger_minimal_dumps/ ==="
            << std::endl;

  // Verify destination
  auto dst_cursor_pub = dst_db.cursor();
  std::cout << "\n=== Destination keys after merge ===" << std::endl;
  dst_cursor_pub.first();
  while (dst_cursor_pub.is_valid()) {
    std::string key(dst_cursor_pub.key().data(), dst_cursor_pub.key().size());
    std::string value(dst_cursor_pub.value().data(),
                      dst_cursor_pub.value().size());
    std::cout << "  " << key << " = " << value << std::endl;
    dst_cursor_pub.next();
  }

  // Verify all 3 keys exist
  dst_cursor_pub.find("key_0");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("value_0"));

  dst_cursor_pub.find("key_1");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("value_1"));

  dst_cursor_pub.find("key_10");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("value_10"));
}

BOOST_AUTO_TEST_CASE(test_merger_large_dataset) {
  // Test with large number of keys
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();

  // Insert 200 keys into source
  for (int i = 0; i < 200; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string value = "src_value_" + std::to_string(i);
    src_cursor_pub.find(key);
    src_cursor_pub.value(value);
  }
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();

  // Insert 100 overlapping keys into destination
  for (int i = 100; i < 300; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string value = "dst_value_" + std::to_string(i);
    dst_cursor_pub.find(key);
    dst_cursor_pub.value(value);
  }
  dst_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Allocator/layout internals changed; keep this test focused on semantic
  // key/value merge correctness rather than structural dump identity.

  // Create a fresh cursor to see the merged results
  dst_cursor_pub.update();

  // First, scan all keys to see what's actually there
  std::cout << "\n=== Scanning all keys after merge ===" << std::endl;
  dst_cursor_pub.first();
  int count = 0;
  while (dst_cursor_pub.is_valid()) {
    std::string key(dst_cursor_pub.key().data(), dst_cursor_pub.key().size());
    std::string value(dst_cursor_pub.value().data(),
                      dst_cursor_pub.value().size());
    if (count < 20 || count % 50 == 0) {  // Print first 20 and every 50th
      std::cout << "Key[" << count << "]: " << key << " = " << value
                << std::endl;
    }
    count++;
    dst_cursor_pub.next();
  }
  std::cout << "Total keys found: " << count << " (expected 300)" << std::endl;

  // Verify merged results
  // Keys 0-99: from source
  for (int i = 0; i < 100; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "src_value_" + std::to_string(i);
    dst_cursor_pub.find(key);
    BOOST_CHECK(dst_cursor_pub.is_valid());
    BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice(expected));
  }

  // Keys 100-199: from source (overwritten)
  for (int i = 100; i < 200; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "src_value_" + std::to_string(i);
    dst_cursor_pub.find(key);
    BOOST_CHECK(dst_cursor_pub.is_valid());
    BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice(expected));
  }

  // Keys 200-299: from destination (unchanged)
  for (int i = 200; i < 300; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "dst_value_" + std::to_string(i);
    dst_cursor_pub.find(key);
    BOOST_CHECK(dst_cursor_pub.is_valid());
    BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice(expected));
  }
}

BOOST_AUTO_TEST_CASE(test_merger_empty_key) {
  // Test with empty key
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("");
  src_cursor_pub.value("empty_key_value");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify empty key
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find("");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("empty_key_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_null_branch) {
  // Test merging trie with NONE branch
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("ab");
  src_cursor_pub.value("ab_val");
  src_cursor_pub.find("aba");
  src_cursor_pub.value("aba_val");
  src_cursor_pub.find("abb");
  src_cursor_pub.value("abb_val");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all values including NONE branch
  auto dst_cursor_pub = dest_storage->open("test").cursor();
  dst_cursor_pub.find("ab");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("ab_val"));

  dst_cursor_pub.find("aba");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("aba_val"));

  dst_cursor_pub.find("abb");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("abb_val"));
}

BOOST_AUTO_TEST_CASE(test_merger_big_value_overwrite) {
  // Test merging with big values - covers free_big path (lines 57-58)
  // This happens when a dst leaf with big value needs to be split (line 139)
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create destination with a BIG value (> 4096 bytes triggers big value)
  // MAX_SIZE is 4K, so total leaf size must exceed that
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  std::string big_value(5000, 'X');  // Much bigger than 4K
  dst_cursor_pub.find("longkey");    // Longer key
  dst_cursor_pub.value(big_value);
  dst_cursor_pub.commit();

  // Create source that causes a split - matches only prefix of dst key
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("long");
  src_cursor_pub.value("short_val");
  src_cursor_pub.commit();

  // Merge - dst leaf "longkey" with big value will be split at "long"
  // This should call free(dst_leaf) on line 139, which should free the big
  // value
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify both keys exist
  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("long");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("short_val"));

  verify_cursor.find("longkey");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice(big_value));
}

BOOST_AUTO_TEST_CASE(test_merger_dst_trie_split) {
  // Test destination trie split - covers lines 154-159
  // This happens when dst trie has longer compressed prefix than needed
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create destination with keys that share a long prefix
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abcdefghij");
  dst_cursor_pub.value("val1");
  dst_cursor_pub.find("abcdefghik");
  dst_cursor_pub.value("val2");
  dst_cursor_pub.commit();

  // Create source with TRIE that matches only part of the prefix
  // Need multiple keys to form a trie
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abcde1");
  src_cursor_pub.value("valA");
  src_cursor_pub.find("abcde2");
  src_cursor_pub.value("valB");
  src_cursor_pub.commit();

  // Merge - destination trie "abcdefghi" will need to split at "abcde"
  // because source trie "abcde" matches only partially
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys exist
  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("abcde1");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("valA"));

  verify_cursor.find("abcde2");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("valB"));

  verify_cursor.find("abcdefghij");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val1"));

  verify_cursor.find("abcdefghik");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val2"));
}

BOOST_AUTO_TEST_CASE(test_merger_src_trie_into_dst_leaf) {
  // Test source trie merged into destination leaf - covers lines 181-196
  // Dst has "abc", src has "abcd" and "abce" (forms a trie at "abc")
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create destination with a single key "abc"
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abc");
  dst_cursor_pub.value("dst_val");
  dst_cursor_pub.commit();

  // Create source with keys that extend "abc" to form a trie
  // Need more keys to ensure we get a trie at "abc" level
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abcd");
  src_cursor_pub.value("val_d");
  src_cursor_pub.find("abce");
  src_cursor_pub.value("val_e");
  src_cursor_pub.find("abcf");
  src_cursor_pub.value("val_f");
  src_cursor_pub.commit();

  // Merge - dst leaf "abc" meets src trie "abc" with children d,e
  // This should trigger copy_src_plus_none with src.is_trie()
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto verify_cursor = dest_storage->open("test").cursor();

  verify_cursor.find("abc");
  BOOST_CHECK(verify_cursor.is_valid());  // "abc" shouldn't exist

  verify_cursor.find("abcd");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val_d"));

  verify_cursor.find("abce");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val_e"));

  verify_cursor.find("abcf");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val_f"));
}

BOOST_AUTO_TEST_CASE(test_merger_src_trie_with_none_branch) {
  // Test dst leaf "abc" merged with src trie that has NONE branch at "abc"
  // This covers lines 237-254 in split_trie when src_trie->isset(key1) where
  // key1=NONE
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create destination with single leaf "abc"
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abc");
  dst_cursor_pub.value("dst_abc");
  dst_cursor_pub.commit();

  // Create source with "abc" (creates NONE branch) plus "abcd" and "abce"
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abc");
  src_cursor_pub.value("src_abc");
  src_cursor_pub.find("abcd");
  src_cursor_pub.value("src_abcd");
  src_cursor_pub.find("abce");
  src_cursor_pub.value("src_abce");
  src_cursor_pub.commit();

  // Merge - this should trigger split_trie with src_trie->isset(NONE) = true
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys merged correctly
  auto verify_cursor = dest_storage->open("test").cursor();

  verify_cursor.find("abc");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("src_abc"));  // Overwritten

  verify_cursor.find("abcd");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("src_abcd"));

  verify_cursor.find("abce");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("src_abce"));
}

BOOST_AUTO_TEST_CASE(test_merger_src_trie_longer_prefix) {
  // Test source trie with longer prefix than destination - covers lines 222-239
  // In merge_into_trie when suffix_len > 0
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create destination trie with short prefix
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("ab1");
  dst_cursor_pub.value("val1");
  dst_cursor_pub.find("ab2");
  dst_cursor_pub.value("val2");
  dst_cursor_pub.commit();

  // Create source trie with longer prefix that extends dst's prefix
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abcde1");
  src_cursor_pub.value("valX");
  src_cursor_pub.find("abcde2");
  src_cursor_pub.value("valY");
  src_cursor_pub.commit();

  // Merge - src trie "abcde" needs to be split when inserted into dst trie "ab"
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys
  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("ab1");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val1"));

  verify_cursor.find("ab2");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("val2"));

  verify_cursor.find("abcde1");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("valX"));

  verify_cursor.find("abcde2");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("valY"));
}

// Additional KeepDestPolicy tests to achieve 100% function coverage

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_complex_trie) {
  // Test KeepDestPolicy with complex trie structure to trigger merge_into_trie
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Create multiple keys with common prefix in source
  src_cursor.find("prefix_a");
  src_cursor.value("src_a");
  src_cursor.find("prefix_b");
  src_cursor.value("src_b");
  src_cursor.find("prefix_c");
  src_cursor.value("src_c");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Destination has different keys with same prefix
  dst_cursor.find("prefix_d");
  dst_cursor.value("dst_d");
  dst_cursor.find("prefix_e");
  dst_cursor.value("dst_e");
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys exist
  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("prefix_a");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("prefix_b");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("prefix_c");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("prefix_d");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("prefix_e");
  BOOST_CHECK(verify_cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_leaf_into_trie) {
  // Test KeepDestPolicy with leaf merging into trie to trigger
  // merge_leaf_into_trie
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Source has a single longer key
  src_cursor.find("commonprefixXYZ");
  src_cursor.value("src_value");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Destination has multiple keys with shared prefix
  dst_cursor.find("commonprefix1");
  dst_cursor.value("dst1");
  dst_cursor.find("commonprefix2");
  dst_cursor.value("dst2");
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("commonprefixXYZ");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("src_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_trie_split) {
  // Test KeepDestPolicy with trie splitting to trigger expand_trie_with_branch
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Source with various keys
  src_cursor.find("abc1");
  src_cursor.value("src1");
  src_cursor.find("abd1");
  src_cursor.value("src2");
  src_cursor.find("abe1");
  src_cursor.value("src3");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Destination with different branching
  dst_cursor.find("abf1");
  dst_cursor.value("dst1");
  dst_cursor.find("abg1");
  dst_cursor.value("dst2");
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("abc1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("abd1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("abe1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("abf1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("abg1");
  BOOST_CHECK(verify_cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_deep_copy) {
  // Test KeepDestPolicy with deep copy scenarios
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Create a deep tree structure in source
  for (int i = 0; i < 20; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "val_" + std::to_string(i);
    src_cursor.find(key);
    src_cursor.value(val);
  }
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Destination has non-overlapping keys
  for (int i = 20; i < 40; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "val_" + std::to_string(i);
    dst_cursor.find(key);
    dst_cursor.value(val);
  }
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys from both sources exist
  auto verify_cursor = dest_storage->open("test").cursor();
  for (int i = 0; i < 40; i++) {
    std::string key = "key_" + std::to_string(i);
    verify_cursor.find(key);
    BOOST_CHECK(verify_cursor.is_valid());
  }
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_merge_trie_node) {
  // Test KeepDestPolicy with trie node merging
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  src_cursor.find("xyz1");
  src_cursor.value("v1");
  src_cursor.find("xyz2");
  src_cursor.value("v2");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("xyz3");
  dst_cursor.value("v3");
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("xyz1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("xyz2");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("xyz3");
  BOOST_CHECK(verify_cursor.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_big_value) {
  // Test KeepDestPolicy with big values to trigger migrate_big_value
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Create a big value in source
  std::string big_value(5000, 'X');
  src_cursor.find("bigkey");
  src_cursor.value(big_value);
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Destination has a different value for same key
  dst_cursor.find("bigkey");
  dst_cursor.value("small_dst");
  dst_cursor.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // With KeepDestPolicy, destination value should be kept
  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("bigkey");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("small_dst"));
}

BOOST_AUTO_TEST_CASE(test_merger_keep_dest_empty_to_populated) {
  // Test KeepDestPolicy merging into empty destination
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Create complex tree in source
  src_cursor.find("a1");
  src_cursor.value("v1");
  src_cursor.find("a2");
  src_cursor.value("v2");
  src_cursor.find("b1");
  src_cursor.value("v3");
  src_cursor.find("c1");
  src_cursor.value("v4");
  src_cursor.commit();

  // Destination is empty
  auto dst_db = dest_storage->open("test");

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  KeepDestPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto verify_cursor = dest_storage->open("test").cursor();
  verify_cursor.find("a1");
  BOOST_CHECK(verify_cursor.is_valid());
  BOOST_CHECK_EQUAL(verify_cursor.value(), Slice("v1"));
  verify_cursor.find("a2");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("b1");
  BOOST_CHECK(verify_cursor.is_valid());
  verify_cursor.find("c1");
  BOOST_CHECK(verify_cursor.is_valid());
}

// ── Selective merge (may_add filtering) tests ──────────────────────────

// Policy that only allows keys starting with a given prefix
struct PrefixFilterPolicy {
  std::string allowed_prefix;

  PrefixFilterPolicy(const std::string& prefix) : allowed_prefix(prefix) {}

  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return true;
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return key.substr(0, allowed_prefix.size()) == allowed_prefix;
  }

  bool may_add_trie(const std::string& key) {
    // Allow recursion if the trie path is a prefix of (or starts with) the
    // allowed prefix
    if (key.size() <= allowed_prefix.size())
      return allowed_prefix.substr(0, key.size()) == key;
    return key.substr(0, allowed_prefix.size()) == allowed_prefix;
  }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {}

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    dst_cursor.get_bigmemory().alloc(value_size, &_big_value_storage);
    offset_t dst_offset(_big_value_storage.chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

BOOST_AUTO_TEST_CASE(test_merger_may_add_filter_into_empty) {
  // Merge with filter into empty destination — only "a*" keys should survive
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("apple");
  src_cursor_pub.value("v_apple");
  src_cursor_pub.find("avocado");
  src_cursor_pub.value("v_avocado");
  src_cursor_pub.find("banana");
  src_cursor_pub.value("v_banana");
  src_cursor_pub.find("cherry");
  src_cursor_pub.value("v_cherry");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  PrefixFilterPolicy handler("a");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("apple");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_apple"));
  v.find("avocado");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_avocado"));
  v.find("banana");
  BOOST_CHECK(!v.is_valid());
  v.find("cherry");
  BOOST_CHECK(!v.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_filter_with_existing_keys) {
  // Destination already has keys; source adds new keys but only some pass
  // filter
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Populate destination
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("apple");
  dst_cursor_pub.value("dst_apple");
  dst_cursor_pub.find("date");
  dst_cursor_pub.value("dst_date");
  dst_cursor_pub.commit();

  // Populate source with overlapping and new keys
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("banana");
  src_cursor_pub.value("src_banana");
  src_cursor_pub.find("blueberry");
  src_cursor_pub.value("src_blueberry");
  src_cursor_pub.find("cherry");
  src_cursor_pub.value("src_cherry");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  // Only allow keys starting with "b"
  PrefixFilterPolicy handler("b");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  // Original dst keys should still be present
  v.find("apple");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("dst_apple"));
  v.find("date");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("dst_date"));
  // Accepted source keys
  v.find("banana");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("src_banana"));
  v.find("blueberry");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("src_blueberry"));
  // Rejected source key
  v.find("cherry");
  BOOST_CHECK(!v.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_reject_all) {
  // Filter rejects everything from source — destination should be unchanged
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Populate destination
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("keep1");
  dst_cursor_pub.value("v_keep1");
  dst_cursor_pub.find("keep2");
  dst_cursor_pub.value("v_keep2");
  dst_cursor_pub.commit();

  // Populate source
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("reject1");
  src_cursor_pub.value("v_reject1");
  src_cursor_pub.find("reject2");
  src_cursor_pub.value("v_reject2");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  // Prefix "NOMATCH" won't match anything
  PrefixFilterPolicy handler("NOMATCH");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("keep1");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_keep1"));
  v.find("keep2");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_keep2"));
  v.find("reject1");
  BOOST_CHECK(!v.is_valid());
  v.find("reject2");
  BOOST_CHECK(!v.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_reject_all_into_empty) {
  // Filter rejects everything and destination is empty — result should be empty
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("nope1");
  src_cursor_pub.value("v1");
  src_cursor_pub.find("nope2");
  src_cursor_pub.value("v2");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  PrefixFilterPolicy handler("NOMATCH");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.first();
  BOOST_CHECK(!v.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_single_leaf_rejected) {
  // Single leaf source, filter rejects it — destination unchanged
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("existing");
  dst_cursor_pub.value("v_existing");
  dst_cursor_pub.commit();

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("newkey");
  src_cursor_pub.value("v_newkey");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  PrefixFilterPolicy handler("NOMATCH");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("existing");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_existing"));
  v.find("newkey");
  BOOST_CHECK(!v.is_valid());
}

// ── may_add_trie early rejection tests ─────────────────────────────────

// Policy that tracks may_add_trie/may_add_leaf calls to verify pruning
struct TrackingFilterPolicy {
  std::string allowed_prefix;
  mutable int trie_calls = 0;
  mutable int leaf_calls = 0;

  TrackingFilterPolicy(const std::string& prefix) : allowed_prefix(prefix) {}

  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return true;
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    leaf_calls++;
    return key.substr(0, allowed_prefix.size()) == allowed_prefix;
  }

  bool may_add_trie(const std::string& key) {
    trie_calls++;
    if (key.size() <= allowed_prefix.size())
      return allowed_prefix.substr(0, key.size()) == key;
    return key.substr(0, allowed_prefix.size()) == allowed_prefix;
  }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {}

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    dst_cursor.get_bigmemory().alloc(value_size, &_big_value_storage);
    offset_t dst_offset(_big_value_storage.chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

BOOST_AUTO_TEST_CASE(test_merger_may_add_trie_prunes_subtree) {
  // Verify that may_add_trie can reject entire subtrees without visiting
  // leaves. Source has keys under "aa*" and "bb*" prefixes. Filter allows only
  // "aa". may_add_trie should reject the "bb" subtree early, so no leaf_calls
  // for "bb*".
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("aa1");
  src_cursor_pub.value("v_aa1");
  src_cursor_pub.find("aa2");
  src_cursor_pub.value("v_aa2");
  src_cursor_pub.find("bb1");
  src_cursor_pub.value("v_bb1");
  src_cursor_pub.find("bb2");
  src_cursor_pub.value("v_bb2");
  src_cursor_pub.find("bb3");
  src_cursor_pub.value("v_bb3");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  TrackingFilterPolicy handler("aa");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("aa1");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_aa1"));
  v.find("aa2");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_aa2"));
  v.find("bb1");
  BOOST_CHECK(!v.is_valid());
  v.find("bb2");
  BOOST_CHECK(!v.is_valid());
  v.find("bb3");
  BOOST_CHECK(!v.is_valid());

  // Verify that leaf_calls were NOT made for bb* keys (pruned at trie level)
  BOOST_CHECK_EQUAL(handler.leaf_calls, 2);  // only aa1, aa2
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_trie_rejects_all_subtrees) {
  // may_add_trie rejects every subtree — destination should be empty.
  // Use paired keys so every root branch is a trie (not a leaf),
  // ensuring may_add_trie prunes at the trie level without visiting leaves.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("aa1");
  src_cursor_pub.value("v1");
  src_cursor_pub.find("aa2");
  src_cursor_pub.value("v2");
  src_cursor_pub.find("bb1");
  src_cursor_pub.value("v3");
  src_cursor_pub.find("bb2");
  src_cursor_pub.value("v4");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  TrackingFilterPolicy handler("ZZZZZ");  // matches nothing
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.first();
  BOOST_CHECK(!v.is_valid());

  // No leaves should have been consulted — trie-level rejection
  BOOST_CHECK_EQUAL(handler.leaf_calls, 0);
}

// ── is_big parameter tests ─────────────────────────────────────────────

// Policy that rejects big values but accepts small ones
struct RejectBigPolicy {
  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return true;
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return !is_big;  // reject big values
  }

  bool may_add_trie(const std::string& key) { return true; }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {}

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    _BigValue* src_bvalue = (_BigValue*)leaf.vdata();
    offset_t src_offset(src_bvalue->chunk_offset);
    struct ChunkData { char data; };
    auto src_data = src_cursor._db->template resolve<ChunkData>(&src_offset, READ);
    uint32_t value_size = src_bvalue->value_size;

    dst_cursor.get_bigmemory().alloc(value_size, &_big_value_storage);
    offset_t dst_offset(_big_value_storage.chunk_offset);
    auto dst_data = dst_cursor._db->template resolve<ChunkData>(&dst_offset, WRITE);
    memcpy((char*)dst_data, (char*)src_data, value_size);

    return {Slice((uint8_t*)&_big_value_storage, sizeof(_BigValue)), true};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

BOOST_AUTO_TEST_CASE(test_merger_may_add_leaf_is_big_param) {
  // All source values are small (non-big), so is_big=false → all accepted.
  // This verifies the is_big parameter is correctly passed through.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("small_val1");
  src_cursor_pub.find("key2");
  src_cursor_pub.value("small_val2");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  RejectBigPolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("key1");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("small_val1"));
  v.find("key2");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("small_val2"));
}

// ── may_add_trie with existing destination tests ───────────────────────

BOOST_AUTO_TEST_CASE(test_merger_may_add_trie_with_existing_dst) {
  // Destination has keys. Source adds keys in multiple subtrees.
  // may_add_trie prunes one subtree; the other merges normally.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Destination has keys under "data_"
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("data_existing");
  dst_cursor_pub.value("v_existing");
  dst_cursor_pub.commit();

  // Source has keys under "data_" (should merge) and "meta_" (should be pruned)
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("data_new1");
  src_cursor_pub.value("v_new1");
  src_cursor_pub.find("data_new2");
  src_cursor_pub.value("v_new2");
  src_cursor_pub.find("meta_info1");
  src_cursor_pub.value("v_info1");
  src_cursor_pub.find("meta_info2");
  src_cursor_pub.value("v_info2");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  PrefixFilterPolicy handler("data_");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  // Original dst key preserved
  v.find("data_existing");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_existing"));
  // Accepted source keys
  v.find("data_new1");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_new1"));
  v.find("data_new2");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_new2"));
  // Rejected source keys
  v.find("meta_info1");
  BOOST_CHECK(!v.is_valid());
  v.find("meta_info2");
  BOOST_CHECK(!v.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_may_add_trie_deep_prefix_filter) {
  // Test may_add_trie with a long prefix to exercise pruning at deeper trie
  // levels. Source keys share a long common prefix with divergence deep in the
  // trie.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  // Keys that share "prefix_ab" but diverge after
  src_cursor_pub.find("prefix_abc_key1");
  src_cursor_pub.value("v1");
  src_cursor_pub.find("prefix_abc_key2");
  src_cursor_pub.value("v2");
  src_cursor_pub.find("prefix_abd_key1");
  src_cursor_pub.value("v3");
  src_cursor_pub.find("prefix_abd_key2");
  src_cursor_pub.value("v4");
  src_cursor_pub.find("prefix_xyz_key1");
  src_cursor_pub.value("v5");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  // Only allow keys starting with "prefix_abc"
  PrefixFilterPolicy handler("prefix_abc");
  exec_merger(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  v.find("prefix_abc_key1");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v1"));
  v.find("prefix_abc_key2");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v2"));
  // abd and xyz should be rejected
  v.find("prefix_abd_key1");
  BOOST_CHECK(!v.is_valid());
  v.find("prefix_abd_key2");
  BOOST_CHECK(!v.is_valid());
  v.find("prefix_xyz_key1");
  BOOST_CHECK(!v.is_valid());

  // Verify total key count
  int count = 0;
  v.first();
  while (v.is_valid()) {
    count++;
    v.next();
  }
  BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_CASE(test_merger_into_empty_leaf_root) {
  // Test merging into a trie that has only an empty leaf at its root
  // dst: "" -> "root_value" (leaf with key_size=0)
  // src: "abc" -> "abc_value"
  // Result: trie with NONE branch (dst) and 'a' branch (src)
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create dst with only empty key
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("");
  dst_cursor_pub.value("root_value");
  dst_cursor_pub.commit();

  // Create src with a normal key
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abc");
  src_cursor_pub.value("abc_value");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify both keys exist
  auto v = dest_storage->open("test").cursor();
  v.find("");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("root_value"));

  v.find("abc");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("abc_value"));

  // Verify exactly 2 entries
  int count = 0;
  v.first();
  while (v.is_valid()) {
    count++;
    v.next();
  }
  BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_CASE(test_merger_none_branch_leaf_with_src_trie) {
  // Test merging when dst has a NONE branch leaf (key_size=0) that is NOT at root,
  // and src is a trie. This exercises _merger.hpp lines 154-156:
  //   dst_cursor.pop();
  //   merge_trie_node(dst, src);
  //
  // dst structure: trie "ab" with NONE branch -> leaf("ab_value")
  //                           and 'c' branch  -> leaf("abc_value")
  // src structure: trie "ab" with 'd' branch  -> leaf("abd_value")
  //                           and 'e' branch  -> leaf("abe_value")
  //
  // When merging src "ab" trie into dst's NONE branch leaf at "ab",
  // we should pop the leaf and merge the trie instead.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // Create dst with "ab" (NONE branch) and "abc"
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("ab");
  dst_cursor_pub.value("ab_value");
  dst_cursor_pub.find("abc");
  dst_cursor_pub.value("abc_value");
  dst_cursor_pub.commit();

  // Create src with "abd" and "abe" (forms a trie at "ab")
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abd");
  src_cursor_pub.value("abd_value");
  src_cursor_pub.find("abe");
  src_cursor_pub.value("abe_value");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify all keys exist after merge
  auto v = dest_storage->open("test").cursor();
  
  v.find("ab");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("ab_value"));

  v.find("abc");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("abc_value"));

  v.find("abd");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("abd_value"));

  v.find("abe");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("abe_value"));

  // Verify exactly 4 entries
  int count = 0;
  v.first();
  while (v.is_valid()) {
    count++;
    v.next();
  }
  BOOST_CHECK_EQUAL(count, 4);
}

// ── Dumper coverage: simple=true mode ────────────────────────────────────
// Covers _check.hpp L140, L196, L245, L246 (simple-mode id printing)

BOOST_AUTO_TEST_CASE(test_dumper_simple_mode) {
  MergerPreparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = storage->open("test");

  // Insert a few keys so we have both trie and leaf nodes
  auto c = db.cursor();
  c.find("alpha");
  c.value("val_a");
  c.commit();
  c.find("beta");
  c.value("val_b");
  c.commit();
  c.find("gamma");
  c.value("val_g");
  c.commit();

  // Dump with simple=true — exercises the simple id-printing branches
  std::stringstream simple_out;
  _Dumper(db, &db._internal()->txn()->root, true).dump(simple_out);
  std::string simple_str = simple_out.str();
  BOOST_CHECK(!simple_str.empty());

  // The simple dump uses sequential integer IDs
  BOOST_CHECK(simple_str.find("id: 0") != std::string::npos);
  BOOST_CHECK(simple_str.find("type: trie") != std::string::npos ||
              simple_str.find("type: leaf") != std::string::npos);

  // Also dump with simple=false for comparison
  std::stringstream full_out;
  _Dumper(db, &db._internal()->txn()->root, false).dump(full_out);
  std::string full_str = full_out.str();
  BOOST_CHECK(!full_str.empty());

  // Both should contain the same keys
  BOOST_CHECK(simple_str.find("alpha") != std::string::npos ||
              simple_str.find("key:") != std::string::npos);
}

// ── merge_leaf_into_trie rejection (L627) ────────────────────────────────
// When src has a single leaf and dst has a trie with matching prefix,
// merge_leaf_into_trie is called. If may_add_leaf rejects, return early.

BOOST_AUTO_TEST_CASE(test_merger_leaf_into_trie_rejected) {
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // dst has multiple keys under "ab" → creates a trie at "ab"
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abd");
  dst_cursor_pub.value("v_abd");
  dst_cursor_pub.find("abe");
  dst_cursor_pub.value("v_abe");
  dst_cursor_pub.commit();

  // src has a single leaf "abc" → leaf node in src root
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abc");
  src_cursor_pub.value("v_abc");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  // Filter rejects everything — "abc" will be rejected in merge_leaf_into_trie
  PrefixFilterPolicy handler("NOMATCH");
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify dst unchanged: "abd" and "abe" remain, "abc" not added
  auto v = dest_storage->open("test").cursor();
  v.find("abd");
  BOOST_CHECK(v.is_valid());
  v.find("abe");
  BOOST_CHECK(v.is_valid());
  v.find("abc");
  BOOST_CHECK(!v.is_valid());
}

// ── merge_into_trie suffix_len==0, shared branches (L617) ──────────────
// Both src and dst have tries with same prefix — forces the shared-branch
// merge path and free_node(dst_trie) at L617.

BOOST_AUTO_TEST_CASE(test_merger_shared_trie_prefix) {
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  // dst has keys that create a trie with prefix "ab" and branches d,e
  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abd");
  dst_cursor_pub.value("v_dst_abd");
  dst_cursor_pub.find("abe");
  dst_cursor_pub.value("v_dst_abe");
  dst_cursor_pub.commit();

  // src also has keys under "ab" trie with branches d (shared), f (new)
  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abd");
  src_cursor_pub.value("v_src_abd");
  src_cursor_pub.find("abf");
  src_cursor_pub.value("v_src_abf");
  src_cursor_pub.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();

  OverwritePolicy handler;
  exec_merger(*dst_internal, *src_internal, handler);

  // Verify: "abd" overwritten by src, "abe" kept from dst, "abf" added from src
  auto v = dest_storage->open("test").cursor();
  v.find("abd");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_src_abd"));
  v.find("abe");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_dst_abe"));
  v.find("abf");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("v_src_abf"));
}

BOOST_AUTO_TEST_CASE(test_merger_recursive_merge_src_keypos_bug) {
  // Regression: src_cursor.push() in resolve_divergence recorded keypos=0
  // because src_cursor.current_key was never synced with the local current_key
  // string. The recursive merge_node then computed src_split_pos = split_pos - 0
  // (too large), violating assert(src_leaf->key_size >= src_split_pos).
  //
  // Crash path:
  //   merge_node("") → resolve_divergence(split_pos=5, src=trie "hello")
  //     → src_cursor.push('/'-branch leaf "/world")  ← keypos=0 (bug)
  //     → merge_node("hello") → resolve_divergence(split_pos=11, src.keypos=0)
  //     → src_split_pos=11, assert(key_size=6 >= 11) → CRASH
  //
  // dst:  "hello/world/X" = "V1"
  // src:  "hello" = "V2"        (forces NONE branch in src trie root)
  //       "hello/world" = "V3"  (forces '/' branch shared with dst's path)
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("hello");
  src_cursor_pub.value("V2");
  src_cursor_pub.find("hello/world");
  src_cursor_pub.value("V3");
  src_cursor_pub.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("hello/world/X");
  dst_cursor_pub.value("V1");
  dst_cursor_pub.commit();

  OverwritePolicy handler;
  // Before fix: assert(src_leaf->key_size >= src_split_pos) i.e. assert(6 >= 11) → crash
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  // After fix: all three keys merged into destination
  auto v = dest_storage->open("test").cursor();
  v.find("hello");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("V2"));
  v.find("hello/world");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("V3"));
  v.find("hello/world/X");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("V1"));
}

// ── Parallel execution tests ────────────────────────────────────────────
#if LEAVES_HAS_THREADS

BOOST_AUTO_TEST_CASE(test_merger_parallel_multiple_to_empty) {
  // Parallelize deep-copy of a wide source trie into empty destination
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto c = src_db.cursor();
  for (int i = 0; i < 26; i++) {
    std::string key(1, 'a' + i);
    key += "_value_key";
    c.find(key);
    c.value("val_" + std::to_string(i));
  }
  c.commit();

  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage->open("test")._internal();

  OverwritePolicy handler;
  exec_merger_threaded_path(*dst_internal, *src_internal, handler);

  auto v = dest_storage->open("test").cursor();
  int count = 0;
  v.first();
  while (v.is_valid()) { count++; v.next(); }
  BOOST_CHECK_EQUAL(count, 26);
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_disjoint_keys) {
  // Merge disjoint wide tries in parallel
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto dst_db = dest_storage->open("test");
  auto sc = src_db.cursor();
  auto dc = dst_db.cursor();

  for (int i = 0; i < 26; i++) {
    std::string skey = "src_" + std::string(1, 'a' + i);
    sc.find(skey); sc.value("sv_" + std::to_string(i));
    std::string dkey = "dst_" + std::string(1, 'a' + i);
    dc.find(dkey); dc.value("dv_" + std::to_string(i));
  }
  sc.commit();
  dc.commit();

  OverwritePolicy handler;
  exec_merger_threaded_path(*dst_db._internal(), *src_db._internal(), handler);

  auto v = dest_storage->open("test").cursor();
  int count = 0;
  v.first();
  while (v.is_valid()) { count++; v.next(); }
  BOOST_CHECK_EQUAL(count, 52);
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_large_dataset) {
  // Large merge to stress-test parallel deep copy with many branches
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto sc = src_db.cursor();
  for (int i = 0; i < 300; i++) {
    std::string key = "key_" + std::to_string(i);
    sc.find(key);
    sc.value("src_value_" + std::to_string(i));
  }
  sc.commit();

  // Pre-populate destination with some overlapping keys
  auto dst_db = dest_storage->open("test");
  auto dc = dst_db.cursor();
  for (int i = 0; i < 50; i++) {
    std::string key = "key_" + std::to_string(i);
    dc.find(key);
    dc.value("dst_value_" + std::to_string(i));
  }
  dc.commit();

  OverwritePolicy handler;
  exec_merger_threaded_path(*dst_db._internal(), *src_db._internal(), handler);

  auto v = dest_storage->open("test").cursor();
  int count = 0;
  v.first();
  while (v.is_valid()) { count++; v.next(); }
  BOOST_CHECK_EQUAL(count, 300);

  // Check a few values are from source (overwritten)
  v.find("key_0");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("src_value_0"));
  v.find("key_49");
  BOOST_CHECK(v.is_valid());
  BOOST_CHECK_EQUAL(v.value(), Slice("src_value_49"));
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_matches_inline) {
  // Verify parallel merge produces identical results to inline merge
  MergerPreparation p;

  // Build source tree with varied structure
  auto src_storage = Storage::create(TEST_FILE);
  auto src_db = src_storage->open("test");
  auto sc = src_db.cursor();
  for (int i = 0; i < 100; i++) {
    std::string key = "prefix_" + std::string(1, 'a' + (i % 26)) + "_" + std::to_string(i);
    sc.find(key);
    sc.value("value_" + std::to_string(i));
  }
  sc.commit();

  // Merge inline, collect results
  std::vector<std::pair<std::string, std::string>> inline_results;
  {
    auto dst1_storage = Storage::create(TEST_FILE "2");
    OverwritePolicy handler;
    exec_merger(*dst1_storage->open("test")._internal(),
                *src_db._internal(), handler);
    auto v = dst1_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      inline_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }
  std::remove(TEST_FILE "2");

  // Merge parallel, collect results
  std::vector<std::pair<std::string, std::string>> parallel_results;
  {
    auto dst2_storage = Storage::create(TEST_FILE "2");
    OverwritePolicy handler;
    exec_merger_threaded_path(*dst2_storage->open("test")._internal(),
                         *src_db._internal(), handler);
    auto v = dst2_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      parallel_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }

  // Compare results
  BOOST_CHECK_EQUAL(inline_results.size(), parallel_results.size());
  BOOST_CHECK_EQUAL(inline_results.size(), 100u);
  for (size_t i = 0; i < inline_results.size() && i < parallel_results.size(); i++) {
    BOOST_CHECK_EQUAL(inline_results[i].first, parallel_results[i].first);
    BOOST_CHECK_EQUAL(inline_results[i].second, parallel_results[i].second);
  }
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_with_filter) {
  // Parallel merge with may_add filtering — exercises handler SpinLock
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  // Insert keys with distinct first-char prefixes: 'a' for accept, 'r' for reject
  {
    auto sc = src_db.cursor();
    for (int i = 0; i < 50; i++) {
      std::string key = "a" + std::to_string(i);
      sc.find(key);
      sc.value("x");
    }
    for (int i = 0; i < 50; i++) {
      std::string key = "r" + std::to_string(i);
      sc.find(key);
      sc.value("y");
    }
    sc.commit();
  }

  // Filter: only keys starting with "a"
  TrackingFilterPolicy handler("a");

  exec_merger_threaded_path(*dest_storage->open("test")._internal(),
                       *src_db._internal(), handler);

  auto v = dest_storage->open("test").cursor();
  int count = 0;
  v.first();
  while (v.is_valid()) {
    BOOST_CHECK(v.key().string()[0] == 'a');
    count++;
    v.next();
  }
  BOOST_CHECK_EQUAL(count, 50);
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_shared_branches) {
  // Both src and dst have overlapping key ranges, forcing shared-branch merges
  // in merge_into_trie (suffix_len==0 path). Sub-cursors handle each branch.
  MergerPreparation p;

  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto dst_db = dest_storage->open("test");

  // Use single-char prefix + zero-padded numbers to avoid the key1==key2 bug.
  // dst: A00..A09, C00..C09, E00..E09  (30 keys)
  // src: A00..A09, C00..C09, G00..G09  (30 keys)
  // Shared: A, C branches.  dst-only: E.  src-only: G.
  auto pad = [](int i) -> std::string {
    char buf[4]; snprintf(buf, sizeof(buf), "%02d", i); return buf;
  };
  // Use pure numeric keys with branch-letter prefix to avoid cursor insert bugs.
  // Insert in globally sorted order to avoid keep_stack issues.
  auto mk = [](char prefix, int i) -> std::string {
    char buf[8]; snprintf(buf, sizeof(buf), "%c%03d", prefix, i); return buf;
  };
  {
    auto dc = dst_db.cursor();
    for (int i = 0; i < 10; i++) dc.find(mk('A', i)), dc.value(mk('a', i));
    for (int i = 0; i < 10; i++) dc.find(mk('C', i)), dc.value(mk('c', i));
    for (int i = 0; i < 10; i++) dc.find(mk('E', i)), dc.value(mk('e', i));
    dc.commit();
  }
  {
    auto sc = src_db.cursor();
    for (int i = 0; i < 10; i++) sc.find(mk('A', i)), sc.value(mk('a', i + 100));
    for (int i = 0; i < 10; i++) sc.find(mk('C', i)), sc.value(mk('c', i + 100));
    for (int i = 0; i < 10; i++) sc.find(mk('G', i)), sc.value(mk('g', i));
    sc.commit();
  }

  // Inline merge for reference
  std::vector<std::pair<std::string, std::string>> inline_results;
  {
    auto dst1_storage = Storage::create(TEST_FILE "3");
    {
      auto dst1_db = dst1_storage->open("test");
      auto dc1 = dst1_db.cursor();
      for (int i = 0; i < 10; i++) dc1.find(mk('A', i)), dc1.value(mk('a', i));
      for (int i = 0; i < 10; i++) dc1.find(mk('C', i)), dc1.value(mk('c', i));
      for (int i = 0; i < 10; i++) dc1.find(mk('E', i)), dc1.value(mk('e', i));
      dc1.commit();
      OverwritePolicy handler;
      exec_merger(*dst1_db._internal(), *src_db._internal(), handler);
    }
    auto v = dst1_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      inline_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }
  std::remove(TEST_FILE "3");

  // Parallel merge
  OverwritePolicy handler;
  exec_merger_threaded_path(*dst_db._internal(), *src_db._internal(), handler);

  std::vector<std::pair<std::string, std::string>> parallel_results;
  {
    auto v = dest_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      parallel_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }

  // 40 keys total: A (10 overwritten), C (10 overwritten), E (10 kept), G (10 added)
  BOOST_CHECK_EQUAL(inline_results.size(), parallel_results.size());
  BOOST_CHECK_EQUAL(parallel_results.size(), 40u);
  for (size_t i = 0; i < inline_results.size() && i < parallel_results.size(); i++) {
    BOOST_CHECK_EQUAL(inline_results[i].first, parallel_results[i].first);
    BOOST_CHECK_EQUAL(inline_results[i].second, parallel_results[i].second);
  }
}

BOOST_AUTO_TEST_CASE(test_merger_parallel_shared_branches_deep) {
  // Deeper shared branches: both src and dst share multi-level trie structure
  MergerPreparation p;

  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto dst_db = dest_storage->open("test");

  // Keys: "P<branch>D<nn>" in dst, "P<branch>S<nn>" in src
  // branch = A..E (5 branches), nn = 00..09 (10 each)
  // Also src overwrites some dst keys "P<branch>D<nn>" for nn=00..04
  auto pad = [](int i) -> std::string {
    char buf[4]; snprintf(buf, sizeof(buf), "%02d", i); return buf;
  };
  {
    auto dc = dst_db.cursor();
    for (int b = 0; b < 5; b++) {
      for (int i = 0; i < 10; i++) {
        std::string key = "P" + std::string(1, 'A' + b) + "D" + pad(i);
        dc.find(key); dc.value("d" + key);
      }
    }
    dc.commit();
  }
  {
    auto sc = src_db.cursor();
    for (int b = 0; b < 5; b++) {
      for (int i = 0; i < 10; i++) {
        std::string key = "P" + std::string(1, 'A' + b) + "S" + pad(i);
        sc.find(key); sc.value("s" + key);
      }
      // Overwrite 5 dst keys per branch
      for (int i = 0; i < 5; i++) {
        std::string key = "P" + std::string(1, 'A' + b) + "D" + pad(i);
        sc.find(key); sc.value("s_ow_" + key);
      }
    }
    sc.commit();
  }

  // Inline reference
  std::vector<std::pair<std::string, std::string>> inline_results;
  {
    auto dst1_storage = Storage::create(TEST_FILE "3");
    {
      auto dst1_db = dst1_storage->open("test");
      auto dc1 = dst1_db.cursor();
      for (int b = 0; b < 5; b++) {
        for (int i = 0; i < 10; i++) {
          std::string key = "P" + std::string(1, 'A' + b) + "D" + pad(i);
          dc1.find(key); dc1.value("d" + key);
        }
      }
      dc1.commit();
      OverwritePolicy handler;
      exec_merger(*dst1_db._internal(), *src_db._internal(), handler);
    }
    auto v = dst1_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      inline_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }
  std::remove(TEST_FILE "3");

  // Parallel merge
  OverwritePolicy handler;
  exec_merger_threaded_path(*dst_db._internal(), *src_db._internal(), handler);

  std::vector<std::pair<std::string, std::string>> parallel_results;
  {
    auto v = dest_storage->open("test").cursor();
    v.first();
    while (v.is_valid()) {
      parallel_results.emplace_back(v.key().string(), v.value().string());
      v.next();
    }
  }

  // 5 branches * (10 dst + 10 src) = 100, minus 25 overwritten = 75 unique + 25 overwritten = 100
  BOOST_CHECK_EQUAL(inline_results.size(), parallel_results.size());
  for (size_t i = 0; i < inline_results.size() && i < parallel_results.size(); i++) {
    BOOST_CHECK_EQUAL(inline_results[i].first, parallel_results[i].first);
    BOOST_CHECK_EQUAL(inline_results[i].second, parallel_results[i].second);
  }
}

#endif  // LEAVES_HAS_THREADS

// Selective policy that rejects specific keys
struct SelectivePolicy {
  std::set<std::string> rejected_keys;
  std::set<std::string> rejected_overwrites;

  bool may_overwrite(const std::string& key, const Slice& dst,
                     const Slice& src, bool dst_is_big, bool src_is_big) {
    return rejected_overwrites.find(key) == rejected_overwrites.end();
  }

  bool may_add_leaf(const std::string& key, const Slice& src, bool is_big) {
    return rejected_keys.find(key) == rejected_keys.end();
  }

  bool may_add_trie(const std::string& key) {
    return true;
  }

  template <typename LeafNode, typename DstCursor>
  void free_big(LeafNode& leaf, DstCursor& dst_cursor) {}

  mutable _BigValue _big_value_storage;

  template <typename LeafNode, typename SrcCursor, typename DstCursor>
  MigratedValue migrate_big_value(LeafNode& leaf, SrcCursor& src_cursor, DstCursor& dst_cursor) {
    return {Slice(), false};
  }

  template <typename TriePtr, typename DBType>
  void after_trie_merged(TriePtr& trie, DBType* db) {}
};

BOOST_AUTO_TEST_CASE(test_merger_selective_reject_overwrite) {
  // Exercises _merger.hpp L262: may_overwrite returns false
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  src_cursor.find("hello");
  src_cursor.value("src_value");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("hello");
  dst_cursor.value("dst_value");
  dst_cursor.commit();

  SelectivePolicy handler;
  handler.rejected_overwrites.insert("hello");
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("hello");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dst_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_selective_reject_add) {
  // Exercises selective deep copy paths where may_add rejects leaves
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  src_cursor.find("abc1");
  src_cursor.value("val1");
  src_cursor.find("abc2");
  src_cursor.value("val2");
  src_cursor.find("abc3");
  src_cursor.value("val3");
  src_cursor.find("def1");
  src_cursor.value("val4");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("ghi");
  dst_cursor.value("dst_val");
  dst_cursor.commit();

  SelectivePolicy handler;
  handler.rejected_keys.insert("abc1");
  handler.rejected_keys.insert("abc2");
  handler.rejected_keys.insert("abc3");
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("ghi");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dst_val"));
  verify.find("def1");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("val4"));
  verify.find("abc1");
  BOOST_CHECK(!verify.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_trie_into_trie_with_shared_and_srconly) {
  // Exercises merge_into_trie: shared branches merged + src-only deep-copied
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  for (char c = 'a'; c <= 'f'; c++) {
    std::string key(1, c);
    key += "suffix";
    src_cursor.find(key);
    src_cursor.value("src_" + key);
  }
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("asuffix");
  dst_cursor.value("dst_a");
  dst_cursor.find("bsuffix");
  dst_cursor.value("dst_b");
  dst_cursor.find("xsuffix");
  dst_cursor.value("dst_x");
  dst_cursor.commit();

  OverwritePolicy handler;
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("asuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("src_asuffix"));
  verify.find("csuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("src_csuffix"));
  verify.find("xsuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dst_x"));
}

BOOST_AUTO_TEST_CASE(test_merger_leaf_into_existing_trie) {
  // Exercises merge_leaf_into_trie and expand_trie_with_branch
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  src_cursor.find("bsuffix");
  src_cursor.value("src_b");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("asuffix");
  dst_cursor.value("dst_a");
  dst_cursor.find("csuffix");
  dst_cursor.value("dst_c");
  dst_cursor.commit();

  OverwritePolicy handler;
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("asuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dst_a"));
  verify.find("bsuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("src_b"));
  verify.find("csuffix");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dst_c"));
}

BOOST_AUTO_TEST_CASE(test_merger_src_trie_no_shared_branch) {
  // Tests where src trie does NOT have dst's branch key
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  src_cursor.find("prefix_x1");
  src_cursor.value("sx1");
  src_cursor.find("prefix_x2");
  src_cursor.value("sx2");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  dst_cursor.find("prefix_y1");
  dst_cursor.value("dy1");
  dst_cursor.commit();

  OverwritePolicy handler;
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("prefix_x1");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("sx1"));
  verify.find("prefix_x2");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("sx2"));
  verify.find("prefix_y1");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("dy1"));
}

BOOST_AUTO_TEST_CASE(test_merger_selective_trie_into_trie) {
  // Exercises selective deep copy during trie-to-trie merge.
  // src and dst share a common prefix trie so merge_into_trie fires,
  // and selective_deep_copy_subtree filters leaves.
  MergerPreparation p;
  auto src_storage = Storage::create(TEST_FILE);
  auto dest_storage = Storage::create(TEST_FILE "2");

  auto src_db = src_storage->open("test");
  auto src_cursor = src_db.cursor();
  // Create src trie with branch structure at root: 'a' and 'b'
  src_cursor.find("ax");
  src_cursor.value("vax");
  src_cursor.find("ay");
  src_cursor.value("vay");
  src_cursor.find("bx");
  src_cursor.value("vbx");
  src_cursor.commit();

  auto dst_db = dest_storage->open("test");
  auto dst_cursor = dst_db.cursor();
  // Create dst trie with same root structure: branch 'a' exists
  dst_cursor.find("az");
  dst_cursor.value("vaz");
  dst_cursor.commit();

  // Reject "bx" — src-only branch 'b' should still be selectively copied
  SelectivePolicy handler;
  handler.rejected_keys.insert("bx");
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  auto verify = dest_storage->open("test").cursor();
  verify.find("ax");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("vax"));
  verify.find("ay");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("vay"));
  verify.find("bx");
  BOOST_CHECK(!verify.is_valid());  // rejected by may_add_leaf
  verify.find("az");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("vaz"));  // dst unchanged
}

BOOST_AUTO_TEST_CASE(test_merger_selective_reject_all_src_trie_children) {
  // Exercises _merger.hpp L380 — all children of source trie rejected by may_add
  // When src has a trie branch that diverges from dst and ALL leaves are rejected,
  // the "if (!surviving) return;" path is taken.
  MergerPreparation p;

  auto src_storage = Storage::create(TEST_FILE);
  auto src_db = src_storage->open("test");
  auto dest_storage = Storage::create(TEST_FILE "2");
  auto dst_db = dest_storage->open("test");

  // Src: trie with multiple branches under "x"
  {
    auto c = src_db.cursor();
    c.find("xa"); c.value("v1"); c.commit();
  }
  {
    auto c = src_db.cursor();
    c.find("xb"); c.value("v2"); c.commit();
  }

  // Dst: leaf "y" (different first byte, so resolve_divergence is triggered)
  {
    auto c = dst_db.cursor();
    c.find("y"); c.value("vy"); c.commit();
  }

  // Reject ALL src keys — nothing from src trie survives
  SelectivePolicy handler;
  handler.rejected_keys.insert("xa");
  handler.rejected_keys.insert("xb");
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  // Only dst key should remain
  auto verify = dest_storage->open("test").cursor();
  verify.find("y");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("vy"));
  verify.find("xa");
  BOOST_CHECK(!verify.is_valid());
  verify.find("xb");
  BOOST_CHECK(!verify.is_valid());
}

BOOST_AUTO_TEST_CASE(test_merger_selective_merge_into_trie_all_rejected) {
  // Exercises merge_into_trie with suffix_len > 0 where all src children rejected.
  // This is the "if (!surviving) return;" inside merge_into_trie.
  MergerPreparation p;

  auto src_storage = Storage::create(TEST_FILE);
  auto src_db = src_storage->open("test");
  auto dest_storage = Storage::create(TEST_FILE "2");
  auto dst_db = dest_storage->open("test");

  // Dst: trie with prefix "ab" and branches "abc", "abd"
  {
    auto c = dst_db.cursor();
    c.find("abc"); c.value("v_abc"); c.commit();
  }
  {
    auto c = dst_db.cursor();
    c.find("abd"); c.value("v_abd"); c.commit();
  }

  // Src: trie with prefix "ab" and branches "abx", "aby" (suffix differs)
  {
    auto c = src_db.cursor();
    c.find("abx"); c.value("v_abx"); c.commit();
  }
  {
    auto c = src_db.cursor();
    c.find("aby"); c.value("v_aby"); c.commit();
  }

  // Reject all src keys
  SelectivePolicy handler;
  handler.rejected_keys.insert("abx");
  handler.rejected_keys.insert("aby");
  exec_merger(*dst_db._internal(), *src_db._internal(), handler);

  // Dst should be unchanged
  auto verify = dest_storage->open("test").cursor();
  verify.find("abc");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("v_abc"));
  verify.find("abd");
  BOOST_REQUIRE(verify.is_valid());
  BOOST_CHECK_EQUAL(verify.value(), Slice("v_abd"));
  verify.find("abx");
  BOOST_CHECK(!verify.is_valid());
  verify.find("aby");
  BOOST_CHECK(!verify.is_valid());
}

// Recursive trie integrity checker: verifies that for every (X, child) pair
// in a trie, X equals the actual first byte of the child's path. Returns true
// if OK, false on first violation (and prints details to stderr).
template <typename DB>
bool _check_trie_integrity(DB& db, typename DB::Traits::offset_e* link,
                           const std::string& path) {
  using Traits = typename DB::Traits;
  using TrieNode = _TrieNode<Traits>;
  using LeafNode = _LeafNode<Traits>;
  using offset_e = typename Traits::offset_e;
  if (!link || !*link) return true;
  if (link->type() != TRIE) {
    return true;  // root being leaf — nothing to check here
  }
  auto trie = db.template resolve<TrieNode>(link);
  bool ok = true;
  trie->for_each_branch([&](int k, offset_e* off) {
    if (!ok) return;
    if (!*off) {
      std::cerr << "INTEGRITY: empty branch path=" << path
                << " branch_key=" << k << std::endl;
      ok = false;
      return;
    }
    std::string child_path = path;
    child_path.append((const char*)trie->compressed(), trie->len());
    if (k != TrieNode::NONE) child_path.push_back((char)k);

    if (off->type() == LEAF) {
      auto leaf = db.template resolve<LeafNode>(off);
      uint8_t first = leaf->key_size ? leaf->data[0] : 0;
      if (k == TrieNode::NONE) {
        if (leaf->key_size != 0) {
          std::cerr << "INTEGRITY: NONE branch leaf has key_size="
                    << (int)leaf->key_size << " at path='" << path
                    << "' parent_prefix_len=" << (int)trie->len() << std::endl;
          ok = false;
        }
      } else {
        if (leaf->key_size == 0 || first != (uint8_t)k) {
          std::cerr << "INTEGRITY: leaf first byte=" << (int)first
                    << " != branch_key=" << k << " at path='" << path
                    << "' parent_compressed_len=" << (int)trie->len()
                    << " parent_count=" << trie->count()
                    << " leaf->key_size=" << (int)leaf->key_size
                    << " leaf->vsize=" << leaf->vsize()
                    << " leaf_off=0x" << std::hex << (uint64_t)*off << std::dec
                    << std::endl;
          std::cerr << "  parent_compressed=\"";
          for (int i = 0; i < trie->len(); i++)
            std::cerr << "[" << (int)(uint8_t)trie->compressed()[i] << "]";
          std::cerr << "\"" << std::endl;
          std::cerr << "  leaf_key=\"";
          for (int i = 0; i < leaf->key_size; i++)
            std::cerr << "[" << (int)(uint8_t)leaf->data[i] << "]";
          std::cerr << "\"" << std::endl;
          ok = false;
        }
      }
    } else {
      // child is trie
      auto child_trie = db.template resolve<TrieNode>(off);
      if (k != TrieNode::NONE) {
        if (child_trie->len() == 0) {
          std::cerr << "INTEGRITY: child trie has empty compressed at path='"
                    << path << "' branch_key=" << k << std::endl;
          ok = false;
        } else if (child_trie->compressed()[0] != (uint8_t)k) {
          std::cerr << "INTEGRITY: child trie compressed[0]="
                    << (int)child_trie->compressed()[0]
                    << " != branch_key=" << k << " at path='" << path
                    << "' child compressed_len=" << (int)child_trie->len()
                    << " child count=" << child_trie->count() << std::endl;
          ok = false;
        }
      }
      if (!_check_trie_integrity(db, off, child_path)) ok = false;
    }
  });
  return ok;
}


//   ./db_bench_leaves --use_confluence=1 \
//       --benchmarks=fillrandom,overwrite --num=100000 --batch_size=1000
//
// Crash:
//   - ASAN: global-buffer-overflow in _MemManager::alloc() at _memory.hpp:269
//     (PAGE_SIZES[sidx] OOB, sidx=8). Garbage value_size in src_leaf in
//     merge_leaf_into_trie/fill_leaf -> LeafNode::size(key, 21550) -> sidx=8.
//   - Without ASAN: assert `trie_.count() < trie_.MAX_BRANCH_COUNT` in
//     _Transition::find() (cursor.hpp:189) — garbage TrieNode._array_len.
//
// This test mimics the bench workload but routes everything through _Merger
// directly (no _ConfluenceDB, no tributary, no threads). It does many
// sequential merges of 1000-key random batches into a growing destination,
// using a 16-byte zero-padded decimal keyspace of size N (random keys -> heavy
// collisions = lots of overwrite/split paths in the merger).
// =============================================================================
BOOST_AUTO_TEST_CASE(test_merger_repro_bench_overwrite_batches) {
  MergerPreparation p;
  auto dst_storage = Storage::create(TEST_FILE);
  auto dst_db = dst_storage->open("test");

  const int N = 100000;        // keyspace size (matches --num=100000)
  const int BATCH = 1000;      // matches --batch_size=1000
  // Minimal repro: 2 consecutive merges of 1000 random keys into the same
  // dst storage is enough to trigger the assertion in _Transition::next() /
  // _Transition::find() during the second merge. The original bench uses
  // 100+ batches (fillrandom + overwrite phases) but the bug is hit far
  // earlier.
  const int NUM_BATCHES = 2;

  std::mt19937 rng(42);  // deterministic
  std::string value_buf(100, 'x');

  for (int b = 0; b < NUM_BATCHES; b++) {
    // Each batch: build a fresh src DB with BATCH random 16-byte decimal keys
    // and 100-byte values, then merge into dst.
    const std::string src_file = std::string(TEST_FILE) + ".src";
    std::remove(src_file.c_str());
    auto src_storage = Storage::create(src_file.c_str());
    auto src_db = src_storage->open("test");
    {
      auto src_cursor_pub = src_db.cursor();
      for (int i = 0; i < BATCH; i++) {
        char keybuf[17];
        snprintf(keybuf, 17, "%016d", (int)(rng() % N));
        // Make value bytes vary a bit so leaf sizes/contents differ.
        uint32_t r = (uint32_t)rng();
        memcpy(value_buf.data(), &r, sizeof(r));
        src_cursor_pub.find(Slice(keybuf, 16));
        src_cursor_pub.value(Slice(value_buf.data(), value_buf.size()));
      }
      src_cursor_pub.commit();
    }

    auto src_internal = src_db._internal();
    auto dst_internal = dst_db._internal();

    OverwritePolicy handler;
    {
      std::ofstream out("/tmp/dst_before_merge_" + std::to_string(b) + ".yaml");
      _Dumper(*dst_internal, &dst_internal->txn()->root, false).dump(out);
    }
    {
      std::ofstream out("/tmp/src_merge_" + std::to_string(b) + ".yaml");
      _Dumper(*src_internal, &src_internal->txn()->root, false).dump(out);
    }
    exec_merger(*dst_internal, *src_internal, handler,
                "/tmp/dst_after_merge_" + std::to_string(b) + ".yaml");

    // Structural integrity check
    bool ok = _check_trie_integrity(*dst_internal,
                                    &dst_internal->txn()->root, std::string());
    std::cerr << "[after merge " << b << "] integrity=" << ok << std::endl;
    BOOST_REQUIRE(ok);

    // Walk dst after this merge to detect corruption early.
    {
      auto walk = dst_db.cursor();
      walk.first();
      int cnt = 0;
      while (walk.is_valid()) {
        cnt++;
        walk.next();
      }
      std::cerr << "[after merge " << b << "] dst keys=" << cnt << std::endl;
    }
  }

  // If we made it here without aborting, the bug is not reproduced.
  // Sanity check: dst should have somewhere between 1 and N keys.
  auto verify = dst_storage->open("test").cursor();
  int count = 0;
  verify.first();
  while (verify.is_valid() && count <= N) {
    count++;
    verify.next();
  }
  BOOST_CHECK_GT(count, 0);
  BOOST_CHECK_LE(count, N);
  std::cout << "test_merger_repro_bench_overwrite_batches: "
            << count << " keys after " << NUM_BATCHES << " merges" << std::endl;
}

// =============================================================================
// Reconstructs the *bench* crash in a SINGLE-threaded environment.
//
// Background:
//   The db_bench_leaves "fillrandom + overwrite" benchmark with
//   --use_confluence=1 crashes non-deterministically in the background merger
//   (_ConfluenceDB::_do_merge → _Merger::exec). The pre-existing
//   test_merger_repro_bench_overwrite_batches above (which uses two separate
//   storages) does NOT reproduce the bench bug — so the issue must depend on
//   something the bench exercises that the two-storage test does not.
//
// Architectural difference between the two paths:
//   • bench (Confluence): the tributary (src) and the main DB (dst) live in
//     the SAME storage / SAME mmap file. After each merge the tributary's
//     pages are returned to the storage's free list (_free_slot →
//     return_areas) and can then be re-allocated as main-DB pages by
//     subsequent merges.
//   • original repro: src and dst use separate storage files; no page
//     aliasing or recycling between them is possible.
//
// This test re-creates the bench shape in one thread:
//   - one MapStorage
//   - one persistent "dst" DB
//   - per-batch: open "src" DB in the SAME storage, write BATCH random keys,
//     merge into "dst", then remove the src DB so its pages flow back into
//     the storage's free pool (mirroring _free_slot).
//
// If the bug is page-aliasing / lifetime related (use-after-free across the
// src↔dst boundary, or a wrong storage being used to resolve an offset),
// this should expose it deterministically.
// =============================================================================
BOOST_AUTO_TEST_CASE(test_merger_repro_bench_shared_storage) {
  MergerPreparation p;
  auto storage = Storage::create(TEST_FILE);
  auto dst_db = storage->open("dst");

  const int N = 100000;        // keyspace size (matches --num=100000)
  const int BATCH = 1000;      // matches --batch_size=1000
  // Match the bench: fillrandom (100 batches) + overwrite (100 batches).
  const int NUM_BATCHES = 200;

  std::mt19937 rng(42);  // deterministic
  std::string value_buf(100, 'x');

  for (int b = 0; b < NUM_BATCHES; b++) {
    // Build a fresh src DB IN THE SAME STORAGE as dst. Its pages will share
    // the storage's allocator with dst, just like a tributary in Confluence.
    auto src_db = storage->open("src");
    {
      auto src_cursor_pub = src_db.cursor();
      for (int i = 0; i < BATCH; i++) {
        char keybuf[17];
        snprintf(keybuf, 17, "%016d", (int)(rng() % N));
        uint32_t r = (uint32_t)rng();
        memcpy(value_buf.data(), &r, sizeof(r));
        src_cursor_pub.find(Slice(keybuf, 16));
        src_cursor_pub.value(Slice(value_buf.data(), value_buf.size()));
      }
      src_cursor_pub.commit();
    }

    auto src_internal = src_db._internal();
    auto dst_internal = dst_db._internal();

    OverwritePolicy handler;
    exec_merger(*dst_internal, *src_internal, handler);

    // Structural integrity check on dst.
    bool ok = _check_trie_integrity(*dst_internal,
                                    &dst_internal->txn()->root, std::string());
    if (!ok) {
      std::cerr << "[shared_storage] integrity broken after merge " << b
                << std::endl;
    }
    BOOST_REQUIRE(ok);

    // Walk dst end-to-end to surface corruption that integrity_check misses.
    {
      auto walk = dst_db.cursor();
      walk.first();
      int cnt = 0;
      while (walk.is_valid()) {
        cnt++;
        walk.next();
      }
      if ((b % 20) == 19) {
        std::cerr << "[shared_storage] after merge " << b
                  << " dst keys=" << cnt << std::endl;
      }
    }

    // Release the src DB and return its pages to the storage's free pool —
    // this is the analogue of _ConfluenceDB::_free_slot. The next batch's
    // allocations may recycle these pages.
    storage->remove("src");
  }

  auto verify = storage->open("dst").cursor();
  int count = 0;
  verify.first();
  while (verify.is_valid() && count <= N) {
    count++;
    verify.next();
  }
  BOOST_CHECK_GT(count, 0);
  BOOST_CHECK_LE(count, N);
  std::cout << "test_merger_repro_bench_shared_storage: " << count
            << " keys after " << NUM_BATCHES << " merges" << std::endl;
}

// =============================================================================
// Reconstruct the bench crash through the real ConfluenceDB pipeline,
// but with the background merger disabled so the entire write+merge loop
// runs on the test thread.
//
// Strategy:
//   - Open a MapConfluenceDB.  merge_write_threshold is set to UINT32_MAX so
//     the writer's release_tributary path leaves finished tributaries ATTACHED
//     and merges happen only via the explicit merge_all_now() calls below
//     (which run merge_tributary -> _do_merge inline on the caller's thread).
//   - Run fillrandom-style batches via the confluence cursor.
//   - Between batches call merge_all_now(), which executes
//     merge_tributary -> _do_merge -> _Merger::exec INLINE on the caller's
//     thread.
//   - Then overwrite-style batches (same keyspace, different values) to
//     reproduce the bench's "fillrandom + overwrite" workload.
//
// This exercises exactly the merge code path that crashes in the bench
// (_do_merge → _Merger over a _TributaryDB sharing the same storage as the
// main DB), but completely free of races.
// =============================================================================
BOOST_AUTO_TEST_CASE(test_merger_repro_bench_confluence_single_thread) {
  MergerPreparation p;
  auto storage = MapStorage::create(TEST_FILE);
  MapConfluenceDB cdb(storage, "benchmark");
  // Push the threshold so high that release_tributary will always take the
  // FREE branch (no schedule_after) — merges happen only via merge_all_now().
  cdb.set_merge_write_threshold(UINT32_MAX);

  const int N = 100000;
  const int BATCH = 1000;
  const int FILL_BATCHES = 100;       // matches --num=100000 / --batch_size=1000
  const int OVERWRITE_BATCHES = 100;  // matches the overwrite phase

  std::mt19937 rng(42);
  std::string value_buf(100, 'x');

  auto run_batch = [&](int /*batch_idx*/) {
    auto cur = cdb.cursor();
    cur.start_transaction();
    for (int i = 0; i < BATCH; i++) {
      char keybuf[17];
      snprintf(keybuf, 17, "%016d", (int)(rng() % N));
      uint32_t r = (uint32_t)rng();
      memcpy(value_buf.data(), &r, sizeof(r));
      cur.find(Slice(keybuf, 16));
      cur.value(Slice(value_buf.data(), value_buf.size()));
    }
    cur.commit();
    // Force the merge to happen inline NOW, on this thread.
    cdb.merge_all_now();
  };

  // fillrandom phase
  for (int b = 0; b < FILL_BATCHES; b++) run_batch(b);
  // overwrite phase (same key distribution → heavy overwrite/collision)
  for (int b = 0; b < OVERWRITE_BATCHES; b++) run_batch(FILL_BATCHES + b);

  // Count keys to ensure the DB is still walkable.
  auto cur = cdb.cursor();
  cur.first();
  int count = 0;
  while (cur.is_valid() && count <= N + 1) {
    ++count;
    cur.next();
  }
  BOOST_CHECK_GT(count, 0);
  BOOST_CHECK_LE(count, N);
  std::cout << "test_merger_repro_bench_confluence_single_thread: " << count
            << " keys after " << (FILL_BATCHES + OVERWRITE_BATCHES)
            << " merges" << std::endl;
}

// =============================================================================
// REPLAY the EXACT workload captured by db_bench_leaves --dump_workload=...
// Format on disk (binary, repeated):
//   uint32_t tag = 0x5359454B ('K','E','Y','S')
//   uint32_t key_size
//   uint32_t num_keys
//   <num_keys * key_size> bytes of raw keys
// The dump file path is taken from env LEAVES_BENCH_WORKLOAD (default
// /tmp/bench_workload.bin). Test is skipped if the file is missing.
// =============================================================================
BOOST_AUTO_TEST_CASE(test_merger_repro_bench_replay) {
  const char* path = std::getenv("LEAVES_BENCH_WORKLOAD");
  if (!path) path = "/tmp/bench_workload.bin";
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::cout << "test_merger_repro_bench_replay: SKIP (no " << path << ")\n";
    return;
  }

  struct Chunk {
    uint32_t key_size;
    std::vector<char> keys;  // num_keys * key_size
    uint32_t num_keys() const { return (uint32_t)(keys.size() / key_size); }
  };
  std::vector<Chunk> chunks;
  for (;;) {
    uint32_t tag = 0, ks = 0, nn = 0;
    if (std::fread(&tag, 4, 1, f) != 1) break;
    BOOST_REQUIRE_EQUAL(tag, 0x5359454BU);
    BOOST_REQUIRE_EQUAL(std::fread(&ks, 4, 1, f), 1u);
    BOOST_REQUIRE_EQUAL(std::fread(&nn, 4, 1, f), 1u);
    Chunk c;
    c.key_size = ks;
    c.keys.resize((size_t)ks * nn);
    BOOST_REQUIRE_EQUAL(std::fread(c.keys.data(), ks, nn, f), nn);
    chunks.push_back(std::move(c));
  }
  std::fclose(f);
  std::cout << "test_merger_repro_bench_replay: loaded " << chunks.size()
            << " phases from " << path << "\n";
  for (size_t i = 0; i < chunks.size(); i++) {
    std::cout << "  phase " << i << ": " << chunks[i].num_keys()
              << " keys, key_size=" << chunks[i].key_size << "\n";
  }

  MergerPreparation p;
  auto storage = MapStorage::create(TEST_FILE);
  MapConfluenceDB cdb(storage, "benchmark");
  cdb.set_merge_write_threshold(UINT32_MAX);

  const int BATCH = 1000;  // bench --batch_size=1000
  std::string value_buf(100, 'x');
  uint32_t value_seq = 0;

  auto run_phase = [&](const Chunk& c) {
    auto cur = cdb.cursor();
    uint32_t total = c.num_keys();
    uint32_t ks = c.key_size;
    for (uint32_t off = 0; off < total; off += BATCH) {
      uint32_t end = std::min<uint32_t>(off + BATCH, total);
      cur.start_transaction();
      for (uint32_t i = off; i < end; i++) {
        Slice key(c.keys.data() + (size_t)i * ks, ks);
        cur.find(key);
        // Deterministic value content; size matches bench (100 bytes).
        memcpy(value_buf.data(), &value_seq, sizeof(value_seq));
        value_seq++;
        cur.value(Slice(value_buf.data(), value_buf.size()));
      }
      cur.commit();
      cdb.merge_all_now();
    }
  };

  for (size_t i = 0; i < chunks.size(); i++) {
    std::cout << "  running phase " << i << " ..." << std::flush;
    run_phase(chunks[i]);
    std::cout << " done\n";
  }

  auto cur = cdb.cursor();
  cur.first();
  int count = 0;
  while (cur.is_valid()) {
    ++count;
    cur.next();
  }
  std::cout << "test_merger_repro_bench_replay: " << count
            << " keys after replay" << std::endl;
  BOOST_CHECK_GT(count, 0);
}

// =============================================================================
// REPLAY-LAST-MERGE test
//
// db_bench_leaves, when run with env LEAVES_SRC_DUMP_DIR=<dir>, writes the
// dst (main DB) and src (tributary) KV contents to <dir>/last_dst.bin and
// <dir>/last_src.bin BEFORE each merger.exec() call (overwriting on every
// merge).  After a crash, the files capture the dst + src state at the
// failing merge exactly.
//
// This test:
//   - Reads last_dst.bin and last_src.bin from $LEAVES_SRC_DUMP_DIR
//     (default /tmp/srcdump).
//   - Reconstructs both DBs from the captured KVs.
//   - Runs a single _Merger from src into dst.
//   - Verifies no crash and reports the result.
// =============================================================================
namespace {
static std::vector<std::pair<std::string, std::string>> _load_kv_dump(
    const std::string& path) {
  std::vector<std::pair<std::string, std::string>> kvs;
  FILE* f = std::fopen(path.c_str(), "rb");
  BOOST_REQUIRE(f != nullptr);
  uint64_t magic = 0, tid = 0, nent = 0;
  BOOST_REQUIRE_EQUAL(std::fread(&magic, 8, 1, f), 1u);
  BOOST_REQUIRE_EQUAL(magic, 0x4C56535344554D50ULL);
  BOOST_REQUIRE_EQUAL(std::fread(&tid, 8, 1, f), 1u);
  BOOST_REQUIRE_EQUAL(std::fread(&nent, 8, 1, f), 1u);
  kvs.reserve((size_t)nent);
  for (uint64_t i = 0; i < nent; ++i) {
    uint16_t ks = 0;
    uint32_t vs = 0;
    BOOST_REQUIRE_EQUAL(std::fread(&ks, 2, 1, f), 1u);
    BOOST_REQUIRE_EQUAL(std::fread(&vs, 4, 1, f), 1u);
    std::string k(ks, '\0');
    std::string v(vs, '\0');
    if (ks) BOOST_REQUIRE_EQUAL(std::fread(k.data(), 1, ks, f), ks);
    if (vs) BOOST_REQUIRE_EQUAL(std::fread(v.data(), 1, vs, f), vs);
    kvs.emplace_back(std::move(k), std::move(v));
  }
  std::fclose(f);
  return kvs;
}
}  // namespace

BOOST_AUTO_TEST_CASE(test_merger_repro_bench_last_merge) {
  const char* dir = std::getenv("LEAVES_SRC_DUMP_DIR");
  if (!dir) dir = "/tmp/srcdump";

  std::string dst_path = std::string(dir) + "/last_dst.bin";
  std::string src_path = std::string(dir) + "/last_src.bin";
  if (::access(dst_path.c_str(), R_OK) != 0 ||
      ::access(src_path.c_str(), R_OK) != 0) {
    std::cout << "test_merger_repro_bench_last_merge: SKIP (no dumps in "
              << dir << ")\n";
    return;
  }

  auto dst_kvs = _load_kv_dump(dst_path);
  auto src_kvs = _load_kv_dump(src_path);
  std::cout << "test_merger_repro_bench_last_merge: dst=" << dst_kvs.size()
            << " keys, src=" << src_kvs.size() << " keys\n";

  // Build dst DB.
  std::remove(TEST_FILE "2");
  auto dst_storage = Storage::create(TEST_FILE "2");
  auto dst_db_pub = dst_storage->open("test");
  {
    auto cur = dst_db_pub.cursor();
    for (auto& kv : dst_kvs) {
      cur.find(Slice(kv.first.data(), kv.first.size()));
      cur.value(Slice(kv.second.data(), kv.second.size()));
    }
    cur.commit();
  }
  auto dst_internal = dst_db_pub._internal();

  // Build src DB.
  std::remove(TEST_FILE);
  auto src_storage = Storage::create(TEST_FILE);
  auto src_db_pub = src_storage->open("test");
  {
    auto cur = src_db_pub.cursor();
    for (auto& kv : src_kvs) {
      cur.find(Slice(kv.first.data(), kv.first.size()));
      cur.value(Slice(kv.second.data(), kv.second.size()));
    }
    cur.commit();
  }
  auto src_internal = src_db_pub._internal();

  OverwritePolicy handler;
  std::cout << "  running merger ..." << std::flush;
  exec_merger(*dst_internal, *src_internal, handler);
  std::cout << " ok\n";

  auto cur = dst_db_pub.cursor();
  cur.first();
  int count = 0;
  while (cur.is_valid()) {
    ++count;
    cur.next();
  }
  std::cout << "test_merger_repro_bench_last_merge: " << count
            << " keys in dst after merge\n";
  BOOST_CHECK_GT(count, 0);
}
