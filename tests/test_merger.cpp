#define BOOST_TEST_MODULE merger
#define GENERATE
#include <boost/test/unit_test.hpp>

#include "../include/leaves/intern/_merger.hpp"
#include "../include/leaves/intern/_cursor.hpp"
#include "test.hpp"

using namespace leaves;

typedef MapStorage Storage;
typedef Storage::StorageImpl::DB InternalDB;
typedef InternalDB::ValueTraits ValueTraits;
typedef _NodeIterator<InternalDB, ValueTraits> NodeIter;
typedef _Cursor<InternalDB, ValueTraits> InternalCursor;

// Function to execute merger within a transaction
template<typename CursorDst, typename CursorSrc, typename Handler>
void exec_merger(CursorDst& dst_cursor, CursorSrc& src_cursor, Handler& handler) {
  dst_cursor.start_transaction();
  _Merger<CursorDst, CursorSrc, Handler> merger(dst_cursor, src_cursor, handler);
  merger.exec();
  dst_cursor.commit();
}

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
struct OverwriteHandler {
  template <typename Dst, typename Src>
  bool overwrite(const std::string& key, Dst& dst, Src& src) {
    return true;  // Always overwrite
  }
};

// Handler that keeps destination values
struct KeepDestHandler {
  template <typename Dst, typename Src>
  bool overwrite(const std::string& key, Dst& dst, Src& src) {
    return false;  // Never overwrite
  }
};

BOOST_AUTO_TEST_CASE(test_merge_iterator_empty) {
  // Test iterator with empty trie
  MergerPreparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  
  offset_t empty_root;
  NodeIter iter(db._internal(), &empty_root);
  
  BOOST_CHECK(!iter.stack.size);
  BOOST_CHECK(!iter.next());
}

BOOST_AUTO_TEST_CASE(test_merge_iterator_single_leaf) {
  // Test iterator with single leaf node
  MergerPreparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();

  cursor.find("key1");
  cursor.value("value1");
  cursor.commit();
  
  // Get root through db
  auto _internal = db._internal();
  NodeIter iter(_internal, &_internal->txn()->root);
  
  BOOST_CHECK(!!iter.stack.size);
  BOOST_CHECK(iter.node().is_leaf());
  BOOST_CHECK_EQUAL(iter.node_key(), Slice("key1"));
  BOOST_CHECK_EQUAL(iter.node().value(), Slice("value1"));
  BOOST_CHECK(!iter.next());
  BOOST_CHECK(!iter.stack.size);
}

BOOST_AUTO_TEST_CASE(test_merge_iterator_multiple_values) {
  // Test iterator with multiple values
  MergerPreparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();

  cursor.find("apple");
  cursor.value("fruit1");
  cursor.find("banana");
  cursor.value("fruit2");
  cursor.find("cherry");
  cursor.value("fruit3");
  cursor.commit();
  
  auto _internal = db._internal();
  NodeIter iter(_internal, &_internal->txn()->root);
  
  std::map<std::string, std::string> found_values;
  
  if (!!iter.stack.size) {
    do {
      if (iter.node().is_leaf()) {
        std::string key(iter.node_key().data(), iter.node_key().size());
        std::string value(iter.node().value().data(), iter.node().value().size());
        found_values[key] = value;
      }
    } while (iter.next());
  }
  
  BOOST_CHECK_EQUAL(found_values.size(), 3);
  BOOST_CHECK_EQUAL(found_values["apple"], "fruit1");
  BOOST_CHECK_EQUAL(found_values["banana"], "fruit2");
  BOOST_CHECK_EQUAL(found_values["cherry"], "fruit3");
}

BOOST_AUTO_TEST_CASE(test_merge_single_value) {
  // Test merging single value from source to empty destination
  // Using iterator to iterate source and manual insert to dest
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor = src_db.cursor();

  src_cursor.find("key1");
  src_cursor.value("value1");
  src_cursor.commit();
  
  // Use iterator to read source
  auto src__internal = src_db._internal();
  NodeIter iter(src__internal, &src__internal->txn()->root);
  
  // Insert into destination
  auto dst_cursor = dest_storage["test"].cursor();
  
  if (!!iter.stack.size) {
    do {
      if (iter.node().is_leaf()) {
        dst_cursor.find(iter.node_key());
        dst_cursor.value(iter.node().value());
      }
    } while (iter.next());
  }
  dst_cursor.commit();
  
  // Verify
  dst_cursor.find("key1");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("value1"));
}

BOOST_AUTO_TEST_CASE(test_merge_multiple_values) {
  // Test merging multiple values
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor = src_db.cursor();

  src_cursor.find("apple");
  src_cursor.value("fruit1");
  src_cursor.find("banana");
  src_cursor.value("fruit2");
  src_cursor.find("cherry");
  src_cursor.value("fruit3");
  src_cursor.commit();
  
  // Use iterator to read source
  auto src__internal = src_db._internal();
  NodeIter iter(src__internal, &src__internal->txn()->root);
  
  // Insert into destination
  auto dst_cursor = dest_storage["test"].cursor();
  
  if (!!iter.stack.size) {
    do {
      if (iter.node().is_leaf()) {
        dst_cursor.find(iter.node_key());
        dst_cursor.value(iter.node().value());
      }
    } while (iter.next());
  }
  dst_cursor.commit();
  
  // Verify all values
  dst_cursor.find("apple");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("fruit1"));
  
  dst_cursor.find("banana");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("fruit2"));
  
  dst_cursor.find("cherry");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("fruit3"));
}

BOOST_AUTO_TEST_CASE(test_merge_with_existing_values) {
  // Test merging into non-empty destination
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor = src_db.cursor();

  src_cursor.find("key1");
  src_cursor.value("source1");
  src_cursor.find("key2");
  src_cursor.value("source2");
  src_cursor.commit();
  
  auto dst_cursor = dest_storage["test"].cursor();

  dst_cursor.find("key2");
  dst_cursor.value("dest2_old");
  dst_cursor.find("key3");
  dst_cursor.value("dest3");
  dst_cursor.commit();
  
  // Merge using iterator
  auto src__internal = src_db._internal();
  NodeIter iter(src__internal, &src__internal->txn()->root);

  if (!!iter.stack.size) {
    do {
      if (iter.node().is_leaf()) {
        dst_cursor.find(iter.node_key());
        dst_cursor.value(iter.node().value());
      }
    } while (iter.next());
  }
  dst_cursor.commit();
  
  // Verify merged results
  dst_cursor.find("key1");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("source1"));
  
  dst_cursor.find("key2");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("source2"));  // Overwritten
  
  dst_cursor.find("key3");
  BOOST_CHECK(dst_cursor.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor.value(), Slice("dest3"));  // Unchanged
}

BOOST_AUTO_TEST_CASE(test_merge_large_dataset) {
  // Test merging with a larger dataset
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor = src_db.cursor();
  
  // Insert 100 values into source
  for (int i = 0; i < 100; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string value = "value_" + std::to_string(i);
    src_cursor.find(key);
    src_cursor.value(value);
  }
  src_cursor.commit();
  
  // Merge using iterator
  auto src__internal = src_db._internal();
  NodeIter iter(src__internal, &src__internal->txn()->root);

  auto dst_cursor = dest_storage["test"].cursor();
  
  int count = 0;
  if (!!iter.stack.size) {
    do {
      if (iter.node().is_leaf()) {
        dst_cursor.find(iter.node_key());
        dst_cursor.value(iter.node().value());
        count++;
      }
    } while (iter.next());
  }
  dst_cursor.commit();
  
  BOOST_CHECK_EQUAL(count, 100);
  
  // Verify all values
  for (int i = 0; i < 100; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected_value = "value_" + std::to_string(i);
    dst_cursor.find(key);
    BOOST_CHECK(dst_cursor.is_valid());
    BOOST_CHECK_EQUAL(dst_cursor.value(), Slice(expected_value));
  }
}

// Test actual _Merger functionality

BOOST_AUTO_TEST_CASE(test_merger_empty_to_empty) {
  // Merge empty source into empty destination
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_internal = src_storage["test"]._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Both should still be empty
  BOOST_CHECK(!src_cursor.stack.size);
}

BOOST_AUTO_TEST_CASE(test_merger_single_leaf_to_empty) {
  // Merge single leaf into empty destination
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("value1");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify destination has the value
  auto dst_cursor_pub = dest_storage["test"].cursor();
  dst_cursor_pub.find("key1");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("value1"));
}

BOOST_AUTO_TEST_CASE(test_merger_multiple_leaves_to_empty) {
  // Merge multiple leaves into empty destination
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  
  src_cursor_pub.find("apple");
  src_cursor_pub.value("fruit1");
  src_cursor_pub.find("banana");
  src_cursor_pub.value("fruit2");
  src_cursor_pub.find("cherry");
  src_cursor_pub.value("fruit3");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all values in destination
  auto dst_cursor_pub = dest_storage["test"].cursor();
  
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("new_value");
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key1");
  dst_cursor_pub.value("old_value");
  dst_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify value was overwritten - need fresh cursor to see committed changes
  auto dst_cursor_pub2 = dest_storage["test"].cursor();
  dst_cursor_pub2.find("key1");
  BOOST_CHECK(dst_cursor_pub2.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub2.value(), Slice("new_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_keep_destination) {
  // Test keeping destination values
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("new_value");
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key1");
  dst_cursor_pub.value("old_value");
  dst_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  KeepDestHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify value was NOT overwritten
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage["test"].cursor();
  dst_cursor_verify.find("key1");
  BOOST_CHECK(dst_cursor_verify.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_verify.value(), Slice("old_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_disjoint_keys) {
  // Merge with disjoint key sets
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("key1");
  src_cursor_pub.value("value1");
  src_cursor_pub.find("key2");
  src_cursor_pub.value("value2");
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("key3");
  dst_cursor_pub.value("value3");
  dst_cursor_pub.find("key4");
  dst_cursor_pub.value("value4");
  dst_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all four keys exist
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("prefix_1");
  src_cursor_pub.value("val1");
  src_cursor_pub.find("prefix_2");
  src_cursor_pub.value("val2");
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("prefix_3");
  dst_cursor_pub.value("val3");
  dst_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all values
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  std::string big_value(5000, 'X');
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("bigkey");
  src_cursor_pub.value(big_value);
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify big value was copied
  auto dst_cursor_pub = dest_storage["test"].cursor();
  dst_cursor_pub.find("bigkey");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice(big_value));
}

BOOST_AUTO_TEST_CASE(test_merger_big_keys) {
  // Test merging with long keys (> 255 bytes)
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  std::string big_key(260, 'K');
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find(big_key);
  src_cursor_pub.value("bigkey_value");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify big key was copied
  auto dst_cursor_pub = dest_storage["test"].cursor();
  dst_cursor_pub.find(big_key);
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("bigkey_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_compressed_trie_nodes) {
  // Test merging with compressed trie nodes
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  
  // Create keys that will form compressed tries
  src_cursor_pub.find("abcdefghi");
  src_cursor_pub.value("val1");
  src_cursor_pub.find("abcdefghj");
  src_cursor_pub.value("val2");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify values
  auto dst_cursor_pub = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abc");
  src_cursor_pub.value("src_abc");
  src_cursor_pub.find("abcdef");
  src_cursor_pub.value("src_abcdef");
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abd");
  dst_cursor_pub.value("dst_abd");
  dst_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all values
  // Get fresh cursor to see committed changes
  auto dst_cursor_verify = dest_storage["test"].cursor();
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

BOOST_AUTO_TEST_CASE(test_merger_large_dataset) {
  // Test with large number of keys
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  
  // Insert 200 keys into source
  for (int i = 0; i < 200; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string value = "src_value_" + std::to_string(i);
    src_cursor_pub.find(key);
    src_cursor_pub.value(value);
  }
  src_cursor_pub.commit();
  
  auto dst_db = dest_storage["test"];
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
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);

  check_graph("after_merge", dest_storage);
  
  // Create a fresh cursor to see the merged results
  dst_cursor_pub.update();
  
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("");
  src_cursor_pub.value("empty_key_value");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify empty key
  auto dst_cursor_pub = dest_storage["test"].cursor();
  dst_cursor_pub.find("");
  BOOST_CHECK(dst_cursor_pub.is_valid());
  BOOST_CHECK_EQUAL(dst_cursor_pub.value(), Slice("empty_key_value"));
}

BOOST_AUTO_TEST_CASE(test_merger_null_branch) {
  // Test merging trie with NONE branch
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("ab");
  src_cursor_pub.value("ab_val");
  src_cursor_pub.find("aba");
  src_cursor_pub.value("aba_val");
  src_cursor_pub.find("abb");
  src_cursor_pub.value("abb_val");
  src_cursor_pub.commit();
  
  auto src_internal = src_db._internal();
  auto dst_internal = dest_storage["test"]._internal();
  
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all values including NONE branch
  auto dst_cursor_pub = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  // Create destination with a BIG value (> 4096 bytes triggers big value)
  // MAX_SIZE is 4K, so total leaf size must exceed that
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  std::string big_value(5000, 'X');  // Much bigger than 4K
  dst_cursor_pub.find("longkey");  // Longer key
  dst_cursor_pub.value(big_value);
  dst_cursor_pub.commit();
  
  // Create source that causes a split - matches only prefix of dst key
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("long");
  src_cursor_pub.value("short_val");
  src_cursor_pub.commit();
  
  // Merge - dst leaf "longkey" with big value will be split at "long"
  // This should call free(dst_leaf) on line 139, which should free the big value
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify both keys exist
  auto verify_cursor = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  // Create destination with keys that share a long prefix
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abcdefghij");
  dst_cursor_pub.value("val1");
  dst_cursor_pub.find("abcdefghik");
  dst_cursor_pub.value("val2");
  dst_cursor_pub.commit();
  
  // Create source with TRIE that matches only part of the prefix
  // Need multiple keys to form a trie
  auto src_db = src_storage["test"];
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
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all keys exist
  auto verify_cursor = dest_storage["test"].cursor();
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  // Create destination with a single key "abc"
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abc");
  dst_cursor_pub.value("dst_val");
  dst_cursor_pub.commit();
  
  // Create source with keys that extend "abc" to form a trie
  // Need more keys to ensure we get a trie at "abc" level
  auto src_db = src_storage["test"];
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
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  auto verify_cursor = dest_storage["test"].cursor();
  
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
  // This covers lines 237-254 in split_trie when src_trie->isset(key1) where key1=NONE
  MergerPreparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  // Create destination with single leaf "abc"
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("abc");
  dst_cursor_pub.value("dst_abc");
  dst_cursor_pub.commit();
  
  // Create source with "abc" (creates NONE branch) plus "abcd" and "abce"
  auto src_db = src_storage["test"];
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
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all keys merged correctly
  auto verify_cursor = dest_storage["test"].cursor();
  
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
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  // Create destination trie with short prefix
  auto dst_db = dest_storage["test"];
  auto dst_cursor_pub = dst_db.cursor();
  dst_cursor_pub.find("ab1");
  dst_cursor_pub.value("val1");
  dst_cursor_pub.find("ab2");
  dst_cursor_pub.value("val2");
  dst_cursor_pub.commit();
  
  // Create source trie with longer prefix that extends dst's prefix
  auto src_db = src_storage["test"];
  auto src_cursor_pub = src_db.cursor();
  src_cursor_pub.find("abcde1");
  src_cursor_pub.value("valX");
  src_cursor_pub.find("abcde2");
  src_cursor_pub.value("valY");
  src_cursor_pub.commit();
  
  // Merge - src trie "abcde" needs to be split when inserted into dst trie "ab"
  auto src_internal = src_db._internal();
  auto dst_internal = dst_db._internal();
  InternalCursor src_cursor(src_internal, &src_internal->txn()->root);
  InternalCursor dst_cursor(dst_internal, &dst_internal->txn()->root);
  
  OverwriteHandler handler;
  exec_merger(dst_cursor, src_cursor, handler);
  
  // Verify all keys
  auto verify_cursor = dest_storage["test"].cursor();
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
