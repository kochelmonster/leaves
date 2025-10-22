#define BOOST_TEST_MODULE merger
#include <boost/test/unit_test.hpp>

#include "../include/leaves/intern/_merger.hpp"
#include "test.hpp"

using namespace leaves;

typedef MapStorage Storage;
typedef decltype(std::declval<Storage::DB>()._internal())::element_type InternalDB;
typedef InternalDB::ValueTraits ValueTraits;
typedef _NodeIterator<InternalDB, ValueTraits> NodeIter;

BOOST_AUTO_TEST_CASE(test_merge_iterator_empty) {
  // Test iterator with empty trie
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  
  NodeIter iter(db._internal(), offset_t());
  
  BOOST_CHECK(!iter.stack.size);
  BOOST_CHECK(!iter.next());
}

BOOST_AUTO_TEST_CASE(test_merge_iterator_single_leaf) {
  // Test iterator with single leaf node
  Preparation p;
  Storage storage(TEST_FILE);
  auto db = storage["test"];
  auto cursor = db.cursor();

  cursor.find("key1");
  cursor.value("value1");
  cursor.commit();
  
  // Get root through db
  auto _internal = db._internal();
  NodeIter iter(_internal);
  
  BOOST_CHECK(!!iter.stack.size);
  BOOST_CHECK(iter.node().is_leaf());
  BOOST_CHECK_EQUAL(iter.node_key(), Slice("key1"));
  BOOST_CHECK_EQUAL(iter.node().value(), Slice("value1"));
  BOOST_CHECK(!iter.next());
  BOOST_CHECK(!iter.stack.size);
}

BOOST_AUTO_TEST_CASE(test_merge_iterator_multiple_values) {
  // Test iterator with multiple values
  Preparation p;
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
  NodeIter iter(_internal);
  
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
  Preparation p;
  Storage src_storage(TEST_FILE);
  Storage dest_storage(TEST_FILE "2");
  
  auto src_db = src_storage["test"];
  auto src_cursor = src_db.cursor();

  src_cursor.find("key1");
  src_cursor.value("value1");
  src_cursor.commit();
  
  // Use iterator to read source
  auto src__internal = src_db._internal();
  NodeIter iter(src__internal);
  
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
  Preparation p;
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
  NodeIter iter(src__internal);
  
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
  Preparation p;
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
  NodeIter iter(src__internal);

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
  Preparation p;
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
  NodeIter iter(src__internal);

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
