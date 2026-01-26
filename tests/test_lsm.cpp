#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE LSMTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/lsm.hpp"

using namespace leaves;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_lsm";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

BOOST_AUTO_TEST_CASE(test_lsm_basic_insert) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  auto storage = LSMMapStorage::create(dbFilePath.c_str());
  auto db = storage->lsm("test");
  
  auto cursor = db.cursor();
  cursor.start_transaction();
  
  // Insert some key-value pairs
  cursor.find("key1");
  cursor.value("value1");
  
  cursor.find("key2");
  cursor.value("value2");
  
  cursor.find("key3");
  cursor.value("value3");
  
  cursor.commit();
  
  // Read back the values
  auto read_cursor = db.cursor();
  
  read_cursor.find("key1");
  BOOST_CHECK(read_cursor.is_valid());
  BOOST_CHECK_EQUAL(read_cursor.value().string(), "value1");
  
  read_cursor.find("key2");
  BOOST_CHECK(read_cursor.is_valid());
  BOOST_CHECK_EQUAL(read_cursor.value().string(), "value2");
  
  read_cursor.find("key3");
  BOOST_CHECK(read_cursor.is_valid());
  BOOST_CHECK_EQUAL(read_cursor.value().string(), "value3");
}

BOOST_AUTO_TEST_CASE(test_lsm_iteration) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  auto storage = LSMMapStorage::create(dbFilePath.c_str());
  auto db = storage->lsm("test");
  
  auto cursor = db.cursor();
  cursor.start_transaction();
  
  // Insert keys in random order
  cursor.find("charlie");
  cursor.value("3");
  
  cursor.find("alpha");
  cursor.value("1");
  
  cursor.find("bravo");
  cursor.value("2");
  
  cursor.commit();
  
  // Iterate forward - should be sorted
  auto iter = db.cursor();
  iter.first();
  
  BOOST_CHECK(iter.is_valid());
  BOOST_CHECK_EQUAL(iter.key().string(), "alpha");
  
  iter.next();
  BOOST_CHECK(iter.is_valid());
  BOOST_CHECK_EQUAL(iter.key().string(), "bravo");
  
  iter.next();
  BOOST_CHECK(iter.is_valid());
  BOOST_CHECK_EQUAL(iter.key().string(), "charlie");
  
  iter.next();
  BOOST_CHECK(!iter.is_valid());
}

BOOST_AUTO_TEST_CASE(test_lsm_overwrite) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  auto storage = LSMMapStorage::create(dbFilePath.c_str());
  auto db = storage->lsm("test");
  
  // First write
  {
    auto cursor = db.cursor();
    cursor.start_transaction();
    cursor.find("key1");
    cursor.value("original");
    cursor.commit();
  }
  
  // Verify first write
  {
    auto cursor = db.cursor();
    cursor.find("key1");
    BOOST_CHECK(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value().string(), "original");
  }
  
  // Overwrite
  {
    auto cursor = db.cursor();
    cursor.start_transaction();
    cursor.find("key1");
    cursor.value("updated");
    cursor.commit();
  }
  
  // Verify overwrite - L0 should take precedence
  {
    auto cursor = db.cursor();
    cursor.find("key1");
    BOOST_CHECK(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value().string(), "updated");
  }
}

BOOST_AUTO_TEST_CASE(test_lsm_merge_threshold) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  
  auto storage = LSMMapStorage::create(dbFilePath.c_str());
  auto db = storage->lsm("test");
  
  // Set a small merge threshold for testing
  db.set_merge_threshold(1024);  // 1KB
  
  auto cursor = db.cursor();
  cursor.start_transaction();
  
  // Insert enough data to trigger merge
  for (int i = 0; i < 100; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value(100, 'x');  // 100 byte value
    cursor.find(key);
    cursor.value(value);
  }
  
  cursor.commit();
  
  // Data should be readable even if a merge is in progress
  // because cursors now also read from the inactive L0 during merge
  auto read_cursor = db.cursor();
  read_cursor.find("key0");
  BOOST_CHECK(read_cursor.is_valid());
  if (read_cursor.is_valid()) {
    BOOST_CHECK_EQUAL(read_cursor.value().string().size(), 100);
  }
}
