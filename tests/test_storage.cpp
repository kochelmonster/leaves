#define BOOST_TEST_MODULE StorageTest

#include <cstdio>
#include <boost/test/included/unit_test.hpp>

#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;


Options TEST_OPTIONS(1024*16, 100, 1);


BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 0);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 4);
    *storage.version = 1;
  }

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 1);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 4);

    segment_ptr ptr1 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr1.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr1.delta, 144);

    segment_ptr ptr2 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 144+100);

    storage.pools[4].free(ptr1);
    storage.pools[4].free(ptr2);

    ptr1 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr1.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr1.delta, 144+100);

    ptr2 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 144);

    ptr1 = storage.pools[2].allocate();
    for(int i = 0; i < 500; i++) {
      ptr2 = storage.pools[2].allocate();
    }
    // std::cout << "allocated1 " << ptr1.delta << ", " << ptr1.segment_id << std::endl;
    // std::cout << "allocated2 " << ptr2.delta << ", " << ptr2.segment_id << std::endl;
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 4);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 5360);

    storage.pools[2].free(ptr1);
    ptr2 = storage.pools[2].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 4864);

    ptr2 = storage.allocate(2000);
    storage.free(ptr2);

    ptr1 = storage.allocate(2000);
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, ptr1.segment_id);
    BOOST_REQUIRE_EQUAL(ptr2.delta, ptr1.delta);

    // std::cout << "allocated " << ptr2.delta << ", " << ptr2.segment_id << std::endl;

    std::remove(TEST_FILE);
  }

}
