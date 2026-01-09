#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMMapTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_db";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

void wrong_signature(const char* path) { DBMMap db(path); }
BOOST_AUTO_TEST_CASE(test_init) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";

  {
    // Create a DBMMap instance and initialize it
    DBMMap db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the active head is not null after initialization
    BOOST_REQUIRE(db._memory != nullptr);
    BOOST_REQUIRE_EQUAL(db._memory->file_size, 4 * K);
    BOOST_REQUIRE_EQUAL(db._memory->db_version, 0);
    BOOST_REQUIRE_EQUAL(db._memory->signature, MMAP_SIGNATURE);
  }

  {
    DBMMap db(dbFilePath.c_str());
  }

  // Change the first byte of the file to 0
  std::fstream file(dbFilePath,
                    std::ios::in | std::ios::out | std::ios::binary);
  if (file.is_open()) {
    file.seekp(0);
    file.put(0);
    file.close();
  }

  BOOST_CHECK_THROW(wrong_signature(dbFilePath.c_str()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_double_open) {
  DirPreparation prep;
  // Create a temporary file path
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db1(dbFilePath.c_str());
  DBMMap db2(dbFilePath.c_str());

  BOOST_CHECK_EQUAL(db1._memory->db_version, 0);
  BOOST_CHECK_EQUAL(db2._memory->db_version, 0);

  db1._memory->db_version = 1;
  BOOST_CHECK_EQUAL(db2._memory->db_version, 1);
}

using PageHeader = DBMMap::Traits::PageHeader;
using page_ptr = DBMMap::page_ptr;


BOOST_AUTO_TEST_CASE(test_sanitize) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());
  db._memory->processes[2] = 0xFFFFFFFF;
  auto first = db.sanitize_processes();
  BOOST_CHECK(!first);

  std::filesystem::resize_file(dbFilePath.c_str(), db._memory->file_size + 20);
  db.sanitize();
  BOOST_CHECK_EQUAL(std::filesystem::file_size(dbFilePath.c_str()),
                    db._memory->file_size);
}

BOOST_AUTO_TEST_CASE(test_exceptions) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap db(dbFilePath.c_str());
  try {
    db.remove_db("test");
    BOOST_FAIL("Expected WrongValue exception not thrown");
  } catch (const WrongValue& e) {
    BOOST_CHECK_EQUAL(std::string(e.what()), "database does not exist.");
  }
  catch(...) {
    BOOST_FAIL("Expected NoProcess exception not thrown");
  }

  for(int i = 0; i < db.MAX_PROCESSES; i++) {
    if (!db._memory->processes[i])
      db._memory->processes[i] = 0xFFFFFFFF;
  }
  
  try {
    db.set_pid();
    BOOST_FAIL("Expected NoProcess exception not thrown");
  } catch (const NoProcess& e) {
    // this is right
  }
  catch(...) {
    BOOST_FAIL("Expected NoProcess exception not thrown");
  }

  try {
    DBMMap db(dbFilePath.c_str(), 2 * G, 2);
    BOOST_FAIL("Expected WrongValue exception not thrown");
  } catch (const WrongValue& e) {
    BOOST_CHECK_EQUAL(std::string(e.what()), "db_count may not be changed.");
  }
  catch(...) {
    BOOST_FAIL("Expected NoProcess exception not thrown");
  }

  
}

