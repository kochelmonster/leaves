#define BOOST_TEST_MODULE MoveTest
#include <cstdio>
#include <boost/test/included/unit_test.hpp>
#include "test.hpp"


void insert(Storage& storage, const Slice& key, const char* test_name) {

  




BOOST_AUTO_TEST_SUITE(Move)

BOOST_AUTO_TEST_CASE(insert) {
  Preparation p;
  Storage storage(TEST_FILE, SEGMENT_SIZE);
  test_insert_first(storage);
}

BOOST_AUTO_TEST_SUITE_END()
