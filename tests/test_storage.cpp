#define BOOST_TEST_MODULE StorageTest

#include <cstdio>
#include <boost/test/included/unit_test.hpp>
#include <filesystem>

#include "../src/storage.hpp"


#define TEST_FILE "test.lvs"

using namespace leaves;

#define GROW_SIZE 1024*32

Options TEST_OPTIONS(GROW_SIZE);



size_t get_size(const Storage& storage) {
  return std::filesystem::file_size(storage.file.get_name());
}


#define TEST_SIZE 256

BOOST_AUTO_TEST_CASE(start_storage) {
  std::remove(TEST_FILE);

  size_t fsize;

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(storage.header->version, 0);
    //BOOST_REQUIRE_EQUAL(storage.segments.size(), 3);
    storage.header->version = 1;
    fsize = get_size(storage);
  }

  {
    Storage storage(TEST_FILE, TEST_OPTIONS);
    BOOST_REQUIRE_EQUAL(storage.header->version, 1);
    BOOST_REQUIRE_EQUAL(fsize, get_size(storage));

    int64_t base = (int64_t)storage.region.get_address();
    any_ptr ptr1(storage.allocate(TEST_SIZE));

    int64_t delta = ptr1.as_int;
    std::cout << "allocated1 " << ptr1.as_int - base << std::endl;

    any_ptr ptr2(storage.allocate(TEST_SIZE));
    std::cout << "allocated2 " << ptr2.as_int - base << std::endl;

    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta + TEST_SIZE);

    storage.free(ptr1);
    storage.free(ptr2);

    ptr1 = storage.allocate(TEST_SIZE);
    std::cout << "allocated3 " << ptr1.as_int - base << std::endl;

    BOOST_REQUIRE_EQUAL(ptr1.as_int, delta + TEST_SIZE);

    ptr2 = storage.allocate(TEST_SIZE);
    std::cout << "allocated4 " << ptr2.as_int - base << std::endl;
    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta);

    ptr1 = storage.allocate(TEST_SIZE);
    delta = ptr1.as_int;

    for(int i = 0; i < 110; i++) {
      ptr2 = storage.allocate(TEST_SIZE);
    }
    std::cout << "allocated5 " << ptr1.as_int - base << std::endl;
    std::cout << "allocated6 " << ptr2.as_int - base << std::endl;
    BOOST_REQUIRE_EQUAL(get_size(storage), PAGE_SIZE+2*GROW_SIZE);
    BOOST_REQUIRE_EQUAL(get_size(storage), storage.header->memory.db_size+PAGE_SIZE);

    storage.free(ptr1);
    ptr2 = storage.allocate(TEST_SIZE);
    BOOST_REQUIRE_EQUAL(ptr2.as_int, delta);

    ptr2 = storage.allocate(2000);
    storage.free(ptr2);

    ptr1 = storage.allocate(2000);
    BOOST_REQUIRE_EQUAL(ptr2.as_int, ptr1.as_int);

    std::cout << "allocated7 " << ptr2.as_int - base << std::endl;

    std::remove(TEST_FILE);
  }

}
