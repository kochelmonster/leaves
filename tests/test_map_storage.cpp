#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MapStorageTest

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
#include "leaves/mmap.hpp"

using namespace leaves;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_storage";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
  }

  ~DirPreparation() { 
    std::filesystem::remove_all(tempDir); 
  }

  std::filesystem::path tempDir;
  
  std::string get_map_path() const {
    return (tempDir / "test_map.lvs").string();
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

void test_map_storage_random_insert_and_read(const std::string& db_path, const char* storage_name) {
  const size_t NUM_ENTRIES = 5000;
  
  BOOST_TEST_MESSAGE("Testing " << storage_name << " with " << NUM_ENTRIES << " random entries");
  
  // Generate test data
  auto test_strings = generate_random_strings(NUM_ENTRIES);
  BOOST_REQUIRE_EQUAL(test_strings.size(), NUM_ENTRIES);
  
  std::string db_name = "test_random";
  
  // Phase 1: Insert all entries
  {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto storage = MapStorage::create(db_path.c_str());
    auto db = (*storage)[db_name.c_str()];
    auto cursor = db.cursor();
    
    // Insert without explicit transaction management (auto-handled)
    
    size_t inserted = 0;
    for (const auto& test_string : test_strings) {
      cursor.find(test_string);
      BOOST_REQUIRE(!cursor.is_valid()); // Should not exist yet
      
      // Use the string as both key and value for simplicity
      cursor.value(test_string);
      cursor.commit(); // Commit each entry
      BOOST_REQUIRE(cursor.is_valid());
      
      inserted++;
      
      // Progress reporting every 1000 entries
      if (inserted % 1000 == 0) {
        BOOST_TEST_MESSAGE("Inserted " << inserted << " entries");
      }
    }
    
    // No explicit commit needed at end
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_TEST_MESSAGE("Insert phase completed in " << duration.count() << " ms");
    BOOST_TEST_MESSAGE("Insert rate: " << (NUM_ENTRIES * 1000.0 / duration.count()) << " ops/sec");
  }
  
  // Phase 2: Read back all entries in random order
  {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto storage = MapStorage::create(db_path.c_str());
    auto db = (*storage)[db_name.c_str()];
    auto cursor = db.cursor();
    
    // Shuffle the test strings for random access pattern
    auto shuffled_strings = test_strings;
    std::random_device rd;
    std::mt19937 gen(123); // Different seed for read order
    std::shuffle(shuffled_strings.begin(), shuffled_strings.end(), gen);
    
    size_t found = 0;
    for (const auto& test_string : shuffled_strings) {
      cursor.find(test_string);
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
  
  // Phase 3: Verify sequential iteration works correctly
  {
    auto storage = MapStorage::create(db_path.c_str());
    auto db = (*storage)[db_name.c_str()];
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
    auto storage = MapStorage::create(db_path.c_str());
    auto db = (*storage)[db_name.c_str()];
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

BOOST_AUTO_TEST_CASE(test_map_storage_random_5000) {
  DirPreparation prep;
  test_map_storage_random_insert_and_read(prep.get_map_path(), "MapStorage");
}

// Test with various key sizes and patterns for MapStorage
BOOST_AUTO_TEST_CASE(test_map_storage_key_patterns) {
  DirPreparation prep;
  
  auto test_key_pattern = [&](const std::string& map_path, 
                             const std::vector<std::string>& keys, const char* pattern_name) {
    BOOST_TEST_MESSAGE("Testing MapStorage key pattern: " << pattern_name);
    
    auto storage = MapStorage::create(map_path.c_str());
    auto db = (*storage)["pattern_test"];
    auto cursor = db.cursor();
    
    // Insert without explicit transaction management
    for (const auto& key : keys) {
      cursor.find(key);
      BOOST_REQUIRE(!cursor.is_valid());
      cursor.value("value_" + key);
      cursor.commit(); // Commit each key
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.key().string(), key);
    }
    // No explicit commit needed at end
    
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
  
  test_key_pattern(prep.get_map_path() + "_short", short_keys, "Short keys");
  test_key_pattern(prep.get_map_path() + "_long", long_keys, "Long keys");
  test_key_pattern(prep.get_map_path() + "_numeric", numeric_keys, "Numeric keys");
}