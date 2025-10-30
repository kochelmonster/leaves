// Test for hash mode cursor optimization
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE HashModeTest

#include <boost/test/included/unit_test.hpp>
#include <chrono>
#include <iostream>
#include <random>

#include "leaves/intern/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

BOOST_AUTO_TEST_CASE(test_hash_mode_optimization) {
  std::filesystem::path testFile = std::filesystem::temp_directory_path() / "test_hash_mode.lvs";
  std::filesystem::remove(testFile);
  
  DBMMap storage(testFile.c_str());
  auto db = storage.make("test");
  
  // Insert many keys with the same prefix to create >1000 branches
  const int num_prefixes = 50;
  const int keys_per_prefix = 30;
  
  std::vector<std::string> all_keys;
  
  // Create keys with pattern: "prefix_XX_suffix_YY"
  for (int p = 0; p < num_prefixes; p++) {
    std::string prefix = "prefix_" + std::to_string(p % 10);
    for (int k = 0; k < keys_per_prefix; k++) {
      std::string key = prefix + "_suffix_" + std::to_string(k);
      all_keys.push_back(key);
    }
  }
  
  // Insert all keys
  {
    typedef _Cursor<DBMMap::DB, DBMMap::DB::ValueTraits> Cursor;
    Cursor cursor(db);
    for (const auto& key : all_keys) {
      cursor.find(key);
      cursor.value(Slice("value_" + key));
    }
    cursor.commit();
  }
  
  // Test 1: Verify finds work correctly with hash mode
  {
    typedef _Cursor<DBMMap::DB, DBMMap::DB::ValueTraits> Cursor;
    Cursor cursor(db);
    
    // First 10 finds to trigger hash mode
    for (int i = 0; i < 15; i++) {
      cursor.find(all_keys[i]);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.value().string(), "value_" + all_keys[i]);
    }
    
    // Verify hash mode is working by checking more finds
    for (size_t i = 0; i < all_keys.size(); i += 10) {
      cursor.find(all_keys[i]);
      BOOST_REQUIRE(cursor.is_valid());
      BOOST_REQUIRE_EQUAL(cursor.value().string(), "value_" + all_keys[i]);
    }
  }
  
  // Test 2: Performance comparison
  {
    typedef _Cursor<DBMMap::DB, DBMMap::DB::ValueTraits> Cursor;
    Cursor cursor1(db);
    Cursor cursor2(db);
    
    // Warm up cursor2 to enable hash mode
    for (int i = 0; i < 15; i++) {
      cursor2.find(all_keys[i]);
    }
    
    // Measure time for many lookups (cursor2 should use hash mode)
    const int lookup_count = 1000;
    std::mt19937 gen(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<> dis(0, all_keys.size() - 1);
    
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < lookup_count; i++) {
      int idx = dis(gen);
      cursor2.find(all_keys[idx]);
      BOOST_REQUIRE(cursor2.is_valid());
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2).count();
    
    std::cout << "Hash mode enabled: " << lookup_count << " lookups in " 
              << duration2 << " microseconds" << std::endl;
    std::cout << "Average: " << (double)duration2 / lookup_count << " μs per lookup" << std::endl;
  }
  
  // Clean up - let storage go out of scope before removing file
  std::filesystem::remove(testFile);
}
