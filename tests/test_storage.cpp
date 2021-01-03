#define BOOST_TEST_MODULE StorageTest

#include <cstdio>
#include <boost/test/included/unit_test.hpp>

#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;


Options TEST_OPTIONS(1024*16, 100, 1);

#define NODE_SIZE  64


BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 0);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);
    *storage.version = 1;
  }

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 1);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);

    resolved_ptr ptr1(storage.pools[2].allocate());

    size_t delta = ptr1.me.delta;
    //std::cout << "allocated1 " << ptr1.me.delta << ", " << ptr1.me.segment_id << std::endl;

    BOOST_REQUIRE_EQUAL(ptr1.me.segment_id, 0);

    resolved_ptr ptr2(storage.pools[2].allocate());
    //std::cout << "allocated2 " << ptr2.me.delta << ", " << ptr2.me.segment_id << std::endl;
    BOOST_REQUIRE_EQUAL(ptr2.me.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr2.me.delta, delta + NODE_SIZE);

    storage.pools[2].free(ptr1);
    storage.pools[2].free(ptr2);

    ptr1 = storage.pools[2].allocate();
    //std::cout << "allocated3 " << ptr1.me.delta << ", " << ptr1.me.segment_id << std::endl;
    BOOST_REQUIRE_EQUAL(ptr1.me.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr1.me.delta, delta + NODE_SIZE);

    ptr2 = storage.pools[2].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.me.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr2.me.delta, delta);

    ptr1 = storage.pools[2].allocate();
    delta = ptr1.me.delta;

    for(int i = 0; i < 200; i++) {
      ptr2 = storage.pools[2].allocate();
    }
    //std::cout << "allocated4 " << ptr1.me.delta << ", " << ptr1.me.segment_id << std::endl;
    //std::cout << "allocated5 " << ptr2.me.delta << ", " << ptr2.me.segment_id << std::endl;
    BOOST_REQUIRE_EQUAL(ptr2.me.segment_id, 3);

    storage.pools[2].free(ptr1);
    ptr2 = storage.pools[2].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.me.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr2.me.delta, delta);

    ptr2 = storage.allocate(2000);
    storage.free(ptr2, 2000);

    ptr1 = storage.allocate(2000);
    BOOST_REQUIRE_EQUAL(ptr2.me.segment_id, ptr1.me.segment_id);
    BOOST_REQUIRE_EQUAL(ptr2.me.delta, ptr1.me.delta);

    // std::cout << "allocated " << ptr2.delta << ", " << ptr2.segment_id << std::endl;

    std::remove(TEST_FILE);
  }

}
