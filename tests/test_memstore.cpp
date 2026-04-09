#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemoryStorageTest

#include <boost/test/included/unit_test.hpp>
#include <string>

#include "leaves/intern/storage/_memstore.hpp"

using namespace leaves;

BOOST_AUTO_TEST_CASE(test_memory_storage_area_allocation) {
  _MemoryStorage storage;
  
  // Test single area allocation
  auto area1 = storage.alloc_single_area();
  BOOST_CHECK(area1);
  BOOST_CHECK_EQUAL(area1->size(), _MemoryTraits::AREA_SIZE);
  
  // Test that multi area allocation throws exception
  size_t multi_size = 2 * _MemoryTraits::AREA_SIZE;
  BOOST_CHECK_THROW(storage.alloc_multi_area(multi_size), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_memory_storage_block_allocation) {
  _MemoryStorage storage;
  auto& db = storage.db();
  
  // Test block allocation through the DB
  auto block = db.alloc(100);
  BOOST_CHECK(block);
  
  // Test freeing the block
  db.free(block);  // Should not crash
}

BOOST_AUTO_TEST_CASE(test_memory_storage_resolve) {
  _MemoryStorage storage;
  auto& db = storage.db();
  
  // Test pointer resolution
  auto area = storage.alloc_single_area();
  
  // Test resolve with area - offset to page_ptr using resolve(offset_t) method
  auto area_offset = area->offset();
  offset_t area_off_temp(area_offset);
  auto resolved_block = db.resolve(&area_off_temp);
  BOOST_CHECK(resolved_block);
  
  // Test reverse resolve - page_ptr to offset
  auto offset_back = db.resolve(resolved_block);
  BOOST_CHECK_EQUAL(offset_back, area_offset);
}

BOOST_AUTO_TEST_CASE(test_pageheader_alignment) {
  // PageHeader must be 8 bytes to keep PAGE_SIZES 8-byte aligned
  BOOST_CHECK_EQUAL(sizeof(_MemoryTraits::PageHeader), 8);
  for (int i = 0; i < _MemoryTraits::PAGE_SIZES_COUNT; i++) {
    BOOST_CHECK_EQUAL(_MemoryTraits::PAGE_SIZES[i] % 8, 0);
  }
}

BOOST_AUTO_TEST_CASE(test_cursor_insert_and_find) {
  _MemoryStorage storage;
  auto cursor = storage.create_cursor();

  // Insert keys
  for (int i = 0; i < 100; i++) {
    char key[32];
    snprintf(key, sizeof(key), "%016d", i);
    Slice val("value");
    cursor->find(key);
    cursor->value(val);
  }

  // Find each key
  for (int i = 0; i < 100; i++) {
    char key[32];
    snprintf(key, sizeof(key), "%016d", i);
    cursor->find(key);
    BOOST_CHECK(cursor->is_valid());
    BOOST_CHECK_EQUAL(cursor->value(), Slice("value"));
  }

  // Non-existent key
  cursor->find("9999999999999999");
  BOOST_CHECK(!cursor->is_valid());
}

BOOST_AUTO_TEST_CASE(test_cursor_sequential_iteration) {
  _MemoryStorage storage;
  auto cursor = storage.create_cursor();

  const int N = 50;
  for (int i = 0; i < N; i++) {
    char key[32];
    snprintf(key, sizeof(key), "%08d", i);
    cursor->find(key);
    cursor->value(Slice("v"));
  }

  // Forward iteration
  int count = 0;
  std::string prev;
  for (cursor->first(); cursor->is_valid(); cursor->next()) {
    std::string k(cursor->key().data(), cursor->key().size());
    if (count > 0) BOOST_CHECK_GT(k, prev);
    prev = k;
    count++;
  }
  BOOST_CHECK_EQUAL(count, N);

  // Backward iteration
  count = 0;
  prev.clear();
  for (cursor->last(); cursor->is_valid(); cursor->prev()) {
    std::string k(cursor->key().data(), cursor->key().size());
    if (count > 0) BOOST_CHECK_LT(k, prev);
    prev = k;
    count++;
  }
  BOOST_CHECK_EQUAL(count, N);
}

BOOST_AUTO_TEST_CASE(test_cursor_overwrite) {
  _MemoryStorage storage;
  auto cursor = storage.create_cursor();

  cursor->find("mykey");
  cursor->value(Slice("first"));
  BOOST_CHECK_EQUAL(cursor->value(), Slice("first"));

  cursor->find("mykey");
  cursor->value(Slice("second"));

  cursor->find("mykey");
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("second"));
}

BOOST_AUTO_TEST_CASE(test_cursor_remove) {
  _MemoryStorage storage;
  auto cursor = storage.create_cursor();

  for (int i = 0; i < 10; i++) {
    char key[32];
    snprintf(key, sizeof(key), "%04d", i);
    cursor->find(key);
    cursor->value(Slice("val"));
  }

  // Remove key "0005"
  cursor->find("0005");
  BOOST_CHECK(cursor->is_valid());
  cursor->remove();

  // Verify it's gone
  cursor->find("0005");
  BOOST_CHECK(!cursor->is_valid());

  // Verify others still exist
  int count = 0;
  for (cursor->first(); cursor->is_valid(); cursor->next()) count++;
  BOOST_CHECK_EQUAL(count, 9);
}

BOOST_AUTO_TEST_CASE(test_cursor_random_keys) {
  _MemoryStorage storage;
  auto cursor = storage.create_cursor();

  // Insert 1000 random-order keys (shuffle via modular arithmetic)
  const int N = 1000;
  for (int i = 0; i < N; i++) {
    int k = (i * 7919) % N;  // pseudo-random permutation
    char key[32];
    snprintf(key, sizeof(key), "%016d", k);
    char val[32];
    snprintf(val, sizeof(val), "v%d", k);
    cursor->find(key);
    cursor->value(Slice(val));
  }

  // Verify all present
  int count = 0;
  for (cursor->first(); cursor->is_valid(); cursor->next()) count++;
  BOOST_CHECK_EQUAL(count, N);

  // Spot-check a few
  cursor->find("0000000000000042");
  BOOST_CHECK(cursor->is_valid());
  BOOST_CHECK_EQUAL(cursor->value(), Slice("v42"));
}

BOOST_AUTO_TEST_CASE(test_memory_db_alloc_single_area) {
  // Exercises _memstore.hpp L101 — _MemoryDB::alloc_single_area()
  // Write enough data to exhaust the initial area (512K) and trigger growth.
  _MemoryStorage storage;
  auto& db = storage.db();
  auto cursor = db.create_cursor();

  // Each area is 512K. Use moderately-sized values with many keys to fill it.
  std::string value(200, 'x');
  for (int i = 0; i < 5000; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key_%05d", i);
    cursor->find(key);
    cursor->value(Slice(value));
  }

  // Verify we allocated more than the initial area
  BOOST_CHECK_GT(storage._areas.size(), 1u);

  // Verify data is accessible
  cursor->find("key_00000");
  BOOST_CHECK(cursor->is_valid());
}