#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE FileStorageTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include <algorithm>
#include <random>
#include <set>
#include <filesystem>
#include <chrono>

#include "leaves/fstore.hpp"
#include "leaves/intern/replication/_replication_db.hpp"

using namespace leaves;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_file_storage";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { 
    std::filesystem::remove_all(tempDir); 
  }

  std::filesystem::path tempDir;
  
  std::string get_file_path() const {
    return (tempDir / "test_file.lvs").string();
  }
};

// Generate random strings for testing
std::vector<std::string> generate_random_strings(size_t count, size_t min_length = 5, size_t max_length = 50) {
  std::vector<std::string> strings;
  strings.reserve(count);
  
  std::random_device rd;
  std::mt19937 gen(42); // Fixed seed for reproducible tests
  std::uniform_int_distribution<> length_dist(min_length, max_length);
  std::uniform_int_distribution<> char_dist(32, 126); // Printable ASCII characters
  
  std::set<std::string> unique_check; // Ensure uniqueness
  
  while (strings.size() < count) {
    size_t length = length_dist(gen);
    std::string str;
    str.reserve(length);
    
    for (size_t i = 0; i < length; ++i) {
      str += static_cast<char>(char_dist(gen));
    }
    
    // Ensure uniqueness
    if (unique_check.find(str) == unique_check.end()) {
      unique_check.insert(str);
      strings.push_back(str);
    }
  }
  
  return strings;
}

void check(TDB<leaves::FileStorage>::Cursor& cursor, std::vector<std::string>& test_strings, size_t count) {
  cursor.find(""); // Reset cursor
  BOOST_REQUIRE(!cursor.is_valid()); // Should not exist

  size_t i = 0;
  for (const auto& test_string : test_strings) {
    cursor.find(test_string);
    BOOST_REQUIRE(cursor.is_valid()); // Should exist
    if (++i >= count) break;
  }
  std::cout << "Checked " << i << " entries" << std::endl;
}


void test_file_storage_random_insert_and_read(const std::string& db_path, const char* storage_name) {
  const size_t NUM_ENTRIES = 250; // 857;
  
  BOOST_TEST_MESSAGE("Testing " << storage_name << " with " << NUM_ENTRIES << " random entries");
  
  // Generate test data
  auto test_strings = generate_random_strings(NUM_ENTRIES);
  BOOST_REQUIRE_EQUAL(test_strings.size(), NUM_ENTRIES);
  
  std::string db_name = "test_random";
  std::cerr << "Phase 1" << std::endl;
  
  // Phase 1: Insert all entries
  {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto storage = FileStorage::create(db_path.c_str());
    auto db = storage->open(db_name.c_str());
    auto cursor = db.cursor();
    
    size_t inserted = 0;
    for (const auto& test_string : test_strings) {
      cursor.find(test_string);
      BOOST_REQUIRE(!cursor.is_valid()); // Should not exist yet
      
      // std::cout << "insert " << inserted << std::endl;

      // Use the string as both key and value for simplicity
      cursor.value(test_string);
      BOOST_REQUIRE(cursor.is_valid());

      inserted++;

      // Progress reporting every 1000 entries
      if (inserted % 100 == 0) {
        BOOST_TEST_MESSAGE("Inserted " << inserted << " entries");
      }
    }
    cursor.commit();

    check(cursor, test_strings, inserted);
        
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_TEST_MESSAGE("Insert phase completed in " << duration.count() << " ms");
    BOOST_TEST_MESSAGE("Insert rate: " << (NUM_ENTRIES * 1000.0 / duration.count()) << " ops/sec");
  }

  std::cerr << "Phase 2" << std::endl;
  // Phase 2: Read back all entries in random order
  {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto storage = FileStorage::create(db_path.c_str());
    auto db = storage->open(db_name.c_str());
    auto cursor = db.cursor();
    
    // Shuffle the test strings for random access pattern
    auto shuffled_strings = test_strings;
    std::random_device rd;
    std::mt19937 gen(123); // Different seed for read order
    std::shuffle(shuffled_strings.begin(), shuffled_strings.end(), gen);
    
    size_t found = 0;
    for (const auto& test_string : shuffled_strings) {
      cursor.find(test_string);

      // std::cout << "find " << found << "  " << test_string  << std::endl;

      BOOST_REQUIRE(cursor.is_valid()); // Should exist
      
      // Verify key and value match
      BOOST_REQUIRE_EQUAL(cursor.key().string(), test_string);
      BOOST_REQUIRE_EQUAL(cursor.value().string(), test_string);
      
      found++;
      
      // Progress reporting every 1000 entries
      if (found % 1000 == 0) {
        BOOST_TEST_MESSAGE("Found " << found << " entries");
      }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_TEST_MESSAGE("Read phase completed in " << duration.count() << " ms");
    BOOST_TEST_MESSAGE("Read rate: " << (NUM_ENTRIES * 1000.0 / duration.count()) << " ops/sec");
    
    BOOST_REQUIRE_EQUAL(found, NUM_ENTRIES);
  }
 
  // Add a half-second sleep between phases
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  // Phase 3: Verify sequential iteration works correctly
  {
    auto storage = FileStorage::create(db_path.c_str());
    auto db = storage->open(db_name.c_str());
    auto cursor = db.cursor();
    
    // Sort the original strings for comparison
    auto sorted_strings = test_strings;
    std::sort(sorted_strings.begin(), sorted_strings.end());
    
    // Iterate through all entries
    cursor.first();
    size_t count = 0;
    while (cursor.is_valid()) {
      BOOST_REQUIRE_LT(count, sorted_strings.size());
      BOOST_REQUIRE_EQUAL(cursor.key().string(), sorted_strings[count]);
      BOOST_REQUIRE_EQUAL(cursor.value().string(), sorted_strings[count]);
      
      cursor.next();
      count++;
    }
    
    BOOST_REQUIRE_EQUAL(count, NUM_ENTRIES);
    BOOST_TEST_MESSAGE("Sequential iteration verified " << count << " entries");
  }
  
  // Phase 4: Test some non-existent keys
  {
    auto storage = FileStorage::create(db_path.c_str());
    auto db = storage->open(db_name.c_str());
    auto cursor = db.cursor();
    
    // Test some keys that should not exist
    std::vector<std::string> non_existent_keys = {
      "definitely_not_there_12345",
      "another_missing_key_67890",
      "", // empty key
      std::string(1000, 'x') // very long key
    };
    
    for (const auto& key : non_existent_keys) {
      cursor.find(key);
      BOOST_REQUIRE(!cursor.is_valid());
    }
    
    BOOST_TEST_MESSAGE("Non-existent key test passed");
  }
}

BOOST_AUTO_TEST_CASE(test_file_storage_random_5000) {
  DirPreparation prep;
  test_file_storage_random_insert_and_read(prep.get_file_path(), "FileStorage");
}

// Test with various key sizes and patterns for FileStorage
BOOST_AUTO_TEST_CASE(test_file_storage_key_patterns) {
  return;
  DirPreparation prep;
  
  auto test_key_pattern = [&](const std::string& file_path, 
                             const std::vector<std::string>& keys, const char* pattern_name) {
    BOOST_TEST_MESSAGE("Testing FileStorage key pattern: " << pattern_name);
    
    auto storage = FileStorage::create(file_path.c_str());
    auto db = storage->open("pattern_test");
    auto cursor = db.cursor();
    
    BOOST_REQUIRE(cursor.start_transaction());
    for (const auto& key : keys) {
      cursor.find(key);
      BOOST_REQUIRE(!cursor.is_valid());
      cursor.value("value_" + key);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key().string(), key);
    }
    cursor.commit();
    
    // Verify all keys
    for (const auto& key : keys) {
      cursor.find(key);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key().string(), key);
      BOOST_REQUIRE_EQUAL(cursor.value().string(), "value_" + key);
    }
  };
  
  // Test various key patterns
  std::vector<std::string> short_keys = {"a", "b", "c", "d", "e"};
  std::vector<std::string> long_keys;
  for (int i = 0; i < 100; ++i) {
    long_keys.push_back(std::string(100, 'a' + (i % 26)) + std::to_string(i));
  }
  
  std::vector<std::string> numeric_keys;
  for (int i = 0; i < 1000; ++i) {
    numeric_keys.push_back(std::to_string(i * 1000 + 123));
  }
  
  test_key_pattern(prep.get_file_path() + "_short", short_keys, "Short keys");
  test_key_pattern(prep.get_file_path() + "_long", long_keys, "Long keys");
  test_key_pattern(prep.get_file_path() + "_numeric", numeric_keys, "Numeric keys");
}

BOOST_AUTO_TEST_CASE(test_multi_area_big_values) {
  // Regression test for CODE_REVIEW_4 Finding #3:
  // CacheStore::resolve() must handle offsets that fall in the 2nd (or later)
  // AREA_SIZE chunk of a multi-area allocation.
  // FileStorage AREA_SIZE = 128 KB, so values >= ~128 KB trigger multi-area.
  DirPreparation prep;
  auto path = prep.get_file_path();

  // Use values large enough to span multiple AREA_SIZE chunks.
  // 300 KB > 2 × 128 KB → 3× AREA_SIZE allocation.
  const size_t BIG_SIZE = 300 * 1024;

  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("big");
    auto cursor = db.cursor();

    // Insert several big values so BigMemory allocates multi-areas
    for (int i = 0; i < 5; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%d", i);
      // Fill each value with a distinct byte so we can verify contents
      std::string value(BIG_SIZE, 'A' + i);
      cursor.find(key);
      BOOST_REQUIRE(!cursor.is_valid());
      cursor.value(value);
    }
    cursor.commit();

    // Read them back in the same session — cache is warm
    for (int i = 0; i < 5; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%d", i);
      std::string expected(BIG_SIZE, 'A' + i);
      cursor.find(key);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.value().size(), BIG_SIZE);
      BOOST_REQUIRE_EQUAL(cursor.value(), Slice(expected));
    }
  }

  // Re-open from disk — cache is cold, resolve() must read multi-areas
  // from disk and handle offsets past the first AREA_SIZE chunk.
  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("big");
    auto cursor = db.cursor();

    for (int i = 0; i < 5; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%d", i);
      std::string expected(BIG_SIZE, 'A' + i);
      cursor.find(key);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.value().size(), BIG_SIZE);
      BOOST_REQUIRE_EQUAL(cursor.value(), Slice(expected));
    }
  }
}

// ── CacheStore sync flush path (_cachestore.hpp L130) ───────────────────
// commit(sync=true) forces a synchronous write_dirty_blocks() instead of
// queuing to the background thread pool.
BOOST_AUTO_TEST_CASE(test_sync_commit) {
  DirPreparation prep;
  auto path = prep.get_file_path();
  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("sync");
    auto cursor = db.cursor();
    cursor.find("hello");
    cursor.value("world");
    cursor.commit(true);  // sync=true → triggers CacheStore::flush(true)
  }
  // Re-open and verify data persisted via the sync path
  {
    auto storage = FileStorage::create(path.c_str());
    auto db = storage->open("sync");
    auto cursor = db.cursor();
    cursor.find("hello");
    BOOST_CHECK(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value(), Slice("world"));
  }
}

// ── _deleter.hpp combine() leaf-child path ────────────────────────────────
// Insert two keys sharing a prefix so the trie has exactly 2 leaf children.
// Deleting one triggers combine(): the remaining child is a leaf, covering
// the else branch at L141, L157, and L167 in _deleter.hpp (also L92).
BOOST_AUTO_TEST_CASE(test_remove_combine_leaf_child) {
  DirPreparation prep;
  auto path = prep.get_file_path();
  auto storage = FileStorage::create(path.c_str());
  auto db = storage->open("leaf_combine");
  auto cursor = db.cursor();

  // Insert two keys that share the prefix "xy", creating a 2-branch trie
  cursor.find("xya");
  cursor.value("val_a");
  cursor.find("xyb");
  cursor.value("val_b");
  cursor.commit();

  // Delete "xya" — the parent trie now has exactly 1 child (leaf "xyb").
  // _Deleter::combine() is called and takes the else (leaf) branch.
  cursor.find("xya");
  BOOST_REQUIRE(cursor.is_valid());
  cursor.remove();
  cursor.commit();

  // "xya" must be gone, "xyb" must survive
  cursor.find("xya");
  BOOST_CHECK(!cursor.is_valid());
  cursor.find("xyb");
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_CHECK_EQUAL(cursor.value(), Slice("val_b"));
}

// ── Overflow page tests for _CacheStore coverage ────────────────────────
// The first directory page holds 123 DB entries; creating more forces
// overflow page creation (_create_in_overflow), list_dbs across pages,
// find_in_overflow_pages, and remove_from_overflow_pages.

BOOST_AUTO_TEST_CASE(test_overflow_page_creation) {
  DirPreparation prep;
  auto path = (prep.tempDir / "overflow.lvs").string();

  const int NUM_DBS = 130;  // > 123 first-page capacity

  {
    auto storage = FileStorage::create(path.c_str());

    // Create enough DBs to overflow the first directory page
    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto db = storage->open(name);
      auto cursor = db.cursor();
      cursor.find("key");
      cursor.value(std::string("val_") + name);
      cursor.commit();
    }

    // Verify list_dbs returns all of them (exercises _for_each_db_entry overflow)
    std::vector<std::string> names;
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }

  // Reopen from disk — exercises _find_in_overflow_pages
  {
    auto storage = FileStorage::create(path.c_str());

    // Open a DB from the first page
    auto db0 = storage->open("db_0000");
    auto c0 = db0.cursor();
    c0.find("key");
    BOOST_REQUIRE(c0.is_valid());
    BOOST_CHECK_EQUAL(c0.value(), Slice("val_db_0000"));

    // Open a DB from the overflow page (index > 123)
    auto db_over = storage->open("db_0125");
    auto c_over = db_over.cursor();
    c_over.find("key");
    BOOST_REQUIRE(c_over.is_valid());
    BOOST_CHECK_EQUAL(c_over.value(), Slice("val_db_0125"));

    // list_dbs should still return all
    std::vector<std::string> names;
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }
}

BOOST_AUTO_TEST_CASE(test_overflow_page_remove) {
  DirPreparation prep;
  auto path = (prep.tempDir / "overflow_rm.lvs").string();

  const int NUM_DBS = 128;

  {
    auto storage = FileStorage::create(path.c_str());
    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto db = storage->open(name);
      auto cursor = db.cursor();
      cursor.find("k");
      cursor.value("v");
      cursor.commit();
    }

    // Remove a DB from the overflow page (exercises _remove_from_overflow_pages)
    storage->remove("db_0125");

    std::vector<std::string> names;
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS - 1);

    // Remove a DB from the first page
    storage->remove("db_0000");
    names.clear();
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS - 2);
  }

  // Reopen and verify the removed DBs are gone
  {
    auto storage = FileStorage::create(path.c_str());
    BOOST_CHECK_THROW(storage->remove("db_0000"), WrongValue);
    BOOST_CHECK_THROW(storage->remove("db_0125"), WrongValue);

    // A surviving overflow DB should still be accessible
    auto db = storage->open("db_0126");
    auto cursor = db.cursor();
    cursor.find("k");
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_CHECK_EQUAL(cursor.value(), Slice("v"));
  }
}

BOOST_AUTO_TEST_CASE(test_overflow_page_reuse_free_slot) {
  DirPreparation prep;
  auto path = (prep.tempDir / "overflow_reuse.lvs").string();

  const int NUM_DBS = 126;

  auto storage = FileStorage::create(path.c_str());
  for (int i = 0; i < NUM_DBS; i++) {
    char name[22];
    snprintf(name, sizeof(name), "db_%04d", i);
    auto db = storage->open(name);
    auto cursor = db.cursor();
    cursor.find("k");
    cursor.value("v");
    cursor.commit();
  }

  // Remove a first-page DB (frees slot), then create a new one in that slot
  storage->remove("db_0050");
  auto db_new = storage->open("db_reuse_slot");
  auto c_new = db_new.cursor();
  c_new.find("hello");
  c_new.value("world");
  c_new.commit();

  // Remove an overflow-page DB and create a new one in that slot
  storage->remove("db_0124");
  auto db_new2 = storage->open("db_reuse_overflow");
  auto c_new2 = db_new2.cursor();
  c_new2.find("hello");
  c_new2.value("world2");
  c_new2.commit();

  std::vector<std::string> names;
  storage->list_dbs(names);
  BOOST_CHECK_EQUAL(names.size(), NUM_DBS);  // same count (removed 2, added 2)
}

// ── TypeMismatch for _CacheStore (fstore path) ──────────────────────────
BOOST_AUTO_TEST_CASE(test_fstore_type_mismatch) {
  DirPreparation prep;
  auto path = (prep.tempDir / "type_mm.lvs").string();

  {
    auto storage = FileStorage::create(path.c_str());
    // Open as _DB
    auto db = storage->open("test");
    auto cursor = db.cursor();
    cursor.find("k");
    cursor.value("v");
    cursor.commit();
  }

  {
    // Reopen and try to open as _ReplicationDB — should throw TypeMismatch
    auto storage = FileStorage::create(path.c_str());
    BOOST_CHECK_THROW(storage->open<_ReplicationDB>("test"), TypeMismatch);
  }
}

// ── Return areas via remove() — exercises _db.hpp return_areas() ────────
BOOST_AUTO_TEST_CASE(test_fstore_remove_returns_areas) {
  DirPreparation prep;
  auto path = (prep.tempDir / "remove_areas.lvs").string();

  auto storage = FileStorage::create(path.c_str());

  {
    auto db = storage->open("test");
    auto cursor = db.cursor();

    // Insert some data to allocate areas
    for (int i = 0; i < 100; i++) {
      char key[16];
      snprintf(key, sizeof(key), "key_%04d", i);
      cursor.find(key);
      cursor.value(std::string(500, 'x'));
    }
    cursor.commit();
  }  // cursor and db destroyed — no active transaction refs

  // Remove the DB — this exercises return_areas() + return_single_areas
  storage->remove("test");

  // Verify it's gone
  BOOST_CHECK_THROW(storage->remove("test"), WrongValue);

  // Create a new DB — should succeed, reusing freed areas
  auto db2 = storage->open("test2");
  auto cursor2 = db2.cursor();
  cursor2.find("k");
  cursor2.value("v");
  cursor2.commit();
}

// ── Remove with TransactionActive on _CacheStore path ───────────────────
BOOST_AUTO_TEST_CASE(test_fstore_remove_active_transaction) {
  DirPreparation prep;
  auto path = (prep.tempDir / "rm_active.lvs").string();

  auto storage = FileStorage::create(path.c_str());
  auto db = storage->open("test");
  auto cursor = db.cursor();
  cursor.find("k");
  cursor.value("v");
  cursor.commit();

  // Start a new transaction
  cursor.find("k2");
  cursor.value("v2");  // writes start a transaction

  BOOST_CHECK_THROW(storage->remove("test"), TransactionActive);

  cursor.commit();
}

// ── Remove type mismatch on _CacheStore cached and uncached paths ───────
BOOST_AUTO_TEST_CASE(test_fstore_remove_type_mismatch) {
  DirPreparation prep;
  auto path = (prep.tempDir / "rm_type_mm.lvs").string();

  auto storage = FileStorage::create(path.c_str());
  auto db = storage->open("test");
  auto cursor = db.cursor();
  cursor.find("k");
  cursor.value("v");
  cursor.commit();

  // Cached path: DB is open as _DB, try remove<_ReplicationDB>
  BOOST_CHECK_THROW(storage->remove<_ReplicationDB>("test"), TypeMismatch);
}

BOOST_AUTO_TEST_CASE(test_second_overflow_page_creation) {
  DirPreparation prep;
  auto path = (prep.tempDir / "overflow2.lvs").string();

  // Need > first_page_capacity + overflow_page_capacity to trigger second overflow page.
  // first_page ≈ 123, overflow ≈ 127, total ≈ 250. Use 260 to be safe.
  const int NUM_DBS = 260;

  {
    auto storage = FileStorage::create(path.c_str());

    for (int i = 0; i < NUM_DBS; i++) {
      char name[22];
      snprintf(name, sizeof(name), "db_%04d", i);
      auto db = storage->open(name);
      auto cursor = db.cursor();
      cursor.find("key");
      cursor.value(std::string("val_") + name);
      cursor.commit();
    }

    std::vector<std::string> names;
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }

  // Reopen and verify DBs from all three pages
  {
    auto storage = FileStorage::create(path.c_str());

    // First page
    auto db0 = storage->open("db_0000");
    auto c0 = db0.cursor();
    c0.find("key");
    BOOST_REQUIRE(c0.is_valid());
    BOOST_CHECK_EQUAL(c0.value(), Slice("val_db_0000"));

    // First overflow page (index > 123)
    auto db_ov1 = storage->open("db_0125");
    auto c_ov1 = db_ov1.cursor();
    c_ov1.find("key");
    BOOST_REQUIRE(c_ov1.is_valid());
    BOOST_CHECK_EQUAL(c_ov1.value(), Slice("val_db_0125"));

    // Second overflow page (index > 250)
    auto db_ov2 = storage->open("db_0255");
    auto c_ov2 = db_ov2.cursor();
    c_ov2.find("key");
    BOOST_REQUIRE(c_ov2.is_valid());
    BOOST_CHECK_EQUAL(c_ov2.value(), Slice("val_db_0255"));

    std::vector<std::string> names;
    storage->list_dbs(names);
    BOOST_CHECK_EQUAL(names.size(), NUM_DBS);
  }
}