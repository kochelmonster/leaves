#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE LSMMapStorageTest

#include <boost/test/included/unit_test.hpp>
#include <filesystem>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/lsm_mmap.hpp"

using namespace leaves;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_lsm_mmap";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

BOOST_AUTO_TEST_CASE(test_lsm_mapstorage_creation) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_map.lvs";

  LSMMapStorage storage(dbFilePath.c_str());
  auto db = storage["test"];

  BOOST_CHECK(!db.name().empty());
  BOOST_TEST_MESSAGE(
      "LSMMapStorage created successfully with name: " << db.name().string());
}

BOOST_AUTO_TEST_CASE(test_lsm_mapstorage_basic) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_basic.lvs";

  LSMMapStorage storage(dbFilePath.c_str());
  auto db = storage["test"];

  // Verify storage properties
  BOOST_CHECK(!storage.filename().empty());
  BOOST_CHECK(storage.file_size() > 0);

  BOOST_TEST_MESSAGE("LSMMapStorage filename: " << storage.filename().string());
  BOOST_TEST_MESSAGE("LSMMapStorage file_size: " << storage.file_size());
}

BOOST_AUTO_TEST_CASE(test_lsm_mapstorage_multiple_dbs) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_multi.lvs";

  LSMMapStorage storage(dbFilePath.c_str());

  auto db1 = storage["test1"];
  auto db2 = storage["test2"];
  auto db3 = storage["test3"];

  BOOST_CHECK_EQUAL(db1.name().string(), "test1");
  BOOST_CHECK_EQUAL(db2.name().string(), "test2");
  BOOST_CHECK_EQUAL(db3.name().string(), "test3");

  // List all databases (note: list_dbs returns all slots, including empty ones)
  std::vector<std::string> db_list;
  storage.list_dbs(db_list);

  // Count non-empty database names
  int actual_db_count = 0;
  for (const auto& name : db_list) {
    if (!name.empty() && name[0] != '\0') {
      actual_db_count++;
    }
  }
  
  BOOST_CHECK_EQUAL(actual_db_count, 3);
  BOOST_TEST_MESSAGE("Created " << actual_db_count << " databases");
}

BOOST_AUTO_TEST_CASE(test_lsm_mapstorage_remove_db) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_remove_db.lvs";

  {
    LSMMapStorage storage(dbFilePath.c_str());
    auto db = storage["to_remove"];
    BOOST_CHECK_EQUAL(db.name().string(), "to_remove");
    // db goes out of scope here
  }
  
  // Reopen and remove
  {
    LSMMapStorage storage(dbFilePath.c_str());
    storage.remove_db("to_remove");

    // Verify it's removed (check non-empty names only)
    std::vector<std::string> db_list;
    storage.list_dbs(db_list);

    bool found = false;
    for (const auto& name : db_list) {
      if (name == "to_remove") {
        found = true;
        break;
      }
    }
    
    BOOST_CHECK(!found);
    BOOST_TEST_MESSAGE("Database removed successfully");
  }
}

BOOST_AUTO_TEST_CASE(test_lsm_mapstorage_persistence) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_persist.lvs";

  // Create storage and DB
  {
    LSMMapStorage storage(dbFilePath.c_str());
    auto db = storage["persist_test"];
    BOOST_CHECK_EQUAL(db.name().string(), "persist_test");
  }

  // Reopen and verify DB still exists
  {
    LSMMapStorage storage(dbFilePath.c_str());
    std::vector<std::string> db_list;
    storage.list_dbs(db_list);

    BOOST_CHECK(std::find(db_list.begin(), db_list.end(), "persist_test") !=
                db_list.end());
    BOOST_TEST_MESSAGE("Database persisted correctly across storage reopens");
  }
}

BOOST_AUTO_TEST_CASE(test_lsm_cursor) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test_lsm_cursor.lvs";
  
  // Insert a key-value pair
  {
    LSMMapStorage storage(dbFilePath.c_str());
    auto db = storage["test_cursor"];
    auto cursor = db.cursor();

    cursor.find("hello");
    BOOST_CHECK(!cursor.is_valid());  // Should not exist yet
    
    cursor.value("world");
    cursor.commit();
    
    BOOST_CHECK(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.key().string(), "hello");
    BOOST_CHECK_EQUAL(cursor.value().string(), "world");
  }
  
  // Read it back
  {
    LSMMapStorage storage(dbFilePath.c_str());
    auto db = storage["test_cursor"];
    auto cursor = db.cursor();
    
    cursor.find("hello");
    BOOST_CHECK(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.key().string(), "hello");
    BOOST_CHECK_EQUAL(cursor.value().string(), "world");
  }
  
  BOOST_TEST_MESSAGE("LSM cursor operations successful");
}
