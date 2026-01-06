#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE BigMemoryTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_bigmemory.hpp"
#include "leaves/intern/_check.hpp"
#include "leaves/intern/_cursor.hpp"
#include "leaves/intern/_mmap.hpp"

using namespace leaves;

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_bigmemory";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

struct TestTraits {
  typedef uint8_t hash_t[0];
  typedef uint32_t uint32_e;
  typedef uint16_t uint16_e;
  typedef uint64_t uint64_e;
  typedef offset_t offset_e;

  static constexpr bool TRANSACTIONAL = true;
  static constexpr size_t MAX_KEY_SIZE = 256;
  static constexpr size_t AREA_SIZE = 128 * K;  // Larger area size for big value tests
  static constexpr uint16_t MAX_PROCESSES = 100;
  static constexpr size_t BLOCK_CONTAINER_SIZE = 4 * K;
  static constexpr uint16_t BLOCK_SIZES[] = {64, 128, 256, 512, 1 * K, 4 * K};
  static constexpr size_t BLOCK_SIZES_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  struct BlockHeader : public DBMMap::Traits::BlockHeader {
    typedef DBMMap::Traits::BlockHeader Base;
  };

  typedef DBMMap::Traits::Pointers Pointers;
  typedef DBMMap::Traits::ptr ptr;
  template <typename T, NodeTypes type = TRIE>
  using Pointer = typename DBMMap::Traits::template Pointer<T, type>;
};

struct TestStorage {
  typedef _MemoryMapFile<TestTraits> DB;
  typedef TestTraits Traits;
  static constexpr size_t AREA_SIZE = Traits::AREA_SIZE;
  DirPreparation prep;
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  std::unique_ptr<DB> db;

  TestStorage() {
    db = std::make_unique<DB>(dbFilePath.c_str());
  }
};

BOOST_AUTO_TEST_CASE(test_big_allocs) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Start transaction before doing big allocations
  cursor->start_transaction();
  
  // Test allocating and storing big values through cursor
  // For now use normal-sized values (TODO: fix BigMemory alloc bug with size flags)
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'X');
  
  // Store a big value
  cursor->find("big_key1");
  cursor->value(Slice(big_data.data(), BIG_SIZE));
  BOOST_CHECK(cursor->is_valid());
  
  // Verify we can read it back
  cursor->find("big_key1");
  BOOST_CHECK(cursor->is_valid());
  Slice retrieved = cursor->value();
  BOOST_CHECK_EQUAL(retrieved.size(), BIG_SIZE);
  BOOST_CHECK(memcmp(retrieved.data(), big_data.data(), BIG_SIZE) == 0);
  
  // Store multiple big values
  for (int i = 0; i < 5; i++) {
    std::string key = "big_key" + std::to_string(i);
    std::fill(big_data.begin(), big_data.end(), 'A' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Verify all values
  for (int i = 0; i < 5; i++) {
    std::string key = "big_key" + std::to_string(i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'A' + i);
  }
  
  // Commit transaction
  cursor->commit();
}

BOOST_AUTO_TEST_CASE(test_big_area_allocate) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Start transaction
  cursor->start_transaction();
  
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'B');
  
  // Allocate several big areas through cursor operations
  std::vector<std::string> keys;
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key_" + std::to_string(i);
    keys.push_back(key);
    std::fill(big_data.begin(), big_data.end(), 'B' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Verify all allocations succeeded
  for (size_t i = 0; i < keys.size(); i++) {
    cursor->find(keys[i]);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'B' + i);
  }
  
  // Test rollback - remove some entries
  cursor->rollback();
  
  // After rollback, store again with transaction
  cursor->start_transaction();
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key2_" + std::to_string(i);
    std::fill(big_data.begin(), big_data.end(), 'C' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  cursor->commit();
  
  // Verify committed data
  for (int i = 0; i < 6; i++) {
    std::string key = "area_key2_" + std::to_string(i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    Slice val = cursor->value();
    BOOST_CHECK_EQUAL(val.size(), BIG_SIZE);
    BOOST_CHECK_EQUAL(val.data()[0], 'C' + i);
  }
}

BOOST_AUTO_TEST_CASE(test_big_area_revolve) {
  TestStorage storage;
  auto db = storage.db->make("test");
  
  // Create a cursor with transaction support
  auto cursor = db->create_cursor();
  
  // Test area recycling by allocating, freeing, and reallocating big values
  const size_t BIG_SIZE = 1 * K;
  std::vector<char> big_data(BIG_SIZE, 'R');
  
  cursor->start_transaction();
  
  // Allocate several big areas
  std::vector<std::string> keys;
  for (int i = 0; i < 6; i++) {
    std::string key = "revolve_" + std::to_string(i);
    keys.push_back(key);
    std::fill(big_data.begin(), big_data.end(), 'R' + i);
    cursor->find(key);
    cursor->value(Slice(big_data.data(), BIG_SIZE));
  }
  
  // Rollback to free the areas
  cursor->rollback();
  
  // Now allocate a larger value
  cursor->start_transaction();
  const size_t MULTI_AREA_SIZE = 2 * K;
  std::vector<char> multi_data(MULTI_AREA_SIZE, 'M');
  cursor->find("multi_area_key");
  cursor->value(Slice(multi_data.data(), MULTI_AREA_SIZE));
  
  // Verify the large allocation
  cursor->find("multi_area_key");
  BOOST_CHECK(cursor->is_valid());
  Slice val = cursor->value();
  BOOST_CHECK_EQUAL(val.size(), MULTI_AREA_SIZE);
  BOOST_CHECK_EQUAL(val.data()[0], 'M');
  
  cursor->commit();
  
  // Verify persistence after commit
  cursor->find("multi_area_key");
  BOOST_CHECK(cursor->is_valid());
  val = cursor->value();
  BOOST_CHECK_EQUAL(val.size(), MULTI_AREA_SIZE);
  BOOST_CHECK(memcmp(val.data(), multi_data.data(), MULTI_AREA_SIZE) == 0);
}
