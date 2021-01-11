#define BOOST_TEST_MODULE BurstTest
//#define GENERATE

#include "test.hpp"


BOOST_AUTO_TEST_CASE(insert_null) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "i_null", keys);
}

BOOST_AUTO_TEST_CASE(i_split) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", "abdef", "abklmn", "abklmo", NULL};
  test_insertion(storage, "i_split", keys);
}

BOOST_AUTO_TEST_CASE(i_short_after) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", "abdef", "abklmn", "ab", NULL};
  test_insertion(storage, "i_short_after", keys);
}

BOOST_AUTO_TEST_CASE(i_short_before) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abchij", "abc", "abcklm", "abcklmo", NULL};
  test_insertion(storage, "i_short_before", keys);
}


BOOST_AUTO_TEST_CASE(i_short) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {
      "abcdefg", "abcefghi", "abcfghij", "abcghijk", "abchijkl", "abcd", NULL};
  test_insertion(storage, "i_short", keys);
}

BOOST_AUTO_TEST_CASE(i_same_prefix) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {
      "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh", NULL};
  test_insertion(storage, "i_same_prefix", keys);
}

BOOST_AUTO_TEST_CASE(i_one_letter) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abchij", "abk", "abkmo", "abklm", NULL};
  test_insertion(storage, "i_one_letter", keys);
}

BOOST_AUTO_TEST_CASE(i_all_one_letter) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abd", "abe", "abf", "abg", NULL};
  test_insertion(storage, "i_all_one_letter", keys);
}

BOOST_AUTO_TEST_CASE(i_first_one_letter) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abde", "abdf", "abdg", "abdh", NULL};
  test_insertion(storage, "i_first_one_letter", keys);
}

BOOST_AUTO_TEST_CASE(i_top) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abc", "abe", "ab", NULL};
  test_insertion(storage, "i_top", keys);
}

BOOST_AUTO_TEST_CASE(remove_it) {
  Preparation p;
  Storage storage(TEST_FILE, TEST_OPTIONS);
  const char *keys[] = {"abcdefg", "abhij", "abdef", "abklmn", "abklmo", NULL};
  const char *remove[] = {"abcdefg", "abhij", "abdef", "abklmn", "abklmo", NULL};
  test_remove(storage, "remove", keys, remove);
}
