#define BOOST_TEST_MODULE StorageTest

#include <cstdio>
#include <boost/test/included/unit_test.hpp>

#define AREA_COUNT 100
#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;



BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  {
    Storage storage(TEST_FILE, 8*PAGE_SIZE, 2*PAGE_SIZE);
    BOOST_REQUIRE_EQUAL(storage.start->header.version, 0);
  }

  {
    Storage storage(TEST_FILE, 8*PAGE_SIZE, 2*PAGE_SIZE);

    const Page *root = storage.page(0);
    BOOST_REQUIRE_EQUAL(root, storage.start);

    BOOST_REQUIRE_EQUAL(storage.transaction_id(), 0);
    BOOST_REQUIRE_EQUAL(storage.block_end, 8);
    BOOST_REQUIRE_EQUAL(storage.start->header.freed_head, 0);
    BOOST_REQUIRE_EQUAL(storage.start->header.free_block, 2);

    storage.transaction_inc();
    location_p page = storage.alloc();
    storage.flush();

    BOOST_REQUIRE_EQUAL(page.page, 2);
    BOOST_REQUIRE_EQUAL(storage.transaction_id(), 1);
    BOOST_REQUIRE_EQUAL(storage.start->header.free_block, 3);

    location_p page1 = storage.alloc(7*PAGE_SIZE-4);
    storage.flush();

    root = storage.page(1);
    BOOST_REQUIRE_EQUAL(root, storage.start+1);
    BOOST_REQUIRE_EQUAL(page1.page, 3);

    location_p page2 = storage.alloc();
    storage.flush();

    BOOST_REQUIRE_EQUAL(page2.page, 10);

    storage.free(page1, 7*PAGE_SIZE-4);
    storage.flush();
    BOOST_REQUIRE_EQUAL(storage.start->header.freed_head, 3);

    const Page* fp = storage.page(3);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 4);

    fp = storage.page(4);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 5);

    fp = storage.page(5);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 6);

    fp = storage.page(6);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 7);

    fp = storage.page(7);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 8);

    fp = storage.page(8);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 9);

    fp = storage.page(9);
    BOOST_REQUIRE_EQUAL(fp->next_storage, 0);

   
    page = storage.alloc();
    storage.flush();
    BOOST_REQUIRE_EQUAL(page.page, 3);
    BOOST_REQUIRE_EQUAL(storage.start->header.freed_head, 4);

    Page* wp = storage.get_writable(1);
    wp->content[0] = 1;

    wp = storage.get_writable(10);
    wp->content[0] = 2;

    wp = storage.get_writable(3);
    wp->content[0] = 3;

    wp = storage.get_writable(9);
    wp->content[1] = 4;

    storage.flush();

    fp = storage.page(1);
    BOOST_REQUIRE_EQUAL(fp->content[0], 1);

    fp = storage.page(10);
    BOOST_REQUIRE_EQUAL(fp->content[0], 2);
    
    fp = storage.page(3);
    BOOST_REQUIRE_EQUAL(fp->content[0], 3);

    fp = storage.page(9);
    BOOST_REQUIRE_EQUAL(fp->content[1], 4);

    std::remove(TEST_FILE);
  }

}
