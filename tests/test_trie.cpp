/*
Test the trie nodes without bursting.
*/
#define BOOST_TEST_MODULE TrieTest
#define GENERATE

#include <boost/endian/conversion.hpp>

#include "test.hpp"

using boost::endian::big_to_native;
using boost::endian::native_to_big;

BOOST_AUTO_TEST_CASE(insert_one) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_one", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_extend) {
  Preparation p;
  { DBMemory storage(TEST_FILE); }
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "abcd", NULL};
  test_insertion(storage, "insert_start_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_split) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "a*c", NULL};
  test_insertion(storage, "insert_start_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_start_short) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("abc");
  trace.set_value(string("abc"));
  trace.commit();
  check_graph("insert_start_short_0", storage);

  trace.find("");
  trace.set_value(string(""));
  trace.commit();
  check_graph("insert_start_short_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("insert_start_null_0", storage);

  trace.find("abc");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("insert_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("abc");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("change_start_0", storage);

  trace.find("abc");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("change_start_1", storage);
}

BOOST_AUTO_TEST_CASE(change_start_null) {
  // A variant of insert_compress_extend
  Preparation p;
  DBMemory storage(TEST_FILE);
  Trace trace(storage);
  trace.find("");
  trace.set_value(string("aaa"));
  trace.commit();
  check_graph("change_start_null_0", storage);

  trace.find("");
  trace.set_value(string("bbb"));
  trace.commit();
  check_graph("change_start_null_1", storage);
}

BOOST_AUTO_TEST_CASE(insert_compress_split) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "ab*defghi", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_short) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abc*efg", "ab", NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_compress_start) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ba", NULL};
  test_insertion(storage, "insert_compress_start", keys);
}

BOOST_AUTO_TEST_CASE(insert_big_stack) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"a",     "ab",     "abc",    "abcd",
                        "abcde", "abcdef", "abcdeg", NULL};
  test_insertion(storage, "insert_big_stack", keys);

  Trace trace(storage);
  std::cout << "insert 7: abcd*" << std::endl;
  trace.find("abcd*");
  BOOST_REQUIRE(!trace.is_valid());
  trace.set_value("abcd*");
  BOOST_REQUIRE(trace.is_valid());

  std::cout << "insert 8: abcde*" << std::endl;
  trace.find("abcde*");
  BOOST_REQUIRE(!trace.is_valid());
  trace.set_value("abcde*");
  BOOST_REQUIRE(trace.is_valid());
  trace.commit();
  check_graph("insert_big_stack_7", storage);

#ifdef MOVEMENT
  {
    strings_t strings;
    const char *keys[] = {"a",      "ab",     "abc",   "abcd",   "abcde",
                          "abcdef", "abcdeg", "abcd*", "abcde*", NULL};
    for (const char **key = keys; *key; key++) {
      strings.push_back(*key);
    }
    test_movement(storage, strings);
  }
#endif

  // TODO: move test for all
}

BOOST_AUTO_TEST_CASE(insert_leaf_split) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*e*ghi", NULL};
  test_insertion(storage, "insert_leaf_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_split_short) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abcd*fghi", "abcd*", NULL};
  test_insertion(storage, "insert_leaf_split_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_leaf_extend) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abc*efghi", "abc*efghijk", NULL};
  test_insertion(storage, "insert_leaf_extend", keys);
}

BOOST_AUTO_TEST_CASE(insert_array) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", NULL};
  test_insertion(storage, "insert_array", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "abf", "abg",
                        "abh", "abi", "abj", "abk", "abl", "abm", "abn",
                        "abo", "abp", "abA", "ab",  NULL};
  // the last inserts a null leaf
  test_insertion(storage, "insert_trie", keys);
}

BOOST_AUTO_TEST_CASE(overflow_trie) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  uint16_t i;
  Trace trace(storage);
  for (i = 0; i < 258; i++) {
    uint16_t key = native_to_big(i);
    Slice skey((char *)&key, sizeof(key));
    trace.find(skey);
    BOOST_REQUIRE(!trace.is_valid());
    std::string value = std::to_string(i);
    trace.set_value(value);
    trace.commit();
  }
  check_graph("overflow_trie", storage);

  #ifdef MOVEMENT
  {
    uint16_t i = 0;
    for(trace.first(); trace.is_valid(); trace.next(), i++) {
      BOOST_REQUIRE(trace.is_valid());
      uint16_t key = native_to_big(i);
      Slice cmp_key((char *)&key, sizeof(key));
      //std::cout << "iter forward: " << i << std::endl;
      BOOST_REQUIRE(cmp_key == trace.current_key);
    }
    BOOST_REQUIRE(i == 258);

    i = 257;
    for(trace.last(); trace.is_valid(); trace.prev(), i--) {
      BOOST_REQUIRE(trace.is_valid());
      uint16_t key = native_to_big(i);
      Slice cmp_key((char *)&key, sizeof(key));
      // std::cout << "iter backward: " << i << std::endl;
      BOOST_REQUIRE(cmp_key == trace.current_key);
    }
    BOOST_REQUIRE(i == 0xFFFF);  // overflow to -1
  }
  #endif
}

BOOST_AUTO_TEST_CASE(insert_null_leaf) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "ab", NULL};
  test_insertion(storage, "insert_null_leaf", keys);
}

BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg", NULL};
  test_insertion(storage, "insert_value", keys);
}

#ifdef WITH_REMOVE
BOOST_AUTO_TEST_CASE(remove_trie) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  const char *remove[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                          "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  test_remove(storage, "remove_trie", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_intermediate_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_intermediate_value", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", NULL};
  const char *remove[] = {"ab", NULL};
  test_remove(storage, "remove_compress_short", keys, remove);
}
#endif

BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  DBMemory storage(TEST_FILE);
  Slice key("abcdefg");
  Trace trace(storage);

  trace.find(key);
  BOOST_REQUIRE(!trace.is_valid());
  trace.set_value(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), key.string());

  trace.commit();

  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(trace.is_valid());

  std::string value;
  for (int i = 0; i < 20; i++) {
    value.append("abcdefghijklmn");
  }
  trace.set_value(value);

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  value.resize(50);
  trace.set_value(value);  // use value pool

  trace.find(key);
  BOOST_REQUIRE_EQUAL(trace.get_value().string(), value);

  trace.set_value(key);
  trace.commit();
}
