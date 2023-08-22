/*
Test the trie nodes without bursting.
*/
#define BOOST_TEST_MODULE TrieTest
//#define GENERATE

#include "test.hpp"
#if 0
BOOST_AUTO_TEST_CASE(insert_null) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", NULL};
  test_insertion(storage, "insert_null", keys);
}

BOOST_AUTO_TEST_CASE(insert_compresstrie_split) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefghi", "abddefg", NULL};
  test_insertion(storage, "insert_compress_split", keys);
}

BOOST_AUTO_TEST_CASE(insert_compresstrie_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "ab",  NULL};
  test_insertion(storage, "insert_compress_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_value) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abc", "abcdefg",  NULL};
  test_insertion(storage, "insert_value", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"aba", "abb", "abc", "abd", "abe", "ab", NULL};
  test_insertion(storage, "insert_trie_short", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_lower_grow) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a@", "aM", "aA", "aB", "aD", "aE", "aF", "aG", "aH",
                        "aI", "aJ", "aK", "aC", "aL", "aN", "aO", NULL};
  test_insertion(storage, "insert_trie_lower_grow", keys);
}

BOOST_AUTO_TEST_CASE(insert_trie_upper_grow) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"a ", "a0", "a@", "aP", "ap", NULL};
  test_insertion(storage, "insert_trie_upper_grow", keys);
}

BOOST_AUTO_TEST_CASE(big_compressed) {
  Preparation p;
  Storage storage(TEST_FILE);

  std::string key1;
  for(int i = 0; i < 30; i++) {
    key1.append("abcdefghijklmn");
  }

  const char *keys[] = {key1.c_str(), NULL};
  test_insertion(storage, "big_compressed", keys);
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
  const char *remove[] = {"ab", NULL };
  test_remove(storage, "remove_intermediate_value", keys, remove);
}

BOOST_AUTO_TEST_CASE(remove_compress_short) {
  Preparation p;
  Storage storage(TEST_FILE);
  const char *keys[] = {"abcdefg", "abcefgh", "ab", NULL};
  const char *remove[] = {"ab", NULL };
  test_remove(storage, "remove_compress_short", keys, remove);
}
#endif


BOOST_AUTO_TEST_CASE(replace_value) {
  Preparation p;
  Storage storage(TEST_FILE);

  Slice key("abcdefg");
  Trace trace(storage);

  trace.find(key);
  BOOST_REQUIRE(!trace.valid());
  trace.set_value(key);

  // std::cout << "insert " << test_name << std::endl;
  trace.find(key);
  BOOST_REQUIRE(trace.valid());

  std::string value;
  for(int i = 0; i < 20; i++) {
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
}
#endif

BOOST_AUTO_TEST_CASE(insert_link) {
  Preparation p;
  Storage storage(TEST_FILE);
  string value(4050, 'a');
  insert(storage, "insert_link_0", "abc", value);
  insert(storage, "insert_link_1", "abcdef", value);
  value.append(3000, 'b');
  insert(storage, "insert_link_2", "abcdeghijklmnopqrst", value);
}
