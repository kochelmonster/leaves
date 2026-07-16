#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DBMMapTest

#include <boost/test/included/unit_test.hpp>
#include <vector>
#include <cstring>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/storage/_mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"
#include "leaves/mmap.hpp"

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
  uint32_t created_pivot = 0;

  {
    // Create a DBMMap instance and initialize it
    DBMMap db(dbFilePath.c_str());

    // Check if the database file is created
    BOOST_REQUIRE(std::filesystem::exists(dbFilePath));

    // Check if the active head is not null after initialization
    BOOST_REQUIRE(db._memory != nullptr);
    BOOST_REQUIRE_EQUAL(db._memory->file_size, DBMMap::AREA_SIZE);
    BOOST_REQUIRE_EQUAL(db._memory->db_version, 0);
    BOOST_REQUIRE_EQUAL(db._memory->signature, MMAP_SIGNATURE);
    BOOST_REQUIRE_NE(db._memory->copy_write_pivot_bytes, 0U);
    created_pivot = db._memory->copy_write_pivot_bytes;
  }

  {
    DBMMap db(dbFilePath.c_str());
    BOOST_REQUIRE_EQUAL(db._memory->copy_write_pivot_bytes, created_pivot);
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

BOOST_AUTO_TEST_CASE(test_copy_write_pivot_path) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "copy_pivot.lvs";

  DBMMap db(dbFilePath.c_str());
  db._memory->copy_write_pivot_bytes = 64;

  char* mmap_dest = (char*)db._memory + 512 * K;
  std::vector<char> large(1024, 'L');
  std::vector<char> small(32, 'S');

  uint64_t before = db._copy_write_path_hits.load(std::memory_order_relaxed);

    bool wrote = db.copy(mmap_dest, large.data(), large.size());
    BOOST_CHECK(wrote);
  BOOST_REQUIRE_EQUAL(std::memcmp(mmap_dest, large.data(), large.size()), 0);
  BOOST_CHECK_EQUAL(
      db._copy_write_path_hits.load(std::memory_order_relaxed), before + 1);

    wrote = db.copy(mmap_dest + 4096, small.data(), small.size());
    BOOST_CHECK(!wrote);
  BOOST_CHECK_EQUAL(
      db._copy_write_path_hits.load(std::memory_order_relaxed), before + 1);

  db._memory->copy_write_pivot_bytes = 0;
  std::vector<char> heap_dest(large.size(), 0);
    wrote = db.copy(heap_dest.data(), large.data(), large.size());
    BOOST_CHECK(!wrote);
  BOOST_REQUIRE_EQUAL(std::memcmp(heap_dest.data(), large.data(), large.size()),
                      0);
  BOOST_CHECK_EQUAL(
      db._copy_write_path_hits.load(std::memory_order_relaxed), before + 1);

    wrote = db.copy(mmap_dest, large.data(), 0);
    BOOST_CHECK(!wrote);
}

BOOST_AUTO_TEST_CASE(test_commit_sync_triggers_fd_sync_after_direct_copy_write) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "copy_sync_commit.lvs";

  DBMMap db(dbFilePath.c_str());
  db._memory->copy_write_pivot_bytes = 64;

  uint64_t before_copy = db._copy_write_path_hits.load(std::memory_order_relaxed);
  uint64_t before_sync = db._copy_write_sync_hits.load(std::memory_order_relaxed);

  auto* d = db.open("syncfd");
  auto cursor = d->create_cursor();
  cursor->find("k");
  cursor->value(std::string(512, 'z'));
  BOOST_CHECK(cursor->commit(true));

  BOOST_CHECK_GT(db._copy_write_path_hits.load(std::memory_order_relaxed),
                 before_copy);
  BOOST_CHECK_EQUAL(db._copy_write_sync_hits.load(std::memory_order_relaxed),
                    before_sync + 1);
}

BOOST_AUTO_TEST_CASE(test_constructor_copy_pivot_override) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "pivot_override.lvs";

  DBMMap db(dbFilePath.c_str(), 2 * G, SIZE_MAX, 12345);
  BOOST_CHECK_EQUAL(db._memory->copy_write_pivot_bytes, 12345U);

  DBMMap db_reopen(dbFilePath.c_str(), 2 * G, SIZE_MAX, 23456);
  BOOST_CHECK_EQUAL(db_reopen._memory->copy_write_pivot_bytes, 23456U);
}

BOOST_AUTO_TEST_CASE(test_mmap_tool_calibration_function) {
  DirPreparation prep;
  std::filesystem::path calibration_file = prep.tempDir / "tool-calibration.tmp";

  auto pivot = MapStorage::calibrate_copy_write_pivot(calibration_file.c_str());
  BOOST_CHECK_GT(pivot, 0U);
  BOOST_CHECK(!std::filesystem::exists(calibration_file));
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
  if constexpr (DBMMap::MAX_PROCESSES > 1) {
    db._memory->processes[2] = 0xFFFFFFFF;
    auto first = db.sanitize_processes();
    BOOST_CHECK(!first);
  }

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
    db.remove("test");
    BOOST_FAIL("Expected WrongValue exception not thrown");
  } catch (const WrongValue& e) {
    BOOST_CHECK_EQUAL(std::string(e.what()), "database does not exist.");
  }
  catch(...) {
    BOOST_FAIL("Expected NoProcess exception not thrown");
  }

#ifndef LEAVES_SINGLE_PROCESS
  for(int i = 0; i < db.MAX_PROCESSES; i++) {
    if (!db._memory->processes[i])
      db._memory->processes[i] = db._pid;
  }
  
  try {
    db.sanitize();
    BOOST_FAIL("Expected NoProcess exception not thrown");
  } catch (const NoProcess& e) {
    // this is right
  }
  catch(...) {
    BOOST_FAIL("Expected NoProcess exception not thrown");
  }
#endif

  // Test max_processes mismatch detection
  db._memory->max_processes = db.MAX_PROCESSES + 1;
  try {
    DBMMap db2(dbFilePath.c_str());
    BOOST_FAIL("Expected WrongValue exception not thrown");
  } catch (const WrongValue& e) {
    BOOST_CHECK_EQUAL(std::string(e.what()), "max_processes does not match.");
  }
  db._memory->max_processes = db.MAX_PROCESSES;  // restore
}

// ── Overflow page tests for _MemoryMapFile coverage ─────────────────────
// The first directory page holds ~109 DB entries; creating more forces
// _alloc_overflow_slot, _for_each_db_entry overflow iteration, and
// remove with overflow page traversal.

BOOST_AUTO_TEST_CASE(test_mmap_overflow_page_creation) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "overflow.lvs";

  const int NUM_DBS = 120;  // > 109 first-page capacity

  {
    DBMMap db(dbFilePath.c_str());

    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto* d = db.open(name);
      auto cursor = d->create_cursor();
      cursor->find("key");
      cursor->value(std::string("val_") + name);
      cursor->commit();
    }

    // Verify list_dbs returns all (exercises _for_each_db_entry overflow)
    std::vector<std::string> names;
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }

  // Reopen from disk — exercises _alloc_overflow_slot scan of existing pages
  {
    DBMMap db(dbFilePath.c_str());

    // Open a DB from the first page
    auto* d0 = db.open("db_0000");
    auto c0 = d0->create_cursor();
    c0->find("key");
    BOOST_REQUIRE(c0->is_valid());
    BOOST_CHECK_EQUAL(c0->value(), Slice("val_db_0000"));

    // Open a DB from the overflow page
    auto* d_over = db.open("db_0115");
    auto c_over = d_over->create_cursor();
    c_over->find("key");
    BOOST_REQUIRE(c_over->is_valid());
    BOOST_CHECK_EQUAL(c_over->value(), Slice("val_db_0115"));

    std::vector<std::string> names;
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }
}

BOOST_AUTO_TEST_CASE(test_mmap_overflow_page_remove) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "overflow_rm.lvs";

  const int NUM_DBS = 115;

  {
    DBMMap db(dbFilePath.c_str());
    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto* d = db.open(name);
      auto cursor = d->create_cursor();
      cursor->find("k");
      cursor->value("v");
      cursor->commit();
    }
  }

  {
    DBMMap db(dbFilePath.c_str());

    // Remove a DB from the overflow page
    db.remove("db_0112");

    std::vector<std::string> names;
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS - 1);

    // Remove a DB from the first page
    db.remove("db_0000");
    names.clear();
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS - 2);
  }

  // Reopen and verify removals persisted
  {
    DBMMap db(dbFilePath.c_str());
    BOOST_CHECK_THROW(db.remove("db_0000"), WrongValue);
    BOOST_CHECK_THROW(db.remove("db_0112"), WrongValue);

    // A surviving overflow DB should still be accessible
    auto* d = db.open("db_0113");
    auto cursor = d->create_cursor();
    cursor->find("k");
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value(), Slice("v"));
  }
}

BOOST_AUTO_TEST_CASE(test_mmap_overflow_reuse_free_slot) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "overflow_reuse.lvs";

  const int NUM_DBS = 115;

  DBMMap db(dbFilePath.c_str());
  for (int i = 0; i < NUM_DBS; i++) {
    char name[22];
    snprintf(name, sizeof(name), "db_%04d", i);
    auto* d = db.open(name);
    auto cursor = d->create_cursor();
    cursor->find("k");
    cursor->value("v");
    cursor->commit();
  }

  // Remove a first-page DB (frees slot), then create a new DB reusing it
  db.remove("db_0050");
  {
    auto* d = db.open("db_reuse_slot");
    auto cursor = d->create_cursor();
    cursor->find("hello");
    cursor->value("world");
    cursor->commit();
  }

  // Remove an overflow-page DB and create a new one in that slot
  db.remove("db_0112");
  {
    auto* d = db.open("db_reuse_overflow");
    auto cursor = d->create_cursor();
    cursor->find("hello");
    cursor->value("world2");
    cursor->commit();
  }

  std::vector<std::string> names;
  db.list_dbs(names);
  BOOST_CHECK_EQUAL(names.size(), NUM_DBS);  // same count (removed 2, added 2)
}

BOOST_AUTO_TEST_CASE(test_mmap_remove_returns_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "rm_areas.lvs";

  DBMMap db(dbFilePath.c_str());

  {
    auto* d = db.open("test");
    auto cursor = d->create_cursor();
    for (int i = 0; i < 100; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%04d", i);
      cursor->find(key);
      cursor->value(std::string(500, 'x'));
    }
    cursor->commit();
  }

  // Remove the DB — exercises return_areas() + return_single_areas
  db.remove("test");
  BOOST_CHECK_THROW(db.remove("test"), WrongValue);

  // Create a new DB — should succeed, reusing freed areas
  auto* d2 = db.open("test2");
  auto cursor2 = d2->create_cursor();
  cursor2->find("k");
  cursor2->value("v");
  cursor2->commit();
}

BOOST_AUTO_TEST_CASE(test_mmap_type_mismatch) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "type_mm.lvs";

  {
    DBMMap db(dbFilePath.c_str());
    auto* d = db.open("test");
    auto cursor = d->create_cursor();
    cursor->find("k");
    cursor->value("v");
    cursor->commit();
  }

  {
    DBMMap db(dbFilePath.c_str());
    // Open as _ReplicationDB — should throw TypeMismatch
    BOOST_CHECK_THROW(db.open<_ReplicationDB>("test"), TypeMismatch);
  }
}

BOOST_AUTO_TEST_CASE(test_mmap_remove_type_mismatch) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "rm_type_mm.lvs";

  {
    DBMMap db(dbFilePath.c_str());
    auto* d = db.open("test");
    auto cursor = d->create_cursor();
    cursor->find("k");
    cursor->value("v");
    cursor->commit();
  }

  {
    DBMMap db(dbFilePath.c_str());
    // Remove with wrong type — should throw TypeMismatch
    BOOST_CHECK_THROW(db.remove<_ReplicationDB>("test"), TypeMismatch);
  }
}

BOOST_AUTO_TEST_CASE(test_mmap_recover_areas) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "recover.lvs";

  {
    DBMMap db(dbFilePath.c_str());
    auto* d = db.open("test");
    auto cursor = d->create_cursor();
    for (int i = 0; i < 50; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%04d", i);
      cursor->find(key);
      cursor->value(std::string(200, 'y'));
    }
    cursor->commit();
  }

  // Reopen — sanitize() calls recover_areas() when it detects no live processes
  // In single-process mode, sanitize_processes() always returns true,
  // so recover_areas() is always called on reopen.
  {
    DBMMap db(dbFilePath.c_str());
    auto* d = db.open("test");
    auto cursor = d->create_cursor();
    cursor->find("key_0000");
    BOOST_REQUIRE(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value(), Slice(std::string(200, 'y')));
  }
}

BOOST_AUTO_TEST_CASE(test_mmap_second_overflow_page) {
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "overflow2.lvs";

  // Need > first_page + first_overflow to trigger second overflow page.
  // first_page ≈ 109, overflow ≈ 146. Use 280 to be safe.
  const int NUM_DBS = 280;

  {
    DBMMap db(dbFilePath.c_str());

    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto* d = db.open(name);
      auto cursor = d->create_cursor();
      cursor->find("key");
      cursor->value(std::string("val_") + name);
      cursor->commit();
    }

    std::vector<std::string> names;
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }

  // Reopen and verify DBs from all three pages
  {
    DBMMap db(dbFilePath.c_str());

    // First page
    auto* d0 = db.open("db_0000");
    auto c0 = d0->create_cursor();
    c0->find("key");
    BOOST_REQUIRE(c0->is_valid());
    BOOST_CHECK_EQUAL(c0->value(), Slice("val_db_0000"));

    // Second overflow page
    auto* d_ov2 = db.open("db_0275");
    auto c_ov2 = d_ov2->create_cursor();
    c_ov2->find("key");
    BOOST_REQUIRE(c_ov2->is_valid());
    BOOST_CHECK_EQUAL(c_ov2->value(), Slice("val_db_0275"));

    std::vector<std::string> names;
    db.list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }
}

BOOST_AUTO_TEST_CASE(test_pool_threads_zero) {
  // Exercises _mmap.hpp L177 — pool_threads=0 auto-detection
  DirPreparation p;
  std::filesystem::path dbFilePath = p.tempDir / "test_pool.lvs";

  // Constructor with pool_threads=0: auto-detect thread count
  DBMMap storage(dbFilePath.c_str(), 4 * G, 0);
  auto db = storage.open("test");

  // Basic operations should work
  BOOST_REQUIRE(db->start_transaction(0));
  db->alloc_page(80);
  BOOST_CHECK(db->commit(0));
}

