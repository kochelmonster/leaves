#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MemoryStorageTest

#include <boost/test/included/unit_test.hpp>
#include <string>

#include "leaves/intern/_memstore.hpp"

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
  auto db = storage.db();
  
  // Test block allocation through the DB
  auto block = db->alloc(100);
  BOOST_CHECK(block);
  
  // Test freeing the block
  db->free(block);  // Should not crash
}

BOOST_AUTO_TEST_CASE(test_memory_storage_resolve) {
  _MemoryStorage storage;
  auto db = storage.db();
  
  // Test pointer resolution
  auto area = storage.alloc_single_area();
  
  // Test resolve with area - offset to block_ptr using resolve(offset_t) method
  auto area_offset = area->offset();
  auto resolved_block = db->resolve(area_offset);
  BOOST_CHECK(resolved_block);
  
  // Test reverse resolve - block_ptr to offset
  auto offset_back = db->resolve(resolved_block);
  BOOST_CHECK_EQUAL(offset_back, area_offset);
}