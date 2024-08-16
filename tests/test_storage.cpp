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
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
  }

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
  }
}


BOOST_AUTO_TEST_CASE(multi_open) {
  Preparation p;

  {
    Storage storage(TEST_FILE);
    BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);

    {
      Storage storage(TEST_FILE);
      BOOST_REQUIRE_EQUAL(storage.memory->db->db_version, 0);
    }
  }
}
