#define BOOST_TEST_MODULE StorageTest
//#define BOOST_TEST_NO_MAIN
#include <cstdio>
#include <boost/test/included/unit_test.hpp>

#define AREA_COUNT 100
#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace larch_leaves;

typedef unsigned char trieindex_t;


#define SEGMENT_SIZE 1024*16


BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  {
    Storage storage(TEST_FILE, SEGMENT_SIZE);
    BOOST_REQUIRE_EQUAL(*storage.version, 0);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);
    *storage.version = 1;
  }

  {
    Storage storage(TEST_FILE, SEGMENT_SIZE);
    BOOST_REQUIRE_EQUAL(*storage.version, 1);
    BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);

    segment_ptr ptr1 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr1.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr1.delta, 144);

    segment_ptr ptr2 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 144+104);

    storage.pools[4].free(ptr1);
    storage.pools[4].free(ptr2);

    ptr1 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr1.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr1.delta, 144+104);

    ptr2 = storage.pools[4].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 144);

    ptr1 = storage.pools[0].allocate();
    for(int i = 0; i < 100; i++) {
      ptr2 = storage.pools[0].allocate();
    }
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 2);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 10560);

    storage.pools[0].free(ptr1);
    ptr2 = storage.pools[0].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.segment_id, 0);
    BOOST_REQUIRE_EQUAL(ptr2.delta, 360);
        
    std::cout << "allocated " << ptr2.delta << ", " << ptr2.segment_id << std::endl;


  }

}
