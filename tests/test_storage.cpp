#define BOOST_TEST_MODULE StorageTest

#include <cstdio>
#include <boost/test/included/unit_test.hpp>
#include <filesystem>

#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;


#define SEGMENT_SIZE (1024*16)

Options TEST_OPTIONS(1024*16, 100, 1);

#define NODE_SIZE  64


size_t get_size(const Storage& storage) {
  return std::filesystem::file_size(storage.file.get_name());
}


BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  size_t fsize;

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 0);
    //BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);
    *storage.version = 1;

    fsize = get_size(storage);
  }

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(*storage.version, 1);
    BOOST_REQUIRE_EQUAL(fsize, get_size(storage));

    int64_t base = (int64_t)storage.region.get_address();
    any_ptr ptr1(storage.pools[2].allocate());


    int64_t delta = ptr1.as_int;
    std::cout << "allocated1 " << ptr1.as_int - base << std::endl;

    any_ptr ptr2(storage.pools[2].allocate());
    std::cout << "allocated2 " << ptr2.as_int - base << std::endl;

    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta + NODE_SIZE);

    storage.free(ptr1);
    storage.free(ptr2);

    ptr1 = storage.pools[2].allocate();
    std::cout << "allocated3 " << ptr1.as_int - base << std::endl;

    BOOST_REQUIRE_EQUAL(ptr1.as_int, delta + NODE_SIZE);

    ptr2 = storage.pools[2].allocate();
    std::cout << "allocated4 " << ptr2.as_int - base << std::endl;
    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta);

    ptr1 = storage.pools[2].allocate();
    delta = ptr1.as_int;

    for(int i = 0; i < 300; i++) {
      ptr2 = storage.pools[2].allocate();
    }
    std::cout << "allocated5 " << ptr1.as_int - base << std::endl;
    std::cout << "allocated6 " << ptr2.as_int - base << std::endl;
    BOOST_REQUIRE_EQUAL(get_size(storage), fsize+SEGMENT_SIZE);

    storage.pools[2].free(ptr1);
    ptr2 = storage.pools[2].allocate();
    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta);

    ptr2 = storage.allocate(2000);
    storage.free(ptr2);

    ptr1 = storage.allocate(2000);
    BOOST_REQUIRE_EQUAL(ptr2.as_int, ptr1.as_int);

    std::cout << "allocated7 " << ptr2.as_int - base << std::endl;

    std::remove(TEST_FILE);
  }

}
