#define BOOST_TEST_MODULE StorageTest

#include <boost/test/included/unit_test.hpp>
#include <cstdio>
#include <vector>

#define AREA_COUNT 100
#include "../src/storage.hpp"

#include "test.hpp"

#define TEST_FILE "test.lvs"

using namespace leaves;


BOOST_AUTO_TEST_CASE(start_storage) {
  Preparation p;

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.get_header()->db_version, 0);
    BOOST_REQUIRE_EQUAL(storage.header.db_version, 0);
  }

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.get_header()->db_version, 0);
    BOOST_REQUIRE_EQUAL(storage.header.db_version, 0);
  }
}

BOOST_AUTO_TEST_CASE(value_pools) {
  Preparation p;
  Storage storage(TEST_FILE);

  std::vector<std::string> vals;
  for (int i = 4; i <= 31; i++) {
    vals.push_back(std::string((1 << i) - 4, 'a'));
  }

  storage.start_transaction();

  std::vector<stored_ptr> vpointer;
  int j = 0;
  for (auto i = vals.begin(); i != vals.end(); i++, j++) {
    vpointer.push_back(storage.new_value(*i));
    StoragePool& pool = storage.header.pools[j];
    offset_t start = pool.slast - block_size_per_pool[j];
    if (j == 8) {
      start += PAGE_SIZE;  // the first page allocation
    }
    BOOST_REQUIRE_EQUAL(pool.scurrent, start + (1 << (j + 4)));
  }

  storage.prepare_commit(stored_ptr());
  storage.commit();

  for (int i = 0; i <= 27; i++) {
    StoragePool& pool = storage.header.pools[i];
    offset_t start = pool.slast - block_size_per_pool[i];
    if (i == 8) {
      start += 28 * PAGE_SIZE +
               PAGE_SIZE;  // 28 pages of txn_head + preallocation of first page
    }
    BOOST_REQUIRE_EQUAL(pool.scurrent, start + (1 << (i + 4)));
  }
}

BOOST_AUTO_TEST_CASE(rollback1) {
  Preparation p;

  Storage storage(TEST_FILE);

  storage.start_transaction();
  stored_ptr ptr1 = storage.new_value(std::string(16, 'a'));
  BOOST_REQUIRE_EQUAL(ptr1.size, 36);
  storage.rollback();

  storage.start_transaction();
  stored_ptr ptr2 = storage.new_value(std::string(16, 'a'));
  BOOST_REQUIRE_EQUAL(ptr1.val, ptr2.val);
  storage.rollback();

  StoragePool& pool = storage.header.pools[0];
  // the complete block is free
  BOOST_REQUIRE_EQUAL(pool.scurrent + block_size_per_pool[0], pool.slast);
}

BOOST_AUTO_TEST_CASE(rollback2) {
  Preparation p;

  Storage storage(TEST_FILE);
  StoragePool& pool = storage.header.pools[0];

  storage.start_transaction();
  stored_ptr ptr1 = storage.new_value(std::string(16, 'a'));
  storage.prepare_commit(stored_ptr());
  storage.rollback();
  BOOST_REQUIRE(pool.sfree);  // a free block was created

  storage.start_transaction();
  stored_ptr ptr2 = storage.new_value(std::string(16, 'a'));
  BOOST_REQUIRE_EQUAL(ptr1.val, ptr2.val);
  storage.rollback();

  BOOST_REQUIRE(!storage.header.pools[0].sfree);

  // the complete block is free
  BOOST_REQUIRE_EQUAL(pool.scurrent + block_size_per_pool[0], pool.slast + 16);
}

BOOST_AUTO_TEST_CASE(overflow_page) {
  Preparation p;

  Storage storage(TEST_FILE);

  auto val = std::string(16, 'a');
  storage.start_transaction();
  std::vector<stored_ptr> values;
  for (int i = 0; i < 800; i++) {
    values.push_back(storage.new_value(val));
  }
  storage.prepare_commit(stored_ptr());
  storage.rollback();

  BOOST_REQUIRE(storage.header.pools[0].sfree);

  MarkedBlocks* b = storage.view->get_blocks(storage.header.pools[0].sfree);
  BOOST_REQUIRE_EQUAL(b->count, 800 - MarkedBlocks::REF_COUNT);
  BOOST_REQUIRE(b->next);
  b = storage.view->get_blocks(b->next);
  BOOST_REQUIRE_EQUAL(b->count, 0 + MarkedBlocks::REF_COUNT);
  BOOST_REQUIRE(!b->next);

  storage.start_transaction();
  for (int i = 0; i < 800; i++) {
    stored_ptr pval = storage.new_value(val);
    BOOST_REQUIRE_EQUAL(pval.val, values.back().val);
    values.pop_back();
  }

  for (int i = 0; i < 400; i++) {
    storage.new_value(val);
  }

  storage.prepare_commit(stored_ptr());
  storage.rollback();

  BOOST_REQUIRE(storage.header.pools[0].sfree);

  b = storage.view->get_blocks(storage.header.pools[0].sfree);
  BOOST_REQUIRE_EQUAL(b->count, 1200 - 2 * MarkedBlocks::REF_COUNT);
  BOOST_REQUIRE(b->next);
  b = storage.view->get_blocks(b->next);
  BOOST_REQUIRE_EQUAL(b->count, 0 + MarkedBlocks::REF_COUNT);
  BOOST_REQUIRE(b->next);
  b = storage.view->get_blocks(b->next);
  BOOST_REQUIRE_EQUAL(b->count, 0 + MarkedBlocks::REF_COUNT);
  BOOST_REQUIRE(!b->next);
}

BOOST_AUTO_TEST_CASE(page_pool_overflow) {
  Preparation p;

  Storage storage(TEST_FILE);

  storage.start_transaction();
  storage.new_value(std::string(16, 'a'));

  for (int i = 0; i < MarkedBlocks::REF_COUNT - 1; i++) {
    Page* p = storage.alloc_new_page();
    storage.write_page(p);
  }
  storage.prepare_commit(stored_ptr());
  storage.rollback();

  MarkedBlocks* b = storage.view->get_blocks(
      storage.header.pools[stored_ptr::PAGE_POOL].sfree);

  BOOST_REQUIRE_EQUAL(b->count, MarkedBlocks::REF_COUNT - 1);
}

BOOST_AUTO_TEST_CASE(multi_open) {
  Preparation p;

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.get_header()->db_version, 0);
    BOOST_REQUIRE_EQUAL(storage.header.db_version, 0);

    {
      Storage storage(TEST_FILE);
      BOOST_REQUIRE_EQUAL(storage.get_header()->db_version, 0);
      BOOST_REQUIRE_EQUAL(storage.header.db_version, 0);
    }
  }
}
