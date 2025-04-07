/*
Test the the cursor with burst table
*/
#define BOOST_TEST_MODULE BurstTest
#define GENERATE

#include "test.hpp"

BOOST_AUTO_TEST_CASE(insert_simple) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"a", "b", "d", "c", NULL};
  test_insertion(storage, "insert_burst", keys, 500);
}

BOOST_AUTO_TEST_CASE(insert_burst) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"a", "b", "c", "d", "e", "h", "f", "g", "l", "i",
                        "j", "k", "m", "n", "o", "p", "q", "r", NULL};
  test_insertion(storage, "insert_burst", keys, 500);
}

BOOST_AUTO_TEST_CASE(insert_prefix_burst) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"00a", "00b", "00c", "00d", "00e", "00h", "00f",
                        "00g", "00l", "00i", "00j", "00k", "00m", "00n",
                        "00o", "00p", "00q", "00r", "00", NULL};
  test_insertion(storage, "insert_prefix_burst", keys, 500);
}

// trie burst trie
BOOST_AUTO_TEST_CASE(insert_tbt) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"001a", "001b", "001c", "003a", "003b", "003c",
                        "001d", "003d", "001e", "003e", "002a", NULL};
  test_insertion(storage, "insert_tbt", keys, 1000);
}

// trie burst
BOOST_AUTO_TEST_CASE(insert_tb) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"001a", "001b", "001c", "003a", "003b",
                        "003c", "001d", "001e", "002a", NULL};
  test_insertion(storage, "insert_tb", keys, 1000);
}

BOOST_AUTO_TEST_CASE(insert_none) {
  Preparation p;
  DBMMapBurst storage(TEST_FILE);
  const char *keys[] = {"00", "00a", "00b", "00c", "00d", NULL};
  test_insertion(storage, "insert_none", keys, 1000);
}